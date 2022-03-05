#include "zb_discord.h"

#ifdef USE_DISCORD
#	include <orca/discord.h>
#	include <orca/discord-internal.h>
#	include <unistd.h>

typedef struct queue_cmd_s
{
	struct queue_cmd_s * next;
	char                 msg[];
} queue_cmd_t;

typedef struct
{
	struct discord * client;
	char             config[256];
	char             token[64];
	u64_snowflake_t  channel;
	u64_snowflake_t  rcuser;
	u64_snowflake_t  rcgroup;
	char             name[32];
} q2d_bot_t;

static q2d_bot_t q2d_bot;

// USED BY DISCORD LISTENER THREAD
// Should not access Quake II state.

#	define Q2D_BOT_NULL 0
#	define Q2D_BOT_READY 1
#	define Q2D_BOT_TRYEXIT 2
#	define Q2D_BOT_CLOSED 3

volatile int q2d_bot_state    = Q2D_BOT_NULL;
volatile int q2d_bot_transmit = Q2D_BOT_NULL;

static pthread_t q2d_bot_thread;

static pthread_mutex_t q2d_incoming_lock  = PTHREAD_MUTEX_INITIALIZER;
static queue_cmd_t *   q2d_incoming_begin = NULL;
static queue_cmd_t *   q2d_incoming_end   = NULL;

static pthread_mutex_t q2d_outgoing_lock  = PTHREAD_MUTEX_INITIALIZER;
static queue_cmd_t *   q2d_outgoing_begin = NULL;
static queue_cmd_t *   q2d_outgoing_end   = NULL;

static void q2d_cleanup_incoming()
{
	queue_cmd_t * prevcmd = NULL;

	if( pthread_mutex_lock( &q2d_incoming_lock ) == 0 )
	{
		while( prevcmd = q2d_incoming_begin )
		{
			if( q2d_incoming_end == prevcmd ) q2d_incoming_end = NULL;
			q2d_incoming_begin = prevcmd->next;
			free( prevcmd );
		}
		pthread_mutex_unlock( &q2d_incoming_lock );
	}
}

static void q2d_cleanup_outgoing()
{
	queue_cmd_t * prevcmd = NULL;

	if( pthread_mutex_lock( &q2d_outgoing_lock ) == 0 )
	{
		while( prevcmd = q2d_outgoing_begin )
		{
			if( q2d_outgoing_end == prevcmd ) q2d_outgoing_end = NULL;
			q2d_outgoing_begin = prevcmd->next;
			free( prevcmd );
		}
		pthread_mutex_unlock( &q2d_outgoing_lock );
	}
}

static u64_snowflake_t q2d_get_thread_id( struct discord * client, const u64_snowflake_t channel, char * name )
{
	if( !client || !channel || !name || !name[0] ) return channel;

	u64_snowflake_t rv = 0;

	int t = 0;

	struct discord_thread_response_body active_list;
	discord_thread_response_body_init( &active_list );
	discord_list_active_threads( client, channel, &active_list );
	if( active_list.threads )
		for( t = 0; active_list.threads[t]; ++t )
			if( !strcmp( name, active_list.threads[t]->name ) )
			{
				rv = active_list.threads[t]->id;
				break;
			}
	discord_thread_response_body_cleanup( &active_list );
	if( rv ) return rv;

	struct discord_channel thread;
	discord_channel_init( &thread );
	struct discord_start_thread_without_message_params params = { .name = name, .type = DISCORD_CHANNEL_GUILD_PUBLIC_THREAD };
	if( discord_start_thread_without_message( client, channel, &params, &thread ) == ORCA_OK ) rv = thread.id;
	discord_channel_cleanup( &thread );

	return rv ? rv : channel;
}

static void q2d_message_to_discord( char * msg )
{
	if( !msg ) return;

	size_t        msgsize = strlen( msg ) + 1;
	queue_cmd_t * newcmd  = (queue_cmd_t *)malloc( sizeof( queue_cmd_t ) + sizeof( char[msgsize] ) );
	snprintf( newcmd->msg, msgsize, "%s", msg );
	newcmd->next = NULL;

	if( pthread_mutex_lock( &q2d_outgoing_lock ) == 0 )
	{
		if( q2d_bot_transmit < Q2D_BOT_TRYEXIT )
		{
			if( q2d_outgoing_end ) q2d_outgoing_end->next = newcmd;
			if( !q2d_outgoing_begin ) q2d_outgoing_begin = newcmd;

			q2d_outgoing_end = newcmd;
			pthread_mutex_unlock( &q2d_outgoing_lock );
			return;
		}
		pthread_mutex_unlock( &q2d_outgoing_lock );
	}

	free( newcmd );
}

static void q2d_message_to_game( char * msg )
{
	if( !msg ) return;

	size_t        msgsize = strlen( msg ) + 2;
	queue_cmd_t * newcmd  = (queue_cmd_t *)malloc( sizeof( queue_cmd_t ) + sizeof( char[msgsize] ) );
	snprintf( newcmd->msg, msgsize, "%s\n", msg );
	newcmd->next = NULL;

	if( pthread_mutex_lock( &q2d_incoming_lock ) == 0 )
	{
		if( q2d_bot_state < Q2D_BOT_TRYEXIT )
		{
			if( !q2d_incoming_begin ) q2d_incoming_begin = newcmd;
			if( q2d_incoming_end ) q2d_incoming_end->next = newcmd;
			q2d_incoming_end = newcmd;

			pthread_mutex_unlock( &q2d_incoming_lock );
			return;
		}
		pthread_mutex_unlock( &q2d_incoming_lock );
	}

	free( newcmd );
}

static void q2d_on_bot_ready( struct discord * client )
{
	// set discord status

	char * activity_json = "{"
	                       "\"name\": \"Quake II\","
	                       "\"type\": 0,"
	                       "\"url\": \"https://fraglimit.nephatrine.net/\","
	                       "\"details\": \"blah blah blah\""
	                       "}\0";
	struct discord_activity activity_info;
	discord_activity_from_json( activity_json, strlen( activity_json ), &activity_info );

	struct discord_activity * activity_list[2];
	activity_list[0] = &activity_info;
	activity_list[1] = NULL;

	struct discord_presence_status presence_info;
	presence_info.since      = discord_timestamp( client );
	presence_info.activities = activity_list;
	presence_info.status     = "idle";
	presence_info.afk        = false;
	discord_set_presence( client, &presence_info );

	// open for business

	if( pthread_mutex_lock( &q2d_incoming_lock ) == 0 )
	{
		q2d_bot_state = Q2D_BOT_READY;
		pthread_mutex_unlock( &q2d_incoming_lock );
	}
	else
		q2d_bot_state = Q2D_BOT_READY;

	if( pthread_mutex_lock( &q2d_outgoing_lock ) == 0 )
	{
		if( q2d_bot.channel )
			q2d_bot_transmit = Q2D_BOT_READY;
		else
			q2d_bot_transmit = Q2D_BOT_CLOSED;
		pthread_mutex_unlock( &q2d_outgoing_lock );
	}
	else
		q2d_bot_transmit = Q2D_BOT_READY;
}

static void q2d_on_command_ping( struct discord * client, const struct discord_message * msg )
{
	if( msg->author->bot || ( q2d_bot.channel && msg->channel_id != q2d_bot.channel ) ) return;
	struct discord_create_message_params params = { .content = "**[SERVER]** PONG" };
	discord_create_message( client, msg->channel_id, &params, NULL );
}

static void q2d_on_command_rcon( struct discord * client, const struct discord_message * msg )
{
	if( msg->author->bot || ( q2d_bot.channel && msg->channel_id != q2d_bot.channel ) ) return;

	int authorized = 0, r = 0;
	if( q2d_bot.rcuser && msg->author->id == q2d_bot.rcuser )
		authorized = 1;
	else if( q2d_bot.rcgroup && msg->member->roles )
		for( r = 0; msg->member->roles[r]; ++r )
			if( msg->member->roles[r]->value == q2d_bot.rcgroup )
			{
				authorized = 1;
				break;
			}

	if( authorized )
	{
		q2d_message_to_game( msg->content );
		struct discord_create_message_params params = { .content = "**[SERVER]** Command Queued" };
		discord_create_message( client, msg->channel_id, &params, NULL );
	}
	else
	{
		struct discord_create_message_params params = { .content = "**[SERVER]** You are not authorized to run commands." };
		discord_create_message( client, msg->channel_id, &params, NULL );
	}
}

static void q2d_on_command_say( struct discord * client, const struct discord_message * msg )
{
	if( msg->author->bot || ( q2d_bot.channel && msg->channel_id != q2d_bot.channel ) ) return;

	static char msg_buffer[320];
	snprintf( msg_buffer, sizeof( msg_buffer ), "say_discord %s: %s", msg->author->username, msg->content );
	q2d_message_to_game( msg_buffer );
}

static void q2d_process_discord_queue( struct discord * client )
{
	if( !q2d_outgoing_begin ) return;

	queue_cmd_t * prevcmd = NULL;

	if( pthread_mutex_trylock( &q2d_outgoing_lock ) == 0 )
	{
		while( prevcmd = q2d_outgoing_begin )
		{
			if( q2d_outgoing_end == prevcmd ) q2d_outgoing_end = NULL;
			if( q2d_bot.channel )
			{
				struct discord_create_message_params params = { .content = prevcmd->msg };
				discord_create_message( client, q2d_bot.channel, &params, NULL );
			}
			q2d_outgoing_begin = prevcmd->next;
			free( prevcmd );
		}
		pthread_mutex_unlock( &q2d_outgoing_lock );
	}
}

static void * q2d_main( void * arg )
{
	discord_global_init();

	// bot authorization
	if( strlen( q2d_bot.token ) )
		q2d_bot.client = discord_init( q2d_bot.token );
	else if( strlen( q2d_bot.config ) )
		q2d_bot.client = discord_config_init( q2d_bot.config );

	if( q2d_bot.client )
	{
		// register callbacks
		discord_set_on_idle( q2d_bot.client, &q2d_process_discord_queue );
		discord_set_on_ready( q2d_bot.client, &q2d_on_bot_ready );
		discord_set_on_command( q2d_bot.client, "ping", &q2d_on_command_ping );
		discord_set_on_command( q2d_bot.client, "rcon", &q2d_on_command_rcon );
		discord_set_on_command( q2d_bot.client, "say", &q2d_on_command_say );

		// create thread if needed
		q2d_bot.channel = q2d_get_thread_id( q2d_bot.client, q2d_bot.channel, q2d_bot.name );

		// discord event loop
		discord_run( q2d_bot.client );
	}

	// closed for business
	if( pthread_mutex_lock( &q2d_outgoing_lock ) == 0 )
	{
		q2d_bot_transmit = Q2D_BOT_CLOSED;
		pthread_mutex_unlock( &q2d_outgoing_lock );
	}
	else
		q2d_bot_transmit = Q2D_BOT_CLOSED;
	if( pthread_mutex_lock( &q2d_incoming_lock ) == 0 )
	{
		q2d_bot_state = Q2D_BOT_CLOSED;
		pthread_mutex_unlock( &q2d_incoming_lock );
	}
	else
		q2d_bot_state = Q2D_BOT_CLOSED;

	// clean up after ourselves
	if( q2d_bot.client )
	{
		discord_cleanup( q2d_bot.client );
		q2d_bot.client = NULL;
	}
	discord_global_cleanup();
	q2d_cleanup_incoming();
	q2d_cleanup_outgoing();
	return NULL;
}

// USED BY APPLICATION ITSELF
// These *can* reference game code, but shouldn't change bot state without a mutex.

#	ifdef true
#		undef true
#	endif
#	ifdef false
#		undef false
#	endif
#	include "g_local.h"

void q2d_initialize()
{
	pthread_mutex_init( &q2d_incoming_lock, NULL );
	pthread_mutex_init( &q2d_outgoing_lock, NULL );

	q2d_bot.client  = NULL;
	q2d_bot.channel = 0;
	q2d_bot.rcuser  = 0;
	q2d_bot.rcgroup = 0;

	q2d_bot_state    = Q2D_BOT_NULL;
	q2d_bot_transmit = Q2D_BOT_NULL;

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

	pthread_create( &q2d_bot_thread, NULL, &q2d_main, NULL );
}

void q2d_message_to_discord2( int level, const char * s )
{
	static char q2d_buffer[512];

	if( !s ) return;

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
	q2d_message_to_discord( q2d_buffer );
}

void q2d_process_game_queue()
{
	if( !q2d_incoming_begin ) return;

	queue_cmd_t * prevcmd = NULL;

	if( pthread_mutex_trylock( &q2d_incoming_lock ) == 0 )
	{
		if( q2d_bot_state == Q2D_BOT_READY )
			while( prevcmd = q2d_incoming_begin )
			{
				if( q2d_incoming_end == prevcmd ) q2d_incoming_end = NULL;
				if( strstr( prevcmd->msg, "say_discord " ) == prevcmd->msg )
					gi.bprintf( PRINT_CHAT, "[Q2D] %s", prevcmd->msg + 12 );
				else
					gi.AddCommandString( prevcmd->msg );
				q2d_incoming_begin = prevcmd->next;
				free( prevcmd );
			}
		pthread_mutex_unlock( &q2d_incoming_lock );
	}
}

void q2d_shutdown()
{
	if( pthread_mutex_lock( &q2d_outgoing_lock ) == 0 )
	{
		if( q2d_bot_transmit < Q2D_BOT_TRYEXIT ) q2d_bot_transmit = Q2D_BOT_TRYEXIT;
		pthread_mutex_unlock( &q2d_outgoing_lock );
	}
	else if( q2d_bot_transmit < Q2D_BOT_TRYEXIT )
		q2d_bot_transmit = Q2D_BOT_TRYEXIT;

	if( pthread_mutex_lock( &q2d_incoming_lock ) == 0 )
	{
		if( q2d_bot_state < Q2D_BOT_TRYEXIT ) q2d_bot_state = Q2D_BOT_TRYEXIT;
		pthread_mutex_unlock( &q2d_incoming_lock );
	}
	else if( q2d_bot_state < Q2D_BOT_TRYEXIT )
		q2d_bot_state = Q2D_BOT_TRYEXIT;

	// TODO: there is surely a better way to stop the bot...
	if( q2d_bot.client ) discord_gateway_shutdown( &( q2d_bot.client->gw ) );
	pthread_join( q2d_bot_thread, NULL );
}

#endif
