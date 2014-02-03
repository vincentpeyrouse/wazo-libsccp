#include <asterisk.h>
#include <asterisk/app.h>
#include <asterisk/astobj2.h>
#include <asterisk/event.h>
#include <asterisk/lock.h>
#include <asterisk/pbx.h>

#include "sccp_config.h"
#include "sccp_device.h"
#include "sccp_session.h"
#include "sccp_msg.h"
#include "sccp_serializer.h"
#include "sccp_utils.h"

#define LINE_INSTANCE_START 1

struct sccp_speeddial {
	/* (static) */
	struct sccp_device *device;
	/* (dynamic) */
	struct sccp_speeddial_cfg *cfg;

	/* (static) */
	uint32_t instance;
	uint32_t index;

	/* (dynamic) */
	int state_id;
	int state;	/* enum ast_extension_states */
};

struct speeddial_group {
	struct sccp_speeddial **speeddials;
	size_t count;
};

struct sccp_line {
	/* (static) */
	struct sccp_device *device;
	/* (dynamic) */
	struct sccp_line_cfg *cfg;

	/* (static) */
	uint32_t instance;

	/* special case of duplicated information from the config (static) */
	char name[SCCP_LINE_NAME_MAX];
};

/* limited to exactly 1 line for now, but is the way on to the support of multiple lines,
 * and more important, it offers symmetry with speeddial_group, so there's only one system
 * to understand
 */
struct line_group {
	struct sccp_line *line;
	size_t count;
};

enum sccp_device_state_id {
	STATE_NEW,
	STATE_REGISTERING,
	STATE_CONNLOST,
};

struct sccp_device_state {
	enum sccp_device_state_id id;
	void (*handle_msg)(struct sccp_device *device, struct sccp_msg *msg, uint32_t msg_id);
};

struct sccp_device {
	/* (static) */
	ast_mutex_t lock;

	struct speeddial_group sd_group;
	struct line_group line_group;

	/* (static) */
	struct sccp_msg_builder msg_builder;

	/* (static) */
	struct sccp_session *session;
	/* (dynamic) */
	struct sccp_device_cfg *cfg;
	/* (dynamic) */
	struct sccp_device_state *state;
	/* (dynamic) */
	struct ast_format_cap *caps;	/* Supported capabilities */
	/* (dynamic) */
	struct ast_event_sub *mwi_event_sub;

	enum sccp_device_type type;
	uint8_t proto_version;

	/* if the device is a guest, then the name will be different then the
	 * device config name (static)
	 */
	char name[SCCP_DEVICE_NAME_MAX];
};

static void subscribe_mwi(struct sccp_device *device);
static void unsubscribe_mwi(struct sccp_device *device);
static void subscribe_hints(struct sccp_device *device);
static void unsubscribe_hints(struct sccp_device *device);
static void handle_msg_state_common(struct sccp_device *device, struct sccp_msg *msg, uint32_t msg_id);
static void transmit_reset(struct sccp_device *device, enum sccp_reset_type type);

static struct sccp_device_state state_new = {
	.id = STATE_NEW,
	.handle_msg = NULL,
};

static struct sccp_device_state state_registering = {
	.id = STATE_REGISTERING,
	.handle_msg = handle_msg_state_common,
};

static struct sccp_device_state state_connlost = {
	.id = STATE_CONNLOST,
	.handle_msg = NULL,
};

static void sccp_speeddial_destructor(void *data)
{
	struct sccp_speeddial *sd = data;

	ast_log(LOG_DEBUG, "in destructor for speeddial %p\n", sd);

	ao2_ref(sd->device, -1);
	ao2_ref(sd->cfg, -1);
}

static struct sccp_speeddial *sccp_speeddial_alloc(struct sccp_speeddial_cfg *cfg, struct sccp_device *device, uint32_t instance, uint32_t index)
{
	struct sccp_speeddial *sd;

	sd = ao2_alloc_options(sizeof(*sd), sccp_speeddial_destructor, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!sd) {
		return NULL;
	}

	sd->device = device;
	ao2_ref(device, +1);
	sd->cfg = cfg;
	ao2_ref(cfg, +1);
	sd->instance = instance;
	sd->index = index;
	sd->state_id = -1;

	return sd;
}

static void sccp_speeddial_destroy(struct sccp_speeddial *sd)
{
	/* nothing to do for now */
	ast_log(LOG_DEBUG, "destroying speeddial %p\n", sd);
}

static int speeddial_group_init(struct speeddial_group *sd_group, struct sccp_device *device, uint32_t instance)
{
	struct sccp_speeddial **speeddials;
	struct sccp_speeddial_cfg **speeddials_cfg = device->cfg->speeddials_cfg;
	size_t i;
	size_t n = device->cfg->speeddial_count;
	uint32_t index = 1;

	if (!n) {
		sd_group->count = 0;
		return 0;
	}

	speeddials = ast_calloc(n, sizeof(*speeddials));
	if (!speeddials) {
		return -1;
	}

	for (i = 0; i < n; i++) {
		speeddials[i] = sccp_speeddial_alloc(speeddials_cfg[i], device, instance++, index++);
		if (!speeddials[i]) {
			goto error;
		}
	}

	sd_group->speeddials = speeddials;
	sd_group->count = n;

	return 0;

error:
	for (; i > 0; i--) {
		ao2_ref(speeddials[i - 1], -1);
	}

	ast_free(speeddials);

	return -1;
}

static void speeddial_group_deinit(struct speeddial_group *sd_group)
{
	size_t i;

	if (!sd_group->count) {
		return;
	}

	for (i = 0; i < sd_group->count; i++) {
		ao2_ref(sd_group->speeddials[i], -1);
	}

	ast_free(sd_group->speeddials);
}

static void speeddial_group_destroy(struct speeddial_group *sd_group)
{
	size_t i;

	for (i = 0; i < sd_group->count; i++) {
		sccp_speeddial_destroy(sd_group->speeddials[i]);
	}
}

static void sccp_line_destructor(void *data)
{
	struct sccp_line *line = data;

	ast_log(LOG_DEBUG, "in destructor for line %s\n", line->name);

	ao2_ref(line->device, -1);
	ao2_ref(line->cfg, -1);
}

static struct sccp_line *sccp_line_alloc(struct sccp_line_cfg *cfg, struct sccp_device *device, uint32_t instance)
{
	struct sccp_line *line;

	line = ao2_alloc_options(sizeof(*line), sccp_line_destructor, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!line) {
		return NULL;
	}

	line->device = device;
	ao2_ref(device, +1);
	line->cfg = cfg;
	ao2_ref(cfg, +1);
	line->instance = instance;
	ast_copy_string(line->name, cfg->name, sizeof(line->name));

	return line;
}

static void sccp_line_destroy(struct sccp_line *line)
{
	/* nothing to do for now */
	ast_log(LOG_DEBUG, "destroying line %s\n", line->name);
}

static int line_group_init(struct line_group *line_group, struct sccp_device *device, uint32_t instance)
{
	struct sccp_line *line;

	/* A: device->cfg has exactly one line */

	line = sccp_line_alloc(device->cfg->line_cfg, device, instance);
	if (!line) {
		return -1;
	}

	line_group->line = line;
	line_group->count = 1;

	return 0;
}

static void line_group_deinit(struct line_group *line_group)
{
	ao2_ref(line_group->line, -1);
}

static void line_group_destroy(struct line_group *line_group)
{
	sccp_line_destroy(line_group->line);
}

static void sccp_device_destructor(void *data)
{
	struct sccp_device *device = data;

	ast_log(LOG_DEBUG, "in destructor for device %s\n", device->name);

	/* no, it is NOT missing an line_group_deinit(&device->line) nor a
	 * speeddial_group_deinit(&device->group). Only completely created
	 * device object have these field initialized, and completely created
	 * device object must be destroyed via sccp_device_destroy
	 */

	ast_mutex_destroy(&device->lock);
	ast_format_cap_destroy(device->caps);
	ao2_ref(device->session, -1);
	ao2_ref(device->cfg, -1);
}

static int device_type_is_supported(enum sccp_device_type device_type)
{
	int supported;

	switch (device_type) {
	case SCCP_DEVICE_7905:
	case SCCP_DEVICE_7906:
	case SCCP_DEVICE_7911:
	case SCCP_DEVICE_7912:
	case SCCP_DEVICE_7920:
	case SCCP_DEVICE_7921:
	case SCCP_DEVICE_7931:
	case SCCP_DEVICE_7937:
	case SCCP_DEVICE_7940:
	case SCCP_DEVICE_7941:
	case SCCP_DEVICE_7941GE:
	case SCCP_DEVICE_7942:
	case SCCP_DEVICE_7960:
	case SCCP_DEVICE_7961:
	case SCCP_DEVICE_7962:
	case SCCP_DEVICE_7970:
	case SCCP_DEVICE_CIPC:
		supported = 1;
		break;
	default:
		supported = 0;
		break;
	}

	return supported;
}

static struct sccp_device *sccp_device_alloc(struct sccp_device_cfg *cfg, struct sccp_session *session, struct sccp_device_info *info)
{
	struct ast_format_cap *caps;
	struct sccp_device *device;

	caps = ast_format_cap_alloc_nolock();
	if (!caps) {
		return NULL;
	}

	device = ao2_alloc_options(sizeof(*device), sccp_device_destructor, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!device) {
		ast_format_cap_destroy(caps);
		return NULL;
	}

	ast_mutex_init(&device->lock);
	sccp_msg_builder_init(&device->msg_builder, info->type, info->proto_version);
	device->session = session;
	ao2_ref(session, +1);
	device->cfg = cfg;
	ao2_ref(cfg, +1);
	device->state = &state_new;
	device->caps = caps;
	device->type = info->type;
	device->proto_version = info->proto_version;
	ast_copy_string(device->name, info->name, sizeof(device->name));

	return device;
}

struct sccp_device *sccp_device_create(struct sccp_device_cfg *device_cfg, struct sccp_session *session, struct sccp_device_info *info)
{
	struct sccp_device *device;

	if (!device_cfg) {
		ast_log(LOG_ERROR, "sccp device create failed: device_cfg is null\n");
		return NULL;
	}

	if (!session) {
		ast_log(LOG_ERROR, "sccp device create failed: session is null\n");
		return NULL;
	}

	if (!info) {
		ast_log(LOG_ERROR, "sccp device create failed: info is null\n");
		return NULL;
	}

	if (!device_type_is_supported(info->type)) {
		ast_log(LOG_WARNING, "Rejecting [%s], unsupported device type [%d]\n", info->name, info->type);
		return NULL;
	}

	device = sccp_device_alloc(device_cfg, session, info);
	if (!device) {
		return NULL;
	}

	if (line_group_init(&device->line_group, device, LINE_INSTANCE_START)) {
		ao2_ref(device, -1);
		return NULL;
	}

	if (speeddial_group_init(&device->sd_group, device, LINE_INSTANCE_START + device->line_group.count)) {
		line_group_deinit(&device->line_group);
		ao2_ref(device, -1);
		return NULL;
	}

	return device;
}

void sccp_device_destroy(struct sccp_device *device)
{
	ast_log(LOG_DEBUG, "destroying device %s\n", device->name);

	unsubscribe_mwi(device);
	unsubscribe_hints(device);

	line_group_destroy(&device->line_group);
	line_group_deinit(&device->line_group);

	speeddial_group_destroy(&device->sd_group);
	speeddial_group_deinit(&device->sd_group);

	switch (device->state->id) {
	case STATE_NEW:
	case STATE_CONNLOST:
		break;
	default:
		transmit_reset(device, SCCP_RESET_SOFT);
		break;
	}
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_line *sccp_device_get_line(struct sccp_device *device, uint32_t instance)
{
	struct sccp_line *line = device->line_group.line;

	if (line->instance == instance) {
		return line;
	}

	return NULL;
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_speeddial *sccp_device_get_speeddial(struct sccp_device *device, uint32_t instance)
{
	struct sccp_speeddial **speeddials = device->sd_group.speeddials;
	size_t i;

	for (i = 0; i < device->sd_group.count; i++) {
		if (speeddials[i]->instance == instance) {
			return speeddials[i];
		}
	}

	return NULL;
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_speeddial *sccp_device_get_speeddial_by_index(struct sccp_device *device, uint32_t index)
{
	struct sccp_speeddial **speeddials = device->sd_group.speeddials;
	size_t i;

	for (i = 0; i < device->sd_group.count; i++) {
		if (speeddials[i]->index == index) {
			return speeddials[i];
		}
	}

	return NULL;
}

static void transmit_button_template_res(struct sccp_device *device)
{
	struct sccp_msg msg;
	struct button_definition definition[MAX_BUTTON_DEFINITION];
	struct sccp_speeddial **speeddials = device->sd_group.speeddials;
	size_t n = 0;
	size_t i;

	/* add the line */
	definition[n].buttonDefinition = BT_LINE;
	definition[n].lineInstance = device->line_group.line->instance;
	n++;

	/* add the speeddials */
	for (i = 0; i < device->sd_group.count && n < MAX_BUTTON_DEFINITION; i++) {
		definition[n].buttonDefinition = BT_FEATUREBUTTON;
		definition[n].lineInstance = speeddials[i]->instance;
		n++;
	}

	sccp_msg_button_template_res(&msg, definition, n);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_capabilities_req(struct sccp_device *device)
{
	struct sccp_msg msg;

	sccp_msg_capabilities_req(&msg);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_config_status_res(struct sccp_device *device)
{
	struct sccp_msg msg;

	sccp_msg_config_status_res(&msg, device->name, device->line_group.count, device->sd_group.count);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_clear_message(struct sccp_device *device)
{
	struct sccp_msg msg;

	sccp_msg_clear_message(&msg);
	sccp_session_transmit_msg(device->session, &msg);
}

static enum sccp_blf_status extstate_ast2sccp(int state)
{
	switch (state) {
	case AST_EXTENSION_DEACTIVATED:
		return SCCP_BLF_STATUS_UNKNOWN;
	case AST_EXTENSION_REMOVED:
		return SCCP_BLF_STATUS_UNKNOWN;
	case AST_EXTENSION_RINGING:
		return SCCP_BLF_STATUS_ALERTING;
	case AST_EXTENSION_INUSE | AST_EXTENSION_RINGING:
		return SCCP_BLF_STATUS_INUSE;
	case AST_EXTENSION_UNAVAILABLE:
		return SCCP_BLF_STATUS_UNKNOWN;
	case AST_EXTENSION_BUSY:
		return SCCP_BLF_STATUS_INUSE;
	case AST_EXTENSION_INUSE:
		return SCCP_BLF_STATUS_INUSE;
	case AST_EXTENSION_ONHOLD:
		return SCCP_BLF_STATUS_INUSE;
	case AST_EXTENSION_INUSE | AST_EXTENSION_ONHOLD:
		return SCCP_BLF_STATUS_INUSE;
	case AST_EXTENSION_NOT_INUSE:
		return SCCP_BLF_STATUS_IDLE;
	default:
		return SCCP_BLF_STATUS_UNKNOWN;
	}
}

static void transmit_feature_status(struct sccp_device *device, struct sccp_speeddial *sd)
{
	struct sccp_msg msg;
	enum sccp_blf_status status = SCCP_BLF_STATUS_UNKNOWN;

	if (sd->cfg->blf) {
		status = extstate_ast2sccp(sd->state);
	}

	sccp_msg_feature_status(&msg, sd->instance, BT_FEATUREBUTTON, status, sd->cfg->label);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_forward_status_res(struct sccp_device *device, struct sccp_line *line)
{
	struct sccp_msg msg;

	sccp_msg_forward_status_res(&msg, line->instance, "", 0);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_keep_alive_ack(struct sccp_device *device)
{
	struct sccp_msg msg;

	sccp_msg_keep_alive_ack(&msg);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_line_status_res(struct sccp_device *device, struct sccp_line *line)
{
	struct sccp_msg msg;
	struct sccp_line_cfg *line_cfg = line->cfg;

	sccp_msg_builder_line_status_res(&device->msg_builder, &msg, line_cfg->cid_name, line_cfg->cid_num, line->instance);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_register_ack(struct sccp_device *device)
{
	struct sccp_msg msg;
	struct sccp_device_cfg *device_cfg = device->cfg;

	sccp_msg_builder_register_ack(&device->msg_builder, &msg, device_cfg->dateformat, device_cfg->keepalive);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_selectsoftkeys(struct sccp_device *device, uint32_t line_instance, uint32_t callid, enum sccp_softkey_status softkey)
{
	struct sccp_msg msg;

	sccp_msg_select_softkeys(&msg, line_instance, callid, softkey);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_speeddial_stat_res(struct sccp_device *device, struct sccp_speeddial *sd)
{
	struct sccp_msg msg;

	sccp_msg_speeddial_stat_res(&msg, sd->index, sd->cfg->extension, sd->cfg->label);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_softkey_set_res(struct sccp_device *device)
{
	struct sccp_msg msg;

	sccp_msg_softkey_set_res(&msg);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_softkey_template_res(struct sccp_device *device)
{
	struct sccp_msg msg;

	sccp_msg_softkey_template_res(&msg);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_time_date_res(struct sccp_device *device)
{
	struct sccp_msg msg;

	sccp_msg_time_date_res(&msg);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_reset(struct sccp_device *device, enum sccp_reset_type type)
{
	struct sccp_msg msg;

	sccp_msg_reset(&msg, type);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_voicemail_lamp_state(struct sccp_device *device, int new_msgs)
{
	struct sccp_msg msg;
	enum sccp_lamp_state indication = new_msgs ? SCCP_LAMP_ON : SCCP_LAMP_OFF;

	sccp_msg_lamp_state(&msg, STIMULUS_VOICEMAIL, 0, indication);
	sccp_session_transmit_msg(device->session, &msg);
}

static void handle_msg_button_template_req(struct sccp_device *device)
{
	transmit_button_template_res(device);
}

/*
 * called from event thread
 */
static void on_mwi_event(const struct ast_event *event, void *data)
{
	struct sccp_device *device = data;
	int new_msgs = ast_event_get_ie_uint(event, AST_EVENT_IE_NEWMSGS);

	/* XXX don't think there's a need to lock the device... we are just
	 *     transmitting a message... but if we do lock, we'll need to
	 *     be careful when subscribing / unsubscribing (especially
	 *     unsubscribing) to not cause a deadlock caused by a lock
	 *     inversion problem
	 */

	transmit_voicemail_lamp_state(device, new_msgs);
}

static void subscribe_mwi(struct sccp_device *device)
{
	if (ast_strlen_zero(device->cfg->voicemail)) {
		return;
	}

	device->mwi_event_sub = ast_event_subscribe(AST_EVENT_MWI, on_mwi_event, "sccp mwi subsciption", device,
			AST_EVENT_IE_MAILBOX, AST_EVENT_IE_PLTYPE_STR, device->cfg->voicemail,
			AST_EVENT_IE_CONTEXT, AST_EVENT_IE_PLTYPE_STR, device->line_group.line->cfg->context,
			AST_EVENT_IE_NEWMSGS, AST_EVENT_IE_PLTYPE_EXISTS,
			AST_EVENT_IE_END);
	if (!device->mwi_event_sub) {
		ast_log(LOG_WARNING, "device %s subscribe mwi failed\n", device->name);
	}
}

static void unsubscribe_mwi(struct sccp_device *device)
{
	if (device->mwi_event_sub) {
		ast_event_unsubscribe(device->mwi_event_sub);
	}
}

static int on_hint_state_change(char *context, char *id, struct ast_state_cb_info *info, void *data)
{
	struct sccp_speeddial *sd = data;

	/* XXX don't think there's a need to lock the device... but if
	 *     we do lock, we'll need to be careful when subscribing / unsubscribing (especially
	 *     unsubscribing) to not cause a deadlock caused by a lock
	 *     inversion problem
	 */
	sd->state = info->exten_state;

	transmit_feature_status(sd->device, sd);

	return 0;
}

static void subscribe_hints(struct sccp_device *device)
{
	struct sccp_speeddial *sd;
	char *context = device->line_group.line->cfg->context;
	size_t i;

	for (i = 0; i < device->sd_group.count; i++) {
		sd = device->sd_group.speeddials[i];
		if (sd->cfg->blf) {
			sd->state_id = ast_extension_state_add(context, sd->cfg->extension, on_hint_state_change, sd);
			if (sd->state_id == -1) {
				ast_log(LOG_WARNING, "Could not subscribe to %s@%s\n", sd->cfg->extension, context);
			} else {
				sd->state = ast_extension_state(NULL, context, sd->cfg->extension);
			}
		}
	}
}

static void unsubscribe_hints(struct sccp_device *device)
{
	struct sccp_speeddial *sd;
	size_t i;

	for (i = 0; i < device->sd_group.count; i++) {
		sd = device->sd_group.speeddials[i];
		if (sd->cfg->blf && sd->state_id != -1) {
			ast_extension_state_del(sd->state_id, NULL);
		}
	}
}

static void codec_sccp2ast(enum sccp_codecs sccpcodec, struct ast_format *result)
{
	switch (sccpcodec) {
	case SCCP_CODEC_G711_ALAW:
		ast_format_set(result, AST_FORMAT_ALAW, 0);
		break;
	case SCCP_CODEC_G711_ULAW:
		ast_format_set(result, AST_FORMAT_ULAW, 0);
		break;
	case SCCP_CODEC_G723_1:
		ast_format_set(result, AST_FORMAT_G723_1, 0);
		break;
	case SCCP_CODEC_G729A:
		ast_format_set(result, AST_FORMAT_G729A, 0);
		break;
	case SCCP_CODEC_H261:
		ast_format_set(result, AST_FORMAT_H261, 0);
		break;
	case SCCP_CODEC_H263:
		ast_format_set(result, AST_FORMAT_H263, 0);
		break;
	default:
		ast_format_clear(result);
		break;
	}
}

static void handle_msg_capabilities_res(struct sccp_device *device, struct sccp_msg *msg)
{
	struct ast_format format;
	uint32_t count = letohl(msg->data.caps.count);
	uint32_t sccpcodec;
	uint32_t i;

	if (count > SCCP_MAX_CAPABILITIES) {
		count = SCCP_MAX_CAPABILITIES;
		ast_log(LOG_WARNING, "Received more capabilities (%d) than we can handle (%d)\n", count, SCCP_MAX_CAPABILITIES);
	}

	for (i = 0; i < count; i++) {
		sccpcodec = letohl(msg->data.caps.caps[i].codec);
		codec_sccp2ast(sccpcodec, &format);

		ast_format_cap_add(device->caps, &format);
	}
}

static void handle_msg_config_status_req(struct sccp_device *device)
{
	transmit_config_status_res(device);
}

static void handle_msg_feature_status_req(struct sccp_device *device, struct sccp_msg *msg)
{
	struct sccp_speeddial *speeddial;
	uint32_t instance = letohl(msg->data.feature.instance);

	speeddial = sccp_device_get_speeddial(device, instance);
	if (!speeddial) {
		ast_log(LOG_DEBUG, "No speeddial [%d] on device [%s]\n", instance, device->name);
		sccp_session_stop(device->session);
		return;
	}

	transmit_feature_status(device, speeddial);
}

static void handle_msg_keep_alive(struct sccp_device *device)
{
	transmit_keep_alive_ack(device);
}

static void handle_msg_line_status_req(struct sccp_device *device, struct sccp_msg *msg)
{
	struct sccp_line *line;
	uint32_t instance = letohl(msg->data.line.lineInstance);

	line = sccp_device_get_line(device, instance);
	if (!line) {
		ast_log(LOG_DEBUG, "Line instance [%d] is not attached to device [%s]\n", instance, device->name);
		sccp_session_stop(device->session);
		return;
	}

	transmit_line_status_res(device, line);
	transmit_forward_status_res(device, line);
}

static void handle_msg_softkey_set_req(struct sccp_device *device)
{
	transmit_softkey_set_res(device);
	transmit_selectsoftkeys(device, 0, 0, KEYDEF_ONHOOK);
}

static void handle_msg_softkey_template_req(struct sccp_device *device)
{
	transmit_softkey_template_res(device);
}

static void handle_msg_speeddial_status_req(struct sccp_device *device, struct sccp_msg *msg)
{
	struct sccp_speeddial *speeddial;
	uint32_t index = letohl(msg->data.speeddial.instance);

	speeddial = sccp_device_get_speeddial_by_index(device, index);
	if (!speeddial) {
		ast_debug(2, "No speeddial [%d] on device [%s]\n", index, device->name);
		return;
	}

	transmit_speeddial_stat_res(device, speeddial);
}

static void handle_msg_time_date_req(struct sccp_device *device)
{
	transmit_time_date_res(device);
}

static void handle_msg_unregister(struct sccp_device *device)
{
	sccp_session_stop(device->session);
}

static void handle_msg_state_common(struct sccp_device *device, struct sccp_msg *msg, uint32_t msg_id)
{
	switch (msg_id) {
	case KEEP_ALIVE_MESSAGE:
		handle_msg_keep_alive(device);
		break;

	case ALARM_MESSAGE:
		ast_debug(1, "Alarm message: %s\n", msg->data.alarm.displayMessage);
		break;

	case FORWARD_STATUS_REQ_MESSAGE:
		break;

	case CAPABILITIES_RES_MESSAGE:
		handle_msg_capabilities_res(device, msg);
		break;

	case SPEEDDIAL_STAT_REQ_MESSAGE:
		handle_msg_speeddial_status_req(device, msg);
		break;

	case FEATURE_STATUS_REQ_MESSAGE:
		handle_msg_feature_status_req(device, msg);
		break;

	case LINE_STATUS_REQ_MESSAGE:
		handle_msg_line_status_req(device, msg);
		break;

	case CONFIG_STATUS_REQ_MESSAGE:
		handle_msg_config_status_req(device);
		break;

	case TIME_DATE_REQ_MESSAGE:
		handle_msg_time_date_req(device);
		break;

	case BUTTON_TEMPLATE_REQ_MESSAGE:
		handle_msg_button_template_req(device);
		break;

	case UNREGISTER_MESSAGE:
		handle_msg_unregister(device);
		break;

	case SOFTKEY_TEMPLATE_REQ_MESSAGE:
		handle_msg_softkey_template_req(device);
		break;

	case SOFTKEY_SET_REQ_MESSAGE:
		handle_msg_softkey_set_req(device);
		break;
	}
}

int sccp_device_handle_msg(struct sccp_device *device, struct sccp_msg *msg)
{
	uint32_t msg_id;

	if (!msg) {
		ast_log(LOG_ERROR, "sccp device handle msg failed: msg is null\n");
		return -1;
	}

	msg_id = letohl(msg->id);

	if (device->state->handle_msg) {
		device->state->handle_msg(device, msg, msg_id);
	}

	return 0;
}

static int sccp_device_test_apply_config(struct sccp_device *device, struct sccp_device_cfg *new_device_cfg)
{
	struct sccp_device_cfg *old_device_cfg = device->cfg;
	struct sccp_line_cfg *new_line_cfg = new_device_cfg->line_cfg;
	struct sccp_line_cfg *old_line_cfg = old_device_cfg->line_cfg;
	struct sccp_speeddial_cfg *new_sd_cfg;
	struct sccp_speeddial_cfg *old_sd_cfg;
	size_t i;

	if (strcmp(old_device_cfg->dateformat, new_device_cfg->dateformat)) {
		return 0;
	}

	if (strcmp(old_device_cfg->voicemail, new_device_cfg->voicemail)) {
		return 0;
	}

	if (old_device_cfg->keepalive != new_device_cfg->keepalive) {
		return 0;
	}

	if (old_device_cfg->speeddial_count != new_device_cfg->speeddial_count) {
		return 0;
	}

	/** check for line **/
	if (strcmp(old_line_cfg->name, new_line_cfg->name)) {
		return 0;
	}

	if (strcmp(old_line_cfg->cid_num, new_line_cfg->cid_num)) {
		return 0;
	}

	if (strcmp(old_line_cfg->cid_name, new_line_cfg->cid_name)) {
		return 0;
	}

	/* right now, the context is also used as the voicemail context and speeddial hint context */
	if (strcmp(old_line_cfg->context, new_line_cfg->context)) {
		return 0;
	}

	/** check for speeddials **/
	/* A: new_device_cfg->speeddial_count == old_device_cfg->speeddial_count */
	for (i = 0; i < new_device_cfg->speeddial_count; i++) {
		new_sd_cfg = new_device_cfg->speeddials_cfg[i];
		old_sd_cfg = old_device_cfg->speeddials_cfg[i];

		if (strcmp(old_sd_cfg->label, new_sd_cfg->label)) {
			return 0;
		}

		if (old_sd_cfg->blf != new_sd_cfg->blf) {
			return 0;
		}

		if (new_sd_cfg->blf && strcmp(old_sd_cfg->extension, new_sd_cfg->extension)) {
			return 0;
		}
	}

	return 1;
}

int sccp_device_reload_config(struct sccp_device *device, struct sccp_device_cfg *new_device_cfg)
{
	struct sccp_line *line = device->line_group.line;
	struct sccp_speeddial *speeddial;
	size_t i;

	if (!new_device_cfg) {
		ast_log(LOG_ERROR, "sccp device reload config failed: device_cfg is null\n");
		return -1;
	}

	if (!sccp_device_test_apply_config(device, new_device_cfg)) {
		transmit_reset(device, SCCP_RESET_SOFT);

		return 0;
	}

	ao2_ref(device->cfg, -1);
	device->cfg = new_device_cfg;
	ao2_ref(device->cfg, +1);

	ao2_ref(line->cfg, -1);
	line->cfg = new_device_cfg->line_cfg;
	ao2_ref(line->cfg, +1);

	for (i = 0; i < device->sd_group.count; i++) {
		speeddial = device->sd_group.speeddials[i];
		ao2_ref(speeddial->cfg, -1);
		speeddial->cfg = new_device_cfg->speeddials_cfg[i];
		ao2_ref(speeddial->cfg, +1);
	}

	return 0;
}

int sccp_device_reset(struct sccp_device *device, enum sccp_reset_type type)
{
	ast_mutex_lock(&device->lock);
	transmit_reset(device, type);
	ast_mutex_unlock(&device->lock);

	return 0;
}

void sccp_device_on_connection_lost(struct sccp_device *device)
{
	device->state = &state_connlost;
}

static void on_keepalive_timeout(struct sccp_device *device, void __attribute__((unused)) *data)
{
	ast_log(LOG_WARNING, "Device %s has timed out\n", device->name);

	sccp_session_stop(device->session);
}

static int add_keepalive_task(struct sccp_device *device)
{
	int timeout = 2 * device->cfg->keepalive;

	return sccp_session_add_device_task(device->session, on_keepalive_timeout, NULL, timeout);
}

void sccp_device_on_data_read(struct sccp_device *device)
{
	add_keepalive_task(device);
}

static void init_voicemail_lamp_state(struct sccp_device *device)
{
	int new_msgs;
	int old_msgs;

	if (ast_strlen_zero(device->cfg->voicemail)) {
		return;
	}

	if (ast_app_inboxcount(device->cfg->voicemail, &new_msgs, &old_msgs) == -1) {
		ast_log(LOG_NOTICE, "could not get voicemail count for %s\n", device->cfg->voicemail);
		return;
	}

	transmit_voicemail_lamp_state(device, new_msgs);
}

void sccp_device_on_registration_success(struct sccp_device *device)
{
	subscribe_mwi(device);
	subscribe_hints(device);
	/* TODO call ast_devstate_changed */

	transmit_register_ack(device);
	transmit_capabilities_req(device);
	transmit_clear_message(device);
	init_voicemail_lamp_state(device);

	add_keepalive_task(device);

	device->state = &state_registering;
}

const char *sccp_device_name(const struct sccp_device *device)
{
	return device->name;
}

void sccp_device_take_snapshot(struct sccp_device *device, struct sccp_device_snapshot *snapshot)
{
	ast_mutex_lock(&device->lock);
	snapshot->type = device->type;
	snapshot->proto_version = device->proto_version;
	ast_copy_string(snapshot->name, device->name, sizeof(snapshot->name));
	ast_copy_string(snapshot->ipaddr, sccp_session_ipaddr(device->session), sizeof(snapshot->ipaddr));
	ast_getformatname_multiple(snapshot->capabilities, sizeof(snapshot->capabilities), device->caps);
	ast_mutex_unlock(&device->lock);
}
