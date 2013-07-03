#include "device.h"
#include "sccp.h"

void device_unregister(struct sccp_device *device)
{
	struct sccp_line *line_itr = NULL;
	struct sccp_subchannel *subchan = NULL;

	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return;
	}

	device->registered = DEVICE_REGISTERED_FALSE;

	speeddial_hints_unsubscribe(device);

	if (device->mwi_event_sub) {
		ast_event_unsubscribe(device->mwi_event_sub);
	}

	AST_RWLIST_RDLOCK(&device->lines);
	AST_RWLIST_TRAVERSE(&device->lines, line_itr, list_per_device) {
		do {
			subchan = NULL;

			AST_RWLIST_WRLOCK(&line_itr->subchans);
			subchan = AST_RWLIST_FIRST(&line_itr->subchans);
			AST_RWLIST_UNLOCK(&line_itr->subchans);

			if (subchan != NULL) {
				do_hangup(line_itr->instance, subchan->id, device->session);
				sleep(1);
			}

			line_itr->active_subchan = NULL;
			line_itr->callfwd = SCCP_CFWD_UNACTIVE;

		} while (subchan != NULL);

	}
	AST_RWLIST_UNLOCK(&device->lines);
}

void device_register(struct sccp_device *device,
			int8_t protoVersion,
			int type,
			void *session,
			struct sockaddr_in localip)
{
	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return;
	}

	device->registered = DEVICE_REGISTERED_TRUE;
	device->protoVersion = protoVersion;
	device->type = type;
	device->session = session;
	device->localip = localip;
}

void device_prepare(struct sccp_device *device)
{
	struct sccp_line *line_itr = NULL;

	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return;
	}

	device->active_line = NULL;
	device->active_line_cnt = 0;

	device->exten[0] = '\0';

	AST_RWLIST_RDLOCK(&device->lines);
	AST_RWLIST_TRAVERSE(&device->lines, line_itr, list_per_device) {
		set_line_state(line_itr, SCCP_ONHOOK);
	}
	AST_RWLIST_UNLOCK(&device->lines);
}

struct sccp_device *find_device_by_name(const char *name, struct list_device *list_device)
{
	struct sccp_device *device_itr = NULL;

	if (name == NULL) {
		ast_log(LOG_DEBUG, "name is NULL\n");
		return NULL;
	}

	if (list_device == NULL) {
		ast_log(LOG_DEBUG, "list_device is NULL\n");
		return NULL;
	}

	AST_RWLIST_RDLOCK(list_device);
	AST_RWLIST_TRAVERSE(list_device, device_itr, list) {
		if (!strncmp(device_itr->name, name, sizeof(device_itr->name)))
			break;
	}
	AST_RWLIST_UNLOCK(list_device);

	return device_itr;
}

struct sccp_subchannel *line_get_next_ringin_subchan(struct sccp_line *line)
{
	struct sccp_subchannel *subchan_itr = NULL;

	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return NULL;
	}

	AST_RWLIST_RDLOCK(&line->subchans);
	AST_RWLIST_TRAVERSE(&line->subchans, subchan_itr, list) {
		if (subchan_itr != NULL && subchan_itr->state == SCCP_RINGIN)
			break;
	}
	AST_RWLIST_UNLOCK(&line->subchans);

	return subchan_itr;
}

struct sccp_line *find_line_by_name(const char *name, struct list_line *list_line)
{
	struct sccp_line *line_itr = NULL;

	if (name == NULL) {
		ast_log(LOG_DEBUG, "name is NULL\n");
		return NULL;
	}

	if (list_line == NULL) {
		ast_log(LOG_DEBUG, "list_line is NULL\n");
		return NULL;
	}

	AST_RWLIST_RDLOCK(list_line);
	AST_RWLIST_TRAVERSE(list_line, line_itr, list) {
		if (!strncmp(line_itr->name, name, sizeof(line_itr->name)))
			break;
	}
	AST_RWLIST_UNLOCK(list_line);

	return line_itr;
}

void speeddial_hints_unsubscribe(struct sccp_device *device)
{
	struct sccp_speeddial *speeddial_itr = NULL;
	AST_RWLIST_RDLOCK(&device->speeddials);
	AST_RWLIST_TRAVERSE(&device->speeddials, speeddial_itr, list_per_device) {
		if (speeddial_itr->blf) {
			ast_extension_state_del(speeddial_itr->state_id, NULL);
		}
	}
	AST_RWLIST_UNLOCK(&device->speeddials);
}

void speeddial_hints_subscribe(struct sccp_device *device, ast_state_cb_type speeddial_hints_cb)
{
	struct sccp_speeddial *speeddial_itr = NULL;
	struct sccp_line *default_line = NULL;
	int dev_state;
	char *context;

	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return;
	}

	if (speeddial_hints_cb == NULL) {
		ast_log(LOG_DEBUG, "speeddial_hints_cb is NULL\n");
		return;
	}

	default_line = device->default_line;
	if (default_line == NULL) {
		ast_log(LOG_WARNING, "Not subscribing to speeddial hints; device has no line\n");
		return;
	}

	context = default_line->context;

	AST_RWLIST_RDLOCK(&device->speeddials);
	AST_RWLIST_TRAVERSE(&device->speeddials, speeddial_itr, list_per_device) {
		if (speeddial_itr->blf) {
			speeddial_itr->state_id = ast_extension_state_add(context, speeddial_itr->extension, speeddial_hints_cb, speeddial_itr);
			if (speeddial_itr->state_id == -1) {
				ast_log(LOG_WARNING, "Could not subscribe to %s@%s\n", speeddial_itr->extension, context);
			} else {
				dev_state = ast_extension_state(NULL, context, speeddial_itr->extension);
				speeddial_itr->state = dev_state;
			}
		}
	}
	AST_RWLIST_UNLOCK(&device->speeddials);
}

struct sccp_speeddial *device_get_speeddial_by_index(struct sccp_device *device, uint32_t index)
{
	struct sccp_speeddial *speeddial_itr = NULL;

	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return NULL;
	}

	AST_RWLIST_RDLOCK(&device->speeddials);
	AST_RWLIST_TRAVERSE(&device->speeddials, speeddial_itr, list_per_device) {
		if (speeddial_itr->index == index)
			break;
	}
	AST_RWLIST_UNLOCK(&device->speeddials);

	return speeddial_itr;
}

struct sccp_speeddial *device_get_speeddial(struct sccp_device *device, uint32_t instance)
{
	struct sccp_speeddial *speeddial_itr = NULL;

	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return NULL;
	}

	AST_RWLIST_RDLOCK(&device->speeddials);
	AST_RWLIST_TRAVERSE(&device->speeddials, speeddial_itr, list_per_device) {
		if (speeddial_itr->instance == instance)
			break;
	}
	AST_RWLIST_UNLOCK(&device->speeddials);

	return speeddial_itr;
}

struct sccp_line *device_get_line(struct sccp_device *device, uint32_t instance)
{
	struct sccp_line *line_itr = NULL;

	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return NULL;
	}

	AST_RWLIST_RDLOCK(&device->lines);
	AST_RWLIST_TRAVERSE(&device->lines, line_itr, list_per_device) {
		if (line_itr->instance == instance)
			break;
	}
	AST_RWLIST_UNLOCK(&device->lines);

	return line_itr;
}

char *line_state_str(int line_state)
{
	switch (line_state) {
	case SCCP_OFFHOOK:
		return "Offhook";
	case SCCP_ONHOOK:
		return "Onhook";
	case SCCP_RINGOUT:
		return "Ringout";
	case SCCP_RINGIN:
		return "Ringin";
	case SCCP_CONNECTED:
		return "Connected";
	case SCCP_BUSY:
		return "Busy";
	case SCCP_CONGESTION:
		return "Congestion";
	case SCCP_HOLD:
		return "Hold";
	case SCCP_CALLWAIT:
		return "Callwait";
	case SCCP_TRANSFER:
		return "Transfer";
	case SCCP_PARK:
		return "Park";
	case SCCP_PROGRESS:
		return "Progress";
	case SCCP_INVALID:
		return "Invalid";
	default:
		return "Unknown";
	}
}

char *device_type_str(int device_type)
{
	switch (device_type) {
	case SCCP_DEVICE_7905:
		return "7905";
	case SCCP_DEVICE_7906:
		return "7906";
	case SCCP_DEVICE_7911:
		return "7911";
	case SCCP_DEVICE_7912:
		return "7912";
	case SCCP_DEVICE_7921:
		return "7921";
	case SCCP_DEVICE_7931:
		return "7931";
	case SCCP_DEVICE_7937:
		return "7937";
	case SCCP_DEVICE_7940:
		return "7940";
	case SCCP_DEVICE_7941:
		return "7941";
	case SCCP_DEVICE_7941GE:
		return "7941GE";
	case SCCP_DEVICE_7942:
		return "7942";
	case SCCP_DEVICE_7960:
		return "7960";
	case SCCP_DEVICE_7961:
		return "7961";
	case SCCP_DEVICE_7962:
		return "7962";
	case SCCP_DEVICE_7970:
		return "7970";
	default:
		return "unknown";
	}
}

char *device_regstate_str(int device_state)
{
	switch (device_state) {
	case DEVICE_REGISTERED_TRUE:
		return "Registered";
	case DEVICE_REGISTERED_FALSE:
		return "Unregistered";
	default:
		return "unknown";
	}
}

int device_type_is_supported(int device_type)
{
	int supported = 0;

	switch (device_type) {
	case SCCP_DEVICE_7905:
	case SCCP_DEVICE_7906:
	case SCCP_DEVICE_7911:
	case SCCP_DEVICE_7912:
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
		supported = 1;
		break;

	default:
		supported = 0;
		break;
	}

	return supported;
}

int device_get_button_count(struct sccp_device *device)
{
	int button_count = 0;
	int i = 0;

	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return -1;
	}

	switch (device->type) {
	case SCCP_DEVICE_7905:
	case SCCP_DEVICE_7906:
	case SCCP_DEVICE_7911:
	case SCCP_DEVICE_7912:
	case SCCP_DEVICE_7937:
		button_count = 1;
		break;

	case SCCP_DEVICE_7940:
	case SCCP_DEVICE_7941:
	case SCCP_DEVICE_7941GE:
	case SCCP_DEVICE_7942:
		button_count = 2;
		break;

	case SCCP_DEVICE_7921:
	case SCCP_DEVICE_7960:
	case SCCP_DEVICE_7961:
	case SCCP_DEVICE_7962:
		button_count = 6;
		break;

	case SCCP_DEVICE_7970:
		button_count = 8;
		break;

	case SCCP_DEVICE_7931:
		button_count = 24;
		break;

	default:
		ast_log(LOG_WARNING, "unknown number of button for device type %d; assuming 1\n", device->type);
		button_count = 1;
		break;
	}

	return button_count;
}

char *complete_sccp_devices(const char *word, int state, struct list_device *list_device)
{
	struct sccp_device *device_itr = NULL;
	char *result = NULL;
	int which = 0;
	int len;

	if (word == NULL) {
		ast_log(LOG_DEBUG, "word is NULL\n");
		return NULL;
	}

	if (list_device == NULL) {
		ast_log(LOG_DEBUG, "list_device is NULL\n");
		return NULL;
	}

	len = strlen(word);

	AST_RWLIST_RDLOCK(list_device);
	AST_RWLIST_TRAVERSE(list_device, device_itr, list) {
		if (!strncasecmp(word, device_itr->name, len) && ++which > state) {
			result = ast_strdup(device_itr->name);
			break;
		}
	}
	AST_RWLIST_UNLOCK(list_device);

	return result;
}

void line_select_subchan(struct sccp_line *line, struct sccp_subchannel *subchan)
{
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return;
	}

	if (subchan == NULL) {
		ast_log(LOG_DEBUG, "subchan is NULL\n");
		return;
	}

	if (line->active_subchan)
		line->active_subchan->state = line->state;

	line->active_subchan = subchan;
}

struct sccp_subchannel *line_get_subchan(struct sccp_line *line, uint32_t subchan_id)
{
	struct sccp_subchannel *subchan_itr = NULL;

	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return NULL;
	}

	AST_LIST_TRAVERSE(&line->subchans, subchan_itr, list) {
		if (subchan_itr->id == subchan_id)
			break;
	}

	return subchan_itr;
}

void line_select_subchan_id(struct sccp_line *line, uint32_t subchan_id)
{
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return;
	}

	struct sccp_subchannel *subchan_itr;
	AST_LIST_TRAVERSE(&line->subchans, subchan_itr, list) {
		if (subchan_itr->id == subchan_id) {
			line_select_subchan(line, subchan_itr);
			break;
		}
	}
}

void subchan_set_on_hold(struct sccp_line *line, uint32_t subchan_id)
{
	struct sccp_subchannel *subchan;

	subchan = line_get_subchan(line, subchan_id);
	if (subchan == NULL) {
		ast_log(LOG_WARNING, "subchan is NULL\n");
		return;
	}

	subchan->on_hold = 1;
}

void subchan_unset_on_hold(struct sccp_line *line, uint32_t subchan_id)
{
	struct sccp_subchannel *subchan;

	subchan = line_get_subchan(line, subchan_id);
	if (subchan == NULL) {
		ast_log(LOG_WARNING, "subchan is NULL\n");
		return;
	}

	subchan->on_hold = 0;
}

void subchan_set_state(struct sccp_subchannel *subchan, int state)
{
	subchan->state = state;
}

void set_line_state(struct sccp_line *line, int state)
{
	line->state = state;
}

void device_enqueue_line(struct sccp_device *device, struct sccp_line *line)
{
	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return;
	}

	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return ;
	}

	device->active_line_cnt++;
}

void device_release_line(struct sccp_device *device, struct sccp_line *line)
{
	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL");
		return;
	}

	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return;
	}

	if (device->active_line == line) {
		device->active_line = NULL;
	}

	device->active_line_cnt--;
}

struct sccp_line *device_get_active_line(struct sccp_device *device)
{
	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return NULL;
	}

	if (device->active_line == NULL) {
		device->active_line = device->default_line;
		device->active_line_cnt++;
	}

	return device->active_line;
}
