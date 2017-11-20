/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Uses some original concepts by:
 *         Joakim Eriksson <joakime@sics.se>
 *         Niclas Finne <nfi@sics.se>
 *         Joel Hoglund <joel@sics.se>
 */

/*
 * TODO:
 *
 * - Use server / security object instance 0 for initial connection
 * - Add DNS support for security uri parsing
 * - BOOTSTRAP/DTLS cleanup
 * - Handle WRITE_ATTRIBUTES (pmin=10&pmax=60)
 * - Handle Resource ObjLink type
 */

#define SYS_LOG_DOMAIN "lib/lwm2m_engine"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_LWM2M_LEVEL
#include <logging/sys_log.h>

#include <zephyr/types.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <init.h>
#include <misc/printk.h>
#include <net/net_app.h>
#include <net/net_ip.h>
#include <net/net_pkt.h>
#include <net/udp.h>
#include <net/zoap.h>
#include <net/lwm2m.h>

#include "lwm2m_object.h"
#include "lwm2m_engine.h"
#include "lwm2m_rw_plain_text.h"
#include "lwm2m_rw_oma_tlv.h"
#ifdef CONFIG_LWM2M_RW_JSON_SUPPORT
#include "lwm2m_rw_json.h"
#endif
#ifdef CONFIG_LWM2M_RD_CLIENT_SUPPORT
#include "lwm2m_rd_client.h"
#endif

#define ENGINE_UPDATE_INTERVAL 500

#define DISCOVER_PREFACE	"</.well-known/core>;ct=40"

/*
 * TODO: to implement a way for clients to specify alternate path
 * via Kconfig (LwM2M specification 8.2.2 Alternate Path)
 *
 * For now, in order to inform server we support JSON format, we have to
 * report 'ct=11543' to the server. '</>' is required in order to append
 * content attribute. And resource type attribute is appended because of
 * Eclipse wakaama will reject the registration when 'rt="oma.lwm2m"' is
 * missing.
 */

#define RESOURCE_TYPE		";rt=\"oma.lwm2m\""

#if defined(CONFIG_LWM2M_RW_JSON_SUPPORT)
#define REG_PREFACE		"</>" RESOURCE_TYPE \
				";ct=" STRINGIFY(LWM2M_FORMAT_OMA_JSON)
#else
#define REG_PREFACE		""
#endif

#define BUF_ALLOC_TIMEOUT K_SECONDS(1)

#define MAX_TOKEN_LEN		8

struct observe_node {
	sys_snode_t node;
	struct lwm2m_ctx *ctx;
	struct lwm2m_obj_path path;
	u8_t  token[MAX_TOKEN_LEN];
	s64_t event_timestamp;
	s64_t last_timestamp;
	u32_t min_period_sec;
	u32_t max_period_sec;
	u32_t counter;
	u16_t format;
	bool used;
	u8_t  tkl;
};

static struct observe_node observe_node_data[CONFIG_LWM2M_ENGINE_MAX_OBSERVER];

static sys_slist_t engine_obj_list;
static sys_slist_t engine_obj_inst_list;
static sys_slist_t engine_observer_list;

#define NUM_BLOCK1_CONTEXT	CONFIG_LWM2M_NUM_BLOCK1_CONTEXT

/* TODO: figure out what's correct value */
#define TIMEOUT_BLOCKWISE_TRANSFER K_SECONDS(30)

#define GET_BLOCK_NUM(v)	((v) >> 4)
#define GET_BLOCK_SIZE(v)	(((v) & 0x7))
#define GET_MORE(v)		(!!((v) & 0x08))

struct block_context {
	struct zoap_block_context ctx;
	s64_t timestamp;
	u8_t token[8];
	u8_t tkl;
};

static struct block_context block1_contexts[NUM_BLOCK1_CONTEXT];

/* periodic / notify / observe handling stack */
static K_THREAD_STACK_DEFINE(engine_thread_stack,
			     CONFIG_LWM2M_ENGINE_STACK_SIZE);
static struct k_thread engine_thread_data;

static struct lwm2m_engine_obj *get_engine_obj(int obj_id);
static struct lwm2m_engine_obj_inst *get_engine_obj_inst(int obj_id,
							 int obj_inst_id);

/* Shared set of in-flight LwM2M messages */
static struct lwm2m_message messages[CONFIG_LWM2M_ENGINE_MAX_MESSAGES];

/* for debugging: to print IP addresses */
char *lwm2m_sprint_ip_addr(const struct sockaddr *addr)
{
	static char buf[NET_IPV6_ADDR_LEN];

#if defined(CONFIG_NET_IPV6)
	if (addr->sa_family == AF_INET6) {
		return net_addr_ntop(AF_INET6, &net_sin6(addr)->sin6_addr,
				     buf, sizeof(buf));
	}
#endif
#if defined(CONFIG_NET_IPV4)
	if (addr->sa_family == AF_INET) {
		return net_addr_ntop(AF_INET, &net_sin(addr)->sin_addr,
				     buf, sizeof(buf));
	}
#endif

	SYS_LOG_ERR("Unknown IP address family:%d", addr->sa_family);
	return NULL;
}

#if CONFIG_SYS_LOG_LWM2M_LEVEL > 3
static char *sprint_token(const u8_t *token, u8_t tkl)
{
	static char buf[32];
	int pos = 0;

	if (token && tkl != LWM2M_MSG_TOKEN_LEN_SKIP) {
		int i;

		for (i = 0; i < tkl; i++) {
			pos += snprintf(&buf[pos], 31 - pos, "%x", token[i]);
		}

		buf[pos] = '\0';
	} else if (tkl == LWM2M_MSG_TOKEN_LEN_SKIP) {
		strcpy(buf, "[skip-token]");
	} else {
		strcpy(buf, "[no-token]");
	}

	return buf;
}
#endif

/* block-wise transfer functions */

enum zoap_block_size lwm2m_default_block_size(void)
{
	switch (CONFIG_LWM2M_COAP_BLOCK_SIZE) {
	case 16:
		return ZOAP_BLOCK_16;
	case 32:
		return ZOAP_BLOCK_32;
	case 64:
		return ZOAP_BLOCK_64;
	case 128:
		return ZOAP_BLOCK_128;
	case 256:
		return ZOAP_BLOCK_256;
	case 512:
		return ZOAP_BLOCK_512;
	case 1024:
		return ZOAP_BLOCK_1024;
	}

	return ZOAP_BLOCK_256;
}

static int
init_block_ctx(const u8_t *token, u8_t tkl, struct block_context **ctx)
{
	int i;
	s64_t timestamp;

	*ctx = NULL;
	timestamp = k_uptime_get();
	for (i = 0; i < NUM_BLOCK1_CONTEXT; i++) {
		if (block1_contexts[i].tkl == 0) {
			*ctx = &block1_contexts[i];
			break;
		}

		if (timestamp - block1_contexts[i].timestamp >
		    TIMEOUT_BLOCKWISE_TRANSFER) {
			*ctx = &block1_contexts[i];
			/* TODO: notify application for block
			 * transfer timeout
			 */
			break;
		}
	}

	if (*ctx == NULL) {
		SYS_LOG_ERR("Cannot find free block context");
		return -ENOMEM;
	}

	(*ctx)->tkl = tkl;
	memcpy((*ctx)->token, token, tkl);
	zoap_block_transfer_init(&(*ctx)->ctx, lwm2m_default_block_size(), 0);
	(*ctx)->timestamp = timestamp;

	return 0;
}

static int
get_block_ctx(const u8_t *token, u8_t tkl, struct block_context **ctx)
{
	int i;

	*ctx = NULL;

	for (i = 0; i < NUM_BLOCK1_CONTEXT; i++) {
		if (block1_contexts[i].tkl == tkl &&
		    memcmp(token, block1_contexts[i].token, tkl) == 0) {
			*ctx = &block1_contexts[i];
			/* refresh timestmap */
			(*ctx)->timestamp = k_uptime_get();
			break;
		}
	}

	if (*ctx == NULL) {
		SYS_LOG_ERR("Cannot find block context");
		return -ENOENT;
	}

	return 0;
}

static void free_block_ctx(struct block_context *ctx)
{
	if (ctx == NULL) {
		return;
	}

	ctx->tkl = 0;
}

/* observer functions */

int lwm2m_notify_observer(u16_t obj_id, u16_t obj_inst_id, u16_t res_id)
{
	struct observe_node *obs;
	int ret = 0;

	/* look for observers which match our resource */
	SYS_SLIST_FOR_EACH_CONTAINER(&engine_observer_list, obs, node) {
		if (obs->path.obj_id == obj_id &&
		    obs->path.obj_inst_id == obj_inst_id &&
		    (obs->path.level < 3 ||
		     obs->path.res_id == res_id)) {
			/* update the event time for this observer */
			obs->event_timestamp = k_uptime_get();

			SYS_LOG_DBG("NOTIFY EVENT %u/%u/%u",
				    obj_id, obj_inst_id, res_id);

			ret++;
		}
	}

	return ret;
}

int lwm2m_notify_observer_path(struct lwm2m_obj_path *path)
{
	return lwm2m_notify_observer(path->obj_id, path->obj_inst_id,
				     path->res_id);
}

static int engine_add_observer(struct lwm2m_message *msg,
			       const u8_t *token, u8_t tkl,
			       struct lwm2m_obj_path *path,
			       u16_t format)
{
	struct lwm2m_engine_obj_inst *obj_inst = NULL;
	struct observe_node *obs;
	struct sockaddr *addr;
	int i;

	if (!msg || !msg->ctx) {
		SYS_LOG_ERR("valid lwm2m message is required");
		return -EINVAL;
	}

	if (!token || (tkl == 0 || tkl > MAX_TOKEN_LEN)) {
		SYS_LOG_ERR("token(%p) and token length(%u) must be valid.",
			    token, tkl);
		return -EINVAL;
	}

	/* remote addr */
	addr = &msg->ctx->net_app_ctx.default_ctx->remote;

	/* check if object exists */
	if (!get_engine_obj(path->obj_id)) {
		SYS_LOG_ERR("unable to find obj: %u", path->obj_id);
		return -ENOENT;
	}

	/* check if object instance exists */
	if (path->level >= 2) {
		obj_inst = get_engine_obj_inst(path->obj_id,
					       path->obj_inst_id);
		if (!obj_inst) {
			SYS_LOG_ERR("unable to find obj_inst: %u/%u",
				    path->obj_id, path->obj_inst_id);
			return -ENOENT;
		}
	}

	/* check if resource exists */
	if (path->level >= 3) {
		for (i = 0; i < obj_inst->resource_count; i++) {
			if (obj_inst->resources[i].res_id == path->res_id) {
				break;
			}
		}

		if (i == obj_inst->resource_count) {
			SYS_LOG_ERR("unable to find res_id: %u/%u/%u",
				    path->obj_id, path->obj_inst_id,
				    path->res_id);
			return -ENOENT;
		}
	}

	/* make sure this observer doesn't exist already */
	SYS_SLIST_FOR_EACH_CONTAINER(&engine_observer_list, obs, node) {
		if (obs->ctx == msg->ctx &&
		    memcmp(&obs->path, path, sizeof(*path)) == 0) {
			/* quietly update the token information */
			memcpy(obs->token, token, tkl);
			obs->tkl = tkl;

			SYS_LOG_DBG("OBSERVER DUPLICATE %u/%u/%u(%u) [%s]",
				    path->obj_id, path->obj_inst_id,
				    path->res_id, path->level,
				    lwm2m_sprint_ip_addr(addr));

			return 0;
		}
	}

	/* find an unused observer index node */
	for (i = 0; i < CONFIG_LWM2M_ENGINE_MAX_OBSERVER; i++) {
		if (!observe_node_data[i].used) {
			break;
		}
	}

	/* couldn't find an index */
	if (i == CONFIG_LWM2M_ENGINE_MAX_OBSERVER) {
		return -ENOMEM;
	}

	/* copy the values and add it to the list */
	observe_node_data[i].used = true;
	observe_node_data[i].ctx = msg->ctx;
	memcpy(&observe_node_data[i].path, path, sizeof(*path));
	memcpy(observe_node_data[i].token, token, tkl);
	observe_node_data[i].tkl = tkl;
	observe_node_data[i].last_timestamp = k_uptime_get();
	observe_node_data[i].event_timestamp =
			observe_node_data[i].last_timestamp;
	/* TODO: use server object instance or WRITE_ATTR values */
	observe_node_data[i].min_period_sec = 10;
	observe_node_data[i].max_period_sec = 60;
	observe_node_data[i].format = format;
	observe_node_data[i].counter = 1;
	sys_slist_append(&engine_observer_list,
			 &observe_node_data[i].node);

	SYS_LOG_DBG("OBSERVER ADDED %u/%u/%u(%u) token:'%s' addr:%s",
		    path->obj_id, path->obj_inst_id, path->res_id, path->level,
		    sprint_token(token, tkl), lwm2m_sprint_ip_addr(addr));

	return 0;
}

static int engine_remove_observer(const u8_t *token, u8_t tkl)
{
	struct observe_node *obs, *found_obj = NULL;
	sys_snode_t *prev_node = NULL;

	if (!token || (tkl == 0 || tkl > MAX_TOKEN_LEN)) {
		SYS_LOG_ERR("token(%p) and token length(%u) must be valid.",
			    token, tkl);
		return -EINVAL;
	}

	/* find the node index */
	SYS_SLIST_FOR_EACH_CONTAINER(&engine_observer_list, obs, node) {
		if (memcmp(obs->token, token, tkl) == 0) {
			found_obj = obs;
			break;
		}

		prev_node = &obs->node;
	}

	if (!found_obj) {
		return -ENOENT;
	}

	sys_slist_remove(&engine_observer_list, prev_node, &found_obj->node);
	memset(found_obj, 0, sizeof(*found_obj));

	SYS_LOG_DBG("observer '%s' removed", sprint_token(token, tkl));

	return 0;
}

static void engine_remove_observer_by_id(u16_t obj_id, s32_t obj_inst_id)
{
	struct observe_node *obs, *tmp;
	sys_snode_t *prev_node = NULL;

	/* remove observer instances accordingly */
	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(
			&engine_observer_list, obs, tmp, node) {
		if (!(obj_id == obs->path.obj_id &&
		      (obj_inst_id < 0 ||
		       obj_inst_id == obs->path.obj_inst_id))) {
			prev_node = &obs->node;
			continue;
		}

		sys_slist_remove(&engine_observer_list, prev_node, &obs->node);
		memset(obs, 0, sizeof(*obs));
	}
}

/* engine object */

void lwm2m_register_obj(struct lwm2m_engine_obj *obj)
{
	sys_slist_append(&engine_obj_list, &obj->node);
}

void lwm2m_unregister_obj(struct lwm2m_engine_obj *obj)
{
	engine_remove_observer_by_id(obj->obj_id, -1);
	sys_slist_find_and_remove(&engine_obj_list, &obj->node);
}

static struct lwm2m_engine_obj *get_engine_obj(int obj_id)
{
	struct lwm2m_engine_obj *obj;

	SYS_SLIST_FOR_EACH_CONTAINER(&engine_obj_list, obj, node) {
		if (obj->obj_id == obj_id) {
			return obj;
		}
	}

	return NULL;
}

struct lwm2m_engine_obj_field *
lwm2m_get_engine_obj_field(struct lwm2m_engine_obj *obj, int res_id)
{
	int i;

	if (obj && obj->fields && obj->field_count > 0) {
		for (i = 0; i < obj->field_count; i++) {
			if (obj->fields[i].res_id == res_id) {
				return &obj->fields[i];
			}
		}
	}

	return NULL;
}

/* engine object instance */

static void engine_register_obj_inst(struct lwm2m_engine_obj_inst *obj_inst)
{
	sys_slist_append(&engine_obj_inst_list, &obj_inst->node);
}

static void engine_unregister_obj_inst(struct lwm2m_engine_obj_inst *obj_inst)
{
	engine_remove_observer_by_id(
			obj_inst->obj->obj_id, obj_inst->obj_inst_id);
	sys_slist_find_and_remove(&engine_obj_inst_list, &obj_inst->node);
}

static struct lwm2m_engine_obj_inst *get_engine_obj_inst(int obj_id,
							 int obj_inst_id)
{
	struct lwm2m_engine_obj_inst *obj_inst;

	SYS_SLIST_FOR_EACH_CONTAINER(&engine_obj_inst_list, obj_inst,
				     node) {
		if (obj_inst->obj->obj_id == obj_id &&
		    obj_inst->obj_inst_id == obj_inst_id) {
			return obj_inst;
		}
	}

	return NULL;
}

static struct lwm2m_engine_obj_inst *
next_engine_obj_inst(struct lwm2m_engine_obj_inst *last,
		     int obj_id, int obj_inst_id)
{
	while (last) {
		last = SYS_SLIST_PEEK_NEXT_CONTAINER(last, node);
		if (last && last->obj->obj_id == obj_id &&
		    last->obj_inst_id == obj_inst_id) {
			return last;
		}
	}

	return NULL;
}

int lwm2m_create_obj_inst(u16_t obj_id, u16_t obj_inst_id,
			  struct lwm2m_engine_obj_inst **obj_inst)
{
	int i;
	struct lwm2m_engine_obj *obj;

	*obj_inst = NULL;
	obj = get_engine_obj(obj_id);
	if (!obj) {
		SYS_LOG_ERR("unable to find obj: %u", obj_id);
		return -ENOENT;
	}

	if (!obj->create_cb) {
		SYS_LOG_ERR("obj %u has no create_cb", obj_id);
		return -EINVAL;
	}

	if (obj->instance_count + 1 > obj->max_instance_count) {
		SYS_LOG_ERR("no more instances available for obj %u", obj_id);
		return -ENOMEM;
	}

	*obj_inst = obj->create_cb(obj_inst_id);
	if (!*obj_inst) {
		SYS_LOG_ERR("unable to create obj %u instance %u",
			    obj_id, obj_inst_id);
		/*
		 * Already checked for instance count total.
		 * This can only be an error if the object instance exists.
		 */
		return -EEXIST;
	}

	obj->instance_count++;
	(*obj_inst)->obj = obj;
	(*obj_inst)->obj_inst_id = obj_inst_id;
	sprintf((*obj_inst)->path, "%u/%u", obj_id, obj_inst_id);
	for (i = 0; i < (*obj_inst)->resource_count; i++) {
		sprintf((*obj_inst)->resources[i].path, "%u/%u/%u",
			obj_id, obj_inst_id, (*obj_inst)->resources[i].res_id);
	}

	engine_register_obj_inst(*obj_inst);
#ifdef CONFIG_LWM2M_RD_CLIENT_SUPPORT
	engine_trigger_update();
#endif
	return 0;
}

int lwm2m_delete_obj_inst(u16_t obj_id, u16_t obj_inst_id)
{
	int i, ret = 0;
	struct lwm2m_engine_obj *obj;
	struct lwm2m_engine_obj_inst *obj_inst;

	obj = get_engine_obj(obj_id);
	if (!obj) {
		return -ENOENT;
	}

	obj_inst = get_engine_obj_inst(obj_id, obj_inst_id);
	if (!obj_inst) {
		return -ENOENT;
	}

	engine_unregister_obj_inst(obj_inst);
	obj->instance_count--;

	if (obj->delete_cb) {
		ret = obj->delete_cb(obj_inst_id);
	}

	/* reset obj_inst and res_inst data structure */
	for (i = 0; i < obj_inst->resource_count; i++) {
		memset(obj_inst->resources + i, 0,
		       sizeof(struct lwm2m_engine_res_inst));
	}

	memset(obj_inst, 0, sizeof(struct lwm2m_engine_obj_inst));
#ifdef CONFIG_LWM2M_RD_CLIENT_SUPPORT
	engine_trigger_update();
#endif
	return ret;
}

/* utility functions */

static int get_option_int(const struct zoap_packet *zpkt, u8_t opt)
{
	struct zoap_option option = {};
	u16_t count = 1;
	int r;

	r = zoap_find_options(zpkt, opt, &option, count);
	if (r <= 0) {
		return -ENOENT;
	}

	return zoap_option_value_to_int(&option);
}

static void engine_clear_context(struct lwm2m_engine_context *context)
{
	if (context->in) {
		memset(context->in, 0, sizeof(struct lwm2m_input_context));
	}

	if (context->out) {
		memset(context->out, 0, sizeof(struct lwm2m_output_context));
	}

	if (context->path) {
		memset(context->path, 0, sizeof(struct lwm2m_obj_path));
	}

	context->operation = 0;
}

static u16_t atou16(u8_t *buf, u16_t buflen, u16_t *len)
{
	u16_t val = 0;
	u16_t pos = 0;

	/* we should get a value first - consume all numbers */
	while (pos < buflen && isdigit(buf[pos])) {
		val = val * 10 + (buf[pos] - '0');
		pos++;
	}

	*len = pos;
	return val;
}

static int zoap_options_to_path(struct zoap_option *opt, int options_count,
				 struct lwm2m_obj_path *path)
{
	u16_t len;

	path->level = options_count;
	path->obj_id = atou16(opt[0].value, opt[0].len, &len);
	if (len == 0 || opt[0].len != len) {
		path->level = 0;
	}

	if (path->level > 1) {
		path->obj_inst_id = atou16(opt[1].value, opt[1].len, &len);
		if (len == 0 || opt[1].len != len) {
			path->level = 1;
		}
	}

	if (path->level > 2) {
		path->res_id = atou16(opt[2].value, opt[2].len, &len);
		if (len == 0 || opt[2].len != len) {
			path->level = 2;
		}
	}

	if (path->level > 3) {
		path->res_inst_id = atou16(opt[3].value, opt[3].len, &len);
		if (len == 0 || opt[3].len != len) {
			path->level = 3;
		}
	}

	return options_count == path->level ? 0 : -EINVAL;
}

static struct lwm2m_message *find_msg(struct zoap_pending *pending,
				      struct zoap_reply *reply)
{
	size_t i;

	if (!pending && !reply) {
		return NULL;
	}

	for (i = 0; i < CONFIG_LWM2M_ENGINE_MAX_MESSAGES; i++) {
		if (messages[i].ctx && messages[i].pending == pending) {
			return &messages[i];
		}

		if (messages[i].ctx && messages[i].reply == reply) {
			return &messages[i];
		}
	}

	return NULL;
}

struct lwm2m_message *lwm2m_get_message(struct lwm2m_ctx *client_ctx)
{
	size_t i;

	for (i = 0; i < CONFIG_LWM2M_ENGINE_MAX_MESSAGES; i++) {
		if (!messages[i].ctx) {
			messages[i].ctx = client_ctx;
			return &messages[i];
		}
	}

	return NULL;
}

void lwm2m_release_message(struct lwm2m_message *msg)
{
	if (!msg) {
		return;
	}

	if (msg->pending) {
		zoap_pending_clear(msg->pending);
	}

	if (msg->reply) {
		/* make sure we want to clear the reply */
		zoap_reply_clear(msg->reply);
	}

	memset(msg, 0, sizeof(*msg));
}

int lwm2m_init_message(struct lwm2m_message *msg)
{
	struct net_pkt *pkt;
	struct net_app_ctx *app_ctx;
	struct net_buf *frag;
	int r;

	if (!msg || !msg->ctx) {
		SYS_LOG_ERR("LwM2M message is invalid.");
		return -EINVAL;
	}

	app_ctx = &msg->ctx->net_app_ctx;
	pkt = net_app_get_net_pkt(app_ctx, AF_UNSPEC, BUF_ALLOC_TIMEOUT);
	if (!pkt) {
		SYS_LOG_ERR("Unable to get TX packet, not enough memory.");
		return -ENOMEM;
	}

	frag = net_app_get_net_buf(app_ctx, pkt, BUF_ALLOC_TIMEOUT);
	if (!frag) {
		SYS_LOG_ERR("Unable to get DATA buffer, not enough memory.");
		r = -ENOMEM;
		goto cleanup;
	}

	r = zoap_packet_init(&msg->zpkt, pkt);
	if (r < 0) {
		SYS_LOG_ERR("zoap packet init error (err:%d)", r);
		goto cleanup;
	}

	/* FIXME: Could be that zoap_packet_init() sets some defaults */
	zoap_header_set_version(&msg->zpkt, 1);
	zoap_header_set_type(&msg->zpkt, msg->type);
	zoap_header_set_code(&msg->zpkt, msg->code);

	if (msg->mid > 0) {
		zoap_header_set_id(&msg->zpkt, msg->mid);
	} else {
		zoap_header_set_id(&msg->zpkt, zoap_next_id());
	}

	/*
	 * tkl == 0 is for a new TOKEN
	 * tkl == LWM2M_MSG_TOKEN_LEN_SKIP means dont set
	 */
	if (msg->tkl == 0) {
		zoap_header_set_token(&msg->zpkt, zoap_next_token(),
				      MAX_TOKEN_LEN);
	} else if (msg->token && msg->tkl != LWM2M_MSG_TOKEN_LEN_SKIP) {
		zoap_header_set_token(&msg->zpkt, msg->token, msg->tkl);
	}

	/* only TYPE_CON messages need pending tracking / reply handling */
	if (msg->type != ZOAP_TYPE_CON) {
		return 0;
	}

	msg->pending = zoap_pending_next_unused(
				msg->ctx->pendings,
				CONFIG_LWM2M_ENGINE_MAX_PENDING);
	if (!msg->pending) {
		SYS_LOG_ERR("Unable to find a free pending to track "
			    "retransmissions.");
		r = -ENOMEM;
		goto cleanup;
	}

	r = zoap_pending_init(msg->pending, &msg->zpkt,
			      &app_ctx->default_ctx->remote);
	if (r < 0) {
		SYS_LOG_ERR("Unable to initialize a pending "
			    "retransmission (err:%d).", r);
		goto cleanup;
	}

	/* clear out pkt to avoid double unref */
	pkt = NULL;

	if (msg->reply_cb) {
		msg->reply = zoap_reply_next_unused(
				msg->ctx->replies,
				CONFIG_LWM2M_ENGINE_MAX_REPLIES);
		if (!msg->reply) {
			SYS_LOG_ERR("No resources for "
				    "waiting for replies.");
			r = -ENOMEM;
			goto cleanup;
		}

		zoap_reply_init(msg->reply, &msg->zpkt);
		msg->reply->reply = msg->reply_cb;
	}

	return 0;

cleanup:
	lwm2m_release_message(msg);
	if (pkt) {
		net_pkt_unref(pkt);
	}

	return r;
}

int lwm2m_send_message(struct lwm2m_message *msg)
{
	int ret;

	if (!msg || !msg->ctx) {
		SYS_LOG_ERR("LwM2M message is invalid.");
		return -EINVAL;
	}

	msg->send_attempts++;
	ret = net_app_send_pkt(&msg->ctx->net_app_ctx, msg->zpkt.pkt,
			       &msg->ctx->net_app_ctx.default_ctx->remote,
			       NET_SOCKADDR_MAX_SIZE, K_NO_WAIT, NULL);
	if (ret < 0) {
		return ret;
	}

	if (msg->type == ZOAP_TYPE_CON) {
		if (msg->send_attempts > 1) {
			return 0;
		}

		zoap_pending_cycle(msg->pending);
		k_delayed_work_submit(&msg->ctx->retransmit_work,
				      msg->pending->timeout);
	} else {
		/* if we're not expecting an ACK, free up the msg data */
		lwm2m_release_message(msg);
	}

	return 0;
}

u16_t lwm2m_get_rd_data(u8_t *client_data, u16_t size)
{
	struct lwm2m_engine_obj *obj;
	struct lwm2m_engine_obj_inst *obj_inst;
	u8_t temp[32];
	u16_t pos = 0;
	int len;

	/* Add resource-type/content-type to the registration message */
	memcpy(client_data, REG_PREFACE, sizeof(REG_PREFACE) - 1);
	pos += sizeof(REG_PREFACE) - 1;

	SYS_SLIST_FOR_EACH_CONTAINER(&engine_obj_list, obj, node) {
		/* Security obj MUST NOT be part of registration message */
		if (obj->obj_id == LWM2M_OBJECT_SECURITY_ID) {
			continue;
		}

		/* Only report <OBJ_ID> when no instance available */
		if (obj->instance_count == 0) {
			len = snprintf(temp, sizeof(temp), "%s</%u>",
				       (pos > 0) ? "," : "", obj->obj_id);
			if (pos + len >= size) {
				/* full buffer -- exit loop */
				break;
			}

			memcpy(&client_data[pos], temp, len);
			pos += len;
			continue;
		}

		SYS_SLIST_FOR_EACH_CONTAINER(&engine_obj_inst_list,
					     obj_inst, node) {
			if (obj_inst->obj->obj_id == obj->obj_id) {
				len = snprintf(temp, sizeof(temp),
					       "%s</%s>",
					       (pos > 0) ? "," : "",
					       obj_inst->path);
				/*
				 * TODO: iterate through resources once block
				 * transfer is handled correctly
				 */
				if (pos + len >= size) {
					/* full buffer -- exit loop */
					break;
				}

				memcpy(&client_data[pos], temp, len);
				pos += len;
			}
		}
	}

	client_data[pos] = '\0';
	return pos;
}

/* input / output selection */

static u16_t select_writer(struct lwm2m_output_context *out, u16_t accept)
{
	switch (accept) {

	case LWM2M_FORMAT_PLAIN_TEXT:
	case LWM2M_FORMAT_OMA_PLAIN_TEXT:
		out->writer = &plain_text_writer;
		break;

	case LWM2M_FORMAT_OMA_TLV:
	case LWM2M_FORMAT_OMA_OLD_TLV:
		out->writer = &oma_tlv_writer;
		break;

#ifdef CONFIG_LWM2M_RW_JSON_SUPPORT
	case LWM2M_FORMAT_OMA_JSON:
	case LWM2M_FORMAT_OMA_OLD_JSON:
		out->writer = &json_writer;
		break;
#endif

	default:
		SYS_LOG_ERR("Unknown Accept type %u, using LWM2M plain text",
			    accept);
		out->writer = &plain_text_writer;
		accept = LWM2M_FORMAT_PLAIN_TEXT;
		break;

	}

	return accept;
}

static u16_t select_reader(struct lwm2m_input_context *in, u16_t format)
{
	switch (format) {

	case LWM2M_FORMAT_PLAIN_TEXT:
	case LWM2M_FORMAT_OMA_PLAIN_TEXT:
		in->reader = &plain_text_reader;
		break;

	case LWM2M_FORMAT_OMA_TLV:
	case LWM2M_FORMAT_OMA_OLD_TLV:
		in->reader = &oma_tlv_reader;
		break;

	default:
		SYS_LOG_ERR("Unknown content type %u, using LWM2M plain text",
			    format);
		in->reader = &plain_text_reader;
		format = LWM2M_FORMAT_PLAIN_TEXT;
		break;

	}

	return format;
}

/* user data setter functions */

static int string_to_path(char *pathstr, struct lwm2m_obj_path *path,
			  char delim)
{
	u16_t value, len;
	int i, tokstart = -1, toklen;
	int end_index = strlen(pathstr) - 1;

	for (i = 0; i <= end_index; i++) {
		/* search for first numeric */
		if (tokstart == -1) {
			if (!isdigit(pathstr[i])) {
				continue;
			}

			tokstart = i;
		}

		/* find delimiter char or end of string */
		if (pathstr[i] == delim || i == end_index) {
			toklen = i - tokstart + 1;

			/* don't process delimiter char */
			if (pathstr[i] == delim) {
				toklen--;
			}

			if (toklen <= 0) {
				continue;
			}

			value = atou16(&pathstr[tokstart], toklen, &len);
			switch (path->level) {

			case 0:
				path->obj_id = value;
				break;

			case 1:
				path->obj_inst_id = value;
				break;

			case 2:
				path->res_id = value;
				break;

			case 3:
				path->res_inst_id = value;
				break;

			default:
				SYS_LOG_ERR("invalid level (%d)",
					    path->level);
				return -EINVAL;

			}

			/* increase the path level for each token found */
			path->level++;
			tokstart = -1;
		}
	}

	return 0;
}

int lwm2m_engine_create_obj_inst(char *pathstr)
{
	struct lwm2m_obj_path path;
	struct lwm2m_engine_obj_inst *obj_inst;
	int ret = 0;

	SYS_LOG_DBG("path:%s", pathstr);

	/* translate path -> path_obj */
	memset(&path, 0, sizeof(path));
	ret = string_to_path(pathstr, &path, '/');
	if (ret < 0) {
		return ret;
	}

	if (path.level != 2) {
		SYS_LOG_ERR("path must have 2 parts");
		return -EINVAL;
	}

	return lwm2m_create_obj_inst(path.obj_id, path.obj_inst_id, &obj_inst);
}

static int lwm2m_engine_set(char *pathstr, void *value, u16_t len)
{
	int ret = 0, i;
	struct lwm2m_obj_path path;
	struct lwm2m_engine_obj_inst *obj_inst;
	struct lwm2m_engine_obj_field *obj_field;
	struct lwm2m_engine_res_inst *res = NULL;
	bool changed = false;
	void *data_ptr = NULL;
	size_t data_len = 0;

	SYS_LOG_DBG("path:%s, value:%p, len:%d", pathstr, value, len);

	/* translate path -> path_obj */
	memset(&path, 0, sizeof(path));
	ret = string_to_path(pathstr, &path, '/');
	if (ret < 0) {
		return ret;
	}

	if (path.level < 3) {
		SYS_LOG_ERR("path must have 3 parts");
		return -EINVAL;
	}

	/* find obj_inst/res_id */
	obj_inst = get_engine_obj_inst(path.obj_id, path.obj_inst_id);
	if (!obj_inst) {
		SYS_LOG_ERR("obj instance %d/%d not found",
			    path.obj_id, path.obj_inst_id);
		return -ENOENT;
	}

	if (!obj_inst->resources || obj_inst->resource_count == 0) {
		SYS_LOG_ERR("obj instance has no resources");
		return -EINVAL;
	}

	obj_field = lwm2m_get_engine_obj_field(obj_inst->obj, path.res_id);
	if (!obj_field) {
		SYS_LOG_ERR("obj field %d not found", path.res_id);
		return -ENOENT;
	}

	for (i = 0; i < obj_inst->resource_count; i++) {
		if (obj_inst->resources[i].res_id == path.res_id) {
			res = &obj_inst->resources[i];
			break;
		}
	}

	if (!res) {
		SYS_LOG_ERR("res instance %d not found", path.res_id);
		return -ENOENT;
	}

	/* setup initial data elements */
	data_ptr = res->data_ptr;
	data_len = res->data_len;

	/* allow user to override data elements via callback */
	if (res->pre_write_cb) {
		data_ptr = res->pre_write_cb(obj_inst->obj_inst_id, &data_len);
	}

	if (!data_ptr) {
		SYS_LOG_ERR("res data pointer is NULL");
		return -EINVAL;
	}

	/* check length (note: we add 1 to string length for NULL pad) */
	if (len > res->data_len -
		(obj_field->data_type == LWM2M_RES_TYPE_STRING ? 1 : 0)) {
		SYS_LOG_ERR("length %u is too long for resource %d data",
			    len, path.res_id);
		return -ENOMEM;
	}

	if (memcmp(data_ptr, value, len) !=  0) {
		changed = true;
	}

	switch (obj_field->data_type) {

	case LWM2M_RES_TYPE_STRING:
		memcpy((u8_t *)data_ptr, value, len);
		((u8_t *)data_ptr)[len] = '\0';
		break;

	case LWM2M_RES_TYPE_U64:
		*((u64_t *)data_ptr) = *(u64_t *)value;
		break;

	case LWM2M_RES_TYPE_U32:
	case LWM2M_RES_TYPE_TIME:
		*((u32_t *)data_ptr) = *(u32_t *)value;
		break;

	case LWM2M_RES_TYPE_U16:
		*((u16_t *)data_ptr) = *(u16_t *)value;
		break;

	case LWM2M_RES_TYPE_U8:
		*((u8_t *)data_ptr) = *(u8_t *)value;
		break;

	case LWM2M_RES_TYPE_S64:
		*((s64_t *)data_ptr) = *(s64_t *)value;
		break;

	case LWM2M_RES_TYPE_S32:
		*((s32_t *)data_ptr) = *(s32_t *)value;
		break;

	case LWM2M_RES_TYPE_S16:
		*((s16_t *)data_ptr) = *(s16_t *)value;
		break;

	case LWM2M_RES_TYPE_S8:
		*((s8_t *)data_ptr) = *(s8_t *)value;
		break;

	case LWM2M_RES_TYPE_BOOL:
		*((bool *)data_ptr) = *(bool *)value;
		break;

	case LWM2M_RES_TYPE_FLOAT32:
		((float32_value_t *)data_ptr)->val1 =
				((float32_value_t *)value)->val1;
		((float32_value_t *)data_ptr)->val2 =
				((float32_value_t *)value)->val2;
		break;

	case LWM2M_RES_TYPE_FLOAT64:
		((float64_value_t *)data_ptr)->val1 =
				((float64_value_t *)value)->val1;
		((float64_value_t *)data_ptr)->val2 =
				((float64_value_t *)value)->val2;
		break;

	default:
		SYS_LOG_ERR("unknown obj data_type %d",
			    obj_field->data_type);
		return -EINVAL;

	}

	if (res->post_write_cb) {
		/* ignore return value here */
		res->post_write_cb(obj_inst->obj_inst_id, data_ptr, len,
				   false, 0);
	}

	if (changed) {
		NOTIFY_OBSERVER_PATH(&path);
	}

	return ret;
}

int lwm2m_engine_set_string(char *pathstr, char *data_ptr)
{
	return lwm2m_engine_set(pathstr, data_ptr, strlen(data_ptr));
}

int lwm2m_engine_set_u8(char *pathstr, u8_t value)
{
	return lwm2m_engine_set(pathstr, &value, 1);
}

int lwm2m_engine_set_u16(char *pathstr, u16_t value)
{
	return lwm2m_engine_set(pathstr, &value, 2);
}

int lwm2m_engine_set_u32(char *pathstr, u32_t value)
{
	return lwm2m_engine_set(pathstr, &value, 4);
}

int lwm2m_engine_set_u64(char *pathstr, u64_t value)
{
	return lwm2m_engine_set(pathstr, &value, 8);
}

int lwm2m_engine_set_s8(char *pathstr, s8_t value)
{
	return lwm2m_engine_set(pathstr, &value, 1);
}

int lwm2m_engine_set_s16(char *pathstr, s16_t value)
{
	return lwm2m_engine_set(pathstr, &value, 2);
}

int lwm2m_engine_set_s32(char *pathstr, s32_t value)
{
	return lwm2m_engine_set(pathstr, &value, 4);
}

int lwm2m_engine_set_s64(char *pathstr, s64_t value)
{
	return lwm2m_engine_set(pathstr, &value, 8);
}

int lwm2m_engine_set_bool(char *pathstr, bool value)
{
	u8_t temp = (value != 0 ? 1 : 0);

	return lwm2m_engine_set(pathstr, &temp, 1);
}

int lwm2m_engine_set_float32(char *pathstr, float32_value_t *value)
{
	return lwm2m_engine_set(pathstr, value, sizeof(float32_value_t));
}

int lwm2m_engine_set_float64(char *pathstr, float64_value_t *value)
{
	return lwm2m_engine_set(pathstr, value, sizeof(float64_value_t));
}

/* user data getter functions */

static int lwm2m_engine_get(char *pathstr, void *buf, u16_t buflen)
{
	int ret = 0, i;
	struct lwm2m_obj_path path;
	struct lwm2m_engine_obj_inst *obj_inst;
	struct lwm2m_engine_obj_field *obj_field;
	struct lwm2m_engine_res_inst *res = NULL;
	u16_t len = 0;
	void *data_ptr = NULL;
	size_t data_len = 0;

	SYS_LOG_DBG("path:%s, buf:%p, buflen:%d", pathstr, buf, buflen);

	/* translate path -> path_obj */
	memset(&path, 0, sizeof(path));
	ret = string_to_path(pathstr, &path, '/');
	if (ret < 0) {
		return ret;
	}

	if (path.level < 3) {
		SYS_LOG_ERR("path must have 3 parts");
		return -EINVAL;
	}

	/* find obj_inst/res_id */
	obj_inst = get_engine_obj_inst(path.obj_id, path.obj_inst_id);
	if (!obj_inst) {
		SYS_LOG_ERR("obj instance %d/%d not found",
			    path.obj_id, path.obj_inst_id);
		return -ENOENT;
	}

	if (!obj_inst->resources || obj_inst->resource_count == 0) {
		SYS_LOG_ERR("obj instance has no resources");
		return -EINVAL;
	}

	obj_field = lwm2m_get_engine_obj_field(obj_inst->obj, path.res_id);
	if (!obj_field) {
		SYS_LOG_ERR("obj field %d not found", path.res_id);
		return -ENOENT;
	}

	for (i = 0; i < obj_inst->resource_count; i++) {
		if (obj_inst->resources[i].res_id == path.res_id) {
			res = &obj_inst->resources[i];
			break;
		}
	}

	if (!res) {
		SYS_LOG_ERR("res instance %d not found", path.res_id);
		return -ENOENT;
	}

	/* setup initial data elements */
	data_ptr = res->data_ptr;
	data_len = res->data_len;

	/* allow user to override data elements via callback */
	if (res->read_cb) {
		data_ptr = res->read_cb(obj_inst->obj_inst_id, &data_len);
	}

	/* TODO: handle data_len > buflen case */

	if (data_ptr && data_len > 0) {
		switch (obj_field->data_type) {

		case LWM2M_RES_TYPE_OPAQUE:
			break;

		case LWM2M_RES_TYPE_STRING:
			strncpy((u8_t *)buf, (u8_t *)data_ptr, buflen);
			len = strlen(buf);
			break;

		case LWM2M_RES_TYPE_U64:
			*(u64_t *)buf = *(u64_t *)data_ptr;
			break;

		case LWM2M_RES_TYPE_U32:
		case LWM2M_RES_TYPE_TIME:
			*(u32_t *)buf = *(u32_t *)data_ptr;
			break;

		case LWM2M_RES_TYPE_U16:
			*(u16_t *)buf = *(u16_t *)data_ptr;
			break;

		case LWM2M_RES_TYPE_U8:
			*(u8_t *)buf = *(u8_t *)data_ptr;
			break;

		case LWM2M_RES_TYPE_S64:
			*(s64_t *)buf = *(s64_t *)data_ptr;
			break;

		case LWM2M_RES_TYPE_S32:
			*(s32_t *)buf = *(s32_t *)data_ptr;
			break;

		case LWM2M_RES_TYPE_S16:
			*(s16_t *)buf = *(s16_t *)data_ptr;
			break;

		case LWM2M_RES_TYPE_S8:
			*(s8_t *)buf = *(s8_t *)data_ptr;
			break;

		case LWM2M_RES_TYPE_BOOL:
			*(bool *)buf = *(bool *)data_ptr;
			break;

		case LWM2M_RES_TYPE_FLOAT32:
			((float32_value_t *)buf)->val1 =
				((float32_value_t *)data_ptr)->val1;
			((float32_value_t *)buf)->val2 =
				((float32_value_t *)data_ptr)->val2;
			break;

		case LWM2M_RES_TYPE_FLOAT64:
			((float64_value_t *)buf)->val1 =
				((float64_value_t *)data_ptr)->val1;
			((float64_value_t *)buf)->val2 =
				((float64_value_t *)data_ptr)->val2;
			break;

		default:
			SYS_LOG_ERR("unknown obj data_type %d",
				    obj_field->data_type);
			return -EINVAL;

		}
	}

	return 0;
}

int lwm2m_engine_get_string(char *pathstr, void *buf, u16_t buflen)
{
	return lwm2m_engine_get(pathstr, buf, buflen);
}

u8_t lwm2m_engine_get_u8(char *pathstr)
{
	u8_t value = 0;

	lwm2m_engine_get(pathstr, &value, 1);
	return value;
}

u16_t lwm2m_engine_get_u16(char *pathstr)
{
	u16_t value = 0;

	lwm2m_engine_get(pathstr, &value, 2);
	return value;
}

u32_t lwm2m_engine_get_u32(char *pathstr)
{
	u32_t value = 0;

	lwm2m_engine_get(pathstr, &value, 4);
	return value;
}

u64_t lwm2m_engine_get_u64(char *pathstr)
{
	u64_t value = 0;

	lwm2m_engine_get(pathstr, &value, 8);
	return value;
}

s8_t lwm2m_engine_get_s8(char *pathstr)
{
	s8_t value = 0;

	lwm2m_engine_get(pathstr, &value, 1);
	return value;
}

s16_t lwm2m_engine_get_s16(char *pathstr)
{
	s16_t value = 0;

	lwm2m_engine_get(pathstr, &value, 2);
	return value;
}

s32_t lwm2m_engine_get_s32(char *pathstr)
{
	s32_t value = 0;

	lwm2m_engine_get(pathstr, &value, 4);
	return value;
}

s64_t lwm2m_engine_get_s64(char *pathstr)
{
	s64_t value = 0;

	lwm2m_engine_get(pathstr, &value, 8);
	return value;
}

bool lwm2m_engine_get_bool(char *pathstr)
{
	return (lwm2m_engine_get_s8(pathstr) != 0);
}

int   lwm2m_engine_get_float32(char *pathstr, float32_value_t *buf)
{
	return lwm2m_engine_get(pathstr, buf, sizeof(float32_value_t));
}

int   lwm2m_engine_get_float64(char *pathstr, float64_value_t *buf)
{
	return lwm2m_engine_get(pathstr, buf, sizeof(float64_value_t));
}

/* user callback functions */
static int engine_get_resource(struct lwm2m_obj_path *path,
			       struct lwm2m_engine_res_inst **res)
{
	int i;
	struct lwm2m_engine_obj_inst *obj_inst;

	if (!path) {
		return -EINVAL;
	}

	/* find obj_inst/res_id */
	obj_inst = get_engine_obj_inst(path->obj_id, path->obj_inst_id);
	if (!obj_inst) {
		SYS_LOG_ERR("obj instance %d/%d not found",
			    path->obj_id, path->obj_inst_id);
		return -ENOENT;
	}

	if (!obj_inst->resources || obj_inst->resource_count == 0) {
		SYS_LOG_ERR("obj instance has no resources");
		return -EINVAL;
	}

	for (i = 0; i < obj_inst->resource_count; i++) {
		if (obj_inst->resources[i].res_id == path->res_id) {
			*res = &obj_inst->resources[i];
			break;
		}
	}

	if (!*res) {
		SYS_LOG_ERR("res instance %d not found", path->res_id);
		return -ENOENT;
	}

	return 0;
}

static int engine_get_resource_from_pathstr(char *pathstr,
			       struct lwm2m_engine_res_inst **res)
{
	int ret;
	struct lwm2m_obj_path path;

	memset(&path, 0, sizeof(path));
	ret = string_to_path(pathstr, &path, '/');
	if (ret < 0) {
		return ret;
	}

	if (path.level < 3) {
		SYS_LOG_ERR("path must have 3 parts");
		return -EINVAL;
	}

	return engine_get_resource(&path, res);
}

int lwm2m_engine_register_read_callback(char *pathstr,
					lwm2m_engine_get_data_cb_t cb)
{
	int ret;
	struct lwm2m_engine_res_inst *res = NULL;

	ret = engine_get_resource_from_pathstr(pathstr, &res);
	if (ret < 0) {
		return ret;
	}

	res->read_cb = cb;
	return 0;
}

int lwm2m_engine_register_pre_write_callback(char *pathstr,
					     lwm2m_engine_get_data_cb_t cb)
{
	int ret;
	struct lwm2m_engine_res_inst *res = NULL;

	ret = engine_get_resource_from_pathstr(pathstr, &res);
	if (ret < 0) {
		return ret;
	}

	res->pre_write_cb = cb;
	return 0;
}

int lwm2m_engine_register_post_write_callback(char *pathstr,
					 lwm2m_engine_set_data_cb_t cb)
{
	int ret;
	struct lwm2m_engine_res_inst *res = NULL;

	ret = engine_get_resource_from_pathstr(pathstr, &res);
	if (ret < 0) {
		return ret;
	}

	res->post_write_cb = cb;
	return 0;
}

int lwm2m_engine_register_exec_callback(char *pathstr,
					lwm2m_engine_exec_cb_t cb)
{
	int ret;
	struct lwm2m_engine_res_inst *res = NULL;

	ret = engine_get_resource_from_pathstr(pathstr, &res);
	if (ret < 0) {
		return ret;
	}

	res->execute_cb = cb;
	return 0;
}

/* generic data handlers */

static int lwm2m_read_handler(struct lwm2m_engine_obj_inst *obj_inst,
			      struct lwm2m_engine_res_inst *res,
			      struct lwm2m_engine_obj_field *obj_field,
			      struct lwm2m_engine_context *context)
{
	struct lwm2m_output_context *out = context->out;
	struct lwm2m_obj_path *path = context->path;
	int i, loop_max = 1;
	u16_t res_inst_id_tmp = 0;
	void *data_ptr = NULL;
	size_t data_len = 0;

	if (!obj_inst || !res || !obj_field || !context) {
		return -EINVAL;
	}

	/* setup initial data elements */
	data_ptr = res->data_ptr;
	data_len = res->data_len;

	/* allow user to override data elements via callback */
	if (res->read_cb) {
		data_ptr = res->read_cb(obj_inst->obj_inst_id, &data_len);
	}

	if (!data_ptr || data_len == 0) {
		return -EINVAL;
	}

	if (res->multi_count_var != NULL) {
		engine_put_begin_ri(out, path);
		loop_max = *res->multi_count_var;
		res_inst_id_tmp = path->res_inst_id;
	}

	for (i = 0; i < loop_max; i++) {
		if (res->multi_count_var != NULL) {
			path->res_inst_id = (u16_t) i;
		}

		switch (obj_field->data_type) {

		/* do nothing for OPAQUE (probably has a callback) */
		case LWM2M_RES_TYPE_OPAQUE:
			break;

		/* TODO: handle multi count for string? */
		case LWM2M_RES_TYPE_STRING:
			engine_put_string(out, path, (u8_t *)data_ptr,
					  strlen((u8_t *)data_ptr));
			break;

		case LWM2M_RES_TYPE_U64:
			engine_put_s64(out, path,
				       (s64_t)((u64_t *)data_ptr)[i]);
			break;

		case LWM2M_RES_TYPE_U32:
		case LWM2M_RES_TYPE_TIME:
			engine_put_s32(out, path,
				       (s32_t)((u32_t *)data_ptr)[i]);
			break;

		case LWM2M_RES_TYPE_U16:
			engine_put_s16(out, path,
				       (s16_t)((u16_t *)data_ptr)[i]);
			break;

		case LWM2M_RES_TYPE_U8:
			engine_put_s8(out, path,
				      (s8_t)((u8_t *)data_ptr)[i]);
			break;

		case LWM2M_RES_TYPE_S64:
			engine_put_s64(out, path,
				       ((s64_t *)data_ptr)[i]);
			break;

		case LWM2M_RES_TYPE_S32:
			engine_put_s32(out, path,
				       ((s32_t *)data_ptr)[i]);
			break;

		case LWM2M_RES_TYPE_S16:
			engine_put_s16(out, path,
				       ((s16_t *)data_ptr)[i]);
			break;

		case LWM2M_RES_TYPE_S8:
			engine_put_s8(out, path,
				      ((s8_t *)data_ptr)[i]);
			break;

		case LWM2M_RES_TYPE_BOOL:
			engine_put_bool(out, path,
					((bool *)data_ptr)[i]);
			break;

		case LWM2M_RES_TYPE_FLOAT32:
			engine_put_float32fix(out, path,
				&((float32_value_t *)data_ptr)[i]);
			break;

		case LWM2M_RES_TYPE_FLOAT64:
			engine_put_float64fix(out, path,
				&((float64_value_t *)data_ptr)[i]);
			break;

		default:
			SYS_LOG_ERR("unknown obj data_type %d",
				    obj_field->data_type);
			return -EINVAL;

		}
	}

	if (res->multi_count_var != NULL) {
		engine_put_end_ri(out, path);
		path->res_inst_id = res_inst_id_tmp;
	}

	return 0;
}

/* This function is exposed for the content format writers */
int lwm2m_write_handler(struct lwm2m_engine_obj_inst *obj_inst,
			struct lwm2m_engine_res_inst *res,
			struct lwm2m_engine_obj_field *obj_field,
			struct lwm2m_engine_context *context)
{
	struct lwm2m_input_context *in = context->in;
	struct lwm2m_obj_path *path = context->path;
	s64_t temp64 = 0;
	s32_t temp32 = 0;
	void *data_ptr = NULL;
	size_t data_len = 0;
	size_t len = 0;
	size_t total_size = 0;
	int ret = 0;
	u8_t tkl = 0;
	const u8_t *token;
	bool last_block = true;
	struct block_context *block_ctx = NULL;

	if (!obj_inst || !res || !obj_field || !context) {
		return -EINVAL;
	}

	/* setup initial data elements */
	data_ptr = res->data_ptr;
	data_len = res->data_len;

	/* setup data_ptr/data_len for OPAQUE when it has none setup */
	if (obj_field->data_type == LWM2M_RES_TYPE_OPAQUE &&
	    data_ptr == NULL && data_len == 0) {
		data_ptr = in->inbuf;
		data_len = in->insize;
	}

	/* allow user to override data elements via callback */
	if (res->pre_write_cb) {
		data_ptr = res->pre_write_cb(obj_inst->obj_inst_id, &data_len);
	}

	if (data_ptr && data_len > 0) {
		switch (obj_field->data_type) {

		/* do nothing for OPAQUE (probably has a callback) */
		case LWM2M_RES_TYPE_OPAQUE:
			data_ptr = in->inbuf;
			len = in->insize;
			break;

		case LWM2M_RES_TYPE_STRING:
			engine_get_string(in, (u8_t *)data_ptr, data_len);
			len = strlen((char *)data_ptr);
			break;

		case LWM2M_RES_TYPE_U64:
			engine_get_s64(in, &temp64);
			*(u64_t *)data_ptr = temp64;
			len = 8;
			break;

		case LWM2M_RES_TYPE_U32:
		case LWM2M_RES_TYPE_TIME:
			engine_get_s32(in, &temp32);
			*(u32_t *)data_ptr = temp32;
			len = 4;
			break;

		case LWM2M_RES_TYPE_U16:
			engine_get_s32(in, &temp32);
			*(u16_t *)data_ptr = temp32;
			len = 2;
			break;

		case LWM2M_RES_TYPE_U8:
			engine_get_s32(in, &temp32);
			*(u8_t *)data_ptr = temp32;
			len = 1;
			break;

		case LWM2M_RES_TYPE_S64:
			engine_get_s64(in, (s64_t *)data_ptr);
			len = 8;
			break;

		case LWM2M_RES_TYPE_S32:
			engine_get_s32(in, (s32_t *)data_ptr);
			len = 4;
			break;

		case LWM2M_RES_TYPE_S16:
			engine_get_s32(in, &temp32);
			*(s16_t *)data_ptr = temp32;
			len = 2;
			break;

		case LWM2M_RES_TYPE_S8:
			engine_get_s32(in, &temp32);
			*(s8_t *)data_ptr = temp32;
			len = 1;
			break;

		case LWM2M_RES_TYPE_BOOL:
			engine_get_bool(in, (bool *)data_ptr);
			len = 1;
			break;

		case LWM2M_RES_TYPE_FLOAT32:
			engine_get_float32fix(in,
					      (float32_value_t *)data_ptr);
			len = 4;
			break;

		case LWM2M_RES_TYPE_FLOAT64:
			engine_get_float64fix(in,
					      (float64_value_t *)data_ptr);
			len = 8;
			break;

		default:
			SYS_LOG_ERR("unknown obj data_type %d",
				    obj_field->data_type);
			return -EINVAL;

		}
	}

	if (res->post_write_cb) {
		/* Get block1 option for checking MORE block flag */
		ret = get_option_int(in->in_zpkt, ZOAP_OPTION_BLOCK1);
		if (ret >= 0) {
			last_block = !GET_MORE(ret);

			/* Get block_ctx for total_size (might be zero) */
			token = zoap_header_get_token(in->in_zpkt, &tkl);
			if (token != NULL &&
			    !get_block_ctx(token, tkl, &block_ctx)) {
				total_size = block_ctx->ctx.total_size;
			}
		}

		/* ignore return value here */
		ret = res->post_write_cb(obj_inst->obj_inst_id, data_ptr, len,
					 last_block, total_size);
		if (ret >= 0) {
			ret = 0;
		}
	}

	NOTIFY_OBSERVER_PATH(path);

	return ret;
}

static int lwm2m_write_attr_handler(struct lwm2m_engine_obj *obj,
			     struct lwm2m_engine_context *context)
{
	if (!obj || !context) {
		return -EINVAL;
	}

	/* TODO: set parameters on resource for notification */

	return 0;
}

static int lwm2m_exec_handler(struct lwm2m_engine_obj *obj,
			      struct lwm2m_engine_context *context)
{
	struct lwm2m_obj_path *path = context->path;
	struct lwm2m_engine_obj_inst *obj_inst;
	struct lwm2m_engine_res_inst *res = NULL;
	int ret;

	if (!obj || !context) {
		return -EINVAL;
	}

	obj_inst = get_engine_obj_inst(path->obj_id, path->obj_inst_id);
	if (!obj_inst) {
		return -ENOENT;
	}

	ret = engine_get_resource(path, &res);
	if (ret < 0) {
		return ret;
	}

	if (res->execute_cb) {
		return res->execute_cb(obj_inst->obj_inst_id);
	}

	/* TODO: something else to handle for execute? */
	return -ENOENT;
}

static int lwm2m_delete_handler(struct lwm2m_engine_obj *obj,
				struct lwm2m_engine_context *context)
{
	if (!context) {
		return -EINVAL;
	}

	return lwm2m_delete_obj_inst(context->path->obj_id,
				     context->path->obj_inst_id);
}

/*
 * ZoAP API needs to create the net_pkt buffer in the correct order.
 * This function performs last minute verification that outbuf is initialized.
 */
static void outbuf_init_check(struct lwm2m_output_context *out)
{
	if (!out->outbuf) {
		out->outbuf = zoap_packet_get_payload(out->out_zpkt,
						      &out->outsize);
	}
}

#define MATCH_NONE	0
#define MATCH_ALL	1
#define MATCH_SINGLE	2

static int do_read_op(struct lwm2m_engine_obj *obj,
		      struct lwm2m_engine_context *context)
{
	struct lwm2m_output_context *out = context->out;
	struct lwm2m_obj_path *path = context->path;
	struct lwm2m_engine_obj_inst *obj_inst;
	int ret = 0, pos = 0, index, match_type;
	u8_t num_read = 0;
	u8_t initialized;
	struct lwm2m_engine_res_inst *res;
	struct lwm2m_engine_obj_field *obj_field;
	u16_t temp_res_id;

	obj_inst = get_engine_obj_inst(path->obj_id, path->obj_inst_id);
	if (!obj_inst) {
		return -ENOENT;
	}

	while (obj_inst) {
		if (!obj_inst->resources || obj_inst->resource_count == 0) {
			continue;
		}

		match_type = MATCH_NONE;
		/* check obj_inst path for at least partial match */
		if (path->obj_id == obj_inst->obj->obj_id &&
		    path->obj_inst_id == obj_inst->obj_inst_id) {
			if (path->level > 2) {
				match_type = MATCH_SINGLE;
			} else {
				match_type = MATCH_ALL;
			}
		}

		if (match_type == MATCH_NONE) {
			continue;
		}

		/* save path's res_id because we may need to change it below */
		temp_res_id = path->res_id;
		initialized = 0;

		for (index = 0; index < obj_inst->resource_count; index++) {
			res = &obj_inst->resources[index];

			/*
			 * On a MATCH_ALL loop, we need to set path's res_id
			 * for lwm2m_read_handler to read this specific
			 * resource.
			 */
			if (match_type == MATCH_ALL) {
				path->res_id = res->res_id;
			} else if (path->res_id != res->res_id) {
				continue;
			}

			obj_field = lwm2m_get_engine_obj_field(obj_inst->obj,
							       res->res_id);
			if (!obj_field) {
				ret = -ENOENT;
			} else if ((obj_field->permissions &
				    LWM2M_PERM_R) != LWM2M_PERM_R) {
				ret = -EPERM;
			} else {
				/* outbuf init / formatter startup if needed */
				if (!initialized) {
					outbuf_init_check(out);
					engine_put_begin(out, path);
					initialized = 1;
				}

				/* perform read operation on this resource */
				ret = lwm2m_read_handler(obj_inst, res,
							 obj_field, context);
				if (ret < 0) {
					/* What to do here? */
					SYS_LOG_ERR("READ OP failed: %d", ret);
				} else {
					num_read += 1;
					pos = out->outlen;
				}
			}

			/* on single read break if errors */
			if (ret < 0 && match_type == MATCH_SINGLE) {
				break;
			}

			/* when reading multiple resources ignore return code */
			ret = 0;
		}

		/* restore path's res_id in case it was changed */
		path->res_id = temp_res_id;

		/* if we wrote anything, finish formatting */
		if (initialized) {
			engine_put_end(out, path);
			pos = out->outlen;
		}

		/* advance to the next object instance */
		obj_inst = next_engine_obj_inst(obj_inst, path->obj_id,
						path->obj_inst_id);
	}

	/* did not read anything even if we should have - on single item */
	if (ret == 0 && num_read == 0 && path->level == 3) {
		return -ENOENT;
	}

	out->outlen = pos;
	return ret;
}

static int do_discover_op(struct lwm2m_engine_context *context)
{
	struct lwm2m_output_context *out = context->out;
	struct lwm2m_engine_obj_inst *obj_inst;
	int i = 0;

	/* set output content-format */
	zoap_add_option_int(out->out_zpkt,
			    ZOAP_OPTION_CONTENT_FORMAT,
			    LWM2M_FORMAT_APP_LINK_FORMAT);

	/* init the outbuffer */
	out->outbuf = zoap_packet_get_payload(out->out_zpkt,
					      &out->outsize);

	/* </.well-known/core>,**;ct=40 */
	memcpy(out->outbuf, DISCOVER_PREFACE, strlen(DISCOVER_PREFACE));
	out->outlen += strlen(DISCOVER_PREFACE);

	SYS_SLIST_FOR_EACH_CONTAINER(&engine_obj_inst_list, obj_inst, node) {
		/* TODO: support bootstrap discover
		 * Avoid discovery for security object (5.2.7.3)
		 */
		if (obj_inst->obj->obj_id == LWM2M_OBJECT_SECURITY_ID) {
			continue;
		}

		out->outlen += sprintf(&out->outbuf[out->outlen], ",</%u/%u>",
				       obj_inst->obj->obj_id,
				       obj_inst->obj_inst_id);

		for (i = 0; i < obj_inst->resource_count; i++) {
			out->outlen += sprintf(&out->outbuf[out->outlen],
					       ",</%u/%u/%u>",
					       obj_inst->obj->obj_id,
					       obj_inst->obj_inst_id,
					       obj_inst->resources[i].res_id);
		}
	}

	out->outbuf[out->outlen] = '\0';
	return 0;
}

int lwm2m_get_or_create_engine_obj(struct lwm2m_engine_context *context,
				   struct lwm2m_engine_obj_inst **obj_inst,
				   u8_t *created)
{
	struct lwm2m_obj_path *path = context->path;
	int ret = 0;

	if (created) {
		*created = 0;
	}

	*obj_inst = get_engine_obj_inst(path->obj_id, path->obj_inst_id);
	if (!*obj_inst) {
		ret = lwm2m_create_obj_inst(path->obj_id, path->obj_inst_id,
					    obj_inst);
		if (ret < 0) {
			return ret;
		}

		zoap_header_set_code(context->in->in_zpkt,
				     ZOAP_RESPONSE_CODE_CREATED);
		/* set created flag to one */
		if (created) {
			*created = 1;
		}
	}

	return ret;
}

static int do_write_op(struct lwm2m_engine_obj *obj,
		       struct lwm2m_engine_context *context,
		       u16_t format)
{
	switch (format) {

	case LWM2M_FORMAT_PLAIN_TEXT:
	case LWM2M_FORMAT_OMA_PLAIN_TEXT:
		return do_write_op_plain_text(obj, context);

	case LWM2M_FORMAT_OMA_TLV:
	case LWM2M_FORMAT_OMA_OLD_TLV:
		return do_write_op_tlv(obj, context);

#ifdef CONFIG_LWM2M_RW_JSON_SUPPORT
	case LWM2M_FORMAT_OMA_JSON:
	case LWM2M_FORMAT_OMA_OLD_JSON:
		return do_write_op_json(obj, context);
#endif

	default:
		SYS_LOG_ERR("Unsupported format: %u", format);
		return -EINVAL;

	}
}

static int handle_request(struct zoap_packet *request,
			  struct lwm2m_message *msg)
{
	int r;
	u8_t code;
	struct zoap_option options[4];
	struct lwm2m_engine_obj *obj;
	const u8_t *token;
	u8_t tkl = 0;
	u16_t format, accept;
	struct lwm2m_input_context in;
	struct lwm2m_output_context out;
	struct lwm2m_obj_path path;
	struct lwm2m_engine_context context;
	int observe = -1; /* default to -1, 0 = ENABLE, 1 = DISABLE */
	bool discover = false;
	struct block_context *block_ctx = NULL;
	size_t block_offset = 0;
	enum zoap_block_size block_size;

	/* setup engine context */
	memset(&context, 0, sizeof(struct lwm2m_engine_context));
	context.in   = &in;
	context.out  = &out;
	context.path = &path;
	engine_clear_context(&context);

	/* set ZoAP request / message */
	in.in_zpkt = request;
	out.out_zpkt = &msg->zpkt;

	/* set default reader/writer */
	in.reader = &plain_text_reader;
	out.writer = &plain_text_writer;

	code = zoap_header_get_code(in.in_zpkt);

	/* parse the URL path into components */
	r = zoap_find_options(in.in_zpkt, ZOAP_OPTION_URI_PATH, options, 4);
	if (r <= 0) {
		/* '/' is used by bootstrap-delete only */

		/*
		 * TODO: Handle bootstrap deleted --
		 * re-add when DTLS support ready
		 */
		r = -EPERM;
		goto error;
	}

	/* check for .well-known/core URI query (DISCOVER) */
	if (r == 2 &&
	    (options[0].len == 11 &&
	     strncmp(options[0].value, ".well-known", 11) == 0) &&
	    (options[1].len == 4 &&
	     strncmp(options[1].value, "core", 4) == 0)) {
		if ((code & ZOAP_REQUEST_MASK) != ZOAP_METHOD_GET) {
			r = -EPERM;
			goto error;
		}

		discover = true;
	} else {
		r = zoap_options_to_path(options, r, &path);
		if (r < 0) {
			r = -ENOENT;
			goto error;
		}
	}

	/* read Content Format */
	r = zoap_find_options(in.in_zpkt, ZOAP_OPTION_CONTENT_FORMAT,
			      options, 1);
	if (r > 0) {
		format = zoap_option_value_to_int(&options[0]);
	} else {
		SYS_LOG_DBG("No content-format given. Assume text plain.");
		format = LWM2M_FORMAT_PLAIN_TEXT;
	}

	/* read Accept */
	r = zoap_find_options(in.in_zpkt, ZOAP_OPTION_ACCEPT, options, 1);
	if (r > 0) {
		accept = zoap_option_value_to_int(&options[0]);
	} else {
		SYS_LOG_DBG("No accept option given. Assume OMA TLV.");
		accept = LWM2M_FORMAT_OMA_TLV;
	}

	/* find registered obj */
	obj = get_engine_obj(path.obj_id);
	if (!obj) {
		/* No matching object found - ignore request */
		r = -ENOENT;
		goto error;
	}

	format = select_reader(&in, format);
	accept = select_writer(&out, accept);

	/* set the operation */
	switch (code & ZOAP_REQUEST_MASK) {

	case ZOAP_METHOD_GET:
		if (discover || format == LWM2M_FORMAT_APP_LINK_FORMAT) {
			context.operation = LWM2M_OP_DISCOVER;
			accept = LWM2M_FORMAT_APP_LINK_FORMAT;
		} else {
			context.operation = LWM2M_OP_READ;
		}
		/* check for observe */
		observe = get_option_int(in.in_zpkt, ZOAP_OPTION_OBSERVE);
		zoap_header_set_code(out.out_zpkt, ZOAP_RESPONSE_CODE_CONTENT);
		break;

	case ZOAP_METHOD_POST:
		if (path.level < 2) {
			/* write/create a object instance */
			context.operation = LWM2M_OP_CREATE;
		} else {
			context.operation = LWM2M_OP_EXECUTE;
		}
		zoap_header_set_code(out.out_zpkt, ZOAP_RESPONSE_CODE_CHANGED);
		break;

	case ZOAP_METHOD_PUT:
		context.operation = LWM2M_OP_WRITE;
		zoap_header_set_code(out.out_zpkt, ZOAP_RESPONSE_CODE_CHANGED);
		break;

	case ZOAP_METHOD_DELETE:
		context.operation = LWM2M_OP_DELETE;
		zoap_header_set_code(out.out_zpkt, ZOAP_RESPONSE_CODE_DELETED);
		break;

	default:
		break;
	}

	/* set response token */
	token = zoap_header_get_token(in.in_zpkt, &tkl);
	if (tkl) {
		zoap_header_set_token(out.out_zpkt, token, tkl);
	}

	in.inpos = 0;
	in.inbuf = zoap_packet_get_payload(in.in_zpkt, &in.insize);

	/* Check for block transfer */
	r = get_option_int(in.in_zpkt, ZOAP_OPTION_BLOCK1);
	if (r > 0) {
		/* RFC7252: 4.6. Message Size */
		block_size = GET_BLOCK_SIZE(r);
		if (GET_MORE(r) &&
		    zoap_block_size_to_bytes(block_size) > in.insize) {
			SYS_LOG_DBG("Trailing payload is discarded!");
			r = -EFBIG;
			goto error;
		}

		if (GET_BLOCK_NUM(r) == 0) {
			r = init_block_ctx(token, tkl, &block_ctx);
		} else {
			r = get_block_ctx(token, tkl, &block_ctx);
		}

		if (r < 0) {
			goto error;
		}

		/* 0 will be returned if it's the last block */
		block_offset = zoap_next_block(in.in_zpkt, &block_ctx->ctx);
	}

	switch (context.operation) {

	case LWM2M_OP_READ:
		if (observe == 0) {
			/* add new observer */
			if (token) {
				r = zoap_add_option_int(out.out_zpkt,
							ZOAP_OPTION_OBSERVE, 1);
				if (r) {
					SYS_LOG_ERR("OBSERVE option error: %d",
						    r);
				}

				r = engine_add_observer(msg, token, tkl, &path,
							accept);
				if (r < 0) {
					SYS_LOG_ERR("add OBSERVE error: %d", r);
				}
			} else {
				SYS_LOG_ERR("OBSERVE request missing token");
			}
		} else if (observe == 1) {
			/* use token from this request */
			token = zoap_header_get_token(in.in_zpkt, &tkl);
			/* remove observer */
			r = engine_remove_observer(token, tkl);
			if (r < 0) {
				SYS_LOG_ERR("remove obserer error: %d", r);
			}
		}

		/* set output content-format */
		r = zoap_add_option_int(out.out_zpkt,
					ZOAP_OPTION_CONTENT_FORMAT, accept);
		if (r > 0) {
			SYS_LOG_ERR("Error setting response content-format: %d",
				    r);
		}

		r = do_read_op(obj, &context);
		break;

	case LWM2M_OP_DISCOVER:
		r = do_discover_op(&context);
		break;

	case LWM2M_OP_WRITE:
	case LWM2M_OP_CREATE:
		r = do_write_op(obj, &context, format);
		break;

	case LWM2M_OP_WRITE_ATTR:
		r = lwm2m_write_attr_handler(obj, &context);
		break;

	case LWM2M_OP_EXECUTE:
		r = lwm2m_exec_handler(obj, &context);
		break;

	case LWM2M_OP_DELETE:
		r = lwm2m_delete_handler(obj, &context);
		break;

	default:
		SYS_LOG_ERR("Unknown operation: %u", context.operation);
		r = -EINVAL;
	}

	if (r) {
		goto error;
	}

	/* Handle blockwise 1 */
	if (block_ctx) {
		if (block_offset > 0) {
			/* More to come, ack with correspond block # */
			r = zoap_add_block1_option(
					out.out_zpkt, &block_ctx->ctx);
			if (r) {
				/* report as internal server error */
				SYS_LOG_ERR("Fail adding block1 option: %d", r);
				r = -EINVAL;
				goto error;
			} else {
				zoap_header_set_code(out.out_zpkt,
						ZOAP_RESPONSE_CODE_CONTINUE);
			}
		} else {
			/* Free context when finished */
			free_block_ctx(block_ctx);
		}
	}

	if (out.outlen > 0) {
		SYS_LOG_DBG("replying with %u bytes", out.outlen);
		zoap_packet_set_used(out.out_zpkt, out.outlen);
	} else {
		SYS_LOG_DBG("no data in reply");
	}

	return 0;

error:
	if (r == -ENOENT) {
		zoap_header_set_code(out.out_zpkt,
				     ZOAP_RESPONSE_CODE_NOT_FOUND);
	} else if (r == -EPERM) {
		zoap_header_set_code(out.out_zpkt,
				     ZOAP_RESPONSE_CODE_NOT_ALLOWED);
	} else if (r == -EEXIST) {
		zoap_header_set_code(out.out_zpkt,
				     ZOAP_RESPONSE_CODE_BAD_REQUEST);
	} else if (r == -EFBIG) {
		zoap_header_set_code(out.out_zpkt,
				     ZOAP_RESPONSE_CODE_REQUEST_TOO_LARGE);
	} else {
		/* Failed to handle the request */
		zoap_header_set_code(out.out_zpkt,
				     ZOAP_RESPONSE_CODE_INTERNAL_ERROR);
	}

	/* Free block context when error happened */
	free_block_ctx(block_ctx);

	return 0;
}

void lwm2m_udp_receive(struct lwm2m_ctx *client_ctx, struct net_pkt *pkt,
		       bool handle_separate_response,
		       udp_request_handler_cb_t udp_request_handler)
{
	struct lwm2m_message *msg = NULL;
	struct net_udp_hdr hdr, *udp_hdr;
	struct zoap_pending *pending;
	struct zoap_reply *reply;
	struct zoap_packet response;
	struct sockaddr from_addr;
	int header_len, r;
	const u8_t *token;
	u8_t tkl;

	udp_hdr = net_udp_get_hdr(pkt, &hdr);
	if (!udp_hdr) {
		SYS_LOG_ERR("Invalid UDP data");
		return;
	}

	/* Save the from address */
#if defined(CONFIG_NET_IPV6)
	if (net_pkt_family(pkt) == AF_INET6) {
		net_ipaddr_copy(&net_sin6(&from_addr)->sin6_addr,
				&NET_IPV6_HDR(pkt)->src);
		net_sin6(&from_addr)->sin6_port = udp_hdr->src_port;
		net_sin6(&from_addr)->sin6_family = AF_INET6;
	}
#endif

#if defined(CONFIG_NET_IPV4)
	if (net_pkt_family(pkt) == AF_INET) {
		net_ipaddr_copy(&net_sin(&from_addr)->sin_addr,
				&NET_IPV4_HDR(pkt)->src);
		net_sin(&from_addr)->sin_port = udp_hdr->src_port;
		net_sin(&from_addr)->sin_family = AF_INET;
	}
#endif

	/*
	 * zoap expects that buffer->data starts at the
	 * beginning of the CoAP header
	 */
	header_len = net_pkt_appdata(pkt) - pkt->frags->data;
	net_buf_pull(pkt->frags, header_len);

	r = zoap_packet_parse(&response, pkt);
	if (r < 0) {
		SYS_LOG_ERR("Invalid data received (err:%d)", r);
		goto cleanup;
	}

	token = zoap_header_get_token(&response, &tkl);
	pending = zoap_pending_received(&response, client_ctx->pendings,
					CONFIG_LWM2M_ENGINE_MAX_PENDING);
	/*
	 * Clear pending pointer because zoap_pending_received() calls
	 * zoap_pending_clear, and later when we call lwm2m_release_message()
	 * it will try and call zoap_pending_clear() again if msg->pending
	 * is != NULL.
	 */
	if (pending) {
		msg = find_msg(pending, NULL);
		if (msg) {
			msg->pending = NULL;
		}
	}

	SYS_LOG_DBG("checking for reply from [%s]",
		    lwm2m_sprint_ip_addr(&from_addr));
	reply = zoap_response_received(&response, &from_addr,
				       client_ctx->replies,
				       CONFIG_LWM2M_ENGINE_MAX_REPLIES);
	if (reply) {
		/*
		 * Separate response is composed of 2 messages, empty ACK with
		 * no token and an additional message with a matching token id
		 * (based on the token used by the CON request).
		 *
		 * Since the ACK received by the notify CON messages are also
		 * empty with no token (consequence of always using the same
		 * token id for all notifications), we have to use an
		 * additional flag to decide when to clear the reply callback.
		 */
		if (handle_separate_response && !tkl &&
			zoap_header_get_type(&response) == ZOAP_TYPE_ACK) {
			SYS_LOG_DBG("separated response, not removing reply");
			goto cleanup;
		}

		if (!msg) {
			msg = find_msg(pending, reply);
		}
	}

	if (reply || pending) {
		/* free up msg resources */
		if (msg) {
			lwm2m_release_message(msg);
		}

		SYS_LOG_DBG("reply %p handled and removed", reply);
		goto cleanup;
	}

	/*
	 * If no normal response handler is found, then this is
	 * a new request coming from the server.  Let's look
	 * at registered objects to find a handler.
	 */
	if (udp_request_handler &&
	    zoap_header_get_type(&response) == ZOAP_TYPE_CON) {
		msg = lwm2m_get_message(client_ctx);
		if (!msg) {
			SYS_LOG_ERR("Unable to get a lwm2m message!");
			goto cleanup;
		}

		/* Create a response message if we reach this point */
		msg->type = ZOAP_TYPE_ACK;
		msg->code = zoap_header_get_code(&response);
		msg->mid = zoap_header_get_id(&response);
		/* skip token generation by default */
		msg->tkl = LWM2M_MSG_TOKEN_LEN_SKIP;

		r = lwm2m_init_message(msg);
		if (r < 0) {
			goto cleanup;
		}

		/* process the response to this request */
		r = udp_request_handler(&response, msg);
		if (r < 0) {
			SYS_LOG_ERR("Request handler error: %d", r);
		} else {
			r = lwm2m_send_message(msg);
			if (r < 0) {
				SYS_LOG_ERR("Err sending response: %d",
					    r);
				lwm2m_release_message(msg);
			}
		}
	} else {
		SYS_LOG_ERR("No handler for response");
	}

cleanup:
	if (pkt) {
		net_pkt_unref(pkt);
	}
}

static void udp_receive(struct net_app_ctx *app_ctx, struct net_pkt *pkt,
			int status, void *user_data)
{
	struct lwm2m_ctx *client_ctx = CONTAINER_OF(app_ctx,
						    struct lwm2m_ctx,
						    net_app_ctx);

	lwm2m_udp_receive(client_ctx, pkt, false, handle_request);
}

static void retransmit_request(struct k_work *work)
{
	struct lwm2m_ctx *client_ctx;
	struct lwm2m_message *msg;
	struct zoap_pending *pending;
	int r;

	client_ctx = CONTAINER_OF(work, struct lwm2m_ctx, retransmit_work);
	pending = zoap_pending_next_to_expire(client_ctx->pendings,
					      CONFIG_LWM2M_ENGINE_MAX_PENDING);
	if (!pending) {
		return;
	}

	msg = find_msg(pending, NULL);
	if (!msg) {
		SYS_LOG_ERR("pending has no valid LwM2M message!");
		return;
	}

	if (!zoap_pending_cycle(pending)) {
		/* pending request has expired */
		if (msg->message_timeout_cb) {
			msg->message_timeout_cb(msg);
		}

		/* final unref to release pkt */
		net_pkt_unref(pending->pkt);
		lwm2m_release_message(msg);
		return;
	}

	r = lwm2m_send_message(msg);
	if (r < 0) {
		SYS_LOG_ERR("Error sending lwm2m message: %d", r);
		/* don't error here, retry until timeout */
	}

	k_delayed_work_submit(&client_ctx->retransmit_work, pending->timeout);
}

static int notify_message_reply_cb(const struct zoap_packet *response,
				   struct zoap_reply *reply,
				   const struct sockaddr *from)
{
	int ret = 0;
	u8_t type, code;

	type = zoap_header_get_type(response);
	code = zoap_header_get_code(response);

	SYS_LOG_DBG("NOTIFY ACK type:%u code:%d.%d reply_token:'%s'",
		type,
		ZOAP_RESPONSE_CODE_CLASS(code),
		ZOAP_RESPONSE_CODE_DETAIL(code),
		sprint_token(reply->token, reply->tkl));

	/* remove observer on ZOAP_TYPE_RESET */
	if (type == ZOAP_TYPE_RESET) {
		if (reply->tkl > 0) {
			ret = engine_remove_observer(reply->token, reply->tkl);
			if (ret) {
				SYS_LOG_ERR("remove obserer error: %d", ret);
			}
		} else {
			SYS_LOG_ERR("notify reply missing token -- ignored.");
		}
	}

	return 0;
}

static int generate_notify_message(struct observe_node *obs,
				   bool manual_trigger)
{
	struct lwm2m_message *msg;
	struct lwm2m_engine_obj_inst *obj_inst;
	struct lwm2m_output_context out;
	struct lwm2m_engine_context context;
	struct lwm2m_obj_path path;
	int ret = 0;

	if (!obs->ctx) {
		SYS_LOG_ERR("observer has no valid LwM2M ctx!");
		return -EINVAL;
	}

	/* setup engine context */
	memset(&context, 0, sizeof(struct lwm2m_engine_context));
	context.out = &out;
	engine_clear_context(&context);
	/* dont clear the path */
	memcpy(&path, &obs->path, sizeof(struct lwm2m_obj_path));
	context.path = &path;
	context.operation = LWM2M_OP_READ;

	SYS_LOG_DBG("[%s] NOTIFY MSG START: %u/%u/%u(%u) token:'%s' [%s] %lld",
		    manual_trigger ? "MANUAL" : "AUTO",
		    obs->path.obj_id,
		    obs->path.obj_inst_id,
		    obs->path.res_id,
		    obs->path.level,
		    sprint_token(obs->token, obs->tkl),
		    lwm2m_sprint_ip_addr(
				&obs->ctx->net_app_ctx.default_ctx->remote),
		    k_uptime_get());

	obj_inst = get_engine_obj_inst(obs->path.obj_id,
				       obs->path.obj_inst_id);
	if (!obj_inst) {
		SYS_LOG_ERR("unable to get engine obj for %u/%u",
			    obs->path.obj_id,
			    obs->path.obj_inst_id);
		return -EINVAL;
	}

	msg = lwm2m_get_message(obs->ctx);
	if (!msg) {
		SYS_LOG_ERR("Unable to get a lwm2m message!");
		return -ENOMEM;
	}

	out.out_zpkt = &msg->zpkt;
	msg->type = ZOAP_TYPE_CON;
	msg->code = ZOAP_RESPONSE_CODE_CONTENT;
	msg->mid = 0;
	msg->token = obs->token;
	msg->tkl = obs->tkl;
	msg->reply_cb = notify_message_reply_cb;

	ret = lwm2m_init_message(msg);
	if (ret) {
		return ret;
	}

	/* each notification should increment the obs counter */
	obs->counter++;
	ret = zoap_add_option_int(out.out_zpkt, ZOAP_OPTION_OBSERVE,
				  obs->counter);
	if (ret) {
		SYS_LOG_ERR("OBSERVE option error: %d", ret);
		goto cleanup;
	}

	/* set the output writer */
	select_writer(&out, obs->format);

	/* set response content-format */
	ret = zoap_add_option_int(out.out_zpkt, ZOAP_OPTION_CONTENT_FORMAT,
				  obs->format);
	if (ret > 0) {
		SYS_LOG_ERR("error setting content-format (err:%d)", ret);
		goto cleanup;
	}

	ret = do_read_op(obj_inst->obj, &context);
	if (ret == 0) {
		if (out.outlen > 0) {
			zoap_packet_set_used(out.out_zpkt, out.outlen);
		} else {
			SYS_LOG_DBG("no data in reply");
		}
	} else {
		SYS_LOG_ERR("error in multi-format read (err:%d)", ret);
		goto cleanup;
	}

	ret = lwm2m_send_message(msg);
	if (ret < 0) {
		SYS_LOG_ERR("Error sending LWM2M packet (err:%d).", ret);
		goto cleanup;
	}

	SYS_LOG_DBG("NOTIFY MSG: SENT");
	return 0;

cleanup:
	lwm2m_release_message(msg);
	return ret;
}

/* TODO: this needs to be triggered via work_queue */
static void lwm2m_engine_service(void)
{
	struct observe_node *obs;
	s64_t timestamp;

	while (true) {
		/*
		 * 1. scan the observer list
		 * 2. For each notify event found, scan the observer list
		 * 3. For each observer match, generate a NOTIFY message,
		 *    attaching the notify response handler
		 */
		timestamp = k_uptime_get();
		SYS_SLIST_FOR_EACH_CONTAINER(&engine_observer_list, obs, node) {
			/*
			 * manual notify requirements:
			 * - event_timestamp > last_timestamp
			 * - current timestamp > last_timestamp + min_period_sec
			 */
			if (obs->event_timestamp > obs->last_timestamp &&
			    timestamp > obs->last_timestamp +
					K_SECONDS(obs->min_period_sec)) {
				obs->last_timestamp = k_uptime_get();
				generate_notify_message(obs, true);

			/*
			 * automatic time-based notify requirements:
			 * - current timestamp > last_timestamp + max_period_sec
			 */
			} else if (timestamp > obs->last_timestamp +
					K_SECONDS(obs->min_period_sec)) {
				obs->last_timestamp = k_uptime_get();
				generate_notify_message(obs, false);
			}

		}

		k_sleep(K_MSEC(ENGINE_UPDATE_INTERVAL));
	}
}

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
int lwm2m_engine_set_net_pkt_pool(struct lwm2m_ctx *ctx,
				  net_pkt_get_slab_func_t tx_slab,
				  net_pkt_get_pool_func_t data_pool)
{
	ctx->tx_slab = tx_slab;
	ctx->data_pool = data_pool;

	return 0;
}
#endif /* CONFIG_NET_CONTEXT_NET_PKT_POOL */

void lwm2m_engine_context_init(struct lwm2m_ctx *client_ctx)
{
	k_delayed_work_init(&client_ctx->retransmit_work, retransmit_request);

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
	net_app_set_net_pkt_pool(&client_ctx->net_app_ctx,
				 client_ctx->tx_slab, client_ctx->data_pool);
#endif
}

int lwm2m_engine_start(struct lwm2m_ctx *client_ctx,
		       char *peer_str, u16_t peer_port)
{
	int ret = 0;

	/* TODO: use security object for initial setup */
	ret = net_app_init_udp_client(&client_ctx->net_app_ctx,
				      NULL, NULL,
				      peer_str,
				      peer_port,
				      client_ctx->net_init_timeout,
				      client_ctx);
	if (ret) {
		SYS_LOG_ERR("net_app_init_udp_client err:%d", ret);
		goto error_start;
	}

	lwm2m_engine_context_init(client_ctx);

	/* set net_app callbacks */
	ret = net_app_set_cb(&client_ctx->net_app_ctx,
			     NULL, udp_receive, NULL, NULL);
	if (ret) {
		SYS_LOG_ERR("Could not set receive callback (err:%d)", ret);
		goto error_start;
	}

	ret = net_app_connect(&client_ctx->net_app_ctx,
			      client_ctx->net_timeout);
	if (ret < 0) {
		SYS_LOG_ERR("Cannot connect UDP (%d)", ret);
		goto error_start;
	}

	return 0;

error_start:
	net_app_close(&client_ctx->net_app_ctx);
	net_app_release(&client_ctx->net_app_ctx);
	return ret;
}

static int lwm2m_engine_init(struct device *dev)
{
	memset(block1_contexts, 0,
	       sizeof(struct block_context) * NUM_BLOCK1_CONTEXT);

	/* start thread to handle OBSERVER / NOTIFY events */
	k_thread_create(&engine_thread_data,
			&engine_thread_stack[0],
			K_THREAD_STACK_SIZEOF(engine_thread_stack),
			(k_thread_entry_t) lwm2m_engine_service,
			NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);
	SYS_LOG_DBG("LWM2M engine thread started");
	return 0;
}

SYS_INIT(lwm2m_engine_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
