#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "hal/uart_types.h"
#include "hal/uart_ll.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "mqtt_client.h"

static const char *TAG = "LINAK_DESK";

esp_mqtt_client_handle_t mqttClient;

#define ESP_WIFI_SSID CONFIG_WIFI_SSID
#define ESP_WIFI_PASS CONFIG_WIFI_PASSWORD
#define ESP_MAXIMUM_RETRY 5

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;

#define MQTT_BROKER CONFIG_MQTT_BROKER
#define MQTT_SET_HEIGHT_TOPIC CONFIG_MQTT_SET_HEIGHT_TOPIC
#define MQTT_HEIGHT_TOPIC CONFIG_MQTT_HEIGHT_TOPIC

#define RX_PIN 0
#define TX_PIN 1
#define WAKE_UP_PIN GPIO_NUM_12

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

#define EX_UART_NUM UART_NUM_1
#define BUF_SIZE (1024)

// Define the UART event queue handle
static QueueHandle_t uart0_queue;

SemaphoreHandle_t gLinSemaphoreId = NULL;

volatile uint8_t rxbuf[10] = {0};
volatile size_t rx_len = 0;
volatile uint8_t txbuf[8] = {0};
volatile size_t tx_len = 0;

volatile uint16_t target_height = 0;
volatile uint16_t current_height = 0;
volatile uint16_t current_target_height = 0;
volatile bool is_moving = false;
volatile bool movement_requested = false;
volatile bool movement_blocked = false;
static int64_t timeout_ticks = 500000;

// PRBS (safety sequence) responses
static const uint8_t prbs_sequence[9][2] = {
    {0x3F, 0xD8},
    {0xDF, 0x38},
    {0xCF, 0x48},
    {0xD7, 0x40},
    {0xC3, 0x54},
    {0xDD, 0x3A},
    {0xCC, 0x4B},
    {0x55, 0xC2},
    {0x80, 0x97},
};

static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t data;

    static int current_pid = -1;
    static size_t rxd_len = 0;
    static size_t prbs_seq = 0;

    for (;;)
    {
        // Waiting for UART event.
        if (xQueueReceive(uart0_queue, (void *)&event, (TickType_t)portMAX_DELAY))
        {
            switch (event.type)
            {
            // Event of UART receving data.
            // This will contain the byte immidiatly following the 13 bit break since we cleared the RX buffer.
            case UART_DATA:;
                uart_read_bytes(EX_UART_NUM, &data, 1, 0);

                if (rxd_len == 0)
                {
                    // This is the PID. It determines what action we can take.
                    switch (data)
                    {
                    case 0x80:
                        // Ref1 position PID -> motor will answer
                        current_pid = 0x80;
                        break;
                    case 0x64:;
                        // Request power -> always respond
                        static const uint8_t frame[2] = {0x9A, 0x01};
                        uart_tx_chars(EX_UART_NUM, (char *)&frame, 2);
                        break;
                    case 0xE7:
                        // Safety sequence -> respond in sequence for as long as there is an active command
                        current_pid = -1; // Discard - do not need to make thread aware of PRBS

                        if (tx_len > 0)
                        {
                            uart_tx_chars(EX_UART_NUM, (char *)&prbs_sequence[prbs_seq], 2);

                            // Progress with next PRBS sequence (unless it's updated again by HS2)
                            prbs_seq += 1;
                            if (prbs_seq >= 9)
                                prbs_seq = 0;
                        }
                        break;
                    case 0xCA:
                        // Ref1 input -> respond with command if there is one
                        if (tx_len == 4 && txbuf[0] == 0xCA)
                        {
                            uint8_t frame[3] = {txbuf[1], txbuf[2], txbuf[3]};
                            uart_tx_chars(EX_UART_NUM, (char *)&frame, 3);
                        }
                        break;
                    default:
                        current_pid = -1; // Set PID to discard this message
                        break;
                    }
                }
                else if (rxd_len < sizeof(rxbuf))
                {
                    if (current_pid >= 0)
                    {
                        rxbuf[rxd_len] = data;
                    }
                }
                rxd_len += 1;
                break;

            // Event of HW FIFO overflow detected
            case UART_FIFO_OVF:
                ESP_LOGI(TAG, "hw fifo overflow");
                //  If fifo overflow happened, you should consider adding flow control for your application.
                //  The ISR has already reset the rx FIFO,
                //  As an example, we directly flush the rx buffer here in order to read more data.
                uart_flush_input(EX_UART_NUM);
                xQueueReset(uart0_queue);
                break;

            // Event of UART ring buffer full
            case UART_BUFFER_FULL:
                ESP_LOGI(TAG, "ring buffer full");
                //  If buffer full happened, you should consider encreasing your buffer size
                //  As an example, we directly flush the rx buffer here in order to read more data.
                uart_flush_input(EX_UART_NUM);
                xQueueReset(uart0_queue);
                break;

            // Event of UART RX break detected.
            // Flush the RX buffer and reset the LIN message. First byte of the next DATA event should be the PID.
            case UART_BREAK:
                uart_flush_input(EX_UART_NUM);

                if (current_pid >= 0)
                {
                    rxbuf[0] = current_pid;
                    rx_len = rxd_len;
                    xSemaphoreGive(gLinSemaphoreId);
                }
                rxd_len = 0;
                current_pid = -1;
                break;

            case UART_PARITY_ERR:
                ESP_LOGI(TAG, "uart parity error");
                break;

            // Event of UART frame error
            case UART_FRAME_ERR:
                ESP_LOGI(TAG, "uart frame error");
                break;

            // Others
            default:
                ESP_LOGI(TAG, "uart event type: %d", event.type);
                break;
            }
        }
    }

    vTaskDelete(NULL);
}

static uint8_t calc_lin_checksum(volatile uint8_t *buf, size_t buf_len)
{
    uint16_t checksum = 0;
    for (size_t i = 0; i < buf_len; i++)
    {
        checksum += buf[i];
        if (checksum > 0xFF)
        {
            checksum = (checksum & 0xFF) + 1;
        }
    }
    return ~((uint8_t)checksum);
}

static void desk_task(void *arg)
{
    vSemaphoreCreateBinary(gLinSemaphoreId);

    ESP_LOGI(TAG, "desk task");

    int64_t last_tick = 0;
    uint16_t last_height = 0;
    while (1)
    {
        xSemaphoreTake(gLinSemaphoreId, portMAX_DELAY);

        // Got poked by ISR
        if (rxbuf[0] == 0x80)
        {
            uint16_t pos = rxbuf[1] + (rxbuf[2] << 8);

            if (pos != current_height)
            {
                ESP_LOGI(TAG, "current id: %x, current pos: %u", rxbuf[0], (unsigned int)pos);

                if (current_height == 0)
                {
                    // Publish current height to MQTT.
                    char payload[6];
                    snprintf(payload, sizeof(payload), "%u", pos);
                    esp_mqtt_client_publish(mqttClient, MQTT_HEIGHT_TOPIC, (const char *)payload, strlen(payload), 0, 0);
                }

                current_height = pos;
            }

            if (is_moving)
            {
                if (last_height != current_height)
                {
                    last_height = current_height;
                    last_tick = esp_timer_get_time();
                }
                else if (esp_timer_get_time() >= last_tick + timeout_ticks)
                {
                    movement_blocked = true;
                }
            }

            if (movement_requested)
            {
                if (!is_moving)
                {
                    if (current_height != target_height)
                    {
                        ESP_LOGI(TAG, "movement requested, current: %d, target: %d", current_height, target_height);

                        current_target_height = target_height;
                        txbuf[0] = 0xCA;
                        txbuf[1] = current_target_height & 0xFF;
                        txbuf[2] = (current_target_height >> 8) & 0xFF;
                        txbuf[3] = calc_lin_checksum(txbuf, 3);
                        tx_len = 4;
                        last_tick = esp_timer_get_time();
                        last_height = current_height;
                        movement_blocked = false;
                        is_moving = true;
                    }
                }
                else if (movement_blocked || (current_height == current_target_height))
                {
                    ESP_LOGI(TAG, "reached target");
                    // Made it to target, or motor stopped by itself
                    tx_len = 0;
                    is_moving = false;
                    movement_requested = false;
                    movement_blocked = false;

                    // Publish current height to MQTT.
                    char payload[6];
                    snprintf(payload, sizeof(payload), "%u", current_height);
                    esp_mqtt_client_publish(mqttClient, MQTT_HEIGHT_TOPIC, (const char *)payload, strlen(payload), 0, 0);
                }
                else if (target_height != current_target_height)
                {
                    // Target was updated while in motion, need to handle this by allowing motor to stop and restart movement
                    // TODO
                }
            }
            else
            {
                if (is_moving)
                {
                    // We were moving but got aborted. Report current state and reset.
                    tx_len = 0;
                    is_moving = false;
                    movement_requested = false;
                    movement_blocked = false;

                    // Publish current height to MQTT.
                    char payload[6];
                    snprintf(payload, sizeof(payload), "%u", current_height);
                    esp_mqtt_client_publish(mqttClient, MQTT_HEIGHT_TOPIC, (const char *)payload, strlen(payload), 0, 0);
                }
            }
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_subscribe(client, MQTT_SET_HEIGHT_TOPIC, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        if (strncmp(event->topic, MQTT_SET_HEIGHT_TOPIC, event->topic_len) == 0)
        {
            // Calculate the actual length of the received data
            size_t data_length = event->data_len;
            while (data_length > 0 && event->data[data_length - 1] == '\0')
            {
                data_length--;
            }

            // Convert the received data to uint16_t using strtol
            char received_data_str[data_length + 1];
            memcpy(received_data_str, event->data, data_length);
            received_data_str[data_length] = '\0';

            char *endptr;
            long received_long = strtol(received_data_str, &endptr, 10);

            if (*endptr == '\0' && received_long >= 0 && received_long <= UINT16_MAX)
            {
                target_height = (uint16_t)received_long;
                movement_requested = true;

                // We received a new height, release the semaphore to let the desk task determine what to do.
                // Not sure if this is the best way to do it, but it works.
                xSemaphoreGive(gLinSemaphoreId);
            }
            else
            {
                // Handle conversion error or out-of-range value
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER,
    };

    mqttClient = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqttClient, ESP_EVENT_ANY_ID, mqtt_event_handler, mqttClient);
    esp_mqtt_client_start(mqttClient);
}

void app_main()
{
    // Bring to ground to not interfere with passive matrix scanning.
    // This actually pulls HB04 away from GND.
    gpio_set_direction(WAKE_UP_PIN, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(WAKE_UP_PIN, 0);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    // Install UART driver using the event queue
    uart_driver_install(EX_UART_NUM, BUF_SIZE * 2, 0, 10, &uart0_queue, 0);

    // Configure UART parameters
    uart_config_t uart_config = {
        .baud_rate = 19200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE};
    uart_param_config(EX_UART_NUM, &uart_config);
    ESP_ERROR_CHECK(uart_set_pin(EX_UART_NUM, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // Set the RX full threshold as low as possible while still allowing breaks to be detected.
    // Also set the timeout for interrupts as low as possible.
    // This seems to lower the delay between receiving bits and being able to react on them, allowing us to reply to LIN packets in time.
    uart_set_rx_full_threshold(EX_UART_NUM, 1);
    uart_set_rx_timeout(EX_UART_NUM, 1);
    uart_set_always_rx_timeout(EX_UART_NUM, true);

    uart_enable_rx_intr(EX_UART_NUM);
    uart_disable_tx_intr(EX_UART_NUM);

    // Invert TX since the NPN transistor is left open when connected to ground.
    uart_set_line_inverse(EX_UART_NUM, UART_SIGNAL_TXD_INV);

    // Create UART event task
    xTaskCreate(uart_event_task, "uart_event_task", 2048, NULL, 3, NULL);

    // Create task for handling desk movement logic
    xTaskCreate(desk_task, "desk_task", 2048, NULL, 2, NULL);

    // Start MQTT
    mqtt_app_start();
}