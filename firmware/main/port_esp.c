// ESP32-S3 포트: sct/port.h를 TWAI(legacy driver/twai.h), 콘솔(UART0 또는 USB Serial/JTAG), GPIO, ADC로 구현한다.
// TWAI 사용법·핀·보드 구분은 v1(../stock-can-tool/main/main.c)과 같다. 바꾼 것은 자체 시험 모드(NO_ACK + self) 하나다.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sct/out.h"
#include "sct/port.h"
#include "sdkconfig.h"
#include "port_esp.h"

#if CONFIG_CANTOOL_BOARD_MODULE
// 전용 모듈 보드 Rev.A0(회로는 이 저장소에 없다). 콘솔 = USB-C(USB Serial/JTAG)
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#define PIN_TALK 47    // DIP 2번(TALK) 읽기: 낮음 = 송신 가능, 높음 = 하드웨어 송신 잠금(게이트 U302가 트랜시버 TXD를 높게 묶는다)
#define PIN_MODE 7     // DIP 3번(MODE): 낮음 = 켜짐
#define PIN_USER 15    // USER 버튼: 낮음 = 눌림
#define PIN_LED_RX 16
#define PIN_LED_TX 17
#define PIN_LED_ERR 18
#define ADC_VIN ADC_CHANNEL_3   // IO4: 차량 전압 x 15/115
#define ADC_CANH ADC_CHANNEL_4  // IO5: CAN-H x 100/320
#define ADC_CANL ADC_CHANNEL_5  // IO6: CAN-L x 100/320
#define VIN_DIVIDER (115.0 / 15.0)  // [설계값] Rev.A0 분압
#define CAN_DIVIDER 3.2             // [설계값] Rev.A0 분압 320/100
#define ADC_SAMPLES 64              // v1과 같다
#define CON_TX_TIMEOUT_MS 100       // USB가 안 읽히면(호스트 없음) 이 시간 뒤 그 조각을 버린다
// 모듈 보드는 송신을 IO21에 둔다. ESP32-S3의 IO1~IO20은 전원이 들어올 때 60 us쯤 튄다(데이터시트 표 2-2).
#define CAN_TX_GPIO 21
#else
#include "driver/uart.h"
#define CONSOLE UART_NUM_0
#define CONSOLE_RX_BUF 1024
// 개발보드 조합은 IO13 그대로다(이미 배포한 결선). 그래서 보드에 전원을 먼저 넣고 그다음에 CAN 선을 물린다.
#define CAN_TX_GPIO 13
#endif
#define CAN_RX_GPIO 14

#define ALERTS (TWAI_ALERT_TX_SUCCESS | TWAI_ALERT_TX_FAILED | TWAI_ALERT_BUS_OFF | TWAI_ALERT_ERR_PASS)
#define TWAI_RX_QUEUE 256   // v1과 같다
#define TWAI_TX_QUEUE 4     // v1과 같다
#define TX_QUEUE_WAIT_MS 100 // v1 tx_once: 송신 대기열에 넣는 시간
#define ALERT_SLICE_MS 10    // v1 tx_once: 알림을 기다리는 한 번의 길이
#define OUT_TASK_STACK 3072
#define OUT_TASK_PRIO (tskIDLE_PRIORITY) // 명령 태스크(main, 우선순위 1)보다 낮게: CAN 수신이 콘솔 쓰기보다 먼저다
#define OUT_CHUNK 512

static struct {
    bool open;
    sct_mode_t mode;
    TaskHandle_t out_task;
#if CONFIG_CANTOOL_BOARD_MODULE
    adc_oneshot_unit_handle_t adc;
    adc_cali_handle_t cali;
#endif
} s;

// ---------------------------------------------------------------- 시간
int64_t port_now_us(void) { return esp_timer_get_time(); }

void port_sleep_ms(int ms)
{
    if (ms <= 0) taskYIELD();
    else vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

// ---------------------------------------------------------------- CAN
// 드라이버가 내려가 있는 동안 TX 핀이 뜨지 않게 풀업 입력으로 둔다(트랜시버 D 높음 = 버스에 아무것도 안 냄)
static void tx_pin_idle(void)
{
    gpio_set_direction(CAN_TX_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(CAN_TX_GPIO, GPIO_PULLUP_ONLY);
}

void port_can_close(void)
{
    if (s.open) {
        twai_stop();
        twai_driver_uninstall();
        s.open = false;
    }
    tx_pin_idle();
}

bool port_can_open(int kbps, sct_mode_t mode)
{
    port_can_close();
    twai_mode_t m = mode == SCT_MODE_NORMAL ? TWAI_MODE_NORMAL : mode == SCT_MODE_SELFTEST ? TWAI_MODE_NO_ACK : TWAI_MODE_LISTEN_ONLY;
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO, CAN_RX_GPIO, m);
    g.rx_queue_len = TWAI_RX_QUEUE;
    g.tx_queue_len = TWAI_TX_QUEUE;
    g.alerts_enabled = ALERTS;
    twai_timing_config_t t;
    switch (kbps) {
    case 1000: t = (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS(); break;
    case 500: t = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS(); break;
    case 250: t = (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS(); break;
    case 125: t = (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS(); break;
    case 100: t = (twai_timing_config_t)TWAI_TIMING_CONFIG_100KBITS(); break;
    default: return false;
    }
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
    if (twai_start() != ESP_OK) {
        twai_driver_uninstall();
        return false;
    }
    s.open = true;
    s.mode = mode;
    return true;
}

sct_tx_result_t port_can_send_once(const sct_frame_t *f, int wait_ms, int64_t *t_txdone_us)
{
    if (!s.open) return SCT_TX_DRIVER_ERR;
    twai_message_t m = {0};
    m.identifier = f->id;
    m.extd = f->ext;
    m.rtr = f->rtr;
    m.data_length_code = f->dlc > 8 ? 8 : f->dlc;
    m.ss = 1;                                  // 단발 송신: ACK가 없어도 다시 보내지 않는다
    m.self = s.mode == SCT_MODE_SELFTEST;      // 자체 시험만 자기 수신 요청
    memcpy(m.data, f->data, 8);
    uint32_t a;
    twai_read_alerts(&a, 0); // 지난 알림 비우기
    if (twai_transmit(&m, pdMS_TO_TICKS(TX_QUEUE_WAIT_MS)) != ESP_OK) {
        port_led(SCT_LED_ERR, true);
        return SCT_TX_DRIVER_ERR;
    }
    port_led(SCT_LED_TX, true);
    sct_tx_result_t r = SCT_TX_NO_ACK;
    int64_t t_end = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    while (esp_timer_get_time() < t_end) {
        if (twai_read_alerts(&a, pdMS_TO_TICKS(ALERT_SLICE_MS)) != ESP_OK) continue;
        if (a & TWAI_ALERT_TX_SUCCESS) {
            r = SCT_TX_ACKED;
            if (t_txdone_us) *t_txdone_us = esp_timer_get_time();
            break;
        }
        if (a & TWAI_ALERT_BUS_OFF) {
            r = SCT_TX_BUS_OFF;
            break;
        }
        if (a & TWAI_ALERT_TX_FAILED) break;
    }
    port_led(SCT_LED_TX, false);
    if (r != SCT_TX_ACKED) port_led(SCT_LED_ERR, true);
    return r;
}

bool port_can_recv(sct_frame_t *f, int timeout_ms)
{
    twai_message_t m;
    if (!s.open || twai_receive(&m, pdMS_TO_TICKS(timeout_ms)) != ESP_OK) return false;
    memset(f, 0, sizeof *f);
    f->id = m.identifier;
    f->ext = m.extd;
    f->rtr = m.rtr;
    f->dlc = m.data_length_code;
    memcpy(f->data, m.data, 8);
    f->t_us = esp_timer_get_time(); // 꺼낸 시각. 버스에서 끝난 시각보다 늦다(설계서 "타임스탬프의 한계")
    return true;
}

bool port_can_status(sct_bus_status_t *st)
{
    twai_status_info_t i;
    if (!s.open || twai_get_status_info(&i) != ESP_OK) return false;
    st->bus_errors = i.bus_error_count;
    st->rec = i.rx_error_counter;
    st->tec = i.tx_error_counter;
    st->tx_failed = i.tx_failed_count;
    st->rx_missed = i.rx_missed_count + i.rx_overrun_count;
    st->arb_lost = i.arb_lost_count;
    st->bus_off = i.state == TWAI_STATE_BUS_OFF;
    st->err_passive = !st->bus_off && (i.tx_error_counter >= 128 || i.rx_error_counter >= 128);
    return true;
}

// ---------------------------------------------------------------- 콘솔과 출력 태스크
// 출력 태스크: 링 버퍼(sct/out.c)를 콘솔로 내보낸다. 명령 태스크가 port_out_kick으로 깨운다.
static void out_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!sct_out_drain(OUT_CHUNK)) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    }
}

void port_out_kick(void)
{
    if (s.out_task) xTaskNotifyGive(s.out_task);
}

#if CONFIG_CANTOOL_BOARD_MODULE
static void con_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 1024;
    cfg.tx_buffer_size = 4096;
    usb_serial_jtag_driver_install(&cfg);
}

int port_con_getc(int timeout_ms)
{
    uint8_t c;
    return usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(timeout_ms)) == 1 ? c : SCT_CON_NONE;
}

bool port_con_key_pressed(void)
{
    uint8_t c;
    bool hit = false;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) == 1) hit = true;
    return hit;
}

void port_con_write(const char *p, size_t n)
{
    // 호스트가 읽지 않으면(USB가 열려 있지 않으면) 쓰기가 막힌다. 오래 막혀 출력 태스크가 서지 않게 시간을 둔다
    usb_serial_jtag_write_bytes(p, n, pdMS_TO_TICKS(CON_TX_TIMEOUT_MS));
}
#else
static void con_init(void) { uart_driver_install(CONSOLE, CONSOLE_RX_BUF, 0, 0, NULL, 0); }

int port_con_getc(int timeout_ms)
{
    uint8_t c;
    return uart_read_bytes(CONSOLE, &c, 1, pdMS_TO_TICKS(timeout_ms)) == 1 ? c : SCT_CON_NONE;
}

bool port_con_key_pressed(void)
{
    size_t n = 0;
    uart_get_buffered_data_len(CONSOLE, &n);
    if (n) uart_flush_input(CONSOLE);
    return n > 0;
}

void port_con_write(const char *p, size_t n) { uart_write_bytes(CONSOLE, p, n); } // 921600 baud로 늘 빠진다
#endif

// ---------------------------------------------------------------- 보드
#if CONFIG_CANTOOL_BOARD_MODULE
static int led_pin(int which) { return which == SCT_LED_RX ? PIN_LED_RX : which == SCT_LED_TX ? PIN_LED_TX : PIN_LED_ERR; }
void port_led(int which, bool on) { gpio_set_level(led_pin(which), on); }
bool port_talk_enabled(void) { return gpio_get_level(PIN_TALK) == 0; }
const char *port_board_name(void) { return "module-revA0"; }

static void board_init(void)
{
    gpio_config_t out = {.pin_bit_mask = BIT64(PIN_LED_RX) | BIT64(PIN_LED_TX) | BIT64(PIN_LED_ERR), .mode = GPIO_MODE_OUTPUT};
    gpio_config(&out);
    gpio_config_t in = {.pin_bit_mask = BIT64(PIN_TALK) | BIT64(PIN_MODE) | BIT64(PIN_USER), .mode = GPIO_MODE_INPUT};
    gpio_config(&in); // 풀업은 보드에 있다
    adc_oneshot_unit_init_cfg_t u = {.unit_id = ADC_UNIT_1};
    adc_oneshot_new_unit(&u, &s.adc);
    adc_oneshot_chan_cfg_t ch = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(s.adc, ADC_VIN, &ch);
    adc_oneshot_config_channel(s.adc, ADC_CANH, &ch);
    adc_oneshot_config_channel(s.adc, ADC_CANL, &ch);
    adc_cali_curve_fitting_config_t cc = {.unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cc, &s.cali) != ESP_OK) s.cali = NULL;
}

// 핀 전압(mV). 64번 평균(v1과 같다)
static int adc_mv(adc_channel_t ch)
{
    int sum = 0, n = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        int raw, mv;
        if (adc_oneshot_read(s.adc, ch, &raw) != ESP_OK) continue;
        if (s.cali && adc_cali_raw_to_voltage(s.cali, raw, &mv) == ESP_OK) sum += mv;
        else sum += raw * 3100 / 4095;
        n++;
    }
    return n ? sum / n : 0;
}

static void cmd_volts(void)
{
    double vin = adc_mv(ADC_VIN) * VIN_DIVIDER / 1000.0;
    double h = adc_mv(ADC_CANH) * CAN_DIVIDER / 1000.0, l = adc_mv(ADC_CANL) * CAN_DIVIDER / 1000.0;
    int64_t t = esp_timer_get_time();
    sct_outf("VOLT,%" PRId64 ",%.2f,%.2f,%.2f\n", t / 1000, vin, h, l);
    sct_outf("# 차량 전압 %.2f V (단자대 1번. 0이면 12 V를 안 물린 것), CAN-H %.2f V, CAN-L %.2f V (평균값)\n", vin, h, l);
    if (h > 4.5 || l > 4.5) sct_outf("# 4.5 V를 넘는다: 선이 전원에 닿았을 수 있다. 결선을 확인\n");
    else if (h < 0.5 && l < 0.5) sct_outf("# 둘 다 0 V 근처: 선이 접지에 닿았거나 측정 회로가 안 붙었다\n");
    else {
        sct_outf("# 도구가 물려 있으면 도구의 트랜시버(TCAN3414)가 쉬는 선을 약 1.9 V 쪽으로 당긴다. 이 값만으로 ECU가 켜졌다고 볼 수 없다.\n");
        sct_outf("# 통신이 오갈 때는 H 평균이 L 평균보다 높아야 한다. 반대면 H와 L이 바뀐 것이다\n");
    }
}

// 자체 점검: 버스로는 아무것도 내보내지 않는다(v1과 같다). 잠금 상태, 수신 핀의 쉬는 값, 전압만 본다.
static void cmd_selftest(void)
{
    port_can_close();
    gpio_set_direction(CAN_RX_GPIO, GPIO_MODE_INPUT);
    int low = 0;
    for (int i = 0; i < 2000; i++) { // 약 20 ms 동안 수신 핀을 본다
        low += gpio_get_level(CAN_RX_GPIO) == 0;
        esp_rom_delay_us(10);
    }
    sct_outf("# 자체 점검(송신 없음): TALK %s, 수신 핀이 낮았던 비율 %.1f %%\n", port_talk_enabled() ? "켜짐(송신 가능)" : "꺼짐(잠금)", low / 20.0);
    if (low == 2000) sct_outf("# 수신 핀이 계속 낮다: 버스가 dominant에 묶였거나 트랜시버 4번 핀 납땜을 확인한다\n");
    else if (low == 0) sct_outf("# 수신 핀이 계속 높다: 버스가 쉬고 있거나(정상) 연결되지 않았다\n");
    else sct_outf("# 수신 핀이 움직인다: 버스에 통신이 있다\n");
    sct_outf("# 송신 경로는 책상에서 loopback bench 로 확인한다(조용한 버스에서만)\n");
    cmd_volts();
}

bool port_board_cmd(int argc, char **argv)
{
    (void)argc;
    if (!strcmp(argv[0], "volts")) cmd_volts();
    else if (!strcmp(argv[0], "selftest")) cmd_selftest();
    else return false;
    return true;
}

void port_board_help(void)
{
    sct_outf("#   volts                  차량 전압과 CAN-H·CAN-L 평균 전압(모듈 보드)\n");
    sct_outf("#   selftest               자체 점검: 잠금 상태, 수신 핀, 전압. 버스로 나가지 않는다(모듈 보드)\n");
}

void port_board_banner(void)
{
    sct_outf("# stock-can-tool (TX GPIO%d, RX GPIO%d). help 로 명령 보기\n", CAN_TX_GPIO, CAN_RX_GPIO);
    sct_outf("# 보드: 순정 CAN 해석 모듈 Rev.A0. TALK %s, MODE %s\n", port_talk_enabled() ? "켜짐(송신 가능)" : "꺼짐(듣기 전용 잠금)",
             gpio_get_level(PIN_MODE) ? "꺼짐" : "켜짐");
}

void port_board_status(void)
{
    sct_outf("# TALK %s, MODE %s, USER 버튼 %s\n", port_talk_enabled() ? "켜짐(송신 가능)" : "꺼짐(듣기 전용 잠금)", gpio_get_level(PIN_MODE) ? "꺼짐" : "켜짐",
             gpio_get_level(PIN_USER) ? "안 눌림" : "눌림");
}
#else
void port_led(int which, bool on)
{
    (void)which;
    (void)on;
}
bool port_talk_enabled(void) { return true; } // 개발보드 + 트랜시버 모듈 조합에는 잠금 스위치가 없다
const char *port_board_name(void) { return "devkit"; }
static void board_init(void) {}
bool port_board_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return false;
}
void port_board_help(void) {}
void port_board_banner(void) { sct_outf("# stock-can-tool (TX GPIO%d, RX GPIO%d). help 로 명령 보기\n", CAN_TX_GPIO, CAN_RX_GPIO); }
void port_board_status(void) {}
#endif

void port_esp_init(void)
{
    con_init();
    board_init();
    tx_pin_idle();
    sct_out_init(true);
    xTaskCreatePinnedToCore(out_task, "sct_out", OUT_TASK_STACK, NULL, OUT_TASK_PRIO, &s.out_task, tskNO_AFFINITY);
}
