#ifndef ZB_DISCORD_H
#define ZB_DISCORD_H 1

typedef struct queue_cmd_s
{
	char                 cmd[128];
	struct queue_cmd_s * next;
} queue_cmd_t;

#ifdef USE_DISCORD
#	include <orca/discord.h>

typedef struct
{
	struct discord * client;
	char             config[MAX_OSPATH];
	char             token[64];
	u64_snowflake_t  channel;
	u64_snowflake_t  rcuser;
	u64_snowflake_t  rcgroup;
	char             name[32];
} q2d_bot_t;

void q2d_init();
void q2d_exit();
void q2d_message( int level, const char * s );
void q2d_queue_process();
#endif

#endif
