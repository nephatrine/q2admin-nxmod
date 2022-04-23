#include "zb_discord.h"

#ifdef USE_DISCORD
#	include "orca/discord.h"

#	include <pthread.h>
#	include <stdlib.h>
#	include <string.h>
#	include <time.h>
#	include <unistd.h>

//
// Thread Queue
//

#	define Q2D_STATE_UNINITIALIZED 1
#	define Q2D_STATE_CLOSED 2
#	define Q2D_STATE_CLOSING 4
#	define Q2D_STATE_READY 8

typedef struct queue_cmd_s
{
	struct queue_cmd_s * next;
	char                 msg[];
} queue_cmd_t;

typedef struct
{
	struct queue_cmd_s * head;
	struct queue_cmd_s * tail;
	pthread_mutex_t *    guard;
	int                  state;
} queue_head_t;

static queue_head_t * queue_construct()
{
	queue_head_t * queue = malloc( sizeof( queue_head_t ) );
	queue->head = queue->tail = NULL;
	queue->state              = Q2D_STATE_UNINITIALIZED;

	queue->guard = malloc( sizeof( pthread_mutex_t ) );
	pthread_mutex_init( queue->guard, NULL );

	return queue;
}

static void queue_destroy( queue_head_t * queue )
{
	queue_cmd_t * element;

	while( queue->head != NULL )
	{
		element     = queue->head;
		queue->head = element->next;
		free( element );
	}

	pthread_mutex_destroy( queue->guard );
	free( queue->guard );
	free( queue );
	queue = NULL;
}

static void queue_push( queue_head_t * queue, const char * message )
{
	if( message == NULL ) return;

	size_t        length   = strlen( message ) + 1;
	queue_cmd_t * element  = malloc( sizeof( queue_cmd_t ) + length );
	queue_cmd_t * previous = NULL;

	snprintf( element->msg, length, "%s", message );
	element->next = NULL;

	if( pthread_mutex_lock( queue->guard ) == 0 )
	{
		if( queue->state & ( Q2D_STATE_UNINITIALIZED | Q2D_STATE_READY ) )
		{
			if( queue->head == NULL ) { queue->head = element; }
			else
			{
				previous       = queue->tail;
				previous->next = element;
			}

			queue->tail = element;
		}
		pthread_mutex_unlock( queue->guard );
	}
	else
		free( element );
}

static queue_cmd_t * queue_pop( queue_head_t * queue )
{
	queue_cmd_t * element = NULL;

	if( pthread_mutex_lock( queue->guard ) == 0 )
	{
		if( ( element = queue->head ) != NULL ) queue->head = element->next;
		pthread_mutex_unlock( queue->guard );
	}

	return element;
}

static queue_cmd_t * queue_try_pop( queue_head_t * queue )
{
	queue_cmd_t * element = NULL;

	if( pthread_mutex_trylock( queue->guard ) == 0 )
	{
		if( ( element = queue->head ) != NULL ) queue->head = element->next;
		pthread_mutex_unlock( queue->guard );
	}

	return element;
}

static void queue_set_state( queue_head_t * queue, int state )
{
	if( pthread_mutex_lock( queue->guard ) == 0 )
	{
		queue->state = state;
		pthread_mutex_unlock( queue->guard );
	}
	else
		queue->state = state;
}

static void queue_set_state_if( queue_head_t * queue, int new_state, const int old_state )
{
	if( pthread_mutex_lock( queue->guard ) == 0 )
	{
		if( queue->state & old_state ) queue->state = new_state;
		pthread_mutex_unlock( queue->guard );
	}
	else if( queue->state | old_state )
		queue->state = new_state;
}

static int queue_get_state( queue_head_t * queue )
{
	int state = queue->state;

	if( pthread_mutex_lock( queue->guard ) == 0 )
	{
		state = queue->state;
		pthread_mutex_unlock( queue->guard );
	}

	return state;
}

static queue_head_t * q2d_incoming_queue = NULL;
static queue_head_t * q2d_outgoing_queue = NULL;

// =================================
// USED BY DISCORD LISTENER THREAD
// Should not access Quake II state.
// =================================

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
static pthread_t q2d_bot_thread;

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

static void q2d_message_to_game( char * msg )
{
	if( !msg ) return;

	size_t buflen  = strlen( msg ) + 2;
	char * command = malloc( buflen );

	snprintf( command, buflen, "%s\n", msg );
	queue_push( q2d_incoming_queue, command );
	free( command );
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
	queue_set_state_if( q2d_outgoing_queue, q2d_bot.channel ? Q2D_STATE_READY : Q2D_STATE_CLOSED, Q2D_STATE_UNINITIALIZED );
}

static void q2d_on_command_ping( struct discord * client, const struct discord_message * msg )
{
	if( msg->author->bot || ( q2d_bot.channel && msg->channel_id != q2d_bot.channel ) ) return;
	struct discord_create_message_params params = { .content = "**[SERVER]** PONG" };

	discord_async_next( client, NULL );
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

		discord_async_next( client, NULL );
		discord_create_message( client, msg->channel_id, &params, NULL );
	}
	else
	{
		struct discord_create_message_params params = { .content = "**[SERVER]** You are not authorized to run commands." };

		discord_async_next( client, NULL );
		discord_create_message( client, msg->channel_id, &params, NULL );
	}
}

static void q2d_on_command_say( struct discord * client, const struct discord_message * msg )
{
	if( msg->author->bot || ( q2d_bot.channel && msg->channel_id != q2d_bot.channel ) ) return;

	char msg_buffer[320];
	snprintf( msg_buffer, sizeof( msg_buffer ), "say_discord %s: %s", msg->author->username, msg->content );
	q2d_message_to_game( msg_buffer );
}

static void q2d_process_discord_queue( struct discord * client )
{
	queue_cmd_t * command = NULL;

	// if we have been asked to shut down
	if( queue_get_state( q2d_outgoing_queue ) == Q2D_STATE_CLOSING )
	{
		discord_shutdown( client );
		return;
	}

	// bail out early so we don't hit mutex at all
	if( q2d_outgoing_queue->head == NULL ) return;

	while( command = queue_try_pop( q2d_outgoing_queue ) )
	{
		if( q2d_bot.channel )
		{
			struct discord_create_message_params params = { .content = command->msg };

			discord_async_next( client, NULL );
			discord_create_message( client, q2d_bot.channel, &params, NULL );
		}
		free( command );
	}
}

static void * q2d_main( void * arg )
{
	orca_global_init();

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
	queue_set_state( q2d_outgoing_queue, Q2D_STATE_CLOSED );

	// clean up after ourselves
	if( q2d_bot.client )
	{
		discord_cleanup( q2d_bot.client );
		q2d_bot.client = NULL;
	}
	orca_global_cleanup();
	pthread_exit( NULL );
}

// =================================
// USED BY APPLICATION ITSELF
// These *can* reference game code.
// =================================

#	ifdef true
#		undef true
#	endif
#	ifdef false
#		undef false
#	endif
#	include "g_local.h"

void q2d_initialize()
{
	q2d_incoming_queue = queue_construct();
	q2d_outgoing_queue = queue_construct();

	q2d_bot.client  = NULL;
	q2d_bot.channel = 0;
	q2d_bot.rcuser  = 0;
	q2d_bot.rcgroup = 0;

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
	queue_set_state( q2d_incoming_queue, Q2D_STATE_READY );
}

static void q2d_message_to_discord( char * msg )
{
	if( !msg ) return;

	size_t buflen  = strlen( msg ) + 1;
	char * command = malloc( buflen );

	snprintf( command, buflen, "%s", msg );
	queue_push( q2d_outgoing_queue, command );
	free( command );
}

void q2d_message_to_discord2( int level, const char * s )
{
	if( !s ) return;

	char * cptr = strstr( s, ": " );
	char   q2d_buffer[512];

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

	// I do not recall what this block does....
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
	queue_cmd_t * command = NULL;

	// bail out early so we don't hit mutex at all
	if( q2d_incoming_queue->head == NULL ) return;

	while( command = queue_try_pop( q2d_incoming_queue ) )
	{
		if( strstr( command->msg, "say_discord " ) == command->msg )
			gi.bprintf( PRINT_CHAT, "[Q2D] %s", command->msg + 12 );
		else
			gi.AddCommandString( command->msg );
		free( command );
	}
}

void q2d_shutdown()
{
	queue_set_state( q2d_incoming_queue, Q2D_STATE_CLOSED );
	queue_set_state_if( q2d_outgoing_queue, Q2D_STATE_CLOSING, ~Q2D_STATE_CLOSED );

#ifdef _GNU_SOURCE
	struct timespec ts;
	if( clock_gettime( CLOCK_REALTIME, &ts ) != -1 )
	{
		ts.tv_sec += 5;
		pthread_timedjoin_np( q2d_bot_thread, NULL, &ts );
	}
	else
#endif
		pthread_join( q2d_bot_thread, NULL );

	queue_destroy( q2d_incoming_queue );
	queue_destroy( q2d_outgoing_queue );
}

#endif
