/**
 * @file imu.c
 * @brief The `imu` API extension: the QMI8658 accelerometer / gyroscope of the Waveshare
 *        ESP32-S3-Matrix (I2C) as Scheme procedures.
 *
 * (imu-accel) => (x y z) in mg, (imu-gyro) => (x y z) in 0.1 degrees a second,
 * (imu-temperature) => 0.1 degrees Celsius; the chip's own axes. #f when the read fails.
 */

#include "imu.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "minischeme_internal.h"

static const char *TAG = "imu";

#define IMU_PIN_A        11                  /* SDA on the board (SCL when the first try finds nothing) */
#define IMU_PIN_B        12
#define IMU_TIMEOUT_MS   50

#define QMI_WHO_AM_I     0x00                /* reads 0x05 */
#define QMI_CTRL1        0x02
#define QMI_CTRL2        0x03                /* accelerometer: full scale, rate */
#define QMI_CTRL3        0x04                /* gyroscope: full scale, rate */
#define QMI_CTRL7        0x08                /* sensors on */
#define QMI_TEMP_L       0x33                /* then accel x y z, gyro x y z: 16 bits each, low byte first */
#define QMI_RESET        0x60

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_ready = false;
static int16_t s_raw[7];                     /* temperature, accel x y z, gyro x y z */
static int64_t s_read_us = 0;

static bool reg_write(uint8_t reg, uint8_t value) {
    uint8_t out[2] = { reg, value };
    return i2c_master_transmit(s_dev, out, 2, IMU_TIMEOUT_MS) == ESP_OK;
}

static bool reg_read(uint8_t reg, uint8_t *buf, size_t len) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, IMU_TIMEOUT_MS) == ESP_OK;
}

/* The chip on these two pins, at either of its addresses */
static bool imu_find(int sda, int scl) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = sda, .scl_io_num = scl, .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        s_bus = NULL;
        return false;
    }
    static const uint8_t ADDRS[] = { 0x6B, 0x6A };
    for (size_t i = 0; i < sizeof(ADDRS); i++) {
        if (i2c_master_probe(s_bus, ADDRS[i], IMU_TIMEOUT_MS) != ESP_OK) continue;
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDRS[i], .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) continue;
        uint8_t id = 0;
        if (reg_read(QMI_WHO_AM_I, &id, 1) && id == 0x05) {
            ESP_LOGI(TAG, "QMI8658 at 0x%02x (SDA %d, SCL %d)", ADDRS[i], sda, scl);
            return true;
        }
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    i2c_del_master_bus(s_bus);
    s_bus = NULL;
    return false;
}

bool imu_init(void) {
    if (s_ready) return true;
    if (!imu_find(IMU_PIN_A, IMU_PIN_B) && !imu_find(IMU_PIN_B, IMU_PIN_A)) {
        ESP_LOGW(TAG, "no QMI8658 on GPIO %d / %d", IMU_PIN_A, IMU_PIN_B);
        return false;
    }
    reg_write(QMI_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(20));
    /* register address auto increment, low byte first; +-4 g and +-512 dps at 112 Hz; both on */
    bool ok = reg_write(QMI_CTRL1, 0x40) && reg_write(QMI_CTRL2, 0x16) && reg_write(QMI_CTRL3, 0x56) &&
              reg_write(QMI_CTRL7, 0x03);
    if (!ok) {
        ESP_LOGW(TAG, "the QMI8658 did not take its settings");
        return false;
    }
    s_ready = true;
    return true;
}

bool imu_ready(void) { return s_ready; }

/* One read serves the calls of the same moment (accel, then gyro) */
static bool imu_read(void) {
    if (!s_ready) return false;
    int64_t now = esp_timer_get_time();
    if (s_read_us && now - s_read_us < 4000) return true;
    uint8_t b[14];
    if (!reg_read(QMI_TEMP_L, b, sizeof(b))) return false;
    for (int i = 0; i < 7; i++) s_raw[i] = (int16_t)((uint16_t)b[2 * i] | ((uint16_t)b[2 * i + 1] << 8));
    s_read_us = now;
    return true;
}

static Value list3(MiniVM *vm, int a, int b, int c) {
    Value lst = mini_nil();
    mini_push_root(vm, &lst);
    lst = mini_make_cons(vm, mini_make_fixnum(c), lst);
    lst = mini_make_cons(vm, mini_make_fixnum(b), lst);
    lst = mini_make_cons(vm, mini_make_fixnum(a), lst);
    mini_pop_root(vm);
    return lst;
}

/* (imu-accel) => (x y z) in mg (1000: one g), or #f */
static Value prim_accel(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!imu_read()) return mini_false();
    /* +-4 g over 16 bits: 4000 / 32768 mg a step */
    return list3(vm, s_raw[1] * 125 / 1024, s_raw[2] * 125 / 1024, s_raw[3] * 125 / 1024);
}

/* (imu-gyro) => (x y z) in 0.1 degrees a second, or #f */
static Value prim_gyro(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!imu_read()) return mini_false();
    /* +-512 dps over 16 bits: 1 / 64 dps a step */
    return list3(vm, s_raw[4] * 10 / 64, s_raw[5] * 10 / 64, s_raw[6] * 10 / 64);
}

/* (imu-temperature) => the chip's temperature in 0.1 degrees Celsius, or #f */
static Value prim_temperature(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    if (!imu_read()) return mini_false();
    return mini_make_fixnum(s_raw[0] * 10 / 256);
}

void imu_register(MiniVM *vm) {
    static const MiniPrimEntry PRIMS[] = {
        { "imu-accel", prim_accel }, { "imu-gyro", prim_gyro }, { "imu-temperature", prim_temperature },
    };
    mini_register_prim_table(vm, PRIMS, sizeof(PRIMS) / sizeof(PRIMS[0]));
}
