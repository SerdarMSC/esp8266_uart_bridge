#ifndef __USER_CONFIG_H__
#define __USER_CONFIG_H__

#define BAUD 115200

#define AP_SSID		"uart_tcp_bridge"
#define AP_PASSWORD	"BridgeTest"

#define TX_POWER 81
#define CHANNEL 6

#define TCP_PORT 9999

/*
 * if KEEP_CLIENT defined:  connection will be kept open until client disconnects by itself
 * otherwise:               when a new client tries to connect, the previously opened connection will be closed
 */
#define KEEP_CLIENT

#define DOUBLE_CLK_FREQ     // increases clock frequency from 80 MHz to 160 Mhz,
                            // might improve performance, but increase power consumption

#endif

