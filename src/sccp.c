#include <asterisk.h>
#include <asterisk/causes.h>
#include <asterisk/channel.h>
#include <asterisk/devicestate.h>
#include <asterisk/io.h>
#include <asterisk/linkedlists.h>
#include <asterisk/module.h>
#include <asterisk/netsock.h>
#include <asterisk/pbx.h>
#include <asterisk/poll-compat.h>
#include <asterisk/rtp_engine.h>
#include <asterisk/utils.h>

#include <netinet/tcp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "device.h"
#include "message.h"
#include "sccp.h"
#include "utils.h"

#define SCCP_PORT "2000"
#define SCCP_BACKLOG 50

extern struct sccp_configs sccp_cfg; /* global */
static AST_LIST_HEAD_STATIC(list_session, sccp_session);
static struct sched_context *sched = NULL;

static struct ast_channel *sccp_request(const char *type, format_t format, const struct ast_channel *requestor, void *destination, int *cause);
static int sccp_call(struct ast_channel *ast, char *dest, int timeout);
static int sccp_devicestate(void *data);
static int sccp_hangup(struct ast_channel *ast);
static int sccp_answer(struct ast_channel *ast);
static struct ast_frame *sccp_read(struct ast_channel *ast);
static int sccp_write(struct ast_channel *ast, struct ast_frame *frame);
static int sccp_indicate(struct ast_channel *ast, int ind, const void *data, size_t datalen);
static int sccp_fixup(struct ast_channel *oldchan, struct ast_channel *newchan);
static int sccp_senddigit_begin(struct ast_channel *ast, char digit);
static int sccp_senddigit_end(struct ast_channel *ast, char digit, unsigned int duration);
static enum ast_rtp_glue_result sccp_get_rtp_peer(struct ast_channel *channel, struct ast_rtp_instance **instance);
static enum ast_rtp_glue_result sccp_get_vrtp_peer(struct ast_channel *channel, struct ast_rtp_instance **instance);
static int sccp_set_rtp_peer(struct ast_channel *channel,
				struct ast_rtp_instance *rtp,
				struct ast_rtp_instance *vrtp,
				struct ast_rtp_instance *trtp,
				format_t codecs,
				int nat_active);

static const struct ast_channel_tech sccp_tech = {
	.type = "sccp",
	.description = "Skinny Client Control Protocol",
	.capabilities = AST_FORMAT_AUDIO_MASK,
	.properties = AST_CHAN_TP_WANTSJITTER | AST_CHAN_TP_CREATESJITTER,
	.requester = sccp_request,
	.devicestate = sccp_devicestate,
	.call = sccp_call,
	.hangup = sccp_hangup,
	.answer = sccp_answer,
	.read = sccp_read,
	.write = sccp_write,
	.indicate = sccp_indicate,
	.fixup = sccp_fixup,
	.send_digit_begin = sccp_senddigit_begin,
	.send_digit_end = sccp_senddigit_end,
	.bridge = ast_rtp_instance_bridge,
};

static struct ast_rtp_glue sccp_rtp_glue = {
	.type = "sccp",
	.get_rtp_info = sccp_get_rtp_peer,
	.update_peer = sccp_set_rtp_peer,
};

static int handle_softkey_template_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;

	msg = msg_alloc(sizeof(struct softkey_template_res_message), SOFTKEY_TEMPLATE_RES_MESSAGE);
	if (msg == NULL)
		return -1;

	msg->data.softkeytemplate.softKeyOffset = htolel(0);
	msg->data.softkeytemplate.softKeyCount = htolel(sizeof(softkey_template_default) / sizeof(struct softkey_template_definition));
	msg->data.softkeytemplate.totalSoftKeyCount = htolel(sizeof(softkey_template_default) / sizeof(struct softkey_template_definition));
	memcpy(msg->data.softkeytemplate.softKeyTemplateDefinition, softkey_template_default, sizeof(softkey_template_default));

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

static int handle_config_status_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;

	msg = msg_alloc(sizeof(struct config_status_res_message), CONFIG_STATUS_RES_MESSAGE);
	if (msg == NULL)
		return -1;

	memcpy(msg->data.configstatus.deviceName, session->device->name, sizeof(msg->data.configstatus.deviceName));
	msg->data.configstatus.stationUserId = htolel(0);
	msg->data.configstatus.stationInstance = htolel(1);
	/*
	memcpy(msg->data.configstatus.userName, "userName", sizeof(msg->data.configstatus.userName));
	memcpy(msg->data.configstatus.serverName, "serverName", sizeof(msg->data.configstatus.serverName));
	*/
	msg->data.configstatus.numberLines = htolel(session->device->line_count);
	msg->data.configstatus.numberSpeedDials = htolel(session->device->speeddial_count);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

static int handle_time_date_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	time_t now = 0;
	struct tm *cmtime = NULL;
	int ret = 0;

	msg = msg_alloc(sizeof(struct time_date_res_message), DATE_TIME_RES_MESSAGE);
	if (msg == NULL)
		return -1;

	now = time(NULL);
	cmtime = localtime(&now);
	if (cmtime == NULL)
		return -1;

	msg->data.timedate.year = htolel(cmtime->tm_year + 1900);
	msg->data.timedate.month = htolel(cmtime->tm_mon + 1);
	msg->data.timedate.dayOfWeek = htolel(cmtime->tm_wday);
	msg->data.timedate.day = htolel(cmtime->tm_mday);
	msg->data.timedate.hour = htolel(cmtime->tm_hour);
	msg->data.timedate.minute = htolel(cmtime->tm_min);
	msg->data.timedate.seconds = htolel(cmtime->tm_sec);
	msg->data.timedate.milliseconds = htolel(0);
	msg->data.timedate.systemTime = htolel(0);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

static int handle_button_template_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	struct button_definition_template btl[42] = {0};
	int button_count = 0;
	int line_instance = 1;
	/*int speeddial_instance = 0;*/
	struct sccp_line *line_itr = NULL;
	int ret = 0;
	int i = 0;

	msg = msg_alloc(sizeof(struct button_template_res_message), BUTTON_TEMPLATE_RES_MESSAGE);
	if (msg == NULL)
		return -1;

	ret = device_get_button_template(session->device, btl);
	if (ret == -1)
		return -1;

	for (i = 0; i < 42; i++) {
		switch (btl[i].buttonDefinition) {
			case BT_CUST_LINESPEEDDIAL:

				msg->data.buttontemplate.definition[i].buttonDefinition = BT_NONE;
				msg->data.buttontemplate.definition[i].instanceNumber = htolel(0);

				AST_LIST_TRAVERSE(&session->device->lines, line_itr, list_per_device) {
					if (line_itr->instance == line_instance) {
						msg->data.buttontemplate.definition[i].buttonDefinition = BT_LINE;
						msg->data.buttontemplate.definition[i].instanceNumber = htolel(line_instance);

						line_instance++;
						button_count++;
					}
				}

				break;

			case BT_NONE:
			default:
				msg->data.buttontemplate.definition[i].buttonDefinition = BT_NONE;
				msg->data.buttontemplate.definition[i].instanceNumber = htolel(0);
				break;
		}
	}

	msg->data.buttontemplate.buttonOffset = htolel(0);
	msg->data.buttontemplate.buttonCount = htolel(button_count);
	msg->data.buttontemplate.totalButtonCount = htolel(button_count);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;
	
	return 0;
}

static int handle_keep_alive_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;

	msg = msg_alloc(0, KEEP_ALIVE_ACK_MESSAGE);
	if (msg == NULL)
		return -1;

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

static int register_device(struct sccp_msg *msg, struct sccp_session *session)
{
	struct sccp_device *device_itr = NULL;
	int ret = 0;

	AST_LIST_TRAVERSE(&list_device, device_itr, list) {

		if (!strcasecmp(device_itr->name, msg->data.reg.name)) {

			if (device_itr->registered == DEVICE_REGISTERED_TRUE) {

				ast_log(LOG_NOTICE, "Device already registered [%s]\n", device_itr->name);
				ret = -1;

			} else {

				ast_log(LOG_NOTICE, "Device found [%s]\n", device_itr->name);
				device_prepare(device_itr);
				device_register(device_itr,
						letohl(msg->data.reg.protoVersion),
						letohl(msg->data.reg.type),
						session);

				session->device = device_itr;
				ret = 1;
			}
			break;
		}
	}

	if (ret == 0)
		ast_log(LOG_NOTICE, "Device not found [%s]\n", device_itr->name);

	return ret;
}

static struct ast_channel *sccp_new_channel(struct sccp_line *line, const char *linkedid)
{
	struct ast_channel *channel = NULL;
	int audio_format = 0;

	if (line == NULL)
		return NULL;

	/* XXX replace hardcoded values */
	channel = ast_channel_alloc(	1,			/* needqueue */
					AST_STATE_DOWN,		/* state */
					line->cid_num,		/* cid_num */
					line->cid_name,		/* cid_name */
					"code",			/* acctcode */
					line->device->exten,	/* exten */
					"default",		/* context */
					linkedid,		/* linked ID */
					0,			/* amaflag */
					"sccp/%s@%s-%d",	/* format */
					line->name,		/* name */
					line->device->name,	/* name */
					1);			/* callnums */

	if (channel == NULL)
		return NULL;

	channel->tech = &sccp_tech;
	channel->tech_pvt = line;
	line->channel = channel;

	channel->nativeformats = line->device->codecs;
	audio_format = ast_best_codec(channel->nativeformats);

	channel->writeformat = audio_format;
	channel->rawwriteformat = audio_format;
	channel->readformat = audio_format;
	channel->rawreadformat = audio_format;

	ast_module_ref(ast_module_info->self);

	return channel;
}

static enum ast_rtp_glue_result sccp_get_rtp_peer(struct ast_channel *channel, struct ast_rtp_instance **instance)
{
	ast_log(LOG_NOTICE, "sccp_get_rtp_peer\n");

	struct sccp_line *line = channel->tech_pvt;

	if (line == NULL || line->rtp == NULL)
		return AST_RTP_GLUE_RESULT_FORBID;

	ao2_ref(line->rtp, +1);
	*instance = line->rtp;

	return AST_RTP_GLUE_RESULT_LOCAL;
}

static int sccp_set_rtp_peer(struct ast_channel *channel,
				struct ast_rtp_instance *rtp,
				struct ast_rtp_instance *vrtp,
				struct ast_rtp_instance *trtp,
				format_t codecs,
				int nat_active)
{

	return 0;
}

static void start_rtp(struct sccp_line *line)
{
	ast_log(LOG_NOTICE, "start_rtp\n");

	struct ast_codec_pref default_prefs = {0};
	struct sccp_session *session = NULL;
	struct ast_sockaddr bindaddr_tmp;

	session = line->device->session;

	ast_sockaddr_from_sin(&bindaddr_tmp, (struct sockaddr_in *)sccp_srv.res->ai_addr);
	line->rtp = ast_rtp_instance_new("asterisk", sched, &bindaddr_tmp, NULL);

	if (line->rtp) {

		ast_rtp_instance_set_prop(line->rtp, AST_RTP_PROPERTY_RTCP, 1);

		ast_channel_set_fd(line->channel, 0, ast_rtp_instance_fd(line->rtp, 0));
		ast_channel_set_fd(line->channel, 1, ast_rtp_instance_fd(line->rtp, 1));

		ast_rtp_instance_set_qos(line->rtp, 0, 0, "sccp rtp");
		ast_rtp_instance_set_prop(line->rtp, AST_RTP_PROPERTY_NAT, 0);

		ast_rtp_codecs_packetization_set(ast_rtp_instance_get_codecs(line->rtp), line->rtp, &line->codec_pref);
	}

	transmit_connect(line);
}

static void sccp_newcall(struct ast_channel *channel)
{
	struct sccp_line *line = channel->tech_pvt;

	set_line_state(line, SCCP_RINGOUT);
	ast_setstate(channel, AST_STATE_RING);

	transmit_callstate(line->device->session, line->instance, SCCP_RINGOUT, line->callid);
	transmit_tone(line->device->session, SCCP_TONE_ALERT, line->instance, 0);
	transmit_callinfo(line->device->session, "", "", line->device->exten, line->device->exten, line->instance, line->callid, 2);

	ast_copy_string(channel->exten, line->device->exten, sizeof(channel->exten));

	ast_set_callerid(channel,
			line->cid_num,
			line->cid_name,
			NULL);

	ast_party_number_free(&channel->connected.id.number);
	ast_party_number_init(&channel->connected.id.number);
	channel->connected.id.number.valid = 1;
	channel->connected.id.number.str = ast_strdup(channel->exten);
	ast_party_name_free(&channel->connected.id.name);
	ast_party_name_init(&channel->connected.id.name);

	start_rtp(line);
	ast_pbx_start(channel);

	return;
}

static void *sccp_lookup_exten(void *data)
{
	struct ast_channel *channel = NULL;
	struct sccp_line *line = NULL;
	size_t len = 0;

	if (data == NULL)
		return NULL;

	channel = (struct ast_channel*)data;
	line = channel->tech_pvt;
	
	len = strlen(line->device->exten);
	while (line->device->registered == DEVICE_REGISTERED_TRUE &&
		line->state == SCCP_OFFHOOK && len < AST_MAX_EXTENSION-1) {

		if (ast_exists_extension(channel, channel->context, line->device->exten, 1, line->cid_num)) {
			if (!ast_matchmore_extension(channel, channel->context, line->device->exten, 1, line->cid_num)) {

				sccp_newcall(channel);
				return NULL;
			}
		}

		ast_safe_sleep(channel, 500);
		len = strlen(line->device->exten);
	}

	ast_hangup(channel);

	return NULL;
}

static int handle_offhook_message(struct sccp_msg *msg, struct sccp_session *session)
{
	struct sccp_device *device = NULL;
	struct sccp_line *line = NULL;
	struct ast_channel *channel = NULL;
	int ret = 0;

	device = session->device;
	line = device_get_active_line(device);

	if (line && line->state == SCCP_RINGIN) {

		ast_queue_control(line->channel, AST_CONTROL_ANSWER);

		ret = transmit_ringer_mode(session, SCCP_RING_OFF);
		if (ret == -1)
			return -1;

		ret = transmit_callstate(session, line->instance, SCCP_OFFHOOK, 0);
		if (ret == -1)
			return -1;

		ret = transmit_tone(session, SCCP_TONE_NONE, line->instance, 0);
		if (ret == -1)
			return -1;

		ret = transmit_callstate(session, line->instance, SCCP_CONNECTED, 0);
		if (ret == -1)
			return -1;

		start_rtp(line);

		ast_setstate(line->channel, AST_STATE_UP);
		set_line_state(line, SCCP_CONNECTED);

	} else if (line->state == SCCP_ONHOOK) {

		channel = sccp_new_channel(line, NULL);

		ast_setstate(line->channel, AST_STATE_DOWN);
		set_line_state(line, SCCP_OFFHOOK);

		ret = transmit_lamp_indication(session, 1, line->instance, SCCP_LAMP_ON);
		if (ret == -1)
			return -1;

		ret = transmit_callstate(session, line->instance, SCCP_OFFHOOK, 0);
		if (ret == -1)
			return -1;

		ret = transmit_tone(session, SCCP_TONE_DIAL, line->instance, 0);
		if (ret == -1)
			return -1;

		ret = transmit_selectsoftkeys(session, line->instance, 0, KEYDEF_OFFHOOK);
		if (ret == -1)
			return -1;

		if (ast_pthread_create(&device->lookup_thread, NULL, sccp_lookup_exten, channel)) {
			ast_log(LOG_WARNING, "Unable to create switch thread: %s\n", strerror(errno));
			ast_hangup(channel);
		} else {
			device->lookup = 1;
		}
	}
	
	return 0;
}

static int handle_onhook_message(struct sccp_msg *msg, struct sccp_session *session)
{
	struct sccp_line *line = NULL;
	int ret = 0;

	line = device_get_active_line(session->device);

	device_release_line(line->device, line);
	set_line_state(line, SCCP_ONHOOK);

	line->device->exten[0] = '\0';

	/* wait for lookup thread to terminate */
	if (session->device->lookup == 1)
		pthread_join(session->device->lookup_thread, NULL);
	else
		session->device->lookup = 0;

	ret = transmit_callstate(session, line->instance, SCCP_ONHOOK, 0);
	if (ret == -1)
		return -1;

	ret = transmit_selectsoftkeys(session, line->instance, 0, KEYDEF_ONHOOK);
	if (ret == -1)
		return -1;

	if (line->channel != NULL) {

		ret = transmit_close_receive_channel(line);
		if (ret == -1)
			return -1;

		ret = transmit_stop_media_transmission(line);
		if (ret == -1)
			return -1;

		if (line->rtp) {
			ast_rtp_instance_destroy(line->rtp);
			line->rtp = NULL;
		}

		ast_setstate(line->channel, AST_STATE_DOWN);
		ast_queue_hangup(line->channel);
		line->channel->tech_pvt = NULL;
		line->channel = NULL;
		ast_module_unref(ast_module_info->self);
	}

	return 0;
}

static int handle_softkey_event_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;

	ast_log(LOG_NOTICE, "softKeyEvent: %d\n", letohl(msg->data.softkeyevent.softKeyEvent));
	ast_log(LOG_NOTICE, "instance: %d\n", msg->data.softkeyevent.instance);
	ast_log(LOG_NOTICE, "callreference: %d\n", msg->data.softkeyevent.callreference);

	switch (letohl(msg->data.softkeyevent.softKeyEvent)) {
	case SOFTKEY_NONE:
		break;

	case SOFTKEY_REDIAL:
		break;

	case SOFTKEY_NEWCALL:
		ret = transmit_speaker_mode(session, SCCP_SPEAKERON);
		if (ret == -1)
			return -1;

		handle_offhook_message(NULL, session);
		break;

	case SOFTKEY_HOLD:
		break;

	case SOFTKEY_TRNSFER:
		break;

	case SOFTKEY_CFWDALL:
		break;

	case SOFTKEY_CFWDBUSY:
		break;

	case SOFTKEY_CFWDNOANSWER:
		break;

	case SOFTKEY_BKSPC:
		break;

	case SOFTKEY_ENDCALL:
		ret = transmit_speaker_mode(session, SCCP_SPEAKEROFF);
		if (ret == -1)
			return -1;

		ret = transmit_ringer_mode(session, SCCP_RING_OFF);
		if (ret == -1)
			return -1;

		handle_onhook_message(NULL, session);
		break;

	case SOFTKEY_RESUME:
		break;

	case SOFTKEY_ANSWER:
		break;

	case SOFTKEY_INFO:
		break;

	case SOFTKEY_CONFRN:
		break;

	case SOFTKEY_PARK:
		break;

	case SOFTKEY_JOIN:
		break;

	case SOFTKEY_MEETME:
		break;

	case SOFTKEY_PICKUP:
		break;

	case SOFTKEY_GPICKUP:
		break;

	default:
		break;
	}

	return 0;
}

static int handle_softkey_set_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	const struct softkey_definitions *softkeymode = softkey_default_definitions;
	int keyset_count = 0;
	int i = 0;
	int j = 0;
	int ret = 0;

	msg = msg_alloc(sizeof(struct softkey_set_res_message), SOFTKEY_SET_RES_MESSAGE);
	if (msg == NULL)
		return -1;

	keyset_count = sizeof(softkey_default_definitions) / sizeof(struct softkey_definitions);

        msg->data.softkeysets.softKeySetOffset = htolel(0);
        msg->data.softkeysets.softKeySetCount = htolel(keyset_count);
        msg->data.softkeysets.totalSoftKeySetCount = htolel(keyset_count);

	for (i = 0; i < keyset_count; i++) {

		for (j = 0; j < softkeymode->count; j++) {
			msg->data.softkeysets.softKeySetDefinition[softkeymode->mode].softKeyTemplateIndex[j]
				= htolel(softkeymode->defaults[j]);

			msg->data.softkeysets.softKeySetDefinition[softkeymode->mode].softKeyInfoIndex[j]
				= htolel(softkeymode->defaults[j]);
		}
		softkeymode++;
	}

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	ret = transmit_selectsoftkeys(session, 0, 0, KEYDEF_ONHOOK);
	if (ret == -1)
		return -1;

	return 0;
}

static int handle_forward_status_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	uint32_t instance = 0;
	int ret = 0;

	instance = letohl(msg->data.forward.lineNumber);
	ast_log(LOG_NOTICE, "Forward status line %d\n", instance);

	msg = msg_alloc(sizeof(struct forward_status_res_message), FORWARD_STATUS_RES_MESSAGE);
	if (msg == NULL)
		return -1;

	msg->data.forwardstatus.status = 0;
	msg->data.forwardstatus.lineNumber = htolel(instance);
	
	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

enum sccp_codecs {
	SCCP_CODEC_ALAW = 2,
	SCCP_CODEC_ULAW = 4,
	SCCP_CODEC_G723_1 = 9,
	SCCP_CODEC_G729A = 12,
	SCCP_CODEC_G726_32 = 82,
	SCCP_CODEC_H261 = 100,
	SCCP_CODEC_H263 = 101
};

int codec_ast2sccp(format_t astcodec)
{
        switch (astcodec) {
        case AST_FORMAT_ALAW:
                return SCCP_CODEC_ALAW;
        case AST_FORMAT_ULAW:
                return SCCP_CODEC_ULAW;
        case AST_FORMAT_G723_1:
                return SCCP_CODEC_G723_1;
        case AST_FORMAT_G729A:
                return SCCP_CODEC_G729A;
        case AST_FORMAT_G726_AAL2:
                return SCCP_CODEC_G726_32;
        case AST_FORMAT_H261:
                return SCCP_CODEC_H261;
        case AST_FORMAT_H263:
                return SCCP_CODEC_H263;
        default:
                return 0;
        }
}

static format_t codec_sccp2ast(enum sccp_codecs sccp_codec)
{
	switch (sccp_codec) {
	case SCCP_CODEC_ALAW:
		return AST_FORMAT_ALAW;
	case SCCP_CODEC_ULAW:
		return AST_FORMAT_ULAW;
	case SCCP_CODEC_G723_1:
		return AST_FORMAT_G723_1;
	case SCCP_CODEC_G729A:
		return AST_FORMAT_G729A;
	case SCCP_CODEC_H261:
		return AST_FORMAT_H261;
	case SCCP_CODEC_H263:
		return AST_FORMAT_H263;
	default:
		return 0;
	}
}

static int handle_capabilities_res_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int count = 0;
	int sccp_codec = 0;
	int i = 0;
	struct sccp_device *device = NULL;

	device = session->device;

	count = letohl(msg->data.caps.count);
	ast_log(LOG_NOTICE, "Received %d capabilities\n", count);

	if (count > SCCP_MAX_CAPABILITIES) {
		count = SCCP_MAX_CAPABILITIES;
		ast_log(LOG_WARNING, "Received more capabilities (%d) than we can handle (%d)\n", count, SCCP_MAX_CAPABILITIES);
	}

	for (i = 0; i < count; i++) {

		/* get the device supported codecs */
		sccp_codec = letohl(msg->data.caps.caps[i].codec);

		/* translate to asterisk format */
		device->codecs |= codec_sccp2ast(sccp_codec);
	}

	return 0;
}

static int handle_open_receive_channel_ack_message(struct sccp_msg *msg, struct sccp_session *session)
{
	struct sccp_line *line = NULL;
	struct ast_format_list fmt = {0};
	struct sockaddr_in remote = {0};
	struct sockaddr_in local = {0};
	struct ast_sockaddr remote_tmp;
	struct ast_sockaddr local_tmp;
	uint32_t addr = 0;
	uint32_t port = 0;
	uint32_t passthruid = 0;
	int ret = 0;

	line = device_get_active_line(session->device);

	addr = msg->data.openreceivechannelack.ipAddr;
	port = letohl(msg->data.openreceivechannelack.port);
	passthruid = letohl(msg->data.openreceivechannelack.passThruId);

	remote.sin_family = AF_INET;
	remote.sin_addr.s_addr = addr;
	remote.sin_port = htons(port);

	ast_sockaddr_from_sin(&remote_tmp, &remote);
	ast_rtp_instance_set_remote_address(line->rtp, &remote_tmp);

	ast_rtp_instance_get_local_address(line->rtp, &local_tmp);
	ast_sockaddr_to_sin(&local_tmp, &local);

	fmt = ast_codec_pref_getsize(&line->codec_pref, ast_best_codec(line->device->codecs));
	msg = msg_alloc(sizeof(struct start_media_transmission_message), START_MEDIA_TRANSMISSION_MESSAGE);
	if (msg == NULL)
		return -1;

	msg->data.startmedia.conferenceId = htolel(0);
	msg->data.startmedia.passThruPartyId = htolel(line->callid ^ 0xFFFFFFFF);
	msg->data.startmedia.remoteIp = htolel(local.sin_addr.s_addr);
	msg->data.startmedia.remotePort = htolel(ntohs(local.sin_port));
	msg->data.startmedia.packetSize = htolel(fmt.cur_ms);
	msg->data.startmedia.payloadType = htolel(codec_ast2sccp(fmt.bits));
	msg->data.startmedia.qualifier.precedence = htolel(127);
	msg->data.startmedia.qualifier.vad = htolel(0);
	msg->data.startmedia.qualifier.packets = htolel(0);
	msg->data.startmedia.qualifier.bitRate = htolel(0);
	msg->data.startmedia.conferenceId1 = htolel(0);
	msg->data.startmedia.rtpTimeout = htolel(10);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

static int handle_line_status_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int line_instance;
	struct sccp_line *line;
	int ret = 0;

	line_instance = letohl(msg->data.line.lineNumber);

	line = device_get_line(session->device, line_instance);
	if (line == NULL) {
		ast_log(LOG_ERROR, "Line instance [%d] is not attached to device [%s]\n", line_instance, session->device->name);
		return -1;
	}

	msg = msg_alloc(sizeof(struct line_status_res_message), LINE_STATUS_RES_MESSAGE);
	if (msg == NULL)
		return -1;

	msg->data.linestatus.lineNumber = letohl(line_instance);

	memcpy(msg->data.linestatus.lineDirNumber, line->name, sizeof(msg->data.linestatus.lineDirNumber));
	memcpy(msg->data.linestatus.lineDisplayName, session->device->name, sizeof(msg->data.linestatus.lineDisplayName));
	memcpy(msg->data.linestatus.lineDisplayAlias, line->name, sizeof(msg->data.linestatus.lineDisplayAlias));

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	msg = msg_alloc(sizeof(struct forward_status_res_message), FORWARD_STATUS_RES_MESSAGE);
	if (msg == NULL)
		return -1;

	msg->data.forwardstatus.status = 0;
	msg->data.forwardstatus.lineNumber = htolel(line_instance);
	
	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0; 
}

static int handle_register_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;

	ret = device_type_is_supported(msg->data.reg.type);
	if (ret == 0) {
		ast_log(LOG_ERROR, "Rejecting [%s], unsupported device type [%d]\n", msg->data.reg.name, msg->data.reg.type);
		msg = msg_alloc(sizeof(struct register_rej_message), REGISTER_REJ_MESSAGE);

		if (msg == NULL) {
			return -1;
		}

		snprintf(msg->data.regrej.errMsg, sizeof(msg->data.regrej.errMsg), "Unsupported device type [%d]\n", msg->data.reg.type);
		ret = transmit_message(msg, session);
		if (ret == -1)
			return -1;

		return 0;
	}

	ret = register_device(msg, session);
	if (ret <= 0) {
		ast_log(LOG_ERROR, "Rejecting device [%s]\n", msg->data.reg.name);
		msg = msg_alloc(sizeof(struct register_rej_message), REGISTER_REJ_MESSAGE);

		if (msg == NULL) {
			return -1;
		}

		snprintf(msg->data.regrej.errMsg, sizeof(msg->data.regrej.errMsg), "Access denied: %s\n", msg->data.reg.name);
		ret = transmit_message(msg, session);
		if (ret == -1)
			return -1;

		return 0;
	}

	msg = msg_alloc(sizeof(struct register_ack_message), REGISTER_ACK_MESSAGE);
	if (msg == NULL) {
		return -1;
	}

        msg->data.regack.keepAlive = htolel(sccp_cfg.keepalive);
        memcpy(msg->data.regack.dateTemplate, sccp_cfg.dateformat, sizeof(msg->data.regack.dateTemplate));

	if (session->device->protoVersion <= 3) {

		msg->data.regack.protoVersion = 3;

		msg->data.regack.unknown1 = 0x00;
		msg->data.regack.unknown2 = 0x00;
		msg->data.regack.unknown3 = 0x00;

	} else if (session->device->protoVersion <= 10) {

		msg->data.regack.protoVersion = session->device->protoVersion;

		msg->data.regack.unknown1 = 0x20;
		msg->data.regack.unknown2 = 0x00;
		msg->data.regack.unknown3 = 0xFE;

	} else if (session->device->protoVersion >= 11) {

		msg->data.regack.protoVersion = 11;

		msg->data.regack.unknown1 = 0x20;
		msg->data.regack.unknown2 = 0xF1;
		msg->data.regack.unknown3 = 0xFF;
	}

        msg->data.regack.secondaryKeepAlive = htolel(sccp_cfg.keepalive);

        ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	msg = msg_alloc(0, CAPABILITIES_REQ_MESSAGE);
	if (msg == NULL) {
		return -1;
	}

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1; 

	return 0;
}

static int handle_ipport_message(struct sccp_msg *msg, struct sccp_session *session)
{
	session->device->station_port = msg->data.ipport.stationIpPort;
	return 0;
}

static int handle_keypad_button_message(struct sccp_msg *msg, struct sccp_session *session)
{
	struct sccp_line *line = NULL;
	struct ast_frame frame = { AST_FRAME_DTMF, };

	char digit;
	int button;
	int instance;
	int callId;
	size_t len;
	int ret = 0;

	button = letohl(msg->data.keypad.button);
	instance = letohl(msg->data.keypad.instance);
	callId = letohl(msg->data.keypad.callId);

	line = device_get_line(session->device, instance);
	if (line == NULL) {
		ast_log(LOG_WARNING, "Device [%s] has no line instance [%d]\n", session->device->name, instance);
		return 0;
	}

	if (button == 14) {
		digit = '*';
	} else if (button == 15) {
		digit = '#';
	} else if (button >= 0 && button <= 9) {
		digit = '0' + button;
	} else {
		digit = '0' + button;
		ast_log(LOG_WARNING, "Unsupported digit %d\n", button);
	}

	if (line->state == SCCP_CONNECTED) {

		frame.subclass.integer = digit;
		frame.src = "sccp";
		frame.len = 100;
		frame.offset = 0;
		frame.datalen = 0;

		ast_queue_frame(line->channel, &frame);

	} else if (line->state == SCCP_OFFHOOK) {

		len = strlen(line->device->exten);
		if (len < sizeof(line->device->exten) - 1) {
			line->device->exten[len] = digit;
			line->device->exten[len+1] = '\0';
		}

		ret = transmit_tone(session, SCCP_TONE_NONE, line->instance, 0);
		if (ret == -1)
			return -1;
	}

	return 0;
}

static void destroy_session(struct sccp_session **session)
{
	ast_mutex_destroy(&(*session)->lock);
	ast_free((*session)->ipaddr);
	close((*session)->sockfd);
	ast_free(*session);
}

static int handle_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;

	/* Prevent unregistered phone from sending non-registering messages */
	if ((session->device == NULL ||
		(session->device != NULL && session->device->registered == DEVICE_REGISTERED_FALSE)) &&
		(msg->id != REGISTER_MESSAGE && msg->id != ALARM_MESSAGE)) {

			ast_log(LOG_ERROR, "Session from [%s::%d] sending non-registering messages\n",
						session->ipaddr, session->sockfd);
			return -1;
	}

	switch (msg->id) {
	case KEEP_ALIVE_MESSAGE:
		ast_log(LOG_DEBUG, "Keep alive message\n");
		ret = handle_keep_alive_message(msg, session);
		break;

	case REGISTER_MESSAGE:
		ast_log(LOG_DEBUG, "Register message\n");
		ret = handle_register_message(msg, session);
		break;

	case IP_PORT_MESSAGE:
		ast_log(LOG_DEBUG, "Ip port message\n");
		ret = handle_ipport_message(msg, session);
		break;

	case KEYPAD_BUTTON_MESSAGE:
		ast_log(LOG_DEBUG, "keypad button message\n");
		ret = handle_keypad_button_message(msg, session);
		break;

	case OFFHOOK_MESSAGE:
		ast_log(LOG_DEBUG, "Offhook message\n");
		ret = handle_offhook_message(msg, session);
		break;

	case ONHOOK_MESSAGE:
		ast_log(LOG_DEBUG, "Onhook message\n");
		ret = handle_onhook_message(msg, session);
		break;

	case FORWARD_STATUS_REQ_MESSAGE:
		ast_log(LOG_DEBUG, "Forward status message\n");
		ret = handle_forward_status_req_message(msg, session);
		break;

	case CAPABILITIES_RES_MESSAGE:
		ast_log(LOG_DEBUG, "Capabilities message\n");
		ret = handle_capabilities_res_message(msg, session);
		break;

	case LINE_STATUS_REQ_MESSAGE:
		ast_log(LOG_DEBUG, "Line status message\n");
		ret = handle_line_status_req_message(msg, session);
		break;

	case CONFIG_STATUS_REQ_MESSAGE:
		ast_log(LOG_DEBUG, "Config status message\n");
		ret = handle_config_status_req_message(msg, session);
		break;

	case TIME_DATE_REQ_MESSAGE:
		ast_log(LOG_DEBUG, "Time date message\n");
		ret = handle_time_date_req_message(msg, session);
		break;

	case BUTTON_TEMPLATE_REQ_MESSAGE:
		ast_log(LOG_DEBUG, "Button template request message\n");
		ret = handle_button_template_req_message(msg, session);
		break;

	case SOFTKEY_TEMPLATE_REQ_MESSAGE:
		ast_log(LOG_DEBUG, "Softkey template request message\n");
		ret = handle_softkey_template_req_message(msg, session);
		break;

	case ALARM_MESSAGE:
		ast_log(LOG_DEBUG, "Alarm message: %s\n", msg->data.alarm.displayMessage);
		break;

	case SOFTKEY_EVENT_MESSAGE:
		ast_log(LOG_DEBUG, "Softkey event message\n");
		ret = handle_softkey_event_message(msg, session);
		break;

	case OPEN_RECEIVE_CHANNEL_ACK_MESSAGE:
		ast_log(LOG_DEBUG, "Open receive channel ack message\n");
		ret = handle_open_receive_channel_ack_message(msg, session);
		break;

	case SOFTKEY_SET_REQ_MESSAGE:
		ast_log(LOG_DEBUG, "Softkey set request message\n");
		ret = handle_softkey_set_req_message(msg, session);
		break;

	case REGISTER_AVAILABLE_LINES_MESSAGE:
		ast_log(LOG_DEBUG, "Register available lines message\n");
		break;

	case START_MEDIA_TRANSMISSION_ACK_MESSAGE:
		ast_log(LOG_DEBUG, "Start media transmission ack message\n");
		break;

	case ACCESSORY_STATUS_MESSAGE:
		break;

	default:
		ast_log(LOG_DEBUG, "Unknown message %x\n", msg->id);
		break;
	}

	return ret;
}

static int fetch_data(struct sccp_session *session)
{
	struct pollfd fds[1] = {0};
	int nfds = 0;
	time_t now = 0;
	ssize_t nbyte = 0;
	int msg_len = 0;
	
	time(&now);
	/* if no device or device is not registered and time has elapsed */
	if ((session->device == NULL || (session->device != NULL && session->device->registered == DEVICE_REGISTERED_FALSE))
		&& now > session->start_time + sccp_cfg.authtimeout) {
		ast_log(LOG_WARNING, "Time has elapsed [%d]\n", sccp_cfg.authtimeout);
		return -1;
	}

	fds[0].fd = session->sockfd;
	fds[0].events = POLLIN;
	fds[0].revents = 0;

	/* wait N times the keepalive frequence */
	nfds = ast_poll(fds, 1, sccp_cfg.keepalive * 1000 * 2);
	if (nfds == -1) { /* something wrong happend */
		ast_log(LOG_WARNING, "Failed to poll socket: %s\n", strerror(errno));
		return -1;

	} else if (nfds == 0) { /* the file descriptor is not ready */
		ast_log(LOG_WARNING, "Device has timed out\n");
		return -1;

	} else if (fds[0].revents & POLLERR || fds[0].revents & POLLHUP) {
		ast_log(LOG_WARNING, "Device has closed the connection\n");
		return -1;

	} else if (fds[0].revents & POLLIN || fds[0].revents & POLLPRI) {

		/* fetch the field that contain the packet length */
		nbyte = read(session->sockfd, session->inbuf, 4);
		ast_log(LOG_DEBUG, "nbyte %d\n", nbyte);
		if (nbyte < 0) { /* something wrong happend */
			ast_log(LOG_WARNING, "Failed to read socket: %s\n", strerror(errno));
			return -1;

		} else if (nbyte == 0) { /* EOF */
			ast_log(LOG_NOTICE, "Device has closed the connection\n");
			return -1;

		} else if (nbyte < 4) {
			ast_log(LOG_WARNING, "Client sent less data than expected. Expected at least 4 bytes but got %d\n", nbyte);
			return -1;
		}

		msg_len = letohl(*((int *)session->inbuf));
		ast_log(LOG_DEBUG, "msg_len %d\n", msg_len);
		if (msg_len > SCCP_MAX_PACKET_SZ || msg_len < 0) {
			ast_log(LOG_WARNING, "Packet length is out of bounds: 0 > %d > %d\n", msg_len, SCCP_MAX_PACKET_SZ);
			return -1;
		}

		/* bypass the length field and fetch the payload */
		nbyte = read(session->sockfd, session->inbuf+4, msg_len+4);
		ast_log(LOG_DEBUG, "nbyte %d\n", nbyte);
		if (nbyte < 0) {
			ast_log(LOG_WARNING, "Failed to read socket: %s\n", strerror(errno));
			return -1;

		} else if (nbyte == 0) { /* EOF */
			ast_log(LOG_NOTICE, "Device has closed the connection\n");
			return -1;
		}

		return nbyte;
	}

	return -1;	
}

static void *thread_session(void *data)
{
	struct sccp_session *session = data;
	struct sccp_msg *msg = NULL;
	int connected = 1;
	int ret = 0;

	while (connected) {

		ret = fetch_data(session);
		if (ret > 0) {
			msg = (struct sccp_msg *)session->inbuf;
			ret = handle_message(msg, session);
			/* take it easy, prevent DoS attack */
			usleep(100000);
		}

		if (ret == -1) {
			AST_LIST_LOCK(&list_session);
			session = AST_LIST_REMOVE(&list_session, session, list);
			AST_LIST_UNLOCK(&list_session);	

			if (session->device) {
				ast_log(LOG_ERROR, "Disconnecting device [%s]\n", session->device->name);
				device_unregister(session->device);
			}

			connected = 0;
		}
	}

	if (session)
		destroy_session(&session);

	return 0;
}

static void *thread_accept(void *data)
{
	int new_sockfd = 0;
	struct sockaddr_in addr = {0};
	struct sccp_session *session = NULL;
	socklen_t addrlen = 0;
	int flag_nodelay = 1;

	while (1) {

		addrlen = sizeof(addr);
		new_sockfd = accept(sccp_srv.sockfd, (struct sockaddr *)&addr, &addrlen);
		if (new_sockfd == -1) {
			ast_log(LOG_ERROR, "Failed to accept new connection: %s... "
						"the main thread is going down now\n", strerror(errno));
			return NULL;
		}

		/* send multiple buffers as individual packets */
		setsockopt(new_sockfd, IPPROTO_TCP, TCP_NODELAY, &flag_nodelay, sizeof(flag_nodelay));

		/* session constructor */	
		session = ast_calloc(1, sizeof(struct sccp_session));
		if (session == NULL) {
			close(new_sockfd);
			continue;
		}

		session->tid = AST_PTHREADT_NULL; 
		session->sockfd = new_sockfd;
		session->ipaddr = ast_strdup(ast_inet_ntoa(addr.sin_addr));
		ast_mutex_init(&session->lock);
		time(&session->start_time);
	
		AST_LIST_LOCK(&list_session);
		AST_LIST_INSERT_HEAD(&list_session, session, list);
		AST_LIST_UNLOCK(&list_session);

		ast_log(LOG_NOTICE, "A new device has connected from: %s\n", session->ipaddr);
		ast_pthread_create_background(&session->tid, NULL, thread_session, session);
	}
}

static int sccp_devicestate(void *data)
{
	ast_log(LOG_NOTICE, "sccp devicestate %s\n", data);

	int state = AST_DEVICE_UNKNOWN;

	state = AST_DEVICE_NOT_INUSE;

	return state;
}

static struct ast_channel *sccp_request(const char *type, format_t format, const struct ast_channel *requestor, void *destination, int *cause)
{
	ast_log(LOG_NOTICE, "sccp request\n");

	struct sccp_line *line = NULL;
	struct ast_channel *channel = NULL;

	ast_log(LOG_NOTICE, "type: %s "
				"format: %s "
				"destination: %s "
				"cause: %d\n",
				type, ast_getformatname(format), (char *)destination, *cause);

	line = find_line_by_name((char *)destination);

	if (line == NULL) {
		ast_log(LOG_NOTICE, "This line doesn't exist: %s\n", (char *)destination);
		*cause = AST_CAUSE_UNREGISTERED;
		return NULL;
	}

	if (line->device->registered == DEVICE_REGISTERED_FALSE) {
		ast_log(LOG_NOTICE, "Line [%s] belong to an unregistered device [%s]\n", line->name, line->device->name);
		*cause = AST_CAUSE_UNREGISTERED;
		return NULL;
	}

	if (line->state != SCCP_ONHOOK || line->channel != NULL) {
		*cause = AST_CAUSE_BUSY;
		return NULL;
	}

	channel = sccp_new_channel(line, requestor ? requestor->linkedid : NULL);

	if (line->channel)
		ast_setstate(line->channel, AST_STATE_DOWN);

	return channel;
}

static int sccp_call(struct ast_channel *channel, char *dest, int timeout)
{
	ast_log(LOG_NOTICE, "sccp call\n");

	struct sccp_line *line = NULL;
	struct sccp_device *device = NULL;
	struct sccp_session *session = NULL;
	int ret = 0;

	line = channel->tech_pvt;
	if (line == NULL)
		return -1;

	device = line->device;
	if (device == NULL) {
		ast_log(LOG_ERROR, "Line [%s] is attached to no device\n", device->name);
		return -1;
	}

	session = device->session;
	if (session == NULL) {
		ast_log(LOG_ERROR, "Device [%s] has no active session\n", device->name);
		return -1;
	}

	if (line->state != SCCP_ONHOOK) {
		channel->hangupcause = AST_CONTROL_BUSY;
		ast_setstate(channel, AST_CONTROL_BUSY);
		ast_queue_control(channel, AST_CONTROL_BUSY);
		return 0;
	}

	device_enqueue_line(device, line);
	line->channel = channel;

	ret = transmit_callstate(session, line->instance, SCCP_RINGIN, line->callid);
	if (ret == -1)
		return -1;

	ret = transmit_selectsoftkeys(session, line->instance, line->callid, KEYDEF_RINGIN);
	if (ret == -1)
		return -1;

	ret = transmit_callinfo(session, channel->connected.id.name.str,
					channel->connected.id.number.str,
					line->cid_name,
					line->cid_num,
					line->instance,
					line->callid, 1);
	if (ret == -1)
		return -1;

	ret = transmit_lamp_indication(session, STIMULUS_LINE, line->instance, SCCP_LAMP_BLINK);
	if (ret == -1)
		return -1;

	if (device->active_line == NULL) {
		ret = transmit_ringer_mode(session, SCCP_RING_INSIDE);
		if (ret == -1)
			return -1;
	}

	set_line_state(line, SCCP_RINGIN);
	ast_setstate(channel, AST_STATE_RINGING);
	ast_queue_control(channel, AST_CONTROL_RINGING);

	return 0;
}

static int sccp_hangup(struct ast_channel *channel)
{
	ast_log(LOG_NOTICE, "sccp hangup\n");

	struct sccp_line *line = NULL;
	int ret = 0;

	line = channel->tech_pvt;
	if (line == NULL)
		return -1;

	if (line->state == SCCP_RINGIN || line->state == SCCP_CONNECTED) {

		device_release_line(line->device, line);
		set_line_state(line, SCCP_ONHOOK);

		if (line->device->active_line_cnt <= 1)
			ret = transmit_ringer_mode(line->device->session, SCCP_RING_OFF);

		ret = transmit_lamp_indication(line->device->session, 1, line->instance, SCCP_LAMP_OFF);
		if (ret == -1)
			return -1;

		ret = transmit_callstate(line->device->session, line->instance, SCCP_ONHOOK, line->callid);
		if (ret == -1)
			return -1;

		ret = transmit_tone(line->device->session, SCCP_TONE_NONE, line->instance, 0);
		if (ret == -1)
			return -1;

		ret = transmit_selectsoftkeys(line->device->session, line->instance, 0, KEYDEF_ONHOOK);
		if (ret == -1)
			return -1;

	} /*XXX else if (line->state == SCCP_CONNECTED) {

		set_line_state(line, SCCP_INVALID);

		transmit_ringer_mode(line->device->session, SCCP_RING_OFF);
		transmit_tone(line->device->session, SCCP_TONE_BUSY, line->instance, 0);

		ret = transmit_lamp_indication(line->device->session, 1, line->instance, SCCP_LAMP_OFF);
		if (ret == -1)
			return -1;

		ret = transmit_callstate(line->device->session, line->instance, SCCP_ONHOOK, line->callid);
		if (ret == -1)
			return -1;

		ret = transmit_selectsoftkeys(line->device->session, line->instance, 0, KEYDEF_ONHOOK);
		if (ret == -1)
			return -1;
	}*/

	if (line->channel != NULL) {

		transmit_close_receive_channel(line);
		transmit_stop_media_transmission(line);

		if (line->rtp) {
			ast_rtp_instance_destroy(line->rtp);
			line->rtp = NULL;
		}

		ast_queue_hangup(line->channel);
		channel->tech_pvt = NULL;
		line->channel = NULL;
		ast_module_unref(ast_module_info->self);
	}

	return 0;
}

static int sccp_answer(struct ast_channel *channel)
{
	ast_log(LOG_NOTICE, "sccp answer\n");

	struct sccp_line *line = NULL;
	line = channel->tech_pvt;

	if (line->rtp == NULL) {
		start_rtp(line);
	}

	transmit_tone(line->device->session, SCCP_TONE_NONE, line->instance, 0);

	ast_setstate(channel, AST_STATE_UP);
	set_line_state(line, SCCP_CONNECTED);

	return 0;
}

static struct ast_frame *sccp_read(struct ast_channel *channel)
{
	struct sccp_line *line = NULL;
	struct ast_frame *frame = NULL;

	line = channel->tech_pvt;

	if (line == NULL) {
		ast_log(LOG_ERROR, "Invalid line\n");
		return &ast_null_frame;
	}

	if (line->rtp == NULL) {
		ast_log(LOG_ERROR, "Invalid RTP\n");
		return &ast_null_frame;
	}

	switch (channel->fdno) {
	case 0:
		frame = ast_rtp_instance_read(line->rtp, 0);
		break;

	case 1:
		frame = ast_rtp_instance_read(line->rtp, 1);
		break;

	default:
		frame = &ast_null_frame;
	}

	if (frame->frametype == AST_FRAME_VOICE) {
		if (frame->subclass.codec != channel->nativeformats) {
			channel->nativeformats = frame->subclass.codec;
			ast_set_read_format(channel, channel->readformat);
			ast_set_write_format(channel, channel->writeformat);
		}
	}

	return frame;
}

static int sccp_write(struct ast_channel *channel, struct ast_frame *frame)
{
//	ast_log(LOG_NOTICE, "sccp_write\n");

	int res = 0;
	struct sccp_line *line = channel->tech_pvt;

	if (line == NULL)
		return res;

	if (line->rtp != NULL && line->state == SCCP_CONNECTED) {
		res = ast_rtp_instance_write(line->rtp, frame);
	}

	return res;
}

static int sccp_indicate(struct ast_channel *channel, int indicate, const void *data, size_t datalen)
{
	ast_log(LOG_NOTICE, "sccp indicate\n");

	struct sccp_line *line = channel->tech_pvt;

	switch (indicate) {
	case AST_CONTROL_HANGUP:
		ast_log(LOG_DEBUG, "hangup\n");
		break;

	case AST_CONTROL_RING:
		ast_log(LOG_DEBUG, "ring\n");
		break;

	case AST_CONTROL_RINGING:
		ast_log(LOG_DEBUG, "ringing\n");
		break;

	case AST_CONTROL_ANSWER:
		ast_log(LOG_DEBUG, "answer\n");
		break;

	case AST_CONTROL_BUSY:

		transmit_ringer_mode(line->device->session, SCCP_RING_OFF);
		transmit_tone(line->device->session, SCCP_TONE_BUSY, line->instance, 0);

		ast_log(LOG_DEBUG, "busy\n");
		break;

	case AST_CONTROL_TAKEOFFHOOK:
		ast_log(LOG_DEBUG, "takeoffhook\n");
		break;

	case AST_CONTROL_OFFHOOK:
		ast_log(LOG_DEBUG, "offhook\n");
		break;

	case AST_CONTROL_CONGESTION:

		transmit_ringer_mode(line->device->session, SCCP_RING_OFF);
		transmit_tone(line->device->session, SCCP_TONE_BUSY, line->instance, 0);

		ast_log(LOG_DEBUG, "congestion\n");
		break;

	case AST_CONTROL_FLASH:
		ast_log(LOG_DEBUG, "flash\n");
		break;

	case AST_CONTROL_WINK:
		ast_log(LOG_DEBUG, "wink\n");
		break;

	case AST_CONTROL_OPTION:
		ast_log(LOG_DEBUG, "option\n");
		break;

	case AST_CONTROL_RADIO_KEY:
		ast_log(LOG_DEBUG, "radio key\n");
		break;

	case AST_CONTROL_RADIO_UNKEY:
		ast_log(LOG_DEBUG, "radio unkey\n");
		break;

	case AST_CONTROL_PROGRESS:
		ast_log(LOG_DEBUG, "progress\n");
		break;

	case AST_CONTROL_PROCEEDING:
		ast_log(LOG_DEBUG, "proceeding\n");
		break;

	case AST_CONTROL_HOLD:
		ast_log(LOG_DEBUG, "hold\n");
		break;

	case AST_CONTROL_UNHOLD:
		ast_log(LOG_DEBUG, "unhold\n");
		break;

	case AST_CONTROL_VIDUPDATE:
		ast_log(LOG_DEBUG, "vid update\n");
		break;

	case AST_CONTROL_SRCUPDATE:
		ast_log(LOG_DEBUG, "src update\n");
		break;

	case AST_CONTROL_SRCCHANGE:
		ast_log(LOG_DEBUG, "src change\n");
		break;

	case AST_CONTROL_END_OF_Q:
		ast_log(LOG_DEBUG, "end of q\n");
		break;

	default:
		break;
	}

	return 0;
}

static int sccp_fixup(struct ast_channel *oldchannel, struct ast_channel *newchannel)
{
	ast_log(LOG_NOTICE, "sccp fixup\n");
	struct sccp_line *line = newchannel->tech_pvt;
	line->channel = newchannel;
	return 0;
}

static int sccp_senddigit_begin(struct ast_channel *ast, char digit)
{
	ast_log(LOG_NOTICE, "sccp senddigit begin\n");
	return 0;
}

static int sccp_senddigit_end(struct ast_channel *ast, char digit, unsigned int duration)
{
	ast_log(LOG_NOTICE, "sccp senddigit end\n");
	return 0;
}

void sccp_server_fini()
{
	struct sccp_session *session_itr = NULL;

	ast_channel_unregister(&sccp_tech);

	pthread_cancel(sccp_srv.thread_accept);
	pthread_kill(sccp_srv.thread_accept, SIGURG);
	pthread_join(sccp_srv.thread_accept, NULL);

	AST_LIST_TRAVERSE_SAFE_BEGIN(&list_session, session_itr, list) {
		if (session_itr != NULL) {

			ast_log(LOG_NOTICE, "Session del %s\n", session_itr->ipaddr);
			AST_LIST_REMOVE_CURRENT(list);

			pthread_cancel(session_itr->tid);
			pthread_kill(session_itr->tid, SIGURG);
			pthread_join(session_itr->tid, NULL);

			destroy_session(&session_itr);
		}
	}
	AST_LIST_TRAVERSE_SAFE_END;

	freeaddrinfo(sccp_srv.res);
	shutdown(sccp_srv.sockfd, SHUT_RDWR);
}

void sccp_rtp_fini()
{
	ast_rtp_glue_unregister(&sccp_rtp_glue);
}

void sccp_rtp_init(struct ast_module_info *module_info)
{
	ast_module_info = module_info;
	ast_rtp_glue_register(&sccp_rtp_glue);
}

int sccp_server_init(void)
{
	struct addrinfo hints = {0};
	const int flag_reuse = 1;
	int ret = 0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICHOST;

	getaddrinfo(sccp_cfg.bindaddr, SCCP_PORT, &hints, &sccp_srv.res);

	sccp_srv.sockfd = socket(sccp_srv.res->ai_family, sccp_srv.res->ai_socktype, sccp_srv.res->ai_protocol);
	setsockopt(sccp_srv.sockfd, SOL_SOCKET, SO_REUSEADDR, &flag_reuse, sizeof(flag_reuse));

	ret = bind(sccp_srv.sockfd, sccp_srv.res->ai_addr, sccp_srv.res->ai_addrlen);
	if (ret == -1) {
		ast_log(LOG_ERROR, "Failed to bind socket: %s\n", strerror(errno));
		return -1;
	}

	ret = listen(sccp_srv.sockfd, SCCP_BACKLOG);
	if (ret == -1) {
		ast_log(LOG_ERROR, "Failed to listen socket: %s\n", strerror(errno));
		return -1;
	}

	sched = sched_context_create();
	if (sched == NULL) {
		ast_log(LOG_ERROR, "Unable to create schedule context\n");
	}

	ast_channel_register(&sccp_tech);
	ast_pthread_create_background(&sccp_srv.thread_accept, NULL, thread_accept, NULL);

	return 0;
}
