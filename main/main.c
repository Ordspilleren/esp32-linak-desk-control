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

#include "stdatomic.h"

static const char *TAG = "LINAK_DESK";

esp_mqtt_client_handle_t mqttClient;

#define ESP_WIFI_SSID CONFIG_WIFI_SSID
#define ESP_WIFI_PASS CONFIG_WIFI_PASSWORD
#define ESP_MAXIMUM_RETRY 10

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;
static bool wifi_connected = false; // Track WiFi connection status

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
        wifi_connected = false; // Update connection status
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
        wifi_connected = true; // Update connection status
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// WiFi monitoring task to handle reconnections
static void wifi_monitor_task(void *pvParameters)
{
    const TickType_t check_interval = pdMS_TO_TICKS(10000); // Check every 10 seconds

    while (1)
    {
        vTaskDelay(check_interval);

        // Check if WiFi is still connected
        wifi_ap_record_t ap_info;
        esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);

        if (ret != ESP_OK)
        {
            // WiFi is not connected
            if (wifi_connected)
            {
                ESP_LOGW(TAG, "WiFi connection lost, attempting to reconnect...");
                wifi_connected = false;
                s_retry_num = 0; // Reset retry counter
                esp_wifi_connect();
            }
        }
        else
        {
            // WiFi is connected
        }
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

    // Start WiFi monitoring task after initial connection attempt
    xTaskCreate(wifi_monitor_task, "wifi_monitor_task", 2048, NULL, 1, NULL);
}

#define EX_UART_NUM UART_NUM_1
#define BUF_SIZE (1024)

// Define the UART event queue handle
static QueueHandle_t uart0_queue;

// Replace gLinSemaphoreId for this purpose, we will use it as a proper mutex
static portMUX_TYPE gLinDataSpinlock = portMUX_INITIALIZER_UNLOCKED;

// The semaphore to wake the desk_task can be separate or you can use task notifications
SemaphoreHandle_t gDeskTaskSemaphore = NULL;

volatile uint8_t rxbuf[10] = {0};
volatile size_t rx_len = 0;
volatile uint8_t txbuf[8] = {0};
volatile size_t tx_len = 0;

volatile uint16_t target_height = 0;
atomic_uint_least16_t current_height = 0;
volatile uint16_t current_target_height = 0;
volatile bool is_moving = false;
volatile bool movement_requested = false;
volatile bool movement_blocked = false;

// PRBS (safety sequence) responses
// The first byte is the actual PRBS number.
// The second byte is the checksum.
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

static size_t prbs_seq = 0;

static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t data;

    static int current_pid = -1;
    static size_t rxd_len = 0;

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
                        // Uncomment the below if using the original circuit from stevew817.
                        // static const uint8_t frame[2] = {0x9A, 0x01};
                        // uart_tx_chars(EX_UART_NUM, (char *)&frame, 2);
                        break;
                    case 0xE7:
                        // Safety sequence -> respond in sequence for as long as there is an active command
                        current_pid = -1; // Discard - do not need to make thread aware of PRBS

                        taskENTER_CRITICAL(&gLinDataSpinlock);
                        if (tx_len > 0)
                        {
                            uart_tx_chars(EX_UART_NUM, (char *)&prbs_sequence[prbs_seq], 2);
                            prbs_seq = (prbs_seq + 1) % 9;
                        }
                        taskEXIT_CRITICAL(&gLinDataSpinlock);
                        break;
                    case 0xA8: // ID40
                        taskENTER_CRITICAL(&gLinDataSpinlock);
                        if (tx_len > 0)
                        {
                            uart_tx_chars(EX_UART_NUM, (char *)&prbs_sequence[prbs_seq], 2);
                            prbs_seq = (prbs_seq + 1) % 9;
                        }
                        taskEXIT_CRITICAL(&gLinDataSpinlock);
                        break;
                    case 0xCA:
                        // Ref1 input -> respond with command if there is one
                        taskENTER_CRITICAL(&gLinDataSpinlock);
                        if (tx_len == 4 && txbuf[0] == 0xCA)
                        {
                            uint8_t frame[3] = {txbuf[1], txbuf[2], txbuf[3]};
                            taskEXIT_CRITICAL(&gLinDataSpinlock);
                            uart_tx_chars(EX_UART_NUM, (char *)&frame, 3);
                        } else {
                            taskEXIT_CRITICAL(&gLinDataSpinlock);
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
                uart_wait_tx_done(EX_UART_NUM, 100);
                uart_flush_input(EX_UART_NUM);

                if (current_pid >= 0)
                {
                    rxbuf[0] = current_pid;
                    rx_len = rxd_len;
                    xSemaphoreGive(gDeskTaskSemaphore);
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

// Define states for clarity
typedef enum
{
    DESK_STATE_IDLE,
    DESK_STATE_MOVING,
    DESK_STATE_STOPPING
} desk_state_t;

static void desk_task(void *arg)
{
    gDeskTaskSemaphore = xSemaphoreCreateBinary();

    if (gDeskTaskSemaphore == NULL)
    {
        ESP_LOGE(TAG, "Failed to create semaphores");
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "desk task started");

    desk_state_t state = DESK_STATE_IDLE;
    int64_t last_move_time = 0;
    uint16_t last_reported_height = 0;
    const int64_t STALL_TIMEOUT_US = 2000000; // 2 seconds, be generous
    const uint16_t HEIGHT_TOLERANCE = 5;      // 0.5mm tolerance

    while (1)
    {
        // Wait for a position update from the LIN bus
        if (xSemaphoreTake(gDeskTaskSemaphore, portMAX_DELAY) == pdTRUE)
        {
            // We only process valid position frames (ID 0x80)
            if (rxbuf[0] != 0x80) continue;

            current_height = rxbuf[1] + (rxbuf[2] << 8);

            // --- State Machine Logic ---
            switch (state)
            {
            case DESK_STATE_IDLE:
                if (movement_requested)
                {
                    if (abs(current_height - target_height) > HEIGHT_TOLERANCE)
                    {
                        ESP_LOGI(TAG, "Starting movement from %d to %d", current_height, target_height);

                        // Power on the desk.
                        // Comment the below if using the original circuit from stevew817.
                        gpio_set_level(WAKE_UP_PIN, 1);
                        gpio_set_level(WAKE_UP_PIN, 0);

                        // Set the move command
                        current_target_height = target_height;

                        taskENTER_CRITICAL(&gLinDataSpinlock);
                        txbuf[0] = 0xCA;
                        txbuf[1] = current_target_height & 0xFF;
                        txbuf[2] = (current_target_height >> 8) & 0xFF;
                        txbuf[3] = calc_lin_checksum((volatile uint8_t*)txbuf, 3);
                        tx_len = 4;
                        taskEXIT_CRITICAL(&gLinDataSpinlock);

                        last_move_time = esp_timer_get_time();
                        last_reported_height = current_height;
                        state = DESK_STATE_MOVING;
                    }
                    else
                    {
                        // Already at target, clear request
                        movement_requested = false;
                    }
                }
                break;

            case DESK_STATE_MOVING:
                // Check for completion
                if (abs(current_height - current_target_height) <= HEIGHT_TOLERANCE)
                {
                    ESP_LOGI(TAG, "Target reached: %d", current_height);
                    state = DESK_STATE_STOPPING;
                }
                // Check for explicit stop request
                else if (!movement_requested)
                {
                    ESP_LOGW(TAG, "Movement cancelled by user");
                    state = DESK_STATE_STOPPING;
                }
                // Check for stall
                else
                {
                    if (current_height != last_reported_height)
                    {
                        // Height has changed, movement is good. Reset stall timer.
                        last_move_time = esp_timer_get_time();
                        last_reported_height = current_height;
                    }
                    else if (esp_timer_get_time() - last_move_time > STALL_TIMEOUT_US)
                    {
                        ESP_LOGE(TAG, "Movement stalled!");
                        state = DESK_STATE_STOPPING;
                    }
                }

                // If we are still moving, ensure tx_len is set. This is redundant but safe.
                if (state == DESK_STATE_MOVING)
                {
                    taskENTER_CRITICAL(&gLinDataSpinlock);
                    tx_len = 4;
                    taskEXIT_CRITICAL(&gLinDataSpinlock);
                }
                break;

            case DESK_STATE_STOPPING:
                // This state is entered to signal that we should stop.
                // The command is cleared here once.
                taskENTER_CRITICAL(&gLinDataSpinlock);
                tx_len = 0;
                taskEXIT_CRITICAL(&gLinDataSpinlock);
                
                movement_requested = false;
                is_moving = false; // Compatibility with your other flags
                ESP_LOGI(TAG, "Movement stopped. Final height: %u", current_height);

                // Publish final height to MQTT
                if (wifi_connected && mqttClient)
                {
                    char payload[6];
                    snprintf(payload, sizeof(payload), "%u", current_height);
                    esp_mqtt_client_publish(mqttClient, MQTT_HEIGHT_TOPIC, (const char *)payload, strlen(payload), 0, 0);
                }

                state = DESK_STATE_IDLE;
                break;
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
        // Only attempt MQTT reconnection if WiFi is still connected
        if (wifi_connected)
        {
            esp_mqtt_client_reconnect(client);
        }
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
                taskENTER_CRITICAL(&gLinDataSpinlock);

                target_height = (uint16_t)received_long;
                movement_requested = true;

                taskEXIT_CRITICAL(&gLinDataSpinlock);

                xSemaphoreGive(gDeskTaskSemaphore);
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
    // Uncomment the below if using the original circuit from stevew817.
    // gpio_set_direction(WAKE_UP_PIN, GPIO_MODE_OUTPUT_OD);
    // gpio_set_level(WAKE_UP_PIN, 0);

    // I am powering my ESP32 from USB, so I use a slightly different circuit with a resistor from base to GND instead of base to HB04.
    // Setting the pin to low powers off the desk entirely, setting it high keeps the desk on.
    // Comment the below if using the original circuit from stevew817.
    gpio_set_direction(WAKE_UP_PIN, GPIO_MODE_OUTPUT);

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