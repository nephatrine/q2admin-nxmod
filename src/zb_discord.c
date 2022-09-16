/*-------------------------------
# SPDX-License-Identifier: ISC
#
# Copyright © 2022 Daniel Wolf <<nephatrine@gmail.com>>
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
# REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
# INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
# LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
# OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
# PERFORMANCE OF THIS SOFTWARE.
# -----------------------------*/

#define _GNU_SOURCE

#include "zb_discord.h"

#include <orca/discord.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

//
// Thread Queue
//

#define Q2D_STATE_UNINITIALIZED 1
#define Q2D_STATE_CLOSED 2
#define Q2D_STATE_CLOSING 4
#define Q2D_STATE_READY 8

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
	queue_head_t * queue = (queue_head_t *)malloc( sizeof( queue_head_t ) );
	queue->head = queue->tail = NULL;
	queue->state              = Q2D_STATE_UNINITIALIZED;

	queue->guard = (pthread_mutex_t *)malloc( sizeof( pthread_mutex_t ) );
	pthread_mutex_init( queue->guard, NULL );

	return queue;
}

static void queue_destroy( queue_head_t * queue )
{
	queue_cmd_t * element;

	if( pthread_mutex_lock( queue->guard ) == 0 )
	{
		while( queue->head != NULL )
		{
			element     = queue->head;
			queue->head = element->next;
			free( element );
		}

		pthread_mutex_unlock( queue->guard );
		pthread_mutex_destroy( queue->guard );
		free( queue->guard );
		free( queue );
	}

	queue = NULL;
}

static void queue_push( queue_head_t * queue, const char * message )
{
	if( message == NULL ) return;

	size_t        length   = strlen( message ) + 1;
	queue_cmd_t * element  = (queue_cmd_t *)malloc( sizeof( queue_cmd_t ) + length );
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

#ifndef USE_DISCORD_OUTGOING
typedef struct
{
	pthread_mutex_t * guard;
	int               state;
} queue_state_t;

static queue_state_t * state_construct()
{
	queue_state_t * queue = (queue_state_t *)malloc( sizeof( queue_state_t ) );
	queue->state          = Q2D_STATE_UNINITIALIZED;

	queue->guard = (pthread_mutex_t *)malloc( sizeof( pthread_mutex_t ) );
	pthread_mutex_init( queue->guard, NULL );

	return queue;
}

static void state_destroy( queue_state_t * queue )
{
	if( pthread_mutex_lock( queue->guard ) == 0 )
	{
		pthread_mutex_unlock( queue->guard );
		pthread_mutex_destroy( queue->guard );
		free( queue->guard );
		free( queue );
	}
	queue = NULL;
}

static void state_set_state( queue_state_t * queue, int state )
{
	if( pthread_mutex_lock( queue->guard ) == 0 )
	{
		queue->state = state;
		pthread_mutex_unlock( queue->guard );
	}
	else
		queue->state = state;
}

static void state_set_state_if( queue_state_t * queue, int new_state, const int old_state )
{
	if( pthread_mutex_lock( queue->guard ) == 0 )
	{
		if( queue->state & old_state ) queue->state = new_state;
		pthread_mutex_unlock( queue->guard );
	}
	else if( queue->state | old_state )
		queue->state = new_state;
}

static int state_get_state( queue_state_t * queue )
{
	int state = queue->state;

	if( pthread_mutex_lock( queue->guard ) == 0 )
	{
		state = queue->state;
		pthread_mutex_unlock( queue->guard );
	}

	return state;
}
#endif

static queue_head_t * q2d_incoming_queue = NULL;
#ifdef USE_DISCORD_OUTGOING
static queue_head_t * q2d_outgoing_queue = NULL;
#else
static queue_state_t * q2d_outgoing_state = NULL;
#endif

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
	u64_snowflake_t  appid;
	char             name[32];
	long int         mirror_high;
	long int         mirror_misc;
	long int         mirror_chat;
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
	char * command = (char *)malloc( buflen );

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
#ifdef USE_DISCORD_OUTGOING
	queue_set_state_if( q2d_outgoing_queue, q2d_bot.channel ? Q2D_STATE_READY : Q2D_STATE_CLOSED, Q2D_STATE_UNINITIALIZED );
#else
	state_set_state_if( q2d_outgoing_state, q2d_bot.channel ? Q2D_STATE_READY : Q2D_STATE_CLOSED, Q2D_STATE_UNINITIALIZED );
#endif
}

static void q2d_internal_ping( struct discord * client ) {}

static void q2d_on_bot_interaction( struct discord * client, const struct discord_interaction * interaction )
{
	if( interaction->type != DISCORD_INTERACTION_APPLICATION_COMMAND ) return;
	if( interaction->data == NULL ) return;

	if( q2d_bot.channel && interaction->channel_id != q2d_bot.channel ) return;

	if( strcmp( interaction->data->name, "q2ping" ) == 0 )
	{
		struct discord_interaction_response params = { .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
		                                               .data = &( struct discord_interaction_callback_data ){ .content = "**[SERVER]** PONG" } };
		discord_async_next( client, NULL );
		discord_create_interaction_response( client, interaction->id, interaction->token, &params, NULL );
	}
	else if( strcmp( interaction->data->name, "q2rcon" ) == 0 )
	{
		if( interaction->data->options == NULL || interaction->data->options[0] == NULL ) return;

		int authorized = 0, r = 0;
		if( q2d_bot.rcuser && interaction->member->user->id == q2d_bot.rcuser )
			authorized = 1;
		else if( q2d_bot.rcgroup && interaction->member->roles )
			for( r = 0; interaction->member->roles[r]; ++r )
				if( interaction->member->roles[r]->value == q2d_bot.rcgroup )
				{
					authorized = 1;
					break;
				}

		if( authorized )
		{
			q2d_message_to_game( interaction->data->options[0]->value );

			struct discord_interaction_response params = { .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
			                                               .data = &( struct discord_interaction_callback_data ){ .content = "**[SERVER]** Command Queued" } };
			discord_async_next( client, NULL );
			discord_create_interaction_response( client, interaction->id, interaction->token, &params, NULL );
		}
		else
		{
			struct discord_interaction_response params = { .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
			                                               .data = &( struct discord_interaction_callback_data ){ .content = "**[SERVER]** You are not authorized to run commands." } };
			discord_async_next( client, NULL );
			discord_create_interaction_response( client, interaction->id, interaction->token, &params, NULL );
		}
	}
	else if( strcmp( interaction->data->name, "q2say" ) == 0 )
	{
		if( interaction->data->options == NULL || interaction->data->options[0] == NULL ) return;

		char msg_buffer[320];
		snprintf( msg_buffer, sizeof( msg_buffer ), "say_discord %s: %s", interaction->member->user->username, interaction->data->options[0]->value );
		q2d_message_to_game( msg_buffer );

		snprintf( msg_buffer, sizeof( msg_buffer ), "%s: %s", interaction->member->user->username, interaction->data->options[0]->value );

		struct discord_interaction_response params = { .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
		                                               .data = &( struct discord_interaction_callback_data ){ .content = msg_buffer } };
		discord_async_next( client, NULL );
		discord_create_interaction_response( client, interaction->id, interaction->token, &params, NULL );
	}
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
	client; // unused

	if( msg->author->bot || ( q2d_bot.channel && msg->channel_id != q2d_bot.channel ) ) return;

	char msg_buffer[320];
	snprintf( msg_buffer, sizeof( msg_buffer ), "say_discord %s: %s", msg->author->username, msg->content );
	q2d_message_to_game( msg_buffer );
}

static void q2d_process_discord_queue( struct discord * client )
{
#ifdef USE_DISCORD_OUTGOING
	queue_cmd_t * command = NULL;

	// bail out early so we don't hit mutex at all
	if( q2d_outgoing_queue->head == NULL ) return;

	while( ( command = queue_try_pop( q2d_outgoing_queue ) ) )
	{
		if( q2d_bot.channel )
		{
			struct discord_create_message_params params = { .content = command->msg };

			discord_async_next( client, NULL );
			discord_create_message( client, q2d_bot.channel, &params, NULL );
		}
		free( command );

		if( q2d_outgoing_queue->head == NULL ) break;
	}
#endif

	// if we have been asked to shut down
#ifdef USE_DISCORD_OUTGOING
	if( queue_get_state( q2d_outgoing_queue ) == Q2D_STATE_CLOSING )
#else
	if( state_get_state( q2d_outgoing_state ) == Q2D_STATE_CLOSING )
#endif
	{
		discord_shutdown( client );
		return;
	}
}

static void * q2d_main( void * arg )
{
	arg; // unused

	orca_global_init();

	// bot authorization
	if( strlen( q2d_bot.token ) )
		q2d_bot.client = discord_init( q2d_bot.token );
	else if( strlen( q2d_bot.config ) )
		q2d_bot.client = discord_config_init( q2d_bot.config );

	if( q2d_bot.client )
	{
		size_t sc = 0;

		// register slash commands
		if( q2d_bot.appid )
		{
			struct discord_create_global_application_command_params say_sc = {
			      .type               = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
			      .name               = "q2say",
			      .description        = "broadcast to quake2 server",
			      .default_permission = true,
			      .options            = ( struct discord_application_command_option *[] ){ &( struct discord_application_command_option ){
			                                                                                     .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
			                                                                                     .name        = "message",
			                                                                                     .description = "message content",
			                                                                                     .required    = true,
                                                                                },
			                                                                               NULL },
			};
			struct discord_create_global_application_command_params rcon_sc = {
			      .type               = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
			      .name               = "q2rcon",
			      .description        = "send command to quake2 server",
			      .default_permission = true,
			      .options            = ( struct discord_application_command_option *[] ){ &( struct discord_application_command_option ){
			                                                                                     .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
			                                                                                     .name        = "command",
			                                                                                     .description = "remote command",
			                                                                                     .required    = true,
                                                                                },
			                                                                               NULL },
			};
			struct discord_create_global_application_command_params ping_sc = {
			      .type               = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
			      .name               = "q2ping",
			      .description        = "request pong from quake2 server",
			      .default_permission = true,
			      .options            = NULL,
			};

			struct discord_application_command ** list_sc;
			discord_get_global_application_commands( q2d_bot.client, q2d_bot.appid, &list_sc );

			int found_say  = 0;
			int found_rcon = 0;
			int found_ping = 0;

			if( list_sc )
			{
				while( list_sc[sc] )
				{
					if( strcmp( list_sc[sc]->name, say_sc.name ) == 0 )
					{
						if( list_sc[sc]->options == NULL )
							found_say = 2;
						else if( list_sc[sc]->options[0] == NULL )
							found_say = 2;
						else if( list_sc[sc]->options[1] != NULL )
							found_say = 2;
						else if( strcmp( list_sc[sc]->description, say_sc.description ) != 0 )
							found_say = 2;
						else if( strcmp( list_sc[sc]->options[0]->name, say_sc.options[0]->name ) != 0 )
							found_say = 2;
						else if( strcmp( list_sc[sc]->options[0]->description, say_sc.options[0]->description ) != 0 )
							found_say = 2;
						else if( list_sc[sc]->options[0]->required != say_sc.options[0]->required )
							found_say = 2;
						else
							found_say = 1;

						if( found_say == 2 ) discord_delete_global_application_command( q2d_bot.client, q2d_bot.appid, list_sc[sc]->id );
					}
					else if( strcmp( list_sc[sc]->name, rcon_sc.name ) == 0 )
					{
						if( list_sc[sc]->options == NULL )
							found_rcon = 2;
						else if( list_sc[sc]->options[0] == NULL )
							found_rcon = 2;
						else if( list_sc[sc]->options[1] != NULL )
							found_rcon = 2;
						else if( strcmp( list_sc[sc]->description, rcon_sc.description ) != 0 )
							found_rcon = 2;
						else if( strcmp( list_sc[sc]->options[0]->name, rcon_sc.options[0]->name ) != 0 )
							found_rcon = 2;
						else if( strcmp( list_sc[sc]->options[0]->description, rcon_sc.options[0]->description ) != 0 )
							found_rcon = 2;
						else if( list_sc[sc]->options[0]->required != rcon_sc.options[0]->required )
							found_rcon = 2;
						else
							found_rcon = 1;

						if( found_rcon == 2 ) discord_delete_global_application_command( q2d_bot.client, q2d_bot.appid, list_sc[sc]->id );
					}
					else if( strcmp( list_sc[sc]->name, ping_sc.name ) == 0 )
					{
						if( list_sc[sc]->options != NULL && list_sc[sc]->options[0] != NULL )
							found_ping = 2;
						else if( strcmp( list_sc[sc]->description, ping_sc.description ) != 0 )
							found_ping = 2;
						else
							found_ping = 1;

						if( found_ping == 2 ) discord_delete_global_application_command( q2d_bot.client, q2d_bot.appid, list_sc[sc]->id );
					}

					++sc;
				}
				discord_application_command_list_free( list_sc );
			}

			if( found_say != 1 ) discord_create_global_application_command( q2d_bot.client, q2d_bot.appid, &say_sc, NULL );
			if( found_rcon != 1 ) discord_create_global_application_command( q2d_bot.client, q2d_bot.appid, &rcon_sc, NULL );
			if( found_ping != 1 ) discord_create_global_application_command( q2d_bot.client, q2d_bot.appid, &ping_sc, NULL );
		}

		// register callbacks
		discord_set_on_idle( q2d_bot.client, &q2d_process_discord_queue );
		discord_set_on_ready( q2d_bot.client, &q2d_on_bot_ready );
		discord_set_on_command( q2d_bot.client, "ping", &q2d_on_command_ping );
		discord_set_on_command( q2d_bot.client, "rcon", &q2d_on_command_rcon );
		discord_set_on_command( q2d_bot.client, "say", &q2d_on_command_say );
		discord_set_on_interaction_create( q2d_bot.client, &q2d_on_bot_interaction );

		// create thread if needed
		q2d_bot.channel = q2d_get_thread_id( q2d_bot.client, q2d_bot.channel, q2d_bot.name );

		// discord event loop
		discord_run( q2d_bot.client );
	}

	// closed for business
#ifdef USE_DISCORD_OUTGOING
	queue_set_state( q2d_outgoing_queue, Q2D_STATE_CLOSED );
#else
	state_set_state( q2d_outgoing_state, Q2D_STATE_CLOSED );
#endif

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

void q2d_send_discord_message( char * command )
{
	// bail out early so we don't hit mutex at all
	if( command == NULL ) return;

#ifdef USE_DISCORD_OUTGOING
	if( queue_get_state( q2d_outgoing_queue ) == Q2D_STATE_READY )
#else
	if( state_get_state( q2d_outgoing_state ) == Q2D_STATE_READY )
#endif
	{
		if( q2d_bot.channel )
		{
			struct discord_create_message_params params = { .content = command };

			discord_async_next( q2d_bot.client, NULL );
			discord_create_message( q2d_bot.client, q2d_bot.channel, &params, NULL );
		}
	}
}

#ifdef true
#	undef true
#endif
#ifdef false
#	undef false
#endif
#include "g_local.h"

void q2d_initialize()
{
	q2d_incoming_queue = queue_construct();
#ifdef USE_DISCORD_OUTGOING
	q2d_outgoing_queue = queue_construct();
#else
	q2d_outgoing_state     = state_construct();
#endif

	q2d_bot.client  = NULL;
	q2d_bot.channel = 0;
	q2d_bot.rcuser  = 0;
	q2d_bot.rcgroup = 0;
	q2d_bot.appid   = 0;

	q2d_bot.mirror_high = 0;
	q2d_bot.mirror_misc = 0;
	q2d_bot.mirror_chat = 0;

	q2d_bot.config[0] = q2d_bot.config[sizeof( q2d_bot.config ) - 1] = 0;
	q2d_bot.token[0] = q2d_bot.token[sizeof( q2d_bot.token ) - 1] = 0;
	q2d_bot.name[0] = q2d_bot.name[sizeof( q2d_bot.name ) - 1] = 0;

	cvar_t * discord_json    = gi.cvar( "discord_json", "q2discord.json", CVAR_NOSET );
	cvar_t * discord_token   = gi.cvar( "discord_token", "", CVAR_LATCH );
	cvar_t * discord_channel = gi.cvar( "discord_channel", "0", CVAR_LATCH );
	cvar_t * discord_thread  = gi.cvar( "discord_thread", "", CVAR_LATCH );
	cvar_t * discord_rcuser  = gi.cvar( "discord_rcuser", "", CVAR_LATCH );
	cvar_t * discord_rcgroup = gi.cvar( "discord_rcgroup", "", CVAR_LATCH );
	cvar_t * discord_appid   = gi.cvar( "discord_appid", "", CVAR_LATCH );

#ifdef USE_DISCORD_OUTGOING
	cvar_t * mirror_high = gi.cvar( "mirror_high", "1", CVAR_LATCH );
	cvar_t * mirror_misc = gi.cvar( "mirror_misc", "1", CVAR_LATCH );
	cvar_t * mirror_chat = gi.cvar( "mirror_chat", "1", CVAR_LATCH );
#else
	cvar_t * mirror_unsafe = gi.cvar( "mirror_unsafe", "0", CVAR_LATCH );
	cvar_t * mirror_high   = gi.cvar( "mirror_high", "1", CVAR_LATCH );
	cvar_t * mirror_misc   = gi.cvar( "mirror_misc", "0", CVAR_LATCH );
	cvar_t * mirror_chat   = gi.cvar( "mirror_chat", "0", CVAR_LATCH );
#endif

	if( discord_thread->string ) strncpy( q2d_bot.name, discord_thread->string, sizeof( q2d_bot.name ) - 1 );
	if( discord_token->string ) strncpy( q2d_bot.token, discord_token->string, sizeof( q2d_bot.token ) - 1 );
	if( discord_channel->string ) q2d_bot.channel = strtoull( discord_channel->string, NULL, 10 );
	if( discord_rcuser->string ) q2d_bot.rcuser = strtoull( discord_rcuser->string, NULL, 10 );
	if( discord_rcgroup->string ) q2d_bot.rcgroup = strtoull( discord_rcgroup->string, NULL, 10 );
	if( discord_appid->string ) q2d_bot.appid = strtoull( discord_appid->string, NULL, 10 );

#ifndef USE_DISCORD_OUTGOING
	if( mirror_unsafe->string && strtol( mirror_unsafe->string, NULL, 10 ) > 0 )
#endif
	{
		if( mirror_high->string ) q2d_bot.mirror_high = strtol( mirror_high->string, NULL, 10 );
		if( mirror_misc->string ) q2d_bot.mirror_misc = strtol( mirror_misc->string, NULL, 10 );
		if( mirror_chat->string ) q2d_bot.mirror_chat = strtol( mirror_chat->string, NULL, 10 );
	}

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
	char * command = (char *)malloc( buflen );

	snprintf( command, buflen, "%s", msg );
#ifdef USE_DISCORD_OUTGOING
	queue_push( q2d_outgoing_queue, command );
#else
	// this seems to work in local testing but probably has data races
	// also if lots of messages go through it will lag the server badly
	q2d_send_discord_message( command );
#endif
	free( command );
}

void q2d_message_to_discord2( int msglevel, const char * s )
{
	if( !s ) return;

	if( msglevel == PRINT_HIGH && q2d_bot.mirror_high < 1 ) return;
	if( msglevel == PRINT_MEDIUM && q2d_bot.mirror_misc < 1 ) return;
	if( msglevel == PRINT_CHAT && q2d_bot.mirror_chat < 1 ) return;

	char * cptr = strstr( s, ": " );
	char   q2d_buffer[512];

	switch( msglevel )
	{
		case PRINT_MEDIUM: snprintf( q2d_buffer, sizeof( q2d_buffer ), "*%s*", s ); break;
		case PRINT_HIGH: snprintf( q2d_buffer, sizeof( q2d_buffer ), "**[SERVER]** %s", s ); break;
		case PRINT_CHAT:
			if( cptr )
			{
				snprintf( q2d_buffer, ( cptr - s ) + 4, "**%s", s );
				snprintf( q2d_buffer + ( cptr - s ) + 3, sizeof( q2d_buffer ) - ( cptr - s ) - 3, "**%s", cptr + 1 );
			}
			else { snprintf( q2d_buffer, sizeof( q2d_buffer ), "*%s*", s ); }
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

	while( ( command = queue_try_pop( q2d_incoming_queue ) ) )
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
	int tries = 0;

	queue_set_state( q2d_incoming_queue, Q2D_STATE_CLOSED );

#ifdef USE_DISCORD_OUTGOING
	queue_set_state_if( q2d_outgoing_queue, Q2D_STATE_CLOSING, ~Q2D_STATE_CLOSED );
	while( queue_get_state( q2d_outgoing_queue ) == Q2D_STATE_CLOSING && tries++ < 3 ) sleep( 1 );
#else
	state_set_state_if( q2d_outgoing_state, Q2D_STATE_CLOSING, ~Q2D_STATE_CLOSED );
	while( state_get_state( q2d_outgoing_state ) == Q2D_STATE_CLOSING && tries++ < 3 ) sleep( 1 );
#endif

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
#ifdef USE_DISCORD_OUTGOING
	queue_destroy( q2d_outgoing_queue );
#else
	state_destroy( q2d_outgoing_state );
#endif
}
