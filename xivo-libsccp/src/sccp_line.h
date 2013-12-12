#ifndef SCCP_LINE_H
#define SCCP_LINE_H

#include <asterisk/channel.h>
#include "sccp_device.h"

struct sccp_configs;
struct list_line;

struct sccp_line {

	char name[80];
	char cid_num[80];
	char cid_name[80];

	char language[MAX_LANGUAGE];
	char context[AST_MAX_CONTEXT];
	struct ast_variable *chanvars;

	uint32_t serial_callid;
	uint32_t instance;
	enum sccp_state state;

	uint8_t dnd;
	enum sccp_call_forward_status callfwd;
	uint32_t callfwd_id;
	char callfwd_exten[AST_MAX_EXTENSION];

	struct sccp_subchannel *active_subchan;
	AST_RWLIST_HEAD(, sccp_subchannel) subchans;

	struct ast_codec_pref codec_pref;
	struct ast_format_cap *caps;	/* Allowed capabilities */
	struct sccp_device *device;

	AST_LIST_ENTRY(sccp_line) list;
	AST_LIST_ENTRY(sccp_line) list_per_device;
};

struct sccp_line *sccp_line_create(const char *name, struct sccp_configs *sccp_cfg);
void sccp_line_destroy(struct sccp_line *line);

/*
 * \note The reference count on the returned subchannel is NOT incremented.
 */
struct sccp_subchannel *sccp_line_get_next_offhook_subchan(struct sccp_line *line);

/*
 * \note The reference count on the returned subchannel is NOT incremented.
 */
struct sccp_subchannel *sccp_line_get_next_ringin_subchan(struct sccp_line *line);

/*
 * \note The reference count on the returned subchannel is incremented.
 */
struct sccp_subchannel *sccp_line_get_subchan(struct sccp_line *line, uint32_t subchan_id);

/*
 * \brief Remove the given subchan from the list of subchans of line.
 *
 * \retval 0 if the subchan was found in line
 * \retval -1 if the subchan was not found
 *
 * \note This will decrement the reference count on the subchan if found.
 */
int sccp_line_remove_subchan(struct sccp_line *line, struct sccp_subchannel *subchan);

void sccp_line_select_subchan(struct sccp_line *line, struct sccp_subchannel *subchan);
void sccp_line_select_subchan_id(struct sccp_line *line, uint32_t subchan_id);
void sccp_line_set_field(struct sccp_line *line, const char *name, const char *value);

struct sccp_line *sccp_line_find_by_name(const char *name, struct list_line *list_line);

#endif /* SCCP_LINE_H */
