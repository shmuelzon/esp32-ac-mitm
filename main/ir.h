#ifndef IR_H
#define IR_H

#include <driver/rmt_types.h>
#include <stddef.h>

/* Event callback types */
typedef void (*ir_on_recv_cb_t)(rmt_symbol_word_t *symbols, size_t len);

/* Event handlers */
void ir_set_on_recv_cb(ir_on_recv_cb_t cb);

int ir_initialize(int rx_gpio, int tx_gpio);
int ir_send(rmt_symbol_word_t *symbols, size_t len);

#endif
