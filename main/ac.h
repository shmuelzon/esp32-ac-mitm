#ifndef AC_H
#define AC_H

#include <driver/rmt_types.h>
#include <stddef.h>

typedef enum {
    AC_MODE_FAN,
    AC_MODE_COOL,
    AC_MODE_HEAT,
    AC_MODE_DRY,
    AC_MODE_AUTO,
} ac_mode_t;

typedef enum {
    AC_FAN_LOW,
    AC_FAN_MEDIUM,
    AC_FAN_HIGH,
    AC_FAN_AUTO,
} ac_fan_t;

/* Event callback types */
typedef void (*ac_on_power_changed_t)(bool on);
typedef void (*ac_on_temperature_changed_t)(int temperature);
typedef void (*ac_on_mode_changed_t)(ac_mode_t mode);
typedef void (*ac_on_fan_changed_t)(ac_fan_t fan);

/* Event handlers */
void ac_set_on_power_changed_cb(ac_on_power_changed_t cb);
void ac_set_on_temperature_changed_cb(ac_on_temperature_changed_t cb);
void ac_set_on_mode_changed_cb(ac_on_mode_changed_t cb);
void ac_set_on_fan_changed_cb(ac_on_fan_changed_t cb);

bool ac_get_power(void);
int ac_get_temperature(void);
ac_mode_t ac_get_mode(void);
ac_fan_t ac_get_fan(void);

void ac_set_detected_power(bool on);
int ac_set_power(bool on);
int ac_set_temperature(int temperature);
int ac_set_mode(ac_mode_t mode);
int ac_set_fan(ac_fan_t mode);

int ac_ir_recv(rmt_symbol_word_t *symbols, size_t len);
int ac_ir_send(void);
int ac_initialize(const char *model);

#endif
