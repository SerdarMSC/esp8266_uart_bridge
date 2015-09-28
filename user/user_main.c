/*
The MIT License (MIT)

Copyright (c) 2015 alu96

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "esp_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"

#include "uart.h"
#include "ringbuf.h"

#include "user_config.h"

#define DBG(str, ...) //printf(str "\n", ##__VA_ARGS__)

int current_sock = -1;

xQueueHandle tx_queue;

xSemaphoreHandle ringbuf_mutex;
ringbuf ringbuf_m, ringbuf_t; // main and temporary ringbuffer

unsigned char rx_buffer[1024];

unsigned char tx_buffer_m[1024 * 4];
unsigned char tx_buffer_t[256];

void uart_rx(int length) {
    // buffer will be filled only if a client is connected
    if (current_sock < 0)
        return;

    int i = 0;
    unsigned char c;
    bool rb_full = false;

    if (xSemaphoreTakeFromISR(ringbuf_mutex, NULL)) {
        while (ringbuf_get(&ringbuf_t, &c) == 0) {
            if (ringbuf_owr(&ringbuf_m, c)) {
                rb_full = true;
            }
        }

        for (i = 0; i < length; i++) {
            if (ringbuf_owr(&ringbuf_m, uart_rx_one_char(UART0))) {
                rb_full = true;
            }
        }
        xSemaphoreGiveFromISR(ringbuf_mutex, NULL);
    } else {
        DBG("uart_rx: failed to take semphr");

        // ringbuf is taken by send task
        // all uart data is copied in temporary ringbuffer that will be copied in main ringbuffer once it is available
        for (i = 0; i < length; i++) {
            if (ringbuf_owr(&ringbuf_t, uart_rx_one_char(UART0))) {
                rb_full = true;
            }
        }
    }

    DBG("uart_rx: %d b", length);

    // notify send task that there is data to be sent
    xQueueSendToBackFromISR(tx_queue, &length, NULL);

    if (rb_full) {
        // not every byte could be put in one of the two ringbuffers
        // we can just print a message about it
        DBG("uart_rx: ringbuff overflow");
    }
}

void read_task(void *pvParameters) {
    int recbytes;

    while (1) {
        while ((recbytes = read(current_sock, rx_buffer, sizeof(rx_buffer))) > 0) {
            int i = 0;
            DBG("net_rx: %d", recbytes);
            while (i < recbytes) {
                uart_tx_one_char(UART0, (uint8) rx_buffer[i++]);
            }
        }

        if (recbytes <= 0) {
            printf("S > read data fail: %d\n", recbytes);

            close(current_sock);
            current_sock = -1;
            break;
        }
    }

    printf("exiting read task\n");
    vTaskDelete(NULL);
}

void write_task(void *pvParameters) {
    int i;
    while (1) {
        if (xQueueReceive(tx_queue, &i, portMAX_DELAY)) {
            if (xSemaphoreTake(ringbuf_mutex, portMAX_DELAY)) {
                int length = ringbuf_m.fill_cnt;
                DBG("net_tx: %d", length);

                unsigned char c;
                for (i = 0; i < length && i < sizeof(tx_buffer_m); i++) {
                    if (ringbuf_get(&ringbuf_m, &c)) {
                        printf("buffer error!\n");
                        break;
                    }
                    tx_buffer_m[i] = c;
                }
                xSemaphoreGive(ringbuf_mutex);
            } else {
                printf("write_task: semphr already taken\n");
            }

            if (write(current_sock, tx_buffer_m, i) < 0) {
                printf("C > send fail\n");

                close(current_sock);
                current_sock = -1;
                break;
            }
        } else {
            printf("tx: failed to receive from queue\n");
        }
    }

    printf("exiting write task\n");
    vTaskDelete(NULL);
}

void listen_task(void *pvParameters) {
    while (1) {
        struct sockaddr_in server_addr, client_addr;
        int server_sock, client_sock;
        socklen_t sin_size;
        bzero(&server_addr, sizeof(struct sockaddr_in));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(8000);

        do {
            if (-1 == (server_sock = socket(AF_INET, SOCK_STREAM, 0))) {
                printf("S > socket error\n");
                break;
            }

            printf("S > create socket: %d\n", server_sock);

            if (-1 == bind(server_sock, (struct sockaddr *) (&server_addr), sizeof(struct sockaddr))) {
                printf("S > bind fail\n");
                break;
            }

            printf("S > bind port: %d\n", ntohs(server_addr.sin_port));

            if (-1 == listen(server_sock, 5)) {
                printf("S > listen fail\n");
                break;
            }

            printf("S > listen ok\n");

            sin_size = sizeof(client_addr);

            while (1) {
                printf("S > wait client\n");

                if ((client_sock = accept(server_sock, (struct sockaddr *) &client_addr, &sin_size)) < 0) {
                    printf("S > accept fail %d\n", client_sock);
                    continue;
                }

                // we allow only one connection
                if (current_sock >= 0) {
#ifdef KEEP_CLIENT
                    close(client_sock);
                    continue;
#else
                    close(current_sock);
                    current_sock = -1;
#endif
                }

                printf("S > Client from %s %d\n", inet_ntoa(client_addr.sin_addr), htons(client_addr.sin_port));

                // reset all ringbuffers that may still contain data from last connection
                ringbuf_truncate(&ringbuf_t);
                ringbuf_truncate(&ringbuf_m);

                current_sock = client_sock;

                xTaskCreate(read_task, "read_task", 256, NULL, 2, NULL);
                xTaskCreate(write_task, "write_task", 256, NULL, 2, NULL);
            }
        } while (0);
    }
}

/******************************************************************************
 * FunctionName : user_init
 * Description  : entry of user application, init user function here
 * Parameters   : none
 * Returns      : none
*******************************************************************************/
void user_init(void) {
    printf("SDK version:%s\n", system_get_sdk_version());

#ifdef DOUBLE_CLK_FREQ
    system_update_cpu_freq(160);
#endif

    struct softap_config ap_conf;

    // make sure the device is in AP mode
    if (wifi_get_opmode() != SOFTAP_MODE) {
        wifi_set_opmode(SOFTAP_MODE);
    }

    // setup the soft AP
    bzero(&ap_conf, sizeof(struct softap_config));

    wifi_softap_get_config(&ap_conf);
    strncpy((char *) ap_conf.ssid, AP_SSID, sizeof(ap_conf.ssid));
    ap_conf.ssid_len = (uint8) strlen(AP_SSID);
    strncpy((char *) ap_conf.password, AP_PASSWORD, sizeof(ap_conf.password));
    ap_conf.authmode = AUTH_WPA_WPA2_PSK;
    ap_conf.channel = CHANNEL;

    wifi_softap_set_config(&ap_conf);

    system_phy_set_max_tpw(TX_POWER); // TX power

    uart_init_new(BAUD, uart_rx);

    // TODO queue is not really needed any more, maybe there is something better to use...
    tx_queue = xQueueCreate(1, sizeof(int));

    ringbuf_mutex = xSemaphoreCreateMutex();

    ringbuf_init(&ringbuf_m, tx_buffer_m, sizeof(tx_buffer_m));
    ringbuf_init(&ringbuf_t, tx_buffer_t, sizeof(tx_buffer_t));

    xTaskCreate(listen_task, "listen_task", 256, NULL, 2, NULL);
}
