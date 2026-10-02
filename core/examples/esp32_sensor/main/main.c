/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* AltSql on an ESP32: a machine sensor that decides for itself.
 *
 * NOT YET TESTED ON HARDWARE. Written against the ESP-IDF 5.1+ APIs; the
 * same engine code is tested on the desktop (make test) with emulated flash.
 *
 * Every second: read the temperature, store it. If it stays above 60 C for
 * 10 seconds, raise an alarm and switch the fan on (GPIO 2) - no network
 * needed. Every minute: store a min/avg/max summary row. A gateway can
 * collect the records with altsql_sync_read() over any link you like.     */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "driver/gpio.h"

#define ALTSQL_ENABLE_SQL  0          /* the sensor build: no SQL, no text export */
#define ALTSQL_ENABLE_TEXT 0
#define ALTSQL_IMPLEMENTATION
#include "altsql.h"

#define FAN_GPIO GPIO_NUM_2
#define SECTOR   4096u

static const char *TAG = "altsql";
static uint8_t engine_mem[2048];      /* all the RAM AltSql gets */

/* ---- flash driver: an ESP-IDF data partition ------------------------------------- */
static int part_read(void *ctx, uint32_t addr, void *buf, uint32_t len) {
    return esp_partition_read((const esp_partition_t *)ctx, addr, buf, len) == ESP_OK ? 0 : -1;
}
static int part_write(void *ctx, uint32_t addr, const void *buf, uint32_t len) {
    return esp_partition_write((const esp_partition_t *)ctx, addr, buf, len) == ESP_OK ? 0 : -1;
}
static int part_erase(void *ctx, uint32_t sector) {
    return esp_partition_erase_range((const esp_partition_t *)ctx, sector * SECTOR, SECTOR) == ESP_OK ? 0 : -1;
}

/* Replace with your sensor driver. Here: a warm machine that overheats now and then. */
static double read_temperature(int64_t t) {
    double base = 45.0 + 3.0 * sin((double)t / 600.0);
    if (t % 1800 > 1700) base += 20.0;
    return base + (double)(esp_random() % 100) / 100.0;
}

void app_main(void) {
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "altsql");
    altsql_flash fl;
    altsql_config cfg;
    altsql_stats st;
    altsql *db = NULL;
    int64_t t;
    int fan = 0, rc;

    if (!part) { ESP_LOGE(TAG, "no 'altsql' partition; see partitions.csv"); return; }
    memset(&fl, 0, sizeof fl);
    fl.ctx = (void *)part;
    fl.sector_size = SECTOR;
    fl.sector_count = part->size / SECTOR;
    fl.write_align = 4;               /* use 16 with flash encryption on */
    fl.read = part_read;
    fl.write = part_write;
    fl.erase = part_erase;
    fl.map = NULL;                    /* read through the driver; no mmap cache issues */

    memset(&cfg, 0, sizeof cfg);
    cfg.mem = engine_mem;
    cfg.mem_size = sizeof engine_mem;
    cfg.kv_slots = 32;
    cfg.max_series = 4;
    cfg.max_record = 128;
    cfg.create = 1;                   /* format the partition on first boot */
    rc = altsql_open(&db, &fl, &cfg);
    if (rc) { ESP_LOGE(TAG, "open failed (%d): %s", rc, db ? altsql_errmsg(db) : "bad settings"); return; }

    rc = altsql_ts_create(db, "raw", "time:time,temp:float");
    if (rc && rc != ALTSQL_EXISTS) ESP_LOGE(TAG, "%s", altsql_errmsg(db));
    rc = altsql_ts_create(db, "minute", "time:time,min:float,avg:float,max:float");
    if (rc && rc != ALTSQL_EXISTS) ESP_LOGE(TAG, "%s", altsql_errmsg(db));

    /* Without a real-time clock, carry on from the newest stored reading. */
    altsql_ts_window(db, "raw", "temp", INT64_MIN, &st);
    t = st.count ? st.last + 1 : 0;
    ESP_LOGI(TAG, "open: %u readings stored, continuing at t=%lld", (unsigned)st.count, (long long)t);

    gpio_reset_pin(FAN_GPIO);
    gpio_set_direction(FAN_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(FAN_GPIO, 0);

    for (;; t++) {
        altsql_stats hot;
        double temp = read_temperature(t);
        if (altsql_append(db, "raw", t, temp)) ESP_LOGE(TAG, "append: %s", altsql_errmsg(db));

        /* the decision is made here, on the device */
        altsql_ts_window(db, "raw", "temp", t - 9, &hot);
        if (!fan && hot.count == 10 && hot.min > 60.0) {
            fan = 1;
            gpio_set_level(FAN_GPIO, 1);
            altsql_put(db, "alarm", "overheat", 8);
            ESP_LOGW(TAG, "t=%lld: above 60 C for 10 s, fan on", (long long)t);
        } else if (fan && hot.count == 10 && hot.max < 55.0) {
            fan = 0;
            gpio_set_level(FAN_GPIO, 0);
            altsql_del(db, "alarm");
            ESP_LOGI(TAG, "t=%lld: back to normal, fan off", (long long)t);
        }

        if (t % 60 == 59) {
            altsql_stats m;
            altsql_info info;
            altsql_ts_window(db, "raw", "temp", t - 59, &m);
            altsql_append(db, "minute", t / 60, m.min, m.avg, m.max);
            altsql_info_get(db, &info);
            ESP_LOGI(TAG, "minute %lld: avg %.2f max %.2f | flash %u/%u sectors, %u rows rolled over",
                     (long long)(t / 60), m.avg, m.max, (unsigned)info.used_sectors, (unsigned)info.sectors,
                     (unsigned)info.rows_dropped);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
