#include "ir.h"
#include <driver/rmt_types.h>
#include <driver/rmt_rx.h>
#include <driver/rmt_tx.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <stdint.h>
#include <stdlib.h>

static const char *TAG = "IR";
static const uint32_t IR_RESOLUTION_HZ = 1000000UL; /* uS */

static ir_on_recv_cb_t on_recv_cb = NULL;
static QueueHandle_t receive_queue = NULL;
static rmt_channel_handle_t tx_channel = NULL;
static rmt_encoder_handle_t copy_encoder = NULL;

/* Event handlers */
void ir_set_on_recv_cb(ir_on_recv_cb_t cb)
{
    on_recv_cb = cb;
}

static bool rmt_recv_done(rmt_channel_handle_t rx_chan,
    const rmt_rx_done_event_data_t *edata, void *user_data)
{
    QueueHandle_t receive_queue = (QueueHandle_t)user_data;
    BaseType_t high_task_wakeup = pdFALSE;

    xQueueSendFromISR(receive_queue, edata, &high_task_wakeup);

    return high_task_wakeup == pdTRUE;
}

static void ir_task(void *pvParameter)
{
    rmt_channel_handle_t rx_channel = (rmt_channel_handle_t)pvParameter;
    rmt_symbol_word_t *raw_symbols = malloc(sizeof(*raw_symbols) * 512);
    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 1250,     // the shortest duration for NEC signal is 560us, 1250ns < 560us, valid signal won't be treated as noise
        .signal_range_max_ns = 12000000, // the longest duration for NEC signal is 9000us, 12000000ns > 9000us, the receive won't stop early
    };
    rmt_rx_done_event_data_t rx_data;

    while (1)
    {
        // ready to receive
        ESP_ERROR_CHECK(rmt_receive(rx_channel, raw_symbols, sizeof(*raw_symbols) * 512, &receive_config));

        // wait for RX done signal
        if (xQueueReceive(receive_queue, &rx_data, portMAX_DELAY) != pdPASS)
            continue;

        /* Ignore short, probably, random noise */
        if (rx_data.num_symbols < 5)
            continue;

        ESP_LOGI(TAG, "Got %zd IR symbols", rx_data.num_symbols);
        if (on_recv_cb)
            on_recv_cb(rx_data.received_symbols, rx_data.num_symbols);
#if 0
        uint64_t val = parse_manchester(&rx_data, 3 * 950, 3 * 950, 950);
        printf("Parsed value: 0x%" PRIx64 "\n", val);
        if (!val)
        {
            printf("Failed parsing command, ignoring\n");
            continue;
        }

        airwell_t value = { .raw = val };
        printf("Power toggle: %d, mode: %d, fan: %d, temp: %dC\n", value.power_toggle, value.mode, value.fan, value.temp + 15);
#endif
    }

    vTaskDelete(NULL);
}

int ir_send(rmt_symbol_word_t *symbols, size_t len)
{
    rmt_transmit_config_t transmit_config = {
        .loop_count = 0, // no loop
        .flags.eot_level = 1,
    };
    return rmt_transmit(tx_channel, copy_encoder, symbols, len, &transmit_config);
}

int ir_initialize(int rx_gpio, int tx_gpio)
{
    ESP_LOGI(TAG, "Initializing IR");

    rmt_rx_channel_config_t rx_channel_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = 512,
        .gpio_num = rx_gpio,
#ifdef SOC_RMT_SUPPORT_DMA
        .flags.with_dma = 1,
#endif
    };
    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = rmt_recv_done,
    };
    rmt_channel_handle_t rx_channel = NULL;
    rmt_tx_channel_config_t tx_channel_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = 512,
        .trans_queue_depth = 4,
        .gpio_num = tx_gpio,
        .flags.invert_out = 1,
#ifdef SOC_RMT_SUPPORT_DMA
        .flags.with_dma = 1,
#endif
    };

    receive_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_channel_cfg, &rx_channel));
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_channel, &cbs, receive_queue));

    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_channel_cfg, &tx_channel));


    rmt_copy_encoder_config_t copy_encoder_config = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_encoder_config, &copy_encoder));

    ESP_ERROR_CHECK(rmt_enable(tx_channel));
    ESP_ERROR_CHECK(rmt_enable(rx_channel));

    /* Reset TX output */
    static rmt_symbol_word_t footer = {
        .level0 = 0,
        .duration0 = 5 * 950ULL,
        .level1 = 1,
        .duration1 = 0x7FFF,
    };
    rmt_transmit_config_t transmit_config = {
        .loop_count = 0, // no loop
        .flags.eot_level = 1,
    };
    ESP_ERROR_CHECK(rmt_transmit(tx_channel, copy_encoder, &footer, sizeof(footer), &transmit_config));

    if (xTaskCreatePinnedToCore(ir_task, "ir_task",
        4096, rx_channel, 5,
        NULL, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed starting sensors task");
        return -1;
    }

    return 0;
}
