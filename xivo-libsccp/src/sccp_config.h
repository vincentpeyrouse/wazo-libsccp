#ifndef SCCP_CONFIG_H
#define SCCP_CONFIG_H

#include <asterisk/channel.h>

#include "sccp_device.h"

struct sccp_configs {

	int set;

	char bindaddr[16];
	char dateformat[6];
	int keepalive;
	int authtimeout;
	int dialtimeout;
	int directmedia;
	char language[MAX_LANGUAGE];
	char context[AST_MAX_EXTENSION];
	char vmexten[AST_MAX_EXTENSION];

	struct list_speeddial list_speeddial;
	struct list_line list_line;
	struct list_device list_device;
	struct ast_codec_pref codec_pref;
	struct ast_format_cap *caps;
};

struct sccp_configs *sccp_new_config(void);
void sccp_config_destroy(struct sccp_configs *config);

int sccp_config_load(struct sccp_configs *sccp_cfg, const char *config_file);
void sccp_config_unload(struct sccp_configs *sccp_cfg);

void destroy_device_config(struct sccp_configs *sccp_cfg, struct sccp_device *device);
void sccp_config_set_field(struct sccp_configs *sccp_cfg, const char *name, const char *value);

#endif /* SCCP_CONFIG_H */
