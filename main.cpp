
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_system.h"  // For esp_restart()
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera_pins.h" // Include your camera pin configuration header

static const char *TAG = "camera_httpd";

// Replace with your network credentials
const char* ssid = "GNNET24G";
const char* password = "Raymond79";

// Unique device name for identification
const char* device_name = "ESCM1";

// Onboard LED GPIO pins (check your board for correct pins)
#define ONBOARD_LED_RED 33
#define ONBOARD_LED_WHITE 4

httpd_handle_t server = NULL;

esp_err_t jpg_stream_httpd_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t * _jpg_buf = NULL;
    char part_buf[64];

    static int64_t last_frame = 0;
    if (!last_frame) {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=123456789000000000000987654321");
    if (res != ESP_OK) {
        return res;
    }

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }

        if (fb->format != PIXFORMAT_JPEG) {
            bool jpeg_converted = frame2jpg(fb, 30, &_jpg_buf, &_jpg_buf_len); 
            esp_camera_fb_return(fb);
            fb = NULL; 
            if (!jpeg_converted) {
                ESP_LOGE(TAG, "JPEG compression failed");
                res = ESP_FAIL;
            }
        } else {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }

        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, 64, "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", _jpg_buf_len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }

        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }

        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, "\r\n--123456789000000000000987654321\r\n", 37);
        }
        
        // LED: White On (Streaming active)
        gpio_set_level(ONBOARD_LED_WHITE, 0); // White LED is typically active low

        // Log frame information less frequently
        static int frame_counter = 0;
        if (frame_counter++ % 10 == 0) {
            int64_t frame_time = esp_timer_get_time() - last_frame;
            last_frame = esp_timer_get_time();
            frame_time /= 1000;
            ESP_LOGI(TAG, "MJPG: %uKB %ums (%.1ffps)", (uint32_t)(_jpg_buf_len / 1024), (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time);
        }

        if (fb) {
            esp_camera_fb_return(fb);
            fb = NULL; 
        } else if (_jpg_buf) {
            free(_jpg_buf);
            _jpg_buf = NULL;
        }

        if (res != ESP_OK) {
            break;
        }

        // LED: White Off (momentary pause between frames)
        gpio_set_level(ONBOARD_LED_WHITE, 1);
    }

    last_frame = 0;
    return res;
}


void setup() {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Configure LED GPIOs as output
    gpio_pad_select_gpio(ONBOARD_LED_RED);
    gpio_set_direction(ONBOARD_LED_RED, GPIO_MODE_OUTPUT);
    gpio_pad_select_gpio(ONBOARD_LED_WHITE);
    gpio_set_direction(ONBOARD_LED_WHITE, GPIO_MODE_OUTPUT);

    // LED: Slow blink (Red) - Starting up
    for (int i = 0; i < 3; i++) {  // Blink 3 times during startup
        gpio_set_level(ONBOARD_LED_RED, 1);
        vTaskDelay(250 / portTICK_PERIOD_MS);
        gpio_set_level(ONBOARD_LED_RED, 0);
        vTaskDelay(250 / portTICK_PERIOD_MS);
    }
    // ...(Rest of your Wi-Fi and Camera initialization remains same)
}

    // LED: Slow blink (Red) - Starting up
    for (int i = 0; i < 3; i++) {  // Blink 3 times during startup
        gpio_set_level(ONBOARD_LED_RED, 1); // Turn LED on (Red)
        vTaskDelay(250 / portTICK_PERIOD_MS);
        gpio_set_level(ONBOARD_LED_RED, 0); // Turn LED off
        vTaskDelay(250 / portTICK_PERIOD_MS);
    }

    // Initialize Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ssid,  // Use the variable ssid here
            .password = password, // Use the variable password here
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Set hostname for easier identification in router's client list
    ESP_ERROR_CHECK(tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, device_name));

    // Connect to Wi-Fi and wait for connection
    ESP_ERROR_CHECK(esp_wifi_connect());
    int retry_count = 0;
    const int MAX_RETRY = 10;
    while (esp_wifi_get_status() != WIFI_STATUS_CONNECTED && retry_count < MAX_RETRY) {
        ESP_LOGI(TAG, "Connecting to Wi-Fi...");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        retry_count++;
    }

    if (retry_count >= MAX_RETRY) {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi");
        esp_restart(); 
        return; 
    }

    ESP_LOGI(TAG, "Connected to Wi-Fi");

    // LED: Red (Wi-Fi error) or Off (Wi-Fi connected)
    gpio_set_level(ONBOARD_LED_RED, esp_wifi_get_status() == WIFI_STATUS_CONNECTED ? 0 : 1);

    // Initialize camera with error handling and restart
    camera_config_t config = {
        .pin_pwdn = PWDN_GPIO_NUM,
        .pin_reset = RESET_GPIO_NUM,
        .pin_xclk = XCLK_GPIO_NUM,
        .pin_sscb_sda = SIOD_GPIO_NUM,
        .pin_sscb_scl = SIOC_GPIO_NUM,
        .pin_d7 = Y7_GPIO_NUM,    // Corrected pin assignments
        .pin_d6 = Y6_GPIO_NUM,
        .pin_d5 = Y5_GPIO_NUM,
        .pin_d4 = Y4_GPIO_NUM,
        .pin_d3 = Y3_GPIO_NUM,
        .pin_d2 = Y2_GPIO_NUM,
        .pin_d1 = Y1_GPIO_NUM,
        .pin_d0 = Y0_GPIO_NUM,
        .pin_vsync = VSYNC_GPIO_NUM,
        .pin_href = HREF_GPIO_NUM,
        .pin_pclk = PCLK_GPIO_NUM,

        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG, 
        .frame_size = FRAMESIZE_VGA,
        .jpeg_quality = 10,
        .fb_count = 2               
    }
    
    // Start HTTP server
    httpd_config_t config_httpd = HTTPD_DEFAULT_CONFIG();
    config_httpd.max_uri_handlers = 8; // Increase max URI handlers for streaming
    ESP_ERROR_CHECK(httpd_start(&server, &config_httpd));
    ESP_LOGI(TAG, "HTTP server started");

    // Register URI handler for MJPEG stream
    httpd_uri_t uri_handler = {
        .uri = "/jpg_stream",
        .method = HTTP_GET,
        .handler = jpg_stream_httpd_handler,
        .user_ctx = NULL
    }
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_handler));
}

// Main function
void app_main() {
    setup();
}

