#include <errno.h>
#include <string.h>

#include <asterisk.h>
#include <asterisk/utils.h>
#include <asterisk/rtp.h>

#include "device.h"
#include "message.h"
#include "sccp.h"
#include "utils.h"

struct sccp_msg *msg_alloc(size_t data_length, int message_id)
{
	struct sccp_msg *msg = NULL;

	msg = ast_calloc(1, 12 + 4 + data_length);	
	if (msg == NULL) {
		ast_log(LOG_ERROR, "Memory allocation failed\n");
		return NULL;
	}

	msg->length = htolel(4 + data_length);
	msg->id = message_id;

	return msg;
}

int transmit_message(struct sccp_msg *msg, struct sccp_session *session)
{
	ssize_t nbyte = 0;

	memcpy(session->outbuf, msg, 12);
	memcpy(session->outbuf+12, &msg->data, letohl(msg->length));

	nbyte = write(session->sockfd, session->outbuf, letohl(msg->length)+8);	
	if (nbyte == -1) {
		ast_log(LOG_ERROR, "Message transmit failed %s\n", strerror(errno));
	}

	ast_log(LOG_DEBUG, "write %d bytes\n", nbyte);
	ast_free(msg);

	return nbyte;
}

int transmit_connect(struct sccp_line *line)
{
	struct ast_format_list fmt = {0};
	struct sccp_msg *msg = NULL;
	int ret = 0;

	/* FIXME `fmt' must be per device */
	struct ast_codec_pref default_prefs;
	ast_parse_allow_disallow(&default_prefs, &line->device->ast_codec, "all", 1);
	fmt = ast_codec_pref_getsize(&default_prefs, ast_best_codec(line->device->ast_codec));

	if (line->device->protoVersion >= 17) {

		msg = msg_alloc(sizeof(struct open_receive_channel_message_v17), OPEN_RECEIVE_CHANNEL_MESSAGE);
		if (msg == NULL)
			return -1;

		msg->data.openreceivechannel_v17.conferenceId = htolel(0);
		msg->data.openreceivechannel_v17.partyId = htolel(0);
		msg->data.openreceivechannel_v17.packets = htolel(fmt.cur_ms);
		msg->data.openreceivechannel_v17.capability = htolel(codec_ast2sccp(fmt.bits));
		msg->data.openreceivechannel_v17.echo = htolel(0);
		msg->data.openreceivechannel_v17.bitrate = htolel(0);
		msg->data.openreceivechannel_v17.conferenceId1 = htolel(0);
		msg->data.openreceivechannel_v17.rtpTimeout = htolel(0);
		msg->data.openreceivechannel_v17.unknown2 = htolel(4000);

	} else {

		msg = msg_alloc(sizeof(struct open_receive_channel_message), OPEN_RECEIVE_CHANNEL_MESSAGE);
		if (msg == NULL)
			return -1;

		msg->data.openreceivechannel.conferenceId = htolel(0);
		msg->data.openreceivechannel.partyId = htolel(0);
		msg->data.openreceivechannel.packets = htolel(fmt.cur_ms);
		msg->data.openreceivechannel.capability = htolel(codec_ast2sccp(fmt.bits));
		msg->data.openreceivechannel.echo = htolel(0);
		msg->data.openreceivechannel.bitrate = htolel(0);
	}

	ret = transmit_message(msg, (struct sccp_session *)line->device->session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_callstate(struct sccp_session *session, int instance, int state, unsigned callid)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	msg = msg_alloc(sizeof(struct call_state_message), CALL_STATE_MESSAGE);
	if (msg == NULL)
		return -1;

	msg->data.callstate.callState = htolel(state);
	msg->data.callstate.lineInstance = htolel(instance);
	msg->data.callstate.callReference = htolel(callid);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_displaymessage(struct sccp_session *session, const char *text, int instance, int reference)
{
	return 0;
}

int transmit_tone(struct sccp_session *session, int tone, int instance, int reference)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	msg = msg_alloc(sizeof(struct start_tone_message), START_TONE_MESSAGE);
	if (msg == NULL)
		return -1;

	msg->data.starttone.tone = htolel(tone);
	msg->data.starttone.instance = htolel(instance);
	msg->data.starttone.reference = htolel(reference);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_lamp_indication(struct sccp_session *session, int stimulus, int instance, int indication)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	msg = msg_alloc(sizeof(struct set_lamp_message), SET_LAMP_MESSAGE);
	if (msg == NULL)
		return -1;

	msg->data.setlamp.stimulus = htolel(stimulus);
	msg->data.setlamp.stimulusInstance = htolel(instance);
	msg->data.setlamp.deviceStimulus = htolel(indication);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_ringer_mode(struct sccp_session *session, int mode)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	msg = msg_alloc(sizeof(struct set_ringer_message), SET_RINGER_MESSAGE);
	if (msg == NULL)
		return -1;

	msg->data.setringer.ringerMode = htolel(mode);
	msg->data.setringer.unknown1 = htolel(1);
	msg->data.setringer.unknown2 = htolel(1);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_selectsoftkeys(struct sccp_session *session, int instance, int callid, int softkey)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	msg = msg_alloc(sizeof(struct select_soft_keys_message), SELECT_SOFT_KEYS_MESSAGE);
	if (msg == NULL)
		return -1;

        msg->data.selectsoftkey.instance = htolel(instance);
        msg->data.selectsoftkey.reference = htolel(callid);
        msg->data.selectsoftkey.softKeySetIndex = htolel(softkey);
        msg->data.selectsoftkey.validKeyMask = htolel(0xFFFFFFFF);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}
