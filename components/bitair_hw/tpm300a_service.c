#include "tpm300a_service.h"

#include <ctype.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "state_service.h"

static const char *TAG = "tpm300a";

static const uart_port_t TPM300A_UART_PORT = UART_NUM_2;
static const uint32_t TPM300A_UART_BAUD = 9600;
static const uint32_t TPM300A_TASK_STACK_SIZE = 4096;
static const uint32_t TPM300A_READ_TIMEOUT_MS = 300;
static const size_t TPM300A_UART_RX_BUF_SIZE = 1024;
static const int TPM300A_RX_GPIO = 39;

enum {
    TPM300A_ASCII_LINE_MAX = 128,
    TPM300A_FRAME_V1_SIZE = 5,
    TPM300A_FRAME_V2_SIZE = 9,
    TPM300A_V2_COMMAND = 0xE4,
};

typedef struct {
    uint8_t frame[TPM300A_FRAME_V2_SIZE];
    size_t len;
} tpm_parser_t;

typedef struct {
    char line[TPM300A_ASCII_LINE_MAX];
    size_t len;
} ascii_parser_t;

static TaskHandle_t s_task_handle = NULL;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_uart_bytes_total = 0;
static volatile uint32_t s_frames_v1_ok = 0;
static volatile uint32_t s_frames_v2_ok = 0;
static volatile uint32_t s_frames_bad_checksum = 0;
static volatile uint32_t s_frames_bad_format = 0;
static volatile uint32_t s_ascii_lines_total = 0;
static volatile bool s_has_values = false;
static volatile uint16_t s_tvoc_raw = 0;
static volatile uint16_t s_ch2o_raw = 0;
static volatile uint16_t s_co2_ppm = 0;
static volatile int s_active_rx_gpio = TPM300A_RX_GPIO;
static volatile int s_active_tx_gpio = -1;

static esp_err_t apply_uart_pin_map(void)
{
    esp_err_t err = uart_set_pin(TPM300A_UART_PORT,
                                 UART_PIN_NO_CHANGE,
                                 TPM300A_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }

    uart_flush_input(TPM300A_UART_PORT);

    portENTER_CRITICAL(&s_lock);
    s_active_rx_gpio = TPM300A_RX_GPIO;
    s_active_tx_gpio = -1;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG,
             "Using UART RX-only mapping RX=GPIO%d TX=disabled @ %lu baud",
             TPM300A_RX_GPIO,
             (unsigned long)TPM300A_UART_BAUD);

    return ESP_OK;
}

static void log_ascii_line(const char *line)
{
    if (line[0] == '\0') {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    s_ascii_lines_total++;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGD(TAG, "ASCII frame: %s", line);
}

static void feed_ascii_parser(ascii_parser_t *parser, uint8_t byte)
{
    if (byte == '\r') {
        return;
    }

    if (byte == '\n') {
        parser->line[parser->len] = '\0';
        log_ascii_line(parser->line);
        parser->len = 0;
        return;
    }

    if (!isprint(byte) && byte != '\t') {
        return;
    }

    if (parser->len < (TPM300A_ASCII_LINE_MAX - 1U)) {
        parser->line[parser->len++] = (char)byte;
    } else {
        parser->line[parser->len] = '\0';
        log_ascii_line(parser->line);
        parser->len = 0;
    }
}

static uint16_t tpm_u16(const uint8_t *frame, size_t offset)
{
    return (uint16_t)(((uint16_t)frame[offset] << 8) | frame[offset + 1]);
}

static bool tpm_v2_checksum_ok(const uint8_t *frame)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < (TPM300A_FRAME_V2_SIZE - 1U); i++) {
        sum = (uint16_t)(sum + frame[i]);
    }
    return (uint8_t)(sum & 0xFFU) == frame[TPM300A_FRAME_V2_SIZE - 1U];
}

static void handle_complete_tpm_v2_frame(const uint8_t *frame)
{
    if (!tpm_v2_checksum_ok(frame)) {
        portENTER_CRITICAL(&s_lock);
        s_frames_bad_checksum++;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "TPM v2.2 frame checksum mismatch");
        return;
    }

    uint16_t tvoc_raw = tpm_u16(frame, 2);
    uint16_t ch2o_raw = tpm_u16(frame, 4);
    uint16_t co2_ppm = tpm_u16(frame, 6);

    float ch2o_mg_m3 = ((float)ch2o_raw) / 1000.0f;

    portENTER_CRITICAL(&s_lock);
    s_frames_v2_ok++;
    s_has_values = true;
    s_tvoc_raw = tvoc_raw;
    s_ch2o_raw = ch2o_raw;
    s_co2_ppm = co2_ppm;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGD(TAG,
             "TPM v2.2: TVOC=%u CH2O_raw=%u (%.3f mg/m3) CO2=%u ppm",
             (unsigned)tvoc_raw,
             (unsigned)ch2o_raw,
             (double)ch2o_mg_m3,
             (unsigned)co2_ppm);

    esp_err_t set_err = state_service_set_air_quality((float)tvoc_raw, ch2o_mg_m3, (float)co2_ppm);
    if (set_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update state sensor_data from TPM v2.2: %s", esp_err_to_name(set_err));
    }

}

static void handle_complete_tpm_v1_frame(const uint8_t *frame)
{
    if (frame[3] != 0x03 || frame[4] != 0xFF) {
        portENTER_CRITICAL(&s_lock);
        s_frames_bad_format++;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "TPM v1.2 frame format mismatch");
        return;
    }

    uint16_t tvoc_raw = tpm_u16(frame, 1);

    portENTER_CRITICAL(&s_lock);
    s_frames_v1_ok++;
    s_has_values = true;
    s_tvoc_raw = tvoc_raw;
    s_ch2o_raw = 0;
    s_co2_ppm = 0;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "TPM v1.2: TVOC=%u", (unsigned)tvoc_raw);
}

static void feed_tpm_parser(tpm_parser_t *parser, uint8_t byte)
{
    if (parser->len == 0) {
        if (byte == 0x2C) {
            parser->frame[parser->len++] = byte;
        }
        return;
    }

    parser->frame[parser->len++] = byte;

    if (parser->len == 2) {
        if (parser->frame[1] == TPM300A_V2_COMMAND) {
            return;
        }
    }

    if (parser->frame[1] == TPM300A_V2_COMMAND) {
        if (parser->len == TPM300A_FRAME_V2_SIZE) {
            handle_complete_tpm_v2_frame(parser->frame);
            parser->len = 0;
        }
        return;
    }

    if (parser->len == TPM300A_FRAME_V1_SIZE) {
        handle_complete_tpm_v1_frame(parser->frame);
        parser->len = 0;
    }
}

static void tpm300a_logger_task(void *arg)
{
    (void)arg;

    tpm_parser_t tpm_parser = {0};
    ascii_parser_t ascii_parser = {0};
    uint8_t rx_buf[128];

    while (true) {
        int read_len = uart_read_bytes(TPM300A_UART_PORT,
                                       rx_buf,
                                       sizeof(rx_buf),
                                       pdMS_TO_TICKS(TPM300A_READ_TIMEOUT_MS));

        if (read_len > 0) {
            portENTER_CRITICAL(&s_lock);
            s_uart_bytes_total += (uint32_t)read_len;
            portEXIT_CRITICAL(&s_lock);

            for (int i = 0; i < read_len; i++) {
                uint8_t byte = rx_buf[i];
                feed_ascii_parser(&ascii_parser, byte);
                feed_tpm_parser(&tpm_parser, byte);
            }
            continue;
        }

    }
}

esp_err_t tpm300a_service_start(void)
{
    if (s_task_handle != NULL) {
        return ESP_OK;
    }

    if (uart_is_driver_installed(TPM300A_UART_PORT)) {
        ESP_LOGE(TAG, "UART%d already in use", TPM300A_UART_PORT);
        return ESP_ERR_INVALID_STATE;
    }

    uart_config_t uart_cfg = {
        .baud_rate = TPM300A_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(TPM300A_UART_PORT, &uart_cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = uart_driver_install(TPM300A_UART_PORT,
                              TPM300A_UART_RX_BUF_SIZE,
                              0,
                              0,
                              NULL,
                              0);
    if (err != ESP_OK) {
        return err;
    }

    err = apply_uart_pin_map();
    if (err != ESP_OK) {
        uart_driver_delete(TPM300A_UART_PORT);
        return err;
    }

    if (xTaskCreate(tpm300a_logger_task, "tpm300a_service", TPM300A_TASK_STACK_SIZE, NULL, 2, &s_task_handle) != pdPASS) {
        uart_driver_delete(TPM300A_UART_PORT);
        s_task_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "TPM-300A UART service started on UART%d @ %lu baud",
             TPM300A_UART_PORT,
             (unsigned long)TPM300A_UART_BAUD);

    return ESP_OK;
}

esp_err_t tpm300a_service_get_snapshot(tpm300a_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    out_snapshot->has_values = s_has_values;
    out_snapshot->tvoc_raw = s_tvoc_raw;
    out_snapshot->ch2o_raw = s_ch2o_raw;
    out_snapshot->co2_ppm = s_co2_ppm;
    out_snapshot->uart_bytes_total = s_uart_bytes_total;
    out_snapshot->frames_v1_ok = s_frames_v1_ok;
    out_snapshot->frames_v2_ok = s_frames_v2_ok;
    out_snapshot->frames_bad_checksum = s_frames_bad_checksum;
    out_snapshot->frames_bad_format = s_frames_bad_format;
    out_snapshot->ascii_lines_total = s_ascii_lines_total;
    out_snapshot->active_rx_gpio = s_active_rx_gpio;
    out_snapshot->active_tx_gpio = s_active_tx_gpio;
    portEXIT_CRITICAL(&s_lock);

    return ESP_OK;
}
