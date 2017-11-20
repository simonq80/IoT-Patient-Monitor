/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <net/mqtt.h>

#include <net/net_context.h>
#include <net/net_mgmt.h>

#include <misc/printk.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "config.h"

/* Container for some structures used by the MQTT publisher app. */
struct mqtt_client_ctx {
	/**
	 * The connect message structure is only used during the connect
	 * stage. Developers must set some msg properties before calling the
	 * mqtt_tx_connect routine. See below.
	 */
	struct mqtt_connect_msg connect_msg;
	/**
	 * This is the message that will be received by the server
	 * (MQTT broker).
	 */
	struct mqtt_publish_msg pub_msg;

	/**
	 * This is the MQTT application context variable.
	 */
	struct mqtt_ctx mqtt_ctx;

	/**
	 * This variable will be passed to the connect callback, declared inside
	 * the mqtt context struct. If not used, it could be set to NULL.
	 */
	void *connect_data;

	/**
	 * This variable will be passed to the disconnect callback, declared
	 * inside the mqtt context struct. If not used, it could be set to NULL.
	 */
	void *disconnect_data;

	/**
	 * This variable will be passed to the publish_tx callback, declared
	 * inside the mqtt context struct. If not used, it could be set to NULL.
	 */
	void *publish_data;
};

/* Callback to tell us when we have a net connection */
#if defined(CONFIG_NET_MGMT_EVENT)
static struct net_mgmt_event_callback cb;
#endif

/* MQTT publisher semaphore */
K_SEM_DEFINE(pub_sem, 0, 2);
K_MUTEX_DEFINE(pub_data);

static bool telemetry_changed=false;
static bool attributes_changed=false;

#define X_STEP 0.05
#define Z_MAX 100
#define PI 3.14159265358979323846
static double x = 0.0;
static double y = 0.0;
static int z = 0;

#define SS_STACK_SIZE 2048
#define SS_PRIORITY 5

K_THREAD_STACK_DEFINE(ss_stack_area, SS_STACK_SIZE);
struct k_thread ss_thread;

void sensors_thread(void * unused1, void * unused2, void * unused3)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	while (true) {
		k_sleep(APP_SLEEP_MSECS);

		k_mutex_lock(&pub_data, K_FOREVER);

		y = sin(x * 2 * PI);

		if ((x = x + X_STEP) >= 1.0)
			x = 0.0;

		if ((z++) > Z_MAX)
			z = 0;

		telemetry_changed = true;

		k_mutex_unlock(&pub_data);
		k_sem_give(&pub_sem);
	}
}


static void start_sensors()
{
	k_tid_t ss_tid = k_thread_create(&ss_thread, ss_stack_area,
								 K_THREAD_STACK_SIZEOF(ss_stack_area),
								 sensors_thread,
								 NULL, NULL, NULL,
								 SS_PRIORITY, 0, K_NO_WAIT);
}


/* The signature of this routine must match the connect callback declared at
 * the mqtt.h header.
 */
static void connect_cb(struct mqtt_ctx *mqtt_ctx)
{
	struct mqtt_client_ctx *client_ctx;

	client_ctx = CONTAINER_OF(mqtt_ctx, struct mqtt_client_ctx, mqtt_ctx);

	printk("[%s:%d]", __func__, __LINE__);

	if (client_ctx->connect_data) {
		printk(" user_data: %s",
		       (const char *)client_ctx->connect_data);
	}

	printk("\n");
}

/* The signature of this routine must match the disconnect callback declared at
 * the mqtt.h header.
 */
static void disconnect_cb(struct mqtt_ctx *mqtt_ctx)
{
	struct mqtt_client_ctx *client_ctx;

	client_ctx = CONTAINER_OF(mqtt_ctx, struct mqtt_client_ctx, mqtt_ctx);

	printk("[%s:%d]", __func__, __LINE__);

	if (client_ctx->disconnect_data) {
		printk(" user_data: %s",
		       (const char *)client_ctx->disconnect_data);
	}

	printk("\n");
}

/**
 * The signature of this routine must match the publish_tx callback declared at
 * the mqtt.h header.
 *
 * NOTE: we have two callbacks for MQTT Publish related stuff:
 *	- publish_tx, for publishers
 *	- publish_rx, for subscribers
 *
 * Applications must keep a "message database" with pkt_id's. So far, this is
 * not implemented here. For example, if we receive a PUBREC message with an
 * unknown pkt_id, this routine must return an error, for example -EINVAL or
 * any negative value.
 */
static int publish_cb(struct mqtt_ctx *mqtt_ctx, u16_t pkt_id,
		      enum mqtt_packet type)
{
	struct mqtt_client_ctx *client_ctx;
	const char *str;
	int rc = 0;

	client_ctx = CONTAINER_OF(mqtt_ctx, struct mqtt_client_ctx, mqtt_ctx);

	switch (type) {
	case MQTT_PUBACK:
		str = "MQTT_PUBACK";
		break;
	case MQTT_PUBCOMP:
		str = "MQTT_PUBCOMP";
		break;
	case MQTT_PUBREC:
		str = "MQTT_PUBREC";
		break;
	default:
		rc = -EINVAL;
		str = "Invalid MQTT packet";
	}

	printk("[%s:%d] <%s> packet id: %u", __func__, __LINE__, str, pkt_id);

	if (client_ctx->publish_data) {
		printk(", user_data: %s",
		       (const char *)client_ctx->publish_data);
	}

	printk("\n");

	return rc;
}

/**
 * The signature of this routine must match the malformed callback declared at
 * the mqtt.h header.
 */
static void malformed_cb(struct mqtt_ctx *mqtt_ctx, u16_t pkt_type)
{
	printk("[%s:%d] pkt_type: %u\n", __func__, __LINE__, pkt_type);
}

static char *get_attributes_payload(enum mqtt_qos qos)
{
	static char payload[128];
	snprintf(payload, sizeof(payload), "{\"firmware_version\":\"%s\", \"serial_number\":\"%s\", \"uptime\":\"%d\"}",
		"1.2.3",
		"jdukes-001",
		(uint32_t)k_uptime_get_32() / 1000);
	return payload;
}


static char *get_telemetry_payload(enum mqtt_qos qos)
{
	static char payload[128];
	snprintf(payload, sizeof(payload), "{\"testval1\":\"%1.4f\", \"testval2\":\"%d\"}", y, z);
	return payload;
}

static void prepare_attributes_msg(struct mqtt_publish_msg *pub_msg,
				     enum mqtt_qos qos)
{
	/* MQTT message payload may be anything, we we use C strings */
	pub_msg->msg = get_attributes_payload(qos);
	/* Payload's length */
	pub_msg->msg_len = strlen(pub_msg->msg);
	/* MQTT Quality of Service */
	pub_msg->qos = qos;
	/* Message's topic */
	pub_msg->topic = "v1/devices/me/attributes";
	pub_msg->topic_len = strlen(pub_msg->topic);
	/* Packet Identifier, always use different values */
	pub_msg->pkt_id = sys_rand32_get();
}

static void prepare_telemetry_msg(struct mqtt_publish_msg *pub_msg,
				     enum mqtt_qos qos)
{
	/* MQTT message payload may be anything, we we use C strings */
	pub_msg->msg = get_telemetry_payload(qos);
	/* Payload's length */
	pub_msg->msg_len = strlen(pub_msg->msg);
	/* MQTT Quality of Service */
	pub_msg->qos = qos;
	/* Message's topic */
	pub_msg->topic = "v1/devices/me/telemetry";
	pub_msg->topic_len = strlen(pub_msg->topic);
	/* Packet Identifier, always use different values */
	pub_msg->pkt_id = sys_rand32_get();
}

#define RC_STR(rc)	((rc) == 0 ? "OK" : "ERROR")

#define PRINT_RESULT(func, rc)	\
	printk("[%s:%d] %s: %d <%s>\n", __func__, __LINE__, \
	       (func), rc, RC_STR(rc))

#define PUB_STACK_SIZE 2048
#define PUB_PRIORITY 5

K_THREAD_STACK_DEFINE(pub_stack_area, PUB_STACK_SIZE);
struct k_thread pub_thread;
static struct mqtt_client_ctx pub_ctx;

void publisher_thread(void * unused1, void * unused2, void * unused3)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	int i, rc;
	bool data_changed;

	/* Set everything to 0 and later just assign the required fields. */
	memset(&pub_ctx, 0x00, sizeof(pub_ctx));

	/* connect, disconnect and malformed may be set to NULL */
	pub_ctx.mqtt_ctx.connect = connect_cb;

	pub_ctx.mqtt_ctx.disconnect = disconnect_cb;
	pub_ctx.mqtt_ctx.malformed = malformed_cb;

	pub_ctx.mqtt_ctx.net_init_timeout = APP_NET_INIT_TIMEOUT;
	pub_ctx.mqtt_ctx.net_timeout = APP_TX_RX_TIMEOUT;

	pub_ctx.mqtt_ctx.peer_addr_str = SERVER_ADDR;
	pub_ctx.mqtt_ctx.peer_port = SERVER_PORT;

	/* Publisher apps TX the MQTT PUBLISH msg */
	pub_ctx.mqtt_ctx.publish_tx = publish_cb;

	/* The connect message will be sent to the MQTT server (broker).
	 * If clean_session here is 0, the mqtt_ctx clean_session variable
	 * will be set to 0 also. Please don't do that, set always to 1.
	 * Clean session = 0 is not yet supported.
	 */
	pub_ctx.connect_msg.user_name = TB_ACCESS_TOKEN;
	pub_ctx.connect_msg.user_name_len = strlen(TB_ACCESS_TOKEN);
	pub_ctx.connect_msg.clean_session = 1;

	pub_ctx.connect_data = "CONNECTED";
	pub_ctx.disconnect_data = "DISCONNECTED";
	pub_ctx.publish_data = "PUBLISH";

	while ((rc = k_sem_take(&pub_sem, K_FOREVER)) == 0) {

		rc = mqtt_init(&pub_ctx.mqtt_ctx, MQTT_APP_PUBLISHER);
		PRINT_RESULT("mqtt_init", rc);

		if (rc != 0) {
			goto exit_pub;
		}

		i = 0;
		do {
			rc = mqtt_connect(&pub_ctx.mqtt_ctx);
			PRINT_RESULT("mqtt_connect", rc);
		} while (rc != 0 && i++ < APP_CONN_TRIES);

		if (rc != 0) {
			goto exit_pub;
		}

		i = 0;
		do {
			rc = mqtt_tx_connect(&pub_ctx.mqtt_ctx, &pub_ctx.connect_msg);
			PRINT_RESULT("mqtt_tx_connect", rc);
			k_sleep(APP_TX_CONN_WAIT_MSECS);
		} while (rc == 0 && i++ < APP_TX_CONN_TRIES && !pub_ctx.mqtt_ctx.connected);

		if (!pub_ctx.mqtt_ctx.connected) {
			mqtt_close(&pub_ctx.mqtt_ctx);
			goto exit_pub;
		}

		do {
			bool data_changed = false;
			k_mutex_lock(&pub_data, K_FOREVER);

			if (telemetry_changed) {
				prepare_telemetry_msg(&pub_ctx.pub_msg, MQTT_QoS0);
				telemetry_changed=false;
				data_changed = true;
			}
			else if (attributes_changed) {
				prepare_attributes_msg(&pub_ctx.pub_msg, MQTT_QoS0);
				attributes_changed = false;
				data_changed = true;
			}

			k_mutex_unlock(&pub_data);

			if (data_changed) {
				rc = mqtt_tx_publish(&pub_ctx.mqtt_ctx, &pub_ctx.pub_msg);
				PRINT_RESULT("mqtt_tx_publish", rc);
			}
		} while ((rc = k_sem_take(&pub_sem, APP_CONN_IDLE_TIMEOUT)) == 0);

		rc = mqtt_tx_disconnect(&pub_ctx.mqtt_ctx);
		PRINT_RESULT("mqtt_tx_disconnect", rc);

		k_sleep(APP_TX_RX_TIMEOUT);

		rc = mqtt_close(&pub_ctx.mqtt_ctx);
		PRINT_RESULT("mqtt_close", rc);

		k_sleep(APP_TX_RX_TIMEOUT);
	}


exit_pub:

	printk("\nPublisher terminated!!\n");
}


static void start_publisher()
{
	k_tid_t tt_tid = k_thread_create(&pub_thread, pub_stack_area,
                                 K_THREAD_STACK_SIZEOF(pub_stack_area),
                                 publisher_thread,
                                 NULL, NULL, NULL,
                                 PUB_PRIORITY, 0, K_NO_WAIT);
}


static void event_iface_up(struct net_mgmt_event_callback *cb,
			   u32_t mgmt_event, struct net_if *iface)
{
	start_publisher();
	start_sensors();
}


void main(void)
{
	struct net_if *iface = net_if_get_default();

#if defined(CONFIG_NET_MGMT_EVENT)
	/* Subscribe to NET_IF_UP if interface is not ready */
	if (!atomic_test_bit(iface->flags, NET_IF_UP)) {
		net_mgmt_init_event_callback(&cb, event_iface_up, NET_EVENT_IF_UP);
		net_mgmt_add_event_callback(&cb);
	}
#else
	event_iface_up(NULL, NET_EVENT_IF_UP, iface);
#endif

	while (true) {
		k_mutex_lock(&pub_data, K_FOREVER);
		attributes_changed = true;
		k_mutex_unlock(&pub_data);
		k_sem_give(&pub_sem);
		k_sleep(10000);
	}

	return;
}
