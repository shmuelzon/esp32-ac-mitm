#include "protocol_parsers.h"
#include "hal/rmt_types.h"
#include <driver/rmt_types.h>
#include <esp_log.h>
#include <stdint.h>
#include <stdlib.h>

static const char *TAG = "PROTOCOL_PARSER";
static const uint16_t TIMING_TOLERANCE_PERCENT = 25;

static bool is_within_tolerance(uint16_t value, uint16_t expected)
{
    uint16_t lowerLimit = expected * (1 - (TIMING_TOLERANCE_PERCENT / 100.0));
    uint16_t upperLimit = expected * (1 + (TIMING_TOLERANCE_PERCENT / 100.0));
    bool match = lowerLimit <= value && value <= upperLimit;

    ESP_LOGD(TAG, "Checking %" PRId16 " <= %" PRId16 " <= % " PRId16 " -> %d",
        lowerLimit, value, upperLimit, match);

    return match;
}

static int parse_manchester_data(rmt_symbol_word_t *symbols, size_t len,
    uint16_t half_period, uint16_t *backlog_length, uint16_t *backlog_level,
        uint64_t *parsed_value)
{
    int i;

    for (i = 0; i < len; i++)
    {
        ESP_LOGD(TAG, "Handling {%d: %d},{%d: %d}, backlog {%d: %d}",
            symbols[i].level0, symbols[i].duration0, symbols[i].level1,
            symbols[i].duration1, *backlog_level, *backlog_length);

        bool is_low_with_backlog = false;
        bool is_high_with_backlog = false;

        /* Low value */
        if (is_within_tolerance(symbols[i].duration0, half_period * 2))
            is_low_with_backlog = true;
        else if (!is_within_tolerance(symbols[i].duration0, half_period))
        {
            ESP_LOGD(TAG, "Found invalid low value: %" PRId16,
                symbols[i].duration0);
            return i;
        }

        if (*backlog_length && *backlog_level == 1)
        {
            ESP_LOGD(TAG, "Transition from high to low -> 1%s",
                is_low_with_backlog ? " (with backlog)" : "");
            *parsed_value <<= 1;
            *parsed_value |= 1;

            *backlog_length = 0;

            if (is_low_with_backlog)
            {
                *backlog_level = 0;
                *backlog_length = half_period;
            }
        }
        else 
        {
            *backlog_level = 0;
            *backlog_length = half_period;
        }

        /* High value */
        if (is_within_tolerance(symbols[i].duration1, half_period * 2))
            is_high_with_backlog = true;
        else if (!is_within_tolerance(symbols[i].duration1, half_period))
        {
            ESP_LOGD(TAG, "Found invalid high value: %" PRId16,
                symbols[i].duration1);
            return i;
        }

        if (*backlog_length && *backlog_level == 0)
        {
            ESP_LOGD(TAG, "Transition from low to high -> 0%s",
                is_high_with_backlog ? " (with backlog)" : "");
            *parsed_value <<= 1;
            *parsed_value |= 0; /* NOP */

            *backlog_length = 0;

            if (is_high_with_backlog)
            {
                *backlog_level = 1;
                *backlog_length = half_period;
            }
        }
        else
        {
            *backlog_level = 1;
            *backlog_length = half_period;
        }
    }

    return i;
}

uint64_t parse_manchester(rmt_symbol_word_t *symbols, size_t len,
    uint16_t header_mark, uint16_t header_space, uint16_t half_period)
{
    int index = 0;
    uint16_t backlog_length = 0;
    uint16_t backlog_level = 0;
    uint16_t parsed_items = 0;
    uint64_t parsed_value = 0;

    if (!is_within_tolerance(symbols[index].duration0, header_mark))
    {
        ESP_LOGE(TAG, "Invalid header, missing mark");
        return 0;
    }
    if (is_within_tolerance(symbols[index].duration1,
        header_space + half_period))
    {
        /* Verify remaining backlog is a valid half_period and not just accepted
         * as such due to tolerance */
        if (is_within_tolerance(symbols[index].duration1 - header_space,
            half_period))
        {
            backlog_length = half_period;
            backlog_level = symbols[index].level1;
        }
    }
    else if (!is_within_tolerance(symbols[index].duration1, header_space))
    {
        ESP_LOGE(TAG, "Invalid header, missing space");
        return 0;
    }
    ESP_LOGD(TAG, "Found valid header with %" PRIu16
        " backlog, level: %" PRIu16, backlog_length, backlog_level);

    parsed_items = parse_manchester_data(&symbols[index + 1], len - 1,
        half_period, &backlog_length,
        &backlog_level, &parsed_value);
    if (!parsed_items)
    {
        ESP_LOGE(TAG, "Failed parsing manchester symbols");
        return 0;
    }
    index += parsed_items + 1;
    ESP_LOGD(TAG, "Tail of %d (backlog_length: %d, backlog_level: %d",
        symbols[index].duration0, backlog_length, backlog_level);

    return parsed_value;
}

#include <stdio.h> 
int generate_manchester(uint16_t header_mark, uint16_t header_space,
    uint16_t repeat, uint16_t tail_mark, uint16_t half_period, uint64_t value,
    rmt_symbol_word_t **symbols, size_t *len)
{
    int index = 0;
    rmt_symbol_word_t header = {
        .level0 = 0,
        .duration0 = header_mark,
        .level1 = 1,
        .duration1 = header_space,
    };
    rmt_symbol_word_t footer = {
        .level0 = 0,
        .duration0 = tail_mark,
        .level1 = 1,
        .duration1 = 0x7FFF,
    };
    rmt_symbol_word_t bit0 = {
        .level0 = 0,
        .duration0 = half_period,
        .level1 = 1,
        .duration1 = half_period,
    };
    rmt_symbol_word_t bit1 = {
        .level0 = 1,
        .duration0 = half_period,
        .level1 = 0,
        .duration1 = half_period,
    };

    *len = sizeof(**symbols) * (((1 + 34) * repeat) + 1);
    if (!(*symbols = malloc(*len)))
        return -1;

    for (int r = 0; r < repeat; r++)
    {
        (*symbols)[index++] = header;
        for (int bit = 33; bit >=0 ; bit--)
            (*symbols)[index++] = value & (1ULL << bit) ? bit1 : bit0;
    }
    (*symbols)[index++] = footer;

    return 0;
}
