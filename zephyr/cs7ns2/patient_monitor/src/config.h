/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

#ifdef CONFIG_NET_APP_SETTINGS
#ifdef CONFIG_NET_IPV6
#define ZEPHYR_ADDR		CONFIG_NET_APP_MY_IPV6_ADDR
#define SERVER_ADDR		CONFIG_NET_APP_PEER_IPV6_ADDR
#else
#define ZEPHYR_ADDR		CONFIG_NET_APP_MY_IPV4_ADDR
#define SERVER_ADDR		CONFIG_NET_APP_PEER_IPV4_ADDR
#endif
#else
#ifdef CONFIG_NET_IPV6
#define ZEPHYR_ADDR		"2001:db8::1"
#define SERVER_ADDR		"2001:db8::2"
#else
#define ZEPHYR_ADDR		"192.168.1.101"
#define SERVER_ADDR		"192.168.1.10"
#endif
#endif

#ifdef CONFIG_MQTT_LIB_TLS
#define SERVER_PORT		8883
#else
#define SERVER_PORT		1883
#endif

#define APP_NET_INIT_TIMEOUT 10000
#define APP_CONN_TRIES 100
#define APP_CONN_IDLE_TIMEOUT 10000
#define APP_TX_CONN_TRIES 20
#define APP_TX_CONN_WAIT_MSECS 1000
#define APP_TX_RX_TIMEOUT 500
#define APP_SLEEP_MSECS 4000

// sensor simulator config

#define SIMULATOR_STACK_SIZE 2048
#define SIMULATOR_PRIORITY 5

// bed occupancy sensor config
#define BOS_STACK_SIZE 2048
#define BOS_PRIORITY 4
#define BOS_SLEEP_MSECS 4000
#define BOS_PROCESS_TIME 100
#define READING_WHEN_OUT_OF_BED 420

// buzzer config
#define BUZZER_AUTO_DISARM 40
#define BUZZER_REST_TIME 3

#define AIN1		2
#define MAX_SAMPLES 16

#define ADC_REG_BASE			0x40007000
#define ADC_ENABLE_REG     		(ADC_REG_BASE + 0x500)
#define ADC_TASK_START_REG    	(ADC_REG_BASE + 0x0)
#define ADC_TASK_SAMPLE_REG     (ADC_REG_BASE + 0x4)
#define ADC_TASK_STOP_REG     	(ADC_REG_BASE + 0x8)
#define ADC_EVENT_DONE_REG      (ADC_REG_BASE + 0x108)
#define ADC_CH0_PSELP_REG      	(ADC_REG_BASE + 0x510)
#define ADC_CH0_CONFIG_REG      (ADC_REG_BASE + 0x518)
#define ADC_RESULT_PTR_REG		(ADC_REG_BASE + 0x62C)
#define ADC_RESULT_MAX_REG		(ADC_REG_BASE + 0x630)
#define ADC_RESULT_CNT_REG		(ADC_REG_BASE + 0x634)

#define ADC_REFSEL_VDD_4		(1 << 12)
#define ADC_TACQ_40				(5 << 16)

#endif
