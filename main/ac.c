#include "ac.h"
#include "config.h"
#include "ir.h"
#include "protocol_parsers.h"
#include <esp_log.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

static const char *TAG = "AC";

typedef struct {
    const char *name;
    int (*ir_recv)(rmt_symbol_word_t *symbols, size_t len);
    int (*ir_send)(void);
    int min_temperature;
    int max_temperature;
    ac_fan_t *supported_fans;
    ac_mode_t *supported_modes;
} ac_ops_t;

static ac_ops_t *ac_ops = NULL;
static ac_on_power_changed_t on_power_changed_cb = NULL;
static ac_on_temperature_changed_t on_temperature_changed_cb = NULL;
static ac_on_mode_changed_t on_mode_changed_cb = NULL;
static ac_on_fan_changed_t on_fan_changed_cb = NULL;

static union {
    uint64_t data;
    struct {
        uint64_t detected_power:1;
        uint64_t power:1;
        uint64_t temperature:7;
        uint64_t fan:4;
        uint64_t mode:4;
        uint64_t :47;
    };
} current_state = {};

typedef struct {
    int value;
    int mode;
} value_mode_t;

static int value_to_mode(value_mode_t *value_modes, int value)
{
    for (; value_modes->value != -1; value_modes++)
    {
        if (value == value_modes->value)
            return value_modes->mode;
    }
    return -1;
}

static int mode_to_value(value_mode_t *value_modes, int mode)
{
    for (; value_modes->value != -1; value_modes++)
    {
        if (mode == value_modes->mode)
            return value_modes->value;
    }
    return -1;
}

static void update_state(int power, int temperature, ac_mode_t mode,
    ac_fan_t fan)
{
    if (power != -1 && current_state.power != power)
    {
        current_state.power = power;
        if (on_power_changed_cb)
            on_power_changed_cb(power);
    }
    if (temperature != -1 && current_state.temperature != temperature)
    {
        current_state.temperature = temperature;
        if (on_temperature_changed_cb)
            on_temperature_changed_cb(temperature);
    }
    if (mode != -1 && current_state.mode != mode)
    {
        current_state.mode = mode;
        if (on_mode_changed_cb)
            on_mode_changed_cb(mode);
    }
    if (fan != -1 && current_state.fan != fan)
    {
        current_state.fan = fan;
        if (on_fan_changed_cb)
            on_fan_changed_cb(fan);
    }

    config_ac_persistent_save(current_state.data);
    ESP_LOGI(TAG, "Power: %d, temp: %dC, mode: %d, fan: %d",
        current_state.power, current_state.temperature, current_state.mode,
        current_state.fan);
}

/* Airwell */
typedef union {
  uint64_t raw;
  struct {
    uint64_t :1;
    uint64_t one:1;
    uint64_t :16;
    uint64_t sleep:1;
    uint64_t temp:4;
    uint64_t :2;
    uint64_t swing:1;
    uint64_t :2;
    uint64_t fan:2;
    uint64_t mode:3;
    uint64_t power_toggle:1;
    uint64_t :0;
  };
} airwell_t;

static value_mode_t airwell_modes[] = {
    { 1, AC_MODE_COOL },
    { 2, AC_MODE_HEAT },
    { 3, AC_MODE_AUTO },
    { 4, AC_MODE_DRY },
    { 5, AC_MODE_FAN },
    { -1, AC_MODE_AUTO },
};

static value_mode_t airwell_fans[] = {
    { 0, AC_FAN_LOW },
    { 1, AC_FAN_MEDIUM },
    { 2, AC_FAN_HIGH },
    { 3, AC_FAN_AUTO },
    { -1, AC_MODE_AUTO },
};

static int airwell_ir_recv(rmt_symbol_word_t *symbols, size_t len)
{
    airwell_t airwell =
        { .raw = parse_manchester(symbols, len, 3 * 950, 3 * 950, 950) };

    ESP_LOGD(TAG, "Parsed value: 0x%" PRIx64, airwell.raw);
    if (!airwell.raw)
        return -1;

    update_state(airwell.power_toggle ? !current_state.power : -1,
        airwell.temp + 15, value_to_mode(airwell_modes, airwell.mode),
        value_to_mode(airwell_fans, airwell.fan));

    return 0;
}

static int airwell_ir_send()
{
    int ret;
    size_t len;
    rmt_symbol_word_t *symbols = NULL;
    airwell_t airwell = {
        .one = 1,
        .temp = current_state.temperature - 15,
        .fan = mode_to_value(airwell_fans, current_state.fan),
        .mode = mode_to_value(airwell_modes, current_state.mode),
        .power_toggle = current_state.detected_power != current_state.power,
    };

    if (current_state.power == 0 && current_state.detected_power == 0)
        return 0;

    ESP_LOGI(TAG, "Transmitting value: 0x%" PRIx64, airwell.raw);
    if (generate_manchester(3 * 950, 3 * 950, 3, 4 * 950, 950, airwell.raw,
        &symbols, &len))
    {
        ESP_LOGE(TAG, "Failed generating IR symbols");
        return -1;
    }
    ret = ir_send(symbols, len);

    free(symbols);
    return ret;
}

static ac_ops_t airwell = {
    .name = "Airwell",
    .ir_recv = airwell_ir_recv,
    .ir_send = airwell_ir_send,
    .min_temperature = 16,
    .max_temperature = 30,
    .supported_fans = (ac_fan_t[]){ AC_FAN_LOW, AC_FAN_MEDIUM, AC_FAN_HIGH, AC_FAN_AUTO, -1 },
    .supported_modes = (ac_mode_t[]){ AC_MODE_COOL, AC_MODE_HEAT, AC_MODE_AUTO, AC_MODE_DRY, AC_MODE_FAN, -1 },
};

/* General */
static ac_ops_t *acs[] = {
    &airwell,
    NULL
};

void ac_set_on_power_changed_cb(ac_on_power_changed_t cb)
{
    on_power_changed_cb = cb;
}

void ac_set_on_temperature_changed_cb(ac_on_temperature_changed_t cb)
{
    on_temperature_changed_cb = cb;
}

void ac_set_on_mode_changed_cb(ac_on_mode_changed_t cb)
{
    on_mode_changed_cb = cb;
}

void ac_set_on_fan_changed_cb(ac_on_fan_changed_t cb)
{
    on_fan_changed_cb = cb;
}

bool ac_get_power(void)
{
    return current_state.power;
}

int ac_get_temperature(void)
{
    return current_state.temperature;
}

ac_mode_t ac_get_mode(void)
{
    return current_state.mode;
}

ac_fan_t ac_get_fan(void)
{
    return current_state.fan;
}

void ac_set_detected_power(bool on)
{
    current_state.detected_power = on;
    if (current_state.detected_power != current_state.power)
        ac_ir_send();
}

int ac_set_power(bool on)
{
    update_state(on, -1, -1, -1);
    return 0;
}

int ac_set_temperature(int temperature)
{
    if (temperature < ac_ops->min_temperature ||
        temperature > ac_ops->max_temperature)
    {
        return -1;
    }

    update_state(-1, temperature, -1, -1);
    return 0;
}

int ac_set_mode(ac_mode_t mode)
{
    for (ac_mode_t *iter = ac_ops->supported_modes; *iter != -1; iter++)
    {
        if (*iter == mode)
        {
            update_state(-1, -1, mode, -1);
            return 0;
        }
    }
    return -1;
}

int ac_set_fan(ac_fan_t fan)
{
    for (ac_fan_t *iter = ac_ops->supported_fans; *iter != -1; iter++)
    {
        if (*iter == fan)
        {
            update_state(-1, -1, -1, fan);
            return 0;
        }
    }
    return -1;
}

int ac_ir_recv(rmt_symbol_word_t *symbols, size_t len)
{
    return ac_ops->ir_recv(symbols, len);
}

int ac_ir_send(void)
{
    return ac_ops->ir_send();
}

int ac_initialize(const char *model)
{
    ac_ops_t **iter;

    for (iter = acs; iter && *iter; iter++)
    {
        if (strcasecmp(model, (*iter)->name))
            continue;
        ac_ops = *iter;
        break;
    }

    if (!ac_ops)
    {
        ESP_LOGE(TAG, "Unsupported AC: %s", model);
        return -1;
    }

    current_state.data = config_ac_persistent_load();
    return 0;
}
