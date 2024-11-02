#ifndef PROTOCOL_PARSERS_H
#define PROTOCOL_PARSERS_H

#include <driver/rmt_types.h>
#include <stdint.h>

uint64_t parse_manchester(rmt_symbol_word_t *symbols, size_t len,
    uint16_t header_mark, uint16_t header_space, uint16_t half_period);

int generate_manchester(uint16_t header_mark, uint16_t header_space,
    uint16_t repeat, uint16_t tail_mark, uint16_t half_period, uint64_t value,
    rmt_symbol_word_t **symbols, size_t *len);

#endif
