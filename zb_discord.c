#include "g_local.h"

#ifdef USE_DISCORD
#	include <orca/discord-internal.h>
#	include <unistd.h>

static queue_cmd_t * q2d_queue_cmds = NULL;
static queue_cmd_t * q2d_queue_last = NULL;

static pthread_mutex_t q2d_queue_lock = PTHREAD_MUTEX_INITIALIZER;

static q2d_bot_t q2d_bot;
static pthread_t q2d_thread;

volatile int q2d_state = 0;

static u64_snowflake_t q2d_create_thread( struct discord * client, const u64_snowflake_t channel, char * name )
{
	if( !client || !channel || !name || !name[0] ) return channel;

	int t = 0;

	struct discord_thread_response_body active_list;
	discord_list_active_threads( client, channel, &active_list );
	if( active_list.threads )
		for( t = 0; active_list.threads[t]; ++t )
			if( !strcmp( name, active_list.threads[t]->name ) ) return active_list.threads[t]->id;

	struct discord_channel thread;
	discord_channel_init( &thread );

	struct discord_start_thread_without_message_params params = { .name = name, .type = DISCORD_CHANNEL_GUILD_PUBLIC_THREAD };
	if( discord_start_thread_without_message( client, channel, &params, &thread ) == ORCA_OK ) return thread.id;

	return channel;
}

static void q2d_queue_add( char * cmd )
{
	if( !cmd ) return;

	queue_cmd_t * newcmd = (queue_cmd_t *)malloc( sizeof( queue_cmd_t ) );
	snprintf( newcmd->cmd, sizeof( newcmd->cmd ), "%s\n", cmd );
	newcmd->next = NULL;

	if( pthread_mutex_lock( &q2d_queue_lock ) == 0 )
	{
		if( q2d_queue_last ) q2d_queue_last->next = newcmd;
		if( !q2d_queue_cmds ) q2d_queue_cmds = newcmd;

		q2d_queue_last = newcmd;
		pthread_mutex_unlock( &q2d_queue_lock );
	}
	else
	{
		free( newcmd );
	}
}

static void q2d_queue_free()
{
	queue_cmd_t * prevcmd = NULL;

	if( pthread_mutex_lock( &q2d_queue_lock ) == 0 )
	{
		while( prevcmd = q2d_queue_cmds )
		{
			if( q2d_queue_last == prevcmd ) q2d_queue_last = NULL;
			q2d_queue_cmds = prevcmd->next;
			free( prevcmd );
		}
		pthread_mutex_unlock( &q2d_queue_lock );
	}
}

void q2d_queue_process()
{
	if( !q2d_queue_cmds ) return;

	queue_cmd_t * prevcmd = NULL;

	if( pthread_mutex_trylock( &q2d_queue_lock ) == 0 )
	{
		while( prevcmd = q2d_queue_cmds )
		{
			if( q2d_queue_last == prevcmd ) q2d_queue_last = NULL;
			if( strstr( prevcmd->cmd, "say_discord " ) == prevcmd->cmd )
				gi.bprintf( PRINT_CHAT, "[DISCORD] %s", prevcmd->cmd + 12 );
			else
				gi.AddCommandString( prevcmd->cmd );
			q2d_queue_cmds = prevcmd->next;
			free( prevcmd );
		}
		pthread_mutex_unlock( &q2d_queue_lock );
	}
}

// callback
void q2d_on_ready( struct discord * client, const struct discord_user * bot )
{
	char * presence_json = "{"
	                       "\"activities\":"
	                       "["
	                       "{"
	                       "\"name\": \"Quake II\","
	                       "\"type\": 0,"
	                       "\"details\": \"blah blah blah\""
	                       "}"
	                       "],"
	                       "\"status\": \"idle\","
	                       "\"afk\": false"
	                       "}\0";
	struct discord_gateway_status_update * presence_info = NULL;
	discord_gateway_status_update_from_json( presence_json, strlen( presence_json ), &presence_info );
	discord_replace_presence( client, presence_info );

	log_info( "Q2Discord successfully connected as %s#%s!", bot->username, bot->discriminator );
	q2d_state = 1;
}

// callback
void q2d_on_command_ping( struct discord * client, const struct discord_user * bot, const struct discord_message * msg )
{
	if( msg->author->bot || msg->channel_id != q2d_bot.channel ) return;
	struct discord_create_message_params params = { .content = "**[SERVER]** PONG" };
	discord_create_message( client, msg->channel_id, &params, NULL );
}

// callback
void q2d_on_command_rcon( struct discord * client, const struct discord_user * bot, const struct discord_message * msg )
{
	if( msg->author->bot || msg->channel_id != q2d_bot.channel ) return;

	int authorized = 0, r = 0;
	if( q2d_bot.rcuser && msg->author->id == q2d_bot.rcuser )
		authorized = 1;
/*	else if( q2d_bot.rcgroup )
	{
		struct discord_permissions_role ** roles = NULL;
		discord_get_guild_roles( client, msg->guild_id, &roles );
		if( roles )
			for( r = 0; roles[r]; ++r )
				if( roles[r]->id == q2d_bot.rcgroup )
				{
					authorized = 1;
					break;
				}
	} */

	if( authorized )
	{
		q2d_queue_add( msg->content );
		struct discord_create_message_params params = { .content = "**[SERVER]** Command Queued" };
		discord_create_message( client, msg->channel_id, &params, NULL );
	}
	else
	{
		struct discord_create_message_params params = { .content = "**[SERVER]** You are not authorized to run commands." };
		discord_create_message( client, msg->channel_id, &params, NULL );
	}
}

// callback
void q2d_on_command_say( struct discord * client, const struct discord_user * bot, const struct discord_message * msg )
{
	if( msg->author->bot || msg->channel_id != q2d_bot.channel ) return;

	static char msg_buffer[320];
	snprintf( msg_buffer, sizeof( msg_buffer ), "say_discord %s: %s", msg->author->username, msg->content );
	q2d_queue_add( msg_buffer );
}

// thread
void * q2d_main( void * arg )
{
	pthread_mutex_init( &q2d_queue_lock, NULL );

	discord_global_init();

	if( strlen( q2d_bot.token ) )
		q2d_bot.client = discord_init( q2d_bot.token );
	else if( strlen( q2d_bot.config ) )
		q2d_bot.client = discord_config_init( q2d_bot.config );

	if( q2d_bot.client )
	{
		discord_set_on_ready( q2d_bot.client, &q2d_on_ready );
		discord_set_on_command( q2d_bot.client, "ping", &q2d_on_command_ping );
		discord_set_on_command( q2d_bot.client, "rcon", &q2d_on_command_rcon );
		discord_set_on_command( q2d_bot.client, "say", &q2d_on_command_say );

		q2d_bot.channel = q2d_create_thread( q2d_bot.client, q2d_bot.channel, q2d_bot.name );

		discord_run( q2d_bot.client );
		discord_cleanup( q2d_bot.client );
		q2d_bot.client = NULL;
	}

	discord_global_cleanup();
	q2d_state = -1;
	return NULL;
}

void q2d_init()
{
	q2d_bot.client  = NULL;
	q2d_bot.channel = 0;
	q2d_bot.rcuser  = 0;
	q2d_bot.rcgroup = 0;
	q2d_state       = 0;

	q2d_bot.config[0] = q2d_bot.config[sizeof( q2d_bot.config ) - 1] = 0;
	q2d_bot.token[0] = q2d_bot.token[sizeof( q2d_bot.token ) - 1] = 0;
	q2d_bot.name[0] = q2d_bot.name[sizeof( q2d_bot.name ) - 1] = 0;

	cvar_t * discord_json    = gi.cvar( "discord_json", "q2discord.json", CVAR_NOSET );
	cvar_t * discord_token   = gi.cvar( "discord_token", "", CVAR_NOSET );
	cvar_t * discord_channel = gi.cvar( "discord_channel", "0", CVAR_NOSET );
	cvar_t * discord_thread  = gi.cvar( "discord_thread", "", CVAR_NOSET );
	cvar_t * discord_rcuser  = gi.cvar( "discord_rcuser", "", CVAR_NOSET );
	cvar_t * discord_rcgroup = gi.cvar( "discord_rcgroup", "", CVAR_NOSET );

	if( discord_thread->string ) strncpy( q2d_bot.name, discord_thread->string, sizeof( q2d_bot.name ) - 1 );
	if( discord_token->string ) strncpy( q2d_bot.token, discord_token->string, sizeof( q2d_bot.token ) - 1 );
	if( discord_channel->string ) q2d_bot.channel = strtoull( discord_channel->string, NULL, 10 );
	if( discord_rcuser->string ) q2d_bot.rcuser = strtoull( discord_rcuser->string, NULL, 10 );
	if( discord_rcgroup->string ) q2d_bot.rcgroup = strtoull( discord_rcgroup->string, NULL, 10 );

	if( discord_json->string )
	{
		strncpy( q2d_bot.config, discord_json->string, sizeof( q2d_bot.config ) - 1 );

		FILE * discord_file = q2a_fopen( q2d_bot.config, sizeof( q2d_bot.config ), "rt" );
		if( discord_file )
			fclose( discord_file );
		else
			q2d_bot.config[0] = 0;
	}

	if( !pthread_create( &q2d_thread, NULL, &q2d_main, NULL ) )
		while( q2d_state == 0 ) sleep( 1 ); // this is not the best
}

void q2d_exit()
{
	q2d_queue_free();
	if( q2d_bot.client ) discord_gateway_shutdown( &( q2d_bot.client->gw ) );
	pthread_join( q2d_thread, NULL );
}

void q2d_message( int level, const char * s )
{
	static char q2d_buffer[512];

	if( !s || q2d_state != 1 || !q2d_bot.channel || !q2d_bot.client ) return;

	char * cptr = strstr( s, ": " );

	switch( level )
	{
		case PRINT_MEDIUM: snprintf( q2d_buffer, sizeof( q2d_buffer ), "*%s*", s ); break;
		case PRINT_HIGH: snprintf( q2d_buffer, sizeof( q2d_buffer ), "**[SERVER]** %s", s ); break;
		case PRINT_CHAT:
			if( cptr )
			{
				snprintf( q2d_buffer, ( cptr - s ) + 4, "**%s", s );
				snprintf( q2d_buffer + ( cptr - s ) + 3, sizeof( q2d_buffer ) - ( cptr - s ) - 3, "**%s", cptr + 1 );
			}
			else
			{
				snprintf( q2d_buffer, sizeof( q2d_buffer ), "*%s*", s );
			}
			break;
		default: return;
	}

	int i = 0, j = 0;
	while( q2d_buffer[i] )
	{
		if( q2d_buffer[i] == '\n' ) ++j;
		if( i != j ) q2d_buffer[i] = q2d_buffer[j];
		++i, ++j;
	}
	struct discord_create_message_params params = { .content = q2d_buffer };
	discord_create_message( q2d_bot.client, q2d_bot.channel, &params, NULL );
}
#endif
