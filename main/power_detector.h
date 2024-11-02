#ifndef POWER_DETECTOR_H
#define POWER_DETECTOR_H

/* Event callback types */
typedef void (*power_detector_on_change_cb_t)(int pin, int on);

/* Event handlers */
void power_detector_set_on_change(power_detector_on_change_cb_t cb);

int power_detector_initialize(int pin);

#endif
