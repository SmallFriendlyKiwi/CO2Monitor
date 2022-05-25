#include <mqtt.h>
#include <Arduino.h>
#include <config.h>

#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <i2c.h>
#include <configManager.h>
#include <model.h>
#include <wifiManager.h>
#include <ota.h>

#include <ArduinoJson.h>

// Local logging tag
static const char TAG[] = __FILE__;

namespace mqtt {

  struct MqttMessage {
    uint8_t cmd;
    uint16_t mask;
  };

  const uint8_t X_CMD_PUBLISH_SENSORS = bit(0);
  const uint8_t X_CMD_PUBLISH_CONFIGURATION = bit(1);

  TaskHandle_t mqttTask;
  QueueHandle_t mqttQueue;

  WiFiClient wifiClient;
  PubSubClient mqtt_client(wifiClient);
  Model* model;

  calibrateCo2SensorCallback_t calibrateCo2SensorCallback;
  setTemperatureOffsetCallback_t setTemperatureOffsetCallback;
  getTemperatureOffsetCallback_t getTemperatureOffsetCallback;
  getSPS30AutoCleanIntervalCallback_t getSPS30AutoCleanIntervalCallback;
  setSPS30AutoCleanIntervalCallback_t setSPS30AutoCleanIntervalCallback;
  cleanSPS30Callback_t cleanSPS30Callback;
  getSPS30StatusCallback_t getSPS30StatusCallback;

  void publishSensors(uint16_t mask) {
    MqttMessage msg;
    msg.cmd = X_CMD_PUBLISH_SENSORS;
    msg.mask = mask;
    xQueueSendToBack(mqttQueue, (void*)&msg, pdMS_TO_TICKS(100));
  }

  void publishSensorsInternal(uint16_t mask) {
    char topic[256];
    char msg[256];
    sprintf(topic, "%s/%u/up/sensors", config.mqttTopic, config.deviceId);

    char buf[8];
    StaticJsonDocument<512> json;
    if (mask & M_CO2) json["co2"] = model->getCo2();
    if (mask & M_TEMPERATURE) {
      sprintf(buf, "%.1f", model->getTemperature());
      json["temperature"] = buf;
    }
    if (mask & M_HUMIDITY) {
      sprintf(buf, "%.1f", model->getHumidity());
      json["humidity"] = buf;
    }
    if (mask & M_PRESSURE) json["pressure"] = model->getPressure();
    if (mask & M_IAQ) json["iaq"] = model->getIAQ();
    if (mask & M_PM0_5) json["pm0.5"] = model->getPM0_5();
    if (mask & M_PM1_0) json["pm1"] = model->getPM1();
    if (mask & M_PM2_5) json["pm2.5"] = model->getPM2_5();
    if (mask & M_PM4) json["pm4"] = model->getPM4();
    if (mask & M_PM10) json["pm10"] = model->getPM10();

    // Serialize JSON to file
    if (serializeJson(json, msg) == 0) {
      ESP_LOGW(TAG, "Failed to serialise payload");
      return;
    }
    ESP_LOGI(TAG, "Publishing sensor values: %s:%s", topic, msg);
    if (!mqtt_client.publish(topic, msg)) ESP_LOGE(TAG, "publish failed!");
  }

  void publishConfiguration() {
    MqttMessage msg;
    msg.cmd = X_CMD_PUBLISH_CONFIGURATION;
    msg.mask = 0;
    xQueueSendToBack(mqttQueue, (void*)&msg, pdMS_TO_TICKS(100));
  }

  void publishConfigurationInternal() {
    char buf[384];
    char msg[512];
    StaticJsonDocument<512> json;
    json["appVersion"] = APP_VERSION;
    json["altitude"] = config.altitude;
    json["yellowThreshold"] = config.yellowThreshold;
    json["redThreshold"] = config.redThreshold;
    json["darkRedThreshold"] = config.darkRedThreshold;
    json["ledPwm"] = config.ledPwm;
    sprintf(buf, "%x", (uint32_t)ESP.getEfuseMac());
    json["mac"] = buf;
    sprintf(buf, "%s", WiFi.localIP().toString().c_str());
    json["ip"] = buf;
    if (I2C::scd30Present())
      json["scd30"] = true;
    if (I2C::scd40Present())
      json["scd40"] = true;
    if (I2C::bme680Present())
      json["bme680"] = true;
    if (I2C::lcdPresent())
      json["lcd"] = true;
    if (I2C::sps30Present()) {
      json["sps30"] = true;
      json["sps30AutoCleanInt"] = getSPS30AutoCleanIntervalCallback();
      json["sps30Status"] = getSPS30StatusCallback();
    }
#ifdef HAS_NEOPIXEL
    json["neopxl"] = NEOPIXEL_NUM;
#endif
#ifdef HAS_LEDS
    json["leds"] = true;
#endif
    sprintf(buf, "%.1f", getTemperatureOffsetCallback());
    json["tempOffset"] = buf;
    if (serializeJson(json, msg) == 0) {
      ESP_LOGW(TAG, "Failed to serialise payload");
      return;
    }
    sprintf(buf, "%s/%u/up/config", config.mqttTopic, config.deviceId);
    ESP_LOGI(TAG, "Publishing configuration: %s:%s", buf, msg);
    if (!mqtt_client.publish(buf, msg)) ESP_LOGE(TAG, "publish failed!");
  }

  void callback(char* topic, byte* payload, unsigned int length) {
    char buf[256];
    char msg[length + 1];
    strncpy(msg, (char*)payload, length);
    msg[length] = 0x00;
    ESP_LOGI(TAG, "Message arrived [%s] %s", topic, msg);

    sprintf(buf, "%s/%u/down/", config.mqttTopic, config.deviceId);
    int16_t cmdIdx = -1;
    if (strncmp(topic, buf, strlen(buf)) == 0) {
      ESP_LOGI(TAG, "Device specific downlink message arrived [%s]", topic);
      cmdIdx = strlen(buf);
    }
    sprintf(buf, "%s/down/", config.mqttTopic);
    if (strncmp(topic, buf, strlen(buf)) == 0) {
      ESP_LOGI(TAG, "Device agnostic downlink message arrived [%s]", topic);
      cmdIdx = strlen(buf);
    }
    if (cmdIdx < 0) return;
    strncpy(buf, topic + cmdIdx, strlen(topic) - cmdIdx + 1);
    ESP_LOGI(TAG, "Received command [%s]", buf);

    if (strncmp(buf, "calibrate", strlen(buf)) == 0) {
      int reference = atoi(msg);
      if (reference >= 400 && reference <= 2000) {
        calibrateCo2SensorCallback(reference);
      }
    } else if (strncmp(buf, "setTemperatureOffset", strlen(buf)) == 0) {
      float tempOffset = atof(msg);
      if (-10.0 < tempOffset && tempOffset <= 10.0) {
        setTemperatureOffsetCallback(tempOffset);
      }
    } else if (strncmp(buf, "setSPS30AutoCleanInterval", strlen(buf)) == 0) {
      char* eptr;
      long interval = std::strtoul(msg, &eptr, 10);
      setSPS30AutoCleanIntervalCallback(interval);
    } else if (strncmp(buf, "cleanSPS30", strlen(buf)) == 0) {
      cleanSPS30Callback();
    } else if (strncmp(buf, "getConfig", strlen(buf)) == 0) {
      publishConfiguration();
    } else if (strncmp(buf, "setConfig", strlen(buf)) == 0) {
      StaticJsonDocument<256> doc;
      DeserializationError error = deserializeJson(doc, msg);
      if (error) {
        ESP_LOGW(TAG, "Failed to parse message: %s", error.f_str());
        return;
      }
      if (doc["altitude"].as<int>()) config.altitude = doc["altitude"];
      if (doc["yellowThreshold"].as<int>()) config.yellowThreshold = doc["yellowThreshold"];
      if (doc["redThreshold"].as<int>()) config.redThreshold = doc["redThreshold"];
      if (doc["darkRedThreshold"].as<uint16_t>()) config.darkRedThreshold = doc["darkRedThreshold"];
      if (doc["ledPwm"].as<uint8_t>()) config.ledPwm = doc["ledPwm"];
      saveConfiguration(config);
    } else if (strncmp(buf, "resetWifi", strlen(buf)) == 0) {
      WifiManager::resetSettings();
    } else if (strncmp(buf, "ota", strlen(buf)) == 0) {
      OTA::checkForUpdate();
    } else if (strncmp(buf, "reboot", strlen(buf)) == 0) {
      esp_restart();
    }
  }

  void reconnect() {
    char buf[256];
    sprintf(buf, "CO2Monitor-%u-%x", config.deviceId, (uint32_t)ESP.getEfuseMac());
    while (!WiFi.isConnected()) { vTaskDelay(pdMS_TO_TICKS(100)); }
    while (!mqtt_client.connected()) {
      ESP_LOGD(TAG, "Attempting MQTT connection...");
      if (mqtt_client.connect(buf, config.mqttUsername, config.mqttPassword)) {
        ESP_LOGD(TAG, "MQTT connected");
        sprintf(buf, "%s/%u/down/#", config.mqttTopic, config.deviceId);
        mqtt_client.subscribe(buf);
        sprintf(buf, "%s/down/#", config.mqttTopic);
        mqtt_client.subscribe(buf);
        sprintf(buf, "%s/%u/up/status", config.mqttTopic, config.deviceId);
        mqtt_client.publish(buf, "{\"online\":true}");
        vTaskDelay(pdMS_TO_TICKS(1000));
      } else {
        ESP_LOGW(TAG, "MQTT connection failed, rc=%i", mqtt_client.state());
        vTaskDelay(pdMS_TO_TICKS(10000));
      }
    }
  }

  void setupMqtt(
    Model* _model,
    calibrateCo2SensorCallback_t _calibrateCo2SensorCallback,
    setTemperatureOffsetCallback_t _setTemperatureOffsetCallback,
    getTemperatureOffsetCallback_t _getTemperatureOffsetCallback,
    getSPS30AutoCleanIntervalCallback_t _getSPS30AutoCleanIntervalCallback,
    setSPS30AutoCleanIntervalCallback_t _setSPS30AutoCleanIntervalCallback,
    cleanSPS30Callback_t _cleanSPS30Callback,
    getSPS30StatusCallback_t _getSPS30StatusCallback
  ) {
    mqttQueue = xQueueCreate(2, sizeof(struct MqttMessage*));
    if (mqttQueue == NULL) {
      ESP_LOGE(TAG, "Queue creation failed!");
    }

    model = _model;
    calibrateCo2SensorCallback = _calibrateCo2SensorCallback;
    setTemperatureOffsetCallback = _setTemperatureOffsetCallback;
    getTemperatureOffsetCallback = _getTemperatureOffsetCallback;
    getSPS30AutoCleanIntervalCallback = _getSPS30AutoCleanIntervalCallback;
    setSPS30AutoCleanIntervalCallback = _setSPS30AutoCleanIntervalCallback;
    cleanSPS30Callback = _cleanSPS30Callback;
    getSPS30StatusCallback = _getSPS30StatusCallback;

    mqtt_client.setServer(config.mqttHost, config.mqttServerPort);
    mqtt_client.setCallback(callback);
    mqtt_client.setBufferSize(512);
  }

  void mqttLoop(void* pvParameters) {
    _ASSERT((uint32_t)pvParameters == 1);
    BaseType_t notified;
    MqttMessage msg;
    while (1) {
      notified = xQueueReceive(mqttQueue, &msg, pdMS_TO_TICKS(100));
      if (notified == pdPASS) {
        if (msg.cmd == X_CMD_PUBLISH_CONFIGURATION) {
          publishConfigurationInternal();
        } else if (msg.cmd == X_CMD_PUBLISH_SENSORS) {
          publishSensorsInternal(msg.mask);
        }
      }
      if (!mqtt_client.connected()) {
        reconnect();
      }
      mqtt_client.loop();
    }
    vTaskDelete(NULL);
  }

}