#include <stdio.h>
#include <string.h>
#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_system.h>
#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "driver/uart.h"
#include "hardware.h"
#include "managed_i2c.h"
#include "pax_gfx.h"
#include "sdcard.h"
#include "appfs.h"
#include "esp_ota_ops.h"
#include "rp2040.h"
#include "rp2040bl.h"

#include "fpga_test.h"

#include "menu.h"
#include "system_wrapper.h"
#include "graphics_wrapper.h"
#include "appfs_wrapper.h"
#include "settings.h"
#include "wifi_connection.h"
#include "rp2040_updater.h"

#include "ws2812.h"

#include "esp32/rom/crc.h"

#include "efuse.h"

#include "wifi_ota.h"

#include "esp_vfs.h"
#include "esp_vfs_fat.h"

#include <pax_codecs.h>

#include "audio.h"

#include "bootscreen.h"

#include "menus/start.h"

#include "factory_test.h"
#include "fpga_download.h"
#include "webusb.h"

#include "sao_eeprom.h"

extern const uint8_t wallpaper_png_start[] asm("_binary_wallpaper_png_start");
extern const uint8_t wallpaper_png_end[] asm("_binary_wallpaper_png_end");

extern const uint8_t logo_screen_png_start[] asm("_binary_logo_screen_png_start");
extern const uint8_t logo_screen_png_end[] asm("_binary_logo_screen_png_end");


static const char *TAG = "main";

void display_fatal_error(pax_buf_t* pax_buffer, ILI9341* ili9341, const char* line0, const char* line1, const char* line2, const char* line3) {
    const pax_font_t *font = pax_get_font("saira regular");
    pax_noclip(pax_buffer);
    pax_background(pax_buffer, 0xa85a32);
    if (line0 != NULL) pax_draw_text(pax_buffer, 0xFFFFFFFF, font, 23, 0, 20*0, line0);
    if (line1 != NULL) pax_draw_text(pax_buffer, 0xFFFFFFFF, font, 18, 0, 20*1, line1);
    if (line2 != NULL) pax_draw_text(pax_buffer, 0xFFFFFFFF, font, 18, 0, 20*2, line2);
    if (line3 != NULL) pax_draw_text(pax_buffer, 0xFFFFFFFF, font, 18, 0, 20*3, line3);
    ili9341_write(ili9341, pax_buffer->buf);
}

void stop() {
    ESP_LOGW(TAG, "*** HALTED ***");
    gpio_set_direction(GPIO_SD_PWR, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_SD_PWR, 1);
    ws2812_init(GPIO_LED_DATA);
    uint8_t led_off[15] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t led_red[15] = {0, 50, 0, 0, 50, 0, 0, 50, 0, 0, 50, 0, 0, 50, 0};
    uint8_t led_red2[15] = {0, 0xFF, 0, 0, 0xFF, 0, 0, 0xFF, 0, 0, 0xFF, 0, 0, 0xFF, 0};
    while (true) {
        ws2812_send_data(led_red2, sizeof(led_red2));
        vTaskDelay(pdMS_TO_TICKS(200));
        ws2812_send_data(led_red, sizeof(led_red));
        vTaskDelay(pdMS_TO_TICKS(200));
        ws2812_send_data(led_off, sizeof(led_off));
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

const char* fatal_error_str = "A fatal error occured";
const char* reset_board_str = "Reset the board to try again";
static pax_buf_t pax_buffer;

void app_main(void) {
    esp_err_t res;
    
    audio_init();
    
    const esp_app_desc_t *app_description = esp_ota_get_app_description();
    ESP_LOGI(TAG, "App version: %s", app_description->version);
    //ESP_LOGI(TAG, "Project name: %s", app_description->project_name);

    /* Initialize GFX */
    pax_buf_init(&pax_buffer, NULL, ILI9341_WIDTH, ILI9341_HEIGHT, PAX_BUF_16_565RGB);

    /* Initialize hardware */

    efuse_protect();

    if (bsp_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize basic board support functions");
        esp_restart();
    }

    ILI9341* ili9341 = get_ili9341();
    if (ili9341 == NULL) {
        ESP_LOGE(TAG, "ili9341 is NULL");
        esp_restart();
    }
    
    /* Start NVS */
    res = nvs_init();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %d", res);
        display_fatal_error(&pax_buffer, ili9341, fatal_error_str, "NVS failed to initialize", "Flash may be corrupted", NULL);
        stop();
    }

    display_boot_screen(&pax_buffer, ili9341, "Starting...");

    /* Initialize RP2040 co-processor */
    if (bsp_rp2040_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize the RP2040 co-processor");
        display_fatal_error(&pax_buffer, ili9341, fatal_error_str, "Failed to communicate with", "the RP2040 co-processor", reset_board_str);
        stop();
    }

    RP2040* rp2040 = get_rp2040();

    rp2040_updater(rp2040, &pax_buffer, ili9341); // Handle RP2040 firmware update & bootloader mode
    
    factory_test(&pax_buffer, ili9341);

    if (bsp_ice40_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize the ICE40 FPGA");
        display_fatal_error(&pax_buffer, ili9341, fatal_error_str, "A hardware failure occured", "while initializing the FPGA", reset_board_str);
        stop();
    }

    ICE40* ice40 = get_ice40();

    /* Start AppFS */
    res = appfs_init();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "AppFS init failed: %d", res);
        display_fatal_error(&pax_buffer, ili9341, fatal_error_str, "Failed to initialize AppFS", "Flash may be corrupted", reset_board_str);
        stop();
    }
    
    /* Start internal filesystem */
    const esp_partition_t* fs_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "locfd");

    wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

    if (fs_partition != NULL) {
        const esp_vfs_fat_mount_config_t mount_config = {
            .format_if_mount_failed = true,
            .max_files              = 5,
            .allocation_unit_size   = 0,
        };
        esp_err_t res = esp_vfs_fat_spiflash_mount("/internal", "locfd", &mount_config, &s_wl_handle);
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "failed to mount locfd (%d)", res);
            display_fatal_error(&pax_buffer, ili9341, fatal_error_str, "Failed to initialize flash FS", "Flash may be corrupted", reset_board_str);
            stop();
        } else {
            ESP_LOGI(TAG, "Internal filesystem mounted");
        }
    } else {
        ESP_LOGE(TAG, "locfd partition not found");
    }

    /* Start SD card filesystem */
    res = mount_sd(GPIO_SD_CMD, GPIO_SD_CLK, GPIO_SD_D0, GPIO_SD_PWR, "/sd", false, 5);
    bool sdcard_ready = (res == ESP_OK);
    if (sdcard_ready) {
        ESP_LOGI(TAG, "SD card filesystem mounted");

        /* LED power is on: start LED driver and turn LEDs off */
        ws2812_init(GPIO_LED_DATA);
        const uint8_t led_off[15] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        ws2812_send_data(led_off, sizeof(led_off));
    } else {
        gpio_set_level(GPIO_SD_PWR, 0); // Disable power to LEDs and SD card
    }

    /* Start WiFi */
    wifi_init();
    
    /* Check WebUSB mode */
    
    uint8_t webusb_mode;
    res = rp2040_get_webusb_mode(rp2040, &webusb_mode);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read WebUSB mode: %d", res);
        display_fatal_error(&pax_buffer, ili9341, fatal_error_str, "Failed to read WebUSB mode", NULL, NULL);
        stop();
    }
    
    ESP_LOGI(TAG, "WebUSB mode 0x%02X", webusb_mode);
    
    if (webusb_mode == 0x00) { // Normal boot
        /* Add-on */

        sao_identify();

        /* Rick that roll */
        play_bootsound();

        /* Launcher menu */
        while (true) {
            menu_start(rp2040->queue, &pax_buffer, ili9341, app_description->version);
        }
    } else if (webusb_mode == 0x01) {
        display_boot_screen(&pax_buffer, ili9341, "WebUSB mode");
        while (true) {
            webusb_main(rp2040->queue, &pax_buffer, ili9341);
        }
    } else if (webusb_mode == 0x02) {
        display_boot_screen(&pax_buffer, ili9341, "FPGA download mode");
        while (true) {
            fpga_download(rp2040->queue, ice40, &pax_buffer, ili9341);
        }
    } else {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "Invalid mode 0x%02X", webusb_mode);
        display_boot_screen(&pax_buffer, ili9341, buffer);
    }
}
