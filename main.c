#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"   // IDF 5.x: tách khỏi esp_system.h
#include "esp_mac.h"      // esp_read_mac(), ESP_MAC_WIFI_STA

#define UART_PORT    UART_NUM_1
#define TXD_PIN      25        // đổi nếu phần cứng khác
#define RXD_PIN      26
#define BAUDRATE     115200
#define RX_BUF_SIZE  256
#define TARGET_SUM   20        // mốc thắng

static const char *TAG = "horse_A";

// -------- trạng thái trò chơi ----------
typedef enum { ST_PLAYING = 0, ST_WON, ST_LOST, ST_TIED } state_t;

static volatile state_t my_state   = ST_PLAYING;
static volatile state_t peer_state = ST_PLAYING;

static volatile uint32_t my_round  = 0;
static volatile int      my_sum    = 0;

static volatile uint32_t peer_round = 0;
static volatile int      peer_sum   = 0;

static uint16_t my_mac_lsb = 0;   // để tie-breaker

// ------------- UART --------------------
static void uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, RX_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, TXD_PIN, RXD_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static inline int roll(void)
{
    return (int)(esp_random() % 6) + 1;   // 1..6
}

static inline void send_line(const char *s)
{
    uart_write_bytes(UART_PORT, s, strlen(s));
}

// ------------- phân xử WIN đồng thời --------------
static state_t decide_winner(uint32_t peer_win_round, int peer_win_sum, uint16_t peer_mac_lsb)
{
    if (peer_win_round < my_round) return ST_LOST;
    if (peer_win_round > my_round) return ST_WON;

    // cùng round
    if (peer_win_sum > my_sum) return ST_LOST;
    if (peer_win_sum < my_sum) return ST_WON;

    // cùng sum -> so MAC LSB cho quyết định ổn định
    if (peer_mac_lsb < my_mac_lsb) return ST_LOST;
    if (peer_mac_lsb > my_mac_lsb) return ST_WON;

    return ST_TIED;
}

// ------------- RX task: đọc UART, cập nhật peer -------------
static void rx_task(void *arg)
{
    uint8_t buf[RX_BUF_SIZE];

    while (1) {
        int len = uart_read_bytes(UART_PORT, buf, sizeof(buf) - 1, pdMS_TO_TICKS(200));
        if (len <= 0) continue;

        buf[len] = 0;
        // Có thể nhận nhiều dòng trong 1 lần đọc → tách theo \n
        char *saveptr;
        char *line = strtok_r((char*)buf, "\r\n", &saveptr);
        while (line) {
            // format 1: R:<round>,D:<dice>,S:<sum>
            uint32_t r; int d; int s;
            if (sscanf(line, "R:%" SCNu32 ",D:%d,S:%d", &r, &d, &s) == 3) {
                peer_round = r;
                peer_sum   = s;
                ESP_LOGI(TAG, "Peer roll: r=%" PRIu32 ", d=%d, peer_sum=%d", r, d, peer_sum);
            }
            else {
                // format 2: WIN:R<round>,S<sum>,M<mac_lsb_hex>
                uint32_t wr; int ws; unsigned pmac_hex;
                if (sscanf(line, "WIN:R%" SCNu32 ",S%d,M%x", &wr, &ws, &pmac_hex) == 3) {
                    uint16_t peer_mlsb = (uint16_t)pmac_hex;

                    ESP_LOGI(TAG, "Peer announces WIN: round=%" PRIu32 ", sum=%d, macLSB=%04X",
                             wr, ws, peer_mlsb);

                    if (my_state == ST_PLAYING) {
                        // mình chưa kết thúc → thua ngay
                        my_state = ST_LOST;
                        ESP_LOGW(TAG, "FINAL: I LOSE (my_sum=%d, peer_sum=%d)", my_sum, peer_sum);
                    } else if (my_state == ST_WON) {
                        // cả hai cùng báo WIN → phân xử
                        state_t res = decide_winner(wr, ws, peer_mlsb);
                        if (res == ST_WON) {
                            ESP_LOGW(TAG, "Simultaneous WIN resolved: I WIN");
                        } else if (res == ST_LOST) {
                            my_state = ST_LOST;
                            ESP_LOGW(TAG, "Simultaneous WIN resolved: I LOSE");
                        } else {
                            my_state = ST_TIED;
                            ESP_LOGW(TAG, "Simultaneous WIN resolved: TIE");
                        }
                        ESP_LOGI(TAG, "FINAL: my_sum=%d (round=%" PRIu32 "), peer_sum=%d (round=%" PRIu32 ")",
                                 my_sum, my_round, peer_sum, peer_round);
                    } // nếu mình đã LOST/TIED thì giữ nguyên
                }
            }
            line = strtok_r(NULL, "\r\n", &saveptr);
        }
    }
}

// ------------- Game task: tung xúc xắc & gửi -------------
static void game_task(void *arg)
{
    // In MAC để biết board nào & lấy MAC LSB cho tie-breaker
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    my_mac_lsb = ((uint16_t)mac[4] << 8) | mac[5];
    ESP_LOGI(TAG, "STA MAC: %02X:%02X:%02X:%02X:%02X:%02X (LSB=%04X)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], my_mac_lsb);

    while (my_state == ST_PLAYING) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (my_state != ST_PLAYING) break;  // đề phòng state đổi do RX

        my_round++;
        int d = roll();
        my_sum += d;

        char msg[48];
        int n = snprintf(msg, sizeof(msg), "R:%" PRIu32 ",D:%d,S:%d\n", my_round, d, my_sum);
        uart_write_bytes(UART_PORT, msg, n);

        ESP_LOGI(TAG, "I rolled %d (my_sum=%d, peer_sum=%d) [round=%" PRIu32 "]",
                 d, my_sum, peer_sum, my_round);

        if (my_sum >= TARGET_SUM) {
            char winmsg[64];
            int wn = snprintf(winmsg, sizeof(winmsg), "WIN:R%" PRIu32 ",S%d,M%04X\n",
                              my_round, my_sum, my_mac_lsb);
            uart_write_bytes(UART_PORT, winmsg, wn);

            my_state = ST_WON;
            ESP_LOGW(TAG, "I REACHED %d! I WIN", TARGET_SUM);
            break;
        }
    }

    // Kết luận cuối (nếu chưa in)
    if (my_state == ST_WON) {
        ESP_LOGI(TAG, "FINAL: I WIN (my_sum=%d, peer_sum=%d)", my_sum, peer_sum);
    } else if (my_state == ST_LOST) {
        ESP_LOGI(TAG, "FINAL: I LOSE (my_sum=%d, peer_sum=%d)", my_sum, peer_sum);
    } else if (my_state == ST_TIED) {
        ESP_LOGI(TAG, "FINAL: TIE (my_sum=%d, peer_sum=%d)", my_sum, peer_sum);
    }

    // Không xoá task để vẫn còn RX đọc thông báo đối phương nếu cần
    vTaskDelete(NULL);
}

void app_main(void)
{
    uart_init();
    xTaskCreate(rx_task,   "rx_task",   4096, NULL, 10, NULL);
    xTaskCreate(game_task, "game_task", 4096, NULL,  9, NULL);
}