#include "ac.h"
#include "config.h"
#include "eth.h"
#include "hal/rmt_types.h"
#include "httpd.h"
#include "ir.h"
#include "log.h"
#include "mqtt.h"
#include "ota.h"
#include "power_detector.h"
#include "resolve.h"
#include "wifi.h"
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <mdns.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOPIC_LEN 256
static const char *TAG = "AC-MITM";

typedef struct {
    int value;
    char *name;
} value_name_t;

static value_name_t action_to_name[] = {
    { AC_MODE_FAN, "fan" },
    { AC_MODE_COOL, "cooling" },
    { AC_MODE_HEAT, "heating" },
    { AC_MODE_DRY, "drying" },
    { AC_MODE_AUTO, "cooling" },
    { -1, NULL },
};

static value_name_t mode_to_name[] = {
    { AC_MODE_FAN, "fan_only" },
    { AC_MODE_COOL, "cool" },
    { AC_MODE_HEAT, "heat" },
    { AC_MODE_DRY, "dry" },
    { AC_MODE_AUTO, "auto" },
    { -1, NULL },
};

static value_name_t fan_to_name[] = {
    { AC_FAN_LOW, "low" },
    { AC_FAN_MEDIUM, "medium" },
    { AC_FAN_HIGH, "high" },
    { AC_FAN_AUTO, "auto" },
    { -1, NULL },
};

static char *value_to_name(value_name_t *value_names, int value)
{
    for (; value_names->value != -1; value_names++)
    {
        if (value == value_names->value)
            return value_names->name;
    }
    return NULL;
}
static int name_to_value(value_name_t *value_names, const char *name)
{
    for (; value_names->value != -1; value_names++)
    {
        if (!strcmp(name, value_names->name))
            return value_names->value;
    }
    return -1;
}

static const char *device_name_get(void)
{
    static const char *name = NULL;
    uint8_t *mac = NULL;

    if (name)
        return name;

    if ((name = config_network_hostname_get()))
        return name;

    switch (config_network_type_get())
    {
    case NETWORK_TYPE_ETH:
        mac = eth_mac_get();
        break;
    case NETWORK_TYPE_WIFI:
        mac = wifi_mac_get();
        break;
    }
    name = malloc(17);
    sprintf((char *)name, "AC-MITM-%02X%02X", mac[4], mac[5]);

    return name;
}

static void reset(void)
{
    wifi_disconnect();
    abort();
}

/* Bookkeeping functions */
static void heartbeat_publish(void)
{
    char topic[MAX_TOPIC_LEN];
    char buf[16];

    /* Only publish uptime when connected, we don't want it to be queued */
    if (!mqtt_is_connected())
        return;

    /* Uptime (in seconds) */
    sprintf(buf, "%" PRId64, esp_timer_get_time() / 1000 / 1000);
    snprintf(topic, MAX_TOPIC_LEN, "%s/Uptime", device_name_get());
    mqtt_publish(topic, (uint8_t *)buf, strlen(buf), config_mqtt_qos_get(),
        config_mqtt_retained_get());

    /* Free memory (in bytes) */
    sprintf(buf, "%" PRIu32, esp_get_free_heap_size());
    snprintf(topic, MAX_TOPIC_LEN, "%s/FreeMemory", device_name_get());
    mqtt_publish(topic, (uint8_t *)buf, strlen(buf), config_mqtt_qos_get(),
        config_mqtt_retained_get());
}

static void self_publish(void)
{
    char topic[MAX_TOPIC_LEN];
    char *payload;

    /* Current status */
    payload = "online";
    snprintf(topic, MAX_TOPIC_LEN, "%s/Status", device_name_get());
    mqtt_publish(topic, (uint8_t *)payload, strlen(payload),
        config_mqtt_qos_get(), config_mqtt_retained_get());

    /* App version */
    payload = AC_MITM_VER;
    snprintf(topic, MAX_TOPIC_LEN, "%s/Version", device_name_get());
    mqtt_publish(topic, (uint8_t *)payload, strlen(payload),
        config_mqtt_qos_get(), config_mqtt_retained_get());

    /* Config version */
    payload = config_version_get();
    snprintf(topic, MAX_TOPIC_LEN, "%s/ConfigVersion", device_name_get());
    mqtt_publish(topic, (uint8_t *)payload, strlen(payload),
        config_mqtt_qos_get(), config_mqtt_retained_get());

    heartbeat_publish();
}

static void publish_action()
{
    char topic[MAX_TOPIC_LEN];
    char *payload;

    if (!ac_get_power())
        payload = "off";
    else
        payload = value_to_name(action_to_name, ac_get_mode());

    snprintf(topic, MAX_TOPIC_LEN, "%s/Action", device_name_get());
    mqtt_publish(topic, (uint8_t *)payload, strlen(payload),
        config_mqtt_qos_get(), config_mqtt_retained_get());
}

static void publish_mode()
{
    char topic[MAX_TOPIC_LEN];
    char *payload;

    if (!ac_get_power())
        payload = "off";
    else
        payload = value_to_name(mode_to_name, ac_get_mode());

    snprintf(topic, MAX_TOPIC_LEN, "%s/Mode", device_name_get());
    mqtt_publish(topic, (uint8_t *)payload, strlen(payload),
        config_mqtt_qos_get(), config_mqtt_retained_get());
}


static void publish_ac()
{
    publish_action();
    publish_mode();
}

/* OTA functions */
static void ota_on_completed(ota_type_t type, ota_err_t err)
{
    ESP_LOGI(TAG, "Update completed: %s", ota_err_to_str(err));

    /* All done, restart */
    if (err == OTA_ERR_SUCCESS)
        reset();

    /* All done, restart */
    vTaskSuspendAll();
    esp_restart();
    xTaskResumeAll();
}

static void _ota_on_completed(ota_type_t type, ota_err_t err);

static void ota_on_mqtt(const char *topic, const uint8_t *payload, size_t len,
    void *ctx)
{
    char *url = malloc(len + 1);
    ota_type_t type = (ota_type_t)ctx;
    ota_err_t err;

    memcpy(url, payload, len);
    url[len] = '\0';
    ESP_LOGI(TAG, "Starting %s update from %s",
        type == OTA_TYPE_FIRMWARE ? "firmware" : "configuration", url);

    if ((err = ota_download(type, url, _ota_on_completed)) != OTA_ERR_SUCCESS)
        ESP_LOGE(TAG, "Failed updating: %s", ota_err_to_str(err));

    free(url);
}

static void _ota_on_mqtt(const char *topic, const uint8_t *payload, size_t len,
    void *ctx);

static void ota_subscribe(void)
{
    char topic[MAX_TOPIC_LEN];

    /* Register for both a specific topic for this device and a general one */
    snprintf(topic, MAX_TOPIC_LEN, "%s/OTA/Firmware", device_name_get());
    mqtt_subscribe(topic, 0, _ota_on_mqtt, (void *)OTA_TYPE_FIRMWARE, NULL);
    mqtt_subscribe("AC-MITM/OTA/Firmware", 0, _ota_on_mqtt,
        (void *)OTA_TYPE_FIRMWARE, NULL);

    snprintf(topic, MAX_TOPIC_LEN, "%s/OTA/Config", device_name_get());
    mqtt_subscribe(topic, 0, _ota_on_mqtt, (void *)OTA_TYPE_CONFIG, NULL);
    mqtt_subscribe("AC-MITM/OTA/Config", 0, _ota_on_mqtt,
        (void *)OTA_TYPE_CONFIG, NULL);
}

static void ota_unsubscribe(void)
{
    char topic[27];

    sprintf(topic, "%s/OTA/Firmware", device_name_get());
    mqtt_unsubscribe(topic);
    mqtt_unsubscribe("AC-MITM/OTA/Firmware");

    sprintf(topic, "%s/OTA/Config", device_name_get());
    mqtt_unsubscribe(topic);
    mqtt_unsubscribe("AC-MITM/OTA/Config");
}

static void _ac_on_mqtt_power(const char *topic, const uint8_t *payload,
    size_t len, void *ctx);
static void _ac_on_mqtt_temperature(const char *topic, const uint8_t *payload,
    size_t len, void *ctx);
static void _ac_on_mqtt_mode(const char *topic, const uint8_t *payload,
    size_t len, void *ctx);
static void _ac_on_mqtt_fan(const char *topic, const uint8_t *payload,
    size_t len, void *ctx);

static void ac_subscribe(void)
{
    char topic[MAX_TOPIC_LEN];

    snprintf(topic, MAX_TOPIC_LEN, "%s/Power/Set", device_name_get());
    mqtt_subscribe(topic, 0, _ac_on_mqtt_power, NULL, NULL);
    snprintf(topic, MAX_TOPIC_LEN, "%s/Temperature/Set", device_name_get());
    mqtt_subscribe(topic, 0, _ac_on_mqtt_temperature, NULL, NULL);
    snprintf(topic, MAX_TOPIC_LEN, "%s/Mode/Set", device_name_get());
    mqtt_subscribe(topic, 0, _ac_on_mqtt_mode, NULL, NULL);
    snprintf(topic, MAX_TOPIC_LEN, "%s/Fan/Set", device_name_get());
    mqtt_subscribe(topic, 0, _ac_on_mqtt_fan, NULL, NULL);
}

static void ac_unsubscribe(void)
{
    char topic[MAX_TOPIC_LEN];

    snprintf(topic, MAX_TOPIC_LEN, "%s/Power/Set", device_name_get());
    mqtt_unsubscribe(topic);
    snprintf(topic, MAX_TOPIC_LEN, "%s/Temperature/Set", device_name_get());
    mqtt_unsubscribe(topic);
    snprintf(topic, MAX_TOPIC_LEN, "%s/Mode/Set", device_name_get());
    mqtt_unsubscribe(topic);
    snprintf(topic, MAX_TOPIC_LEN, "%s/Fan/Set", device_name_get());
    mqtt_unsubscribe(topic);
}

static void cleanup(void)
{
    ota_unsubscribe();
    ac_unsubscribe();
}

/* Network callback functions */
static void network_on_connected(void)
{
    char status_topic[MAX_TOPIC_LEN];

    log_start(config_log_host_get(), config_log_port_get());
    ESP_LOGI(TAG, "Connected to the network, connecting to MQTT");
    snprintf(status_topic, MAX_TOPIC_LEN, "%s/Status", device_name_get());

    mqtt_connect(config_mqtt_host_get(), config_mqtt_port_get(),
        config_mqtt_client_id_get(), config_mqtt_username_get(),
        config_mqtt_password_get(), config_mqtt_ssl_get(),
        config_mqtt_server_cert_get(), config_mqtt_client_cert_get(),
        config_mqtt_client_key_get(), status_topic, "offline",
        config_mqtt_qos_get(), config_mqtt_retained_get());
}

static void network_on_disconnected(void)
{
    log_stop();
    ESP_LOGI(TAG, "Disconnected from the network, stopping MQTT");
    mqtt_disconnect();
    /* We don't get notified when manually stopping MQTT */
    cleanup();
}

/* MQTT callback functions */
static void mqtt_on_connected(void)
{
    ESP_LOGI(TAG, "Connected to MQTT");
    self_publish();
    ota_subscribe();
    ac_subscribe();
}

static void mqtt_on_disconnected(void)
{
    static uint8_t num_disconnections = 0;

    ESP_LOGI(TAG, "Disconnected from MQTT");
    cleanup();

    if (++num_disconnections % 3 == 0)
    {
        ESP_LOGI(TAG,
            "Failed connecting to MQTT 3 times, reconnecting to the network");
        wifi_reconnect();
    }
}

/* Power detector callback functions */
static void power_detector_changed(int pin, int level)
{
    ESP_LOGI(TAG, "Power changed: %d", level);
    ac_set_detected_power(level);
}

/* IR callback functions */
static void ir_on_recv(rmt_symbol_word_t *symbols, size_t len)
{
    if (ac_ir_recv(symbols, len))
        return;

    ac_ir_send();
}

/* AC callback functions */
static void ac_on_power_changed(bool on)
{
    char topic[MAX_TOPIC_LEN];
    char *payload = on ? "on" : "off";

    ESP_LOGI(TAG, "AC power changed: %d", on);
    snprintf(topic, MAX_TOPIC_LEN, "%s/Power", device_name_get());
    mqtt_publish(topic, (uint8_t *)payload, strlen(payload),
        config_mqtt_qos_get(), config_mqtt_retained_get());
    publish_ac();
}

static void ac_on_temperature_changed(int temperature)
{
    char topic[MAX_TOPIC_LEN];
    char payload[16];

    ESP_LOGI(TAG, "AC temperature changed: %dC", temperature);
    snprintf(topic, MAX_TOPIC_LEN, "%s/Temperature", device_name_get());
    snprintf(payload, sizeof(payload), "%d", temperature);
    mqtt_publish(topic, (uint8_t *)payload, strlen(payload),
        config_mqtt_qos_get(), config_mqtt_retained_get());
}

static void ac_on_mode_changed(ac_mode_t mode)
{
#if 0
    char topic[MAX_TOPIC_LEN];
    char *payload = value_to_name(mode_to_name, mode);

    ESP_LOGI(TAG, "AC mode changed: %s", payload);
    snprintf(topic, MAX_TOPIC_LEN, "%s/Mode", device_name_get());
    mqtt_publish(topic, (uint8_t *)payload, strlen(payload),
        config_mqtt_qos_get(), config_mqtt_retained_get());
#endif
    publish_ac();
}

static void ac_on_fan_changed(ac_fan_t fan)
{
    char topic[MAX_TOPIC_LEN];
    char *payload = value_to_name(fan_to_name, fan);

    ESP_LOGI(TAG, "AC fan changed: %s", payload);
    snprintf(topic, MAX_TOPIC_LEN, "%s/Fan", device_name_get());
    mqtt_publish(topic, (uint8_t *)payload, strlen(payload),
        config_mqtt_qos_get(), config_mqtt_retained_get());
}

static void ac_on_mqtt_power(bool on)
{
    if (ac_set_power(on))
    {
        ESP_LOGE(TAG, "Failed setting AC power");
        return;
    }

    ac_ir_send();
}

static void ac_on_mqtt_temperature(int temperature)
{
    if (ac_set_temperature(temperature))
    {
        ESP_LOGE(TAG, "Failed setting AC temperature %d", temperature);
        return;
    }

    ac_ir_send();
}

static void ac_on_mqtt_mode(ac_mode_t mode)
{
    /* If the mode was changed, we also need to power on */
    ac_set_power(1);

    if (ac_set_mode(mode))
    {
        ESP_LOGE(TAG, "Failed setting AC mode %d", mode);
        return;
    }

    ac_ir_send();
}

static void ac_on_mqtt_fan(ac_fan_t fan)
{
    if (ac_set_fan(fan))
    {
        ESP_LOGE(TAG, "Failed setting AC fan %d", fan);
        return;
    }

    ac_ir_send();
}

/* AC MITM task and event callbacks */
typedef enum {
    EVENT_TYPE_HEARTBEAT_TIMER = 0,
    EVENT_TYPE_NETWORK_CONNECTED = 1,
    EVENT_TYPE_NETWORK_DISCONNECTED = 2,
    EVENT_TYPE_OTA_MQTT = 3,
    EVENT_TYPE_OTA_COMPLETED = 4,
    EVENT_TYPE_MQTT_CONNECTED = 5,
    EVENT_TYPE_MQTT_DISCONNECTED = 6,
    EVENT_TYPE_POWER_DETECTOR_CHANGED = 7,
    EVENT_TYPE_IR_RECV = 8,
    EVENT_TYPE_AC_POWER_CHANGED = 9,
    EVENT_TYPE_AC_TEMPERATURE_CHANGED = 10,
    EVENT_TYPE_AC_MODE_CHANGED = 11,
    EVENT_TYPE_AC_FAN_CHANGED = 12,
    EVENT_TYPE_AC_MQTT_POWER = 13,
    EVENT_TYPE_AC_MQTT_TEMPERATURE = 14,
    EVENT_TYPE_AC_MQTT_MODE = 15,
    EVENT_TYPE_AC_MQTT_FAN = 16,
} event_type_t;

typedef struct {
    event_type_t type;
    union {
        struct {
            ota_type_t type;
            ota_err_t err;
        } ota_completed;
        struct {
            char *topic;
            uint8_t *payload;
            size_t len;
            void *ctx;
        } mqtt_message;
        struct {
            int pin;
            int level;
        } power_detector_changed;
        struct {
            rmt_symbol_word_t *symbols;
            size_t len;
        } ir_recv;
        struct {
            bool on;
        } ac_power;
        struct {
            int temperature;
        } ac_temperature;
        struct {
            ac_mode_t mode;
        } ac_mode;
        struct {
            ac_fan_t fan;
        } ac_fan;
    };
} event_t;

static QueueHandle_t event_queue;

static void ac_mitm_handle_event(event_t *event)
{
    switch (event->type)
    {
    case EVENT_TYPE_HEARTBEAT_TIMER:
        heartbeat_publish();
        break;
    case EVENT_TYPE_NETWORK_CONNECTED:
        network_on_connected();
        break;
    case EVENT_TYPE_NETWORK_DISCONNECTED:
        network_on_disconnected();
        break;
    case EVENT_TYPE_OTA_MQTT:
        ota_on_mqtt(event->mqtt_message.topic, event->mqtt_message.payload,
            event->mqtt_message.len, event->mqtt_message.ctx);
        free(event->mqtt_message.topic);
        free(event->mqtt_message.payload);
        break;
    case EVENT_TYPE_OTA_COMPLETED:
        ota_on_completed(event->ota_completed.type, event->ota_completed.err);
        break;
    case EVENT_TYPE_MQTT_CONNECTED:
        mqtt_on_connected();
        break;
    case EVENT_TYPE_MQTT_DISCONNECTED:
        mqtt_on_disconnected();
        break;
    case EVENT_TYPE_POWER_DETECTOR_CHANGED:
        power_detector_changed(event->power_detector_changed.pin,
            event->power_detector_changed.level);
        break;
    case EVENT_TYPE_IR_RECV:
        ir_on_recv(event->ir_recv.symbols, event->ir_recv.len);
        free(event->ir_recv.symbols);
        break;
    case EVENT_TYPE_AC_POWER_CHANGED:
        ac_on_power_changed(event->ac_power.on);
        break;
    case EVENT_TYPE_AC_TEMPERATURE_CHANGED:
        ac_on_temperature_changed(event->ac_temperature.temperature);
        break;
    case EVENT_TYPE_AC_MODE_CHANGED:
        ac_on_mode_changed(event->ac_mode.mode);
        break;
    case EVENT_TYPE_AC_FAN_CHANGED:
        ac_on_fan_changed(event->ac_fan.fan);
        break;
    case EVENT_TYPE_AC_MQTT_POWER:
        ac_on_mqtt_power(event->ac_power.on);
        break;
    case EVENT_TYPE_AC_MQTT_TEMPERATURE:
        ac_on_mqtt_temperature(event->ac_temperature.temperature);
        break;
    case EVENT_TYPE_AC_MQTT_MODE:
        ac_on_mqtt_mode(event->ac_mode.mode);
        break;
    case EVENT_TYPE_AC_MQTT_FAN:
        ac_on_mqtt_fan(event->ac_power.on);
        break;
    }

    free(event);
}

static void ac_mitm_task(void *pvParameter)
{
    event_t *event;

    while (1)
    {
        if (xQueueReceive(event_queue, &event, portMAX_DELAY) != pdTRUE)
            continue;

        ac_mitm_handle_event(event);
    }

    vTaskDelete(NULL);
}

static void heartbeat_timer_cb(TimerHandle_t xTimer)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_HEARTBEAT_TIMER;

    ESP_LOGD(TAG, "Queuing event HEARTBEAT_TIMER");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static int start_ac_mitm_task(void)
{
    TimerHandle_t hb_timer;

    if (!(event_queue = xQueueCreate(10, sizeof(event_t *))))
        return -1;

    if (xTaskCreatePinnedToCore(ac_mitm_task, "ac_mitm_task", 4096,
        NULL, 5, NULL, 1) != pdPASS)
    {
        return -1;
    }

    hb_timer = xTimerCreate("heartbeat", pdMS_TO_TICKS(60 * 1000), pdTRUE,
        NULL, heartbeat_timer_cb);
    xTimerStart(hb_timer, 0);

    return 0;
}

static void _mqtt_on_message(event_type_t type, const char *topic,
    const uint8_t *payload, size_t len, void *ctx)
{
    event_t *event = malloc(sizeof(*event));

    event->type = type;
    event->mqtt_message.topic = strdup(topic);
    event->mqtt_message.payload = malloc(len);
    memcpy(event->mqtt_message.payload, payload, len);
    event->mqtt_message.len = len;
    event->mqtt_message.ctx = ctx;

    ESP_LOGD(TAG, "Queuing event MQTT message %d (%s, %p, %zd, %p)", type,
        topic, payload, len, ctx);
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _network_on_connected(void)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_NETWORK_CONNECTED;

    ESP_LOGD(TAG, "Queuing event NETWORK_CONNECTED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _network_on_disconnected(void)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_NETWORK_DISCONNECTED;

    ESP_LOGD(TAG, "Queuing event NETWORK_DISCONNECTED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ota_on_mqtt(const char *topic, const uint8_t *payload, size_t len,
    void *ctx)
{
    _mqtt_on_message(EVENT_TYPE_OTA_MQTT, topic, payload, len, ctx);
}

static void _ota_on_completed(ota_type_t type, ota_err_t err)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_OTA_COMPLETED;
    event->ota_completed.type = type;
    event->ota_completed.err = err;

    ESP_LOGD(TAG, "Queuing event HEARTBEAT_TIMER (%d, %d)", type, err);
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _mqtt_on_connected(void)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_MQTT_CONNECTED;

    ESP_LOGD(TAG, "Queuing event MQTT_CONNECTED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _mqtt_on_disconnected(void)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_MQTT_DISCONNECTED;

    ESP_LOGD(TAG, "Queuing event MQTT_DISCONNECTED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _power_detector_changed(int pin, int level)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_POWER_DETECTOR_CHANGED;
    event->power_detector_changed.pin = pin;
    event->power_detector_changed.level = level;

    ESP_LOGD(TAG, "Queuing event POWER_DETECTOR_CHANGED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ir_on_recv(rmt_symbol_word_t *symbols, size_t len)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_IR_RECV;
    event->ir_recv.symbols = malloc(sizeof(*symbols) * len);
    memcpy(event->ir_recv.symbols, symbols, sizeof(*symbols) * len); 
    event->ir_recv.len = len;

    ESP_LOGD(TAG, "Queuing event IR_RECV");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ac_on_power_changed(bool on)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_AC_POWER_CHANGED;
    event->ac_power.on = on;

    ESP_LOGD(TAG, "Queuing event AC_POWER_CHANGED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ac_on_temperature_changed(int temperature)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_AC_TEMPERATURE_CHANGED;
    event->ac_temperature.temperature = temperature;

    ESP_LOGD(TAG, "Queuing event AC_TEMPERATURE_CHANGED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ac_on_mode_changed(ac_mode_t mode)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_AC_MODE_CHANGED;
    event->ac_mode.mode = mode;

    ESP_LOGD(TAG, "Queuing event AC_MODE_CHANGED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ac_on_fan_changed(ac_fan_t fan)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_AC_FAN_CHANGED;
    event->ac_fan.fan = fan;

    ESP_LOGD(TAG, "Queuing event AC_FAN_CHANGED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ac_on_mqtt_power(const char *topic, const uint8_t *payload,
    size_t len, void *ctx)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_AC_MQTT_POWER;
    event->ac_power.on = len == 2 && !strncmp((char *)payload, "on", len);

    ESP_LOGD(TAG, "Queuing event AC_MQTT_POWER");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ac_on_mqtt_temperature(const char *topic, const uint8_t *payload,
    size_t len, void *ctx)
{
    event_t *event = malloc(sizeof(*event));
    char *temperature = strndup((char *)payload, len);

    event->type = EVENT_TYPE_AC_MQTT_TEMPERATURE;
    event->ac_temperature.temperature = atoi(temperature);

    ESP_LOGD(TAG, "Queuing event AC_MQTT_TEMPERATURE");
    xQueueSend(event_queue, &event, portMAX_DELAY);
    free(temperature);
}

static void _ac_on_mqtt_mode(const char *topic, const uint8_t *payload,
    size_t len, void *ctx)
{
    event_t *event = malloc(sizeof(*event));
    char *mode = strndup((char *)payload, len);

    if (!strcmp(mode, "off"))
    {
        event->type = EVENT_TYPE_AC_MQTT_POWER;
        event->ac_power.on = 0;
        ESP_LOGD(TAG, "Queuing event AC_MQTT_POWER");
    }
    else
    {
        event->type = EVENT_TYPE_AC_MQTT_MODE;
        event->ac_mode.mode = name_to_value(mode_to_name, mode);
        ESP_LOGD(TAG, "Queuing event AC_MQTT_MODE");
    }

    xQueueSend(event_queue, &event, portMAX_DELAY);
    free(mode);
}

static void _ac_on_mqtt_fan(const char *topic, const uint8_t *payload,
    size_t len, void *ctx)
{
    event_t *event = malloc(sizeof(*event));
    char *fan = strndup((char *)payload, len);

    event->type = EVENT_TYPE_AC_MQTT_FAN;
    event->ac_fan.fan = name_to_value(fan_to_name, fan);

    ESP_LOGD(TAG, "Queuing event AC_MQTT_FAN");
    xQueueSend(event_queue, &event, portMAX_DELAY);
    free(fan);
}

void app_main()
{
    int config_failed;

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Version: %s", AC_MITM_VER);

    /* Init configuration */
    config_failed = config_initialize();

    /* Init remote logging */
    ESP_ERROR_CHECK(log_initialize());

    /* Init OTA */
    ESP_ERROR_CHECK(ota_initialize());

    /* Init Network */
    switch (config_network_type_get())
    {
    case NETWORK_TYPE_ETH:
        /* Init Ethernet */
        ESP_ERROR_CHECK(eth_initialize());
        eth_hostname_set(device_name_get());
        eth_set_on_connected_cb(_network_on_connected);
        eth_set_on_disconnected_cb(_network_on_disconnected);
        break;
    case NETWORK_TYPE_WIFI:
        /* Init Wi-Fi */
        ESP_ERROR_CHECK(wifi_initialize());
        wifi_hostname_set(device_name_get());
        wifi_set_on_connected_cb(_network_on_connected);
        wifi_set_on_disconnected_cb(_network_on_disconnected);
        break;
    }

    /* Init mDNS */
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(device_name_get());

    /* Init name resolver */
    ESP_ERROR_CHECK(resolve_initialize());

    /* Init MQTT */
    ESP_ERROR_CHECK(mqtt_initialize());
    mqtt_set_on_connected_cb(_mqtt_on_connected);
    mqtt_set_on_disconnected_cb(_mqtt_on_disconnected);

    /* Init web server */
    ESP_ERROR_CHECK(httpd_initialize());
    httpd_set_on_ota_completed_cb(_ota_on_completed);

    /* Init AC */
    ac_initialize("airwell");
    ac_set_on_power_changed_cb(_ac_on_power_changed);
    ac_set_on_temperature_changed_cb(_ac_on_temperature_changed);
    ac_set_on_mode_changed_cb(_ac_on_mode_changed);
    ac_set_on_fan_changed_cb(_ac_on_fan_changed);

    /* Init power detector */
    power_detector_initialize(7);
    power_detector_set_on_change(_power_detector_changed);

    /* Init IR */
    ir_initialize(9, 8);
    ir_set_on_recv_cb(_ir_on_recv);

    /* Start AC MITM task */
    ESP_ERROR_CHECK(start_ac_mitm_task());

    /* Failed to load configuration or it wasn't set, create access point */
    if (config_failed || !strcmp(config_network_wifi_ssid_get() ? : "", "MY_SSID"))
    {
        wifi_start_ap(device_name_get(), NULL);
        return;
    }

    switch (config_network_type_get())
    {
    case NETWORK_TYPE_ETH:
        eth_connect(eth_phy_atophy(config_network_eth_phy_get()),
            config_network_eth_phy_power_pin_get());
        break;
    case NETWORK_TYPE_WIFI:
        /* Start by connecting to network */
        wifi_connect(config_network_wifi_ssid_get(), config_network_wifi_password_get(),
            wifi_eap_atomethod(config_eap_method_get()),
            config_eap_identity_get(),
            config_eap_username_get(), config_eap_password_get(),
            config_eap_ca_cert_get(), config_eap_client_cert_get(),
            config_eap_client_key_get());
        break;
    }
}
