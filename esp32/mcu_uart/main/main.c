/*
 * 假 MCU：ESP32-C3 通过 UART1 跟小米 BLE Mesh 模组（MHCB1S 等）对话。
 *
 * 按官方《模组串口指令手册》：
 *   1. 上电第一时间上报 model，再上报 mcu_version（4 位数字）
 *   2. 以 200ms 周期 get_down 轮询，模组回 down none 或 down <方法> <参数>
 *   3. 收到下行命令 1 秒内必须回 result / error
 *   4. 模组自己的调试日志会混在协议行里，原样转发到 USB 串口方便看
 *
 * 接线（3.3V，共地）：
 *   ESP32 GPIO4 (TX) -> 模组 RX
 *   ESP32 GPIO5 (RX) <- 模组 TX
 *   ESP32 3V3        -> 模组 3V3
 *   ESP32 GND        -> 模组 GND
 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define UART_PORT   UART_NUM_1
#define PIN_TX      4           /* ESP32 TX -> 模组 RX */
#define PIN_RX      5           /* ESP32 RX <- 模组 TX */
#define MODEL       "yyzn.light.wy0a04"
#define MCU_VERSION "0001"

static const char *TAG = "MCU";
static char  s_line[320];
static int   s_len;
static bool  s_handshaked;

static void send_cmd(const char *cmd)
{
    ESP_LOGI(TAG, "TX  %s", cmd);
    uart_write_bytes(UART_PORT, cmd, strlen(cmd));
    uart_write_bytes(UART_PORT, "\r", 1);
}

/* 只认协议前缀，其余当模组日志转发出去 */
static bool is_proto(const char *s)
{
    return strncmp(s, "ok", 2) == 0 || strncmp(s, "error", 5) == 0 ||
           strncmp(s, "down", 4) == 0 || strncmp(s, "result", 6) == 0 ||
           strncmp(s, "up", 2) == 0 || strncmp(s, "version", 7) == 0;
}

static void handle_line(char *s)
{
    if (!is_proto(s)) {
        ESP_LOGI(TAG, "LOG %s", s);
        return;
    }
    ESP_LOGW(TAG, "RX  %s", s);

    if (strcmp(s, "ok") == 0) {
        if (!s_handshaked) {
            s_handshaked = true;
            ESP_LOGI(TAG, "===== 握手成功，模组已认下 MCU =====");
        }
        return;
    }

    if (strncmp(s, "down", 4) == 0) {
        const char *body = s + 4;
        while (*body == ' ') body++;
        if (*body == '\0' || strcmp(body, "none") == 0) {
            return;                     /* 没命令，继续轮询 */
        }
        ESP_LOGI(TAG, "★★★★ 下行命令: %s ★★★★", body);
        /* TODO: 在这里按 spec 分发，比如开灯 / 关灯 / 调亮度 */
        send_cmd("result");
    }
}

void app_main(void)
{
    const uart_config_t cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, PIN_TX, PIN_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "===== 假 MCU 启动 TX=GPIO%d RX=GPIO%d model=%s =====",
             PIN_TX, PIN_RX, MODEL);

    int64_t next_poll = 0, next_hs = 0;
    uint8_t buf[256];

    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;   /* ms */

        if (!s_handshaked && now >= next_hs) {
            /* 手册要求：没收到 ok 就重发，否则模组当没有 MCU */
            send_cmd("model " MODEL);
            send_cmd("mcu_version " MCU_VERSION);
            next_hs = now + 1000;
        }

        if (now >= next_poll) {
            send_cmd("get_down");
            next_poll = now + 200;
        }

        int n = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r' || c == '\n') {
                if (s_len > 0) {
                    s_line[s_len] = '\0';
                    handle_line(s_line);
                    s_len = 0;
                }
            } else if (s_len < (int)sizeof(s_line) - 1) {
                s_line[s_len++] = c;
            }
        }
    }
}
