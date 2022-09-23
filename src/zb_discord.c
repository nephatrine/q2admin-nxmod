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

#include <assert.h>
#include <concord/discord.h>
#include <concord/log.h>
#include <inttypes.h>
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
	assert( queue && queue->guard );

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
		queue->guard = NULL;
	}

	free( queue );
	queue = NULL;
}

static void queue_push( queue_head_t * queue, const char * message )
{
	assert( queue && queue->guard );

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
	assert( queue && queue->guard );

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
	assert( queue && queue->guard );

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
	assert( queue && queue->guard );

	if( pthread_mutex_lock( queue->guard ) == 0 )
	{
		queue->state = state;
		pthread_mutex_unlock( queue->guard );
	}
	else
		queue->state = state;
}

static void queue_set_state_if( queue_head_t * queue, int new_state, int old_state )
{
	assert( queue && queue->guard );

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
	assert( queue && queue->guard );

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

// ============================
// Discord Helper Functions
// ============================

static struct discord * q2d_discord_init( const char * token, const char * config )
{
	assert( ( token && token[0] ) || ( config && config[0] ) );

	ccord_global_init();

	if( token && token[0] )
		return discord_init( token );
	else if( config && config[0] )
		return discord_config_init( config );

	log_warn( "q2d_discord_init: failed to initialize bot" );
	return NULL;
}

static void q2d_discord_cleanup( struct discord * client )
{
	// NOTE: Currently Concord creates an error in ThreadSanitizer when it deletes a locked mutex in
	//       discord_cleanup. I don't think this is a serious issue that needs to be addressed, but be aware
	//       of it.

	assert( client );
	discord_cleanup( client );
	ccord_global_cleanup();
}

void q2d_callback_free( struct discord * client, void * data )
{
	/* unused */ client;

	if( data ) free( (char *)data );
}

static void q2d_discord_create_message( struct discord * client, u64snowflake channel_id, char * message )
{
	struct discord_create_message params = {
	      .content = message,
	};
	if( channel_id ) discord_create_message( client, channel_id, &params, NULL );
}
static void q2d_discord_create_message_and_wait( struct discord * client, u64snowflake channel_id, char * message )
{
	struct discord_create_message params = {
	      .content = message,
	};
	struct discord_ret_message ret = {
	      .sync = (struct discord_message *)DISCORD_SYNC_FLAG,
	};
	if( channel_id ) discord_create_message( client, channel_id, &params, &ret );
}
static void q2d_discord_create_message_and_free( struct discord * client, u64snowflake channel_id, char * message )
{
	struct discord_create_message params = {
	      .content = message,
	};
	struct discord_ret_message ret = {
	      .data    = (void *)message,
	      .cleanup = q2d_callback_free,
	};
	if( channel_id ) discord_create_message( client, channel_id, &params, &ret );
}

static u64snowflake q2d_discord_get_channel( struct discord * client, u64snowflake channel_id )
{
	assert( client );

	if( !channel_id ) return channel_id;

	struct discord_channel     channel = { 0 };
	struct discord_ret_channel ret     = {
	          .sync = &channel,
    };
	discord_get_channel( client, channel_id, &ret );

	if( channel.id == 0 )
	{
		log_warn( "q2d_discord_get_channel: cannot get info for channel %" PRIu64, channel_id );
		return 0;
	}

	switch( channel.type )
	{
		case DISCORD_CHANNEL_GUILD_TEXT: return channel.id;
		case DISCORD_CHANNEL_GUILD_PUBLIC_THREAD:
		case DISCORD_CHANNEL_GUILD_PRIVATE_THREAD:
			if( channel.thread_metadata )
			{
				if( channel.thread_metadata->locked )
				{
					log_warn( "q2d_discord_get_channel: channel %" PRIu64 " is locked", channel.id );
					return 0;
				}
				else if( channel.thread_metadata->archived )
				{
					log_warn( "q2d_discord_get_channel: channel %" PRIu64 " is archived", channel.id );
					return 0;
				}
			}

			if( channel.member == NULL ) discord_join_thread( client, channel.id, NULL );

			return channel.id;
		default: log_warn( "q2d_discord_get_channel: unsupported type for channel %" PRIu64, channel.id );
	}

	return 0;
}

static int q2d_discord_match_application_command( const struct discord_edit_global_application_command * target, const struct discord_application_command * source )
{
	assert( target && source );

	if( strcmp( source->description, target->description ) != 0 ) return 3;
	if( target->options && source->options == NULL ) return 3;
	if( source->options == target->options ) return 1;
	if( source->options->size != target->options->size ) return 3;

	for( int i = 0; i < target->options->size; ++i )
	{
		if( strcmp( source->options->array[i].name, target->options->array[i].name ) != 0 ) return 3;
		if( strcmp( source->options->array[i].description, target->options->array[i].description ) != 0 ) return 3;
		if( source->options->array[i].required != target->options->array[i].required ) return 3;
	}

	return 1;
}

#define Q2D_DSC_VERSION " (v1.0)"

static void q2d_discord_create_application_commands( struct discord * client, u64snowflake application_id )
{
	assert( client && application_id );

	struct discord_application_command_option dsc_q2say_options[] = {
	      {
	            .type        = DISCORD_APPLICATION_OPTION_STRING,
	            .name        = "message",
	            .description = "Message Content",
	            .required    = true,
	      },
	};
	struct discord_edit_global_application_command dsc_q2say_e = {
	      .name        = "q2say",
	      .description = "Broadcast message to game server." Q2D_DSC_VERSION,
	      .options =
	            &( struct discord_application_command_options ){
	                  .size  = sizeof( dsc_q2say_options ) / sizeof *dsc_q2say_options,
	                  .array = dsc_q2say_options,
	            },
	      .default_member_permissions = DISCORD_PERM_SEND_MESSAGES,
	      .dm_permission              = false,
	};

	struct discord_application_command_option dsc_q2rcon_options[] = {
	      {
	            .type        = DISCORD_APPLICATION_OPTION_STRING,
	            .name        = "command",
	            .description = "Remote Command",
	            .required    = true,
	      },
	};
	struct discord_edit_global_application_command dsc_q2rcon_e = {
	      .name        = "q2rcon",
	      .description = "Send command to game server." Q2D_DSC_VERSION,
	      .options =
	            &( struct discord_application_command_options ){
	                  .size  = sizeof( dsc_q2rcon_options ) / sizeof *dsc_q2rcon_options,
	                  .array = dsc_q2rcon_options,
	            },
	      .default_member_permissions = DISCORD_PERM_SEND_MESSAGES,
	      .dm_permission              = false,
	};

	struct discord_edit_global_application_command dsc_q2ping_e = {
	      .name                       = "q2ping",
	      .description                = "Check bot connectivity." Q2D_DSC_VERSION,
	      .options                    = NULL,
	      .default_member_permissions = DISCORD_PERM_SEND_MESSAGES,
	      .dm_permission              = false,
	};

	struct discord_application_commands     dsc_list = { 0 };
	struct discord_ret_application_commands dsc_sync = {
	      .sync = &dsc_list,
	};

	discord_get_global_application_commands( client, application_id, &dsc_sync );

	int dsc_q2say_found  = 0;
	int dsc_q2rcon_found = 0;
	int dsc_q2ping_found = 0;

	if( dsc_list.size )
	{
		for( int i = 0; i < dsc_list.size; ++i )
		{
			assert( dsc_list.array[i] );

			if( strcmp( dsc_list.array[i].name, dsc_q2say_e.name ) == 0 )
			{
				dsc_q2say_found = q2d_discord_match_application_command( &dsc_q2say_e, &dsc_list.array[i] );

				if( dsc_q2say_found & 2 )
				{
					log_warn( "Q2D: slash command %s out of date", dsc_q2say_e.name );
					discord_edit_global_application_command( client, application_id, dsc_list.array[i].id, &dsc_q2say_e, NULL );
				}
			}
			else if( strcmp( dsc_list.array[i].name, dsc_q2rcon_e.name ) == 0 )
			{
				dsc_q2rcon_found = q2d_discord_match_application_command( &dsc_q2rcon_e, &dsc_list.array[i] );

				if( dsc_q2rcon_found & 2 )
				{
					log_warn( "Q2D: slash command %s out of date", dsc_q2rcon_e.name );
					discord_edit_global_application_command( client, application_id, dsc_list.array[i].id, &dsc_q2rcon_e, NULL );
				}
			}
			else if( strcmp( dsc_list.array[i].name, dsc_q2ping_e.name ) == 0 )
			{
				dsc_q2ping_found = q2d_discord_match_application_command( &dsc_q2ping_e, &dsc_list.array[i] );

				if( dsc_q2ping_found & 2 )
				{
					log_warn( "Q2D: slash command %s out of date", dsc_q2ping_e.name );
					discord_edit_global_application_command( client, application_id, dsc_list.array[i].id, &dsc_q2ping_e, NULL );
				}
			}
		}

		discord_application_commands_cleanup( &dsc_list );
	}

	if( dsc_q2say_found == 0 )
	{
		struct discord_create_global_application_command dsc_q2say_c = {
		      .type                       = DISCORD_APPLICATION_CHAT_INPUT,
		      .name                       = dsc_q2say_e.name,
		      .description                = dsc_q2say_e.description,
		      .options                    = dsc_q2say_e.options,
		      .default_member_permissions = dsc_q2say_e.default_member_permissions,
		      .dm_permission              = dsc_q2say_e.dm_permission,
		};
		discord_create_global_application_command( client, application_id, &dsc_q2say_c, NULL );
	}
	if( dsc_q2rcon_found == 0 )
	{
		struct discord_create_global_application_command dsc_q2rcon_c = {
		      .type                       = DISCORD_APPLICATION_CHAT_INPUT,
		      .name                       = dsc_q2rcon_e.name,
		      .description                = dsc_q2rcon_e.description,
		      .options                    = dsc_q2rcon_e.options,
		      .default_member_permissions = dsc_q2rcon_e.default_member_permissions,
		      .dm_permission              = dsc_q2rcon_e.dm_permission,
		};
		discord_create_global_application_command( client, application_id, &dsc_q2rcon_c, NULL );
	}
	if( dsc_q2ping_found == 0 )
	{
		struct discord_create_global_application_command dsc_q2ping_c = {
		      .type                       = DISCORD_APPLICATION_CHAT_INPUT,
		      .name                       = dsc_q2ping_e.name,
		      .description                = dsc_q2ping_e.description,
		      .options                    = dsc_q2ping_e.options,
		      .default_member_permissions = dsc_q2ping_e.default_member_permissions,
		      .dm_permission              = dsc_q2ping_e.dm_permission,
		};
		discord_create_global_application_command( client, application_id, &dsc_q2ping_c, NULL );
	}
}

// =================================
// USED BY DISCORD LISTENER THREAD
// Should not access Quake II state.
// =================================

typedef struct
{
	struct discord * client;

	char config[256];
	char token[64];

	u64snowflake application_id;
	u64snowflake guild_id;
	u64snowflake channel_id;
	u64snowflake rcon_user_id;
	u64snowflake rcon_role_id;

	long int mirror_high;
	long int mirror_misc;
	long int mirror_chat;
} q2d_bot_t;

static q2d_bot_t q2d_bot;

static void q2d_game_push( const char * msg )
{
	if( !msg ) return;

	size_t buflen  = strlen( msg ) + 2;
	char * command = (char *)malloc( buflen );

	snprintf( command, buflen, "%s\n", msg );

	for( size_t i = 0; i < buflen; ++i )
		if( command[i] && command[i] != '\n' && ( command[i] > 126 || command[i] < 32 ) ) command[i] = '?';

	queue_push( q2d_incoming_queue, command );
	free( command );
}

static char * q2d_game_broadcast( u64snowflake channel_id, const struct discord_user * author, const struct discord_guild_member * member, char * content )
{
	assert( author );
	assert( content );

	if( q2d_bot.channel_id && channel_id != q2d_bot.channel_id ) return "**[Q2Admin]** Oops, All Berries";
	if( author->bot ) return "**[Q2Admin]** Oops, All Berries";

	char msg_buffer[320];
	snprintf( msg_buffer, sizeof( msg_buffer ), "say_discord %s: %s", member ? member->nick : author->username, content );
	q2d_game_push( msg_buffer );

	return content;
}

static char * q2d_game_command( u64snowflake channel_id, const struct discord_user * author, const struct discord_guild_member * member, const char * content )
{
	assert( author );
	assert( content );

	if( q2d_bot.channel_id && channel_id != q2d_bot.channel_id ) return "**[Q2Admin]** Oops, All Berries";
	if( author->bot ) return "**[Q2Admin]** Oops, All Berries";

	bool authorized = false;

	if( q2d_bot.rcon_user_id && author->id == q2d_bot.rcon_user_id )
		authorized = true;
	else if( q2d_bot.rcon_role_id && member && member->roles )
		for( int i = 0; i < member->roles->size; ++i )
			if( member->roles->array[i] == q2d_bot.rcon_role_id )
			{
				authorized = true;
				break;
			}

	if( !authorized ) return "**[Q2Admin]** You are not authorized to run commands.";

	q2d_game_push( content );
	return "**[Q2Admin]** Command Queued";
}

static char * q2d_game_ping( u64snowflake channel_id, const struct discord_user * author )
{
	assert( author );

	if( q2d_bot.channel_id && channel_id != q2d_bot.channel_id ) return "**[Q2Admin]** Oops, All Berries";
	if( author->bot ) return "**[Q2Admin]** Oops, All Berries";

	return "**[Q2Admin]** PONG. I await your commands.";
}

// ============================
// Event: Bot Ready
// ============================

static void q2d_on_bot_ready( struct discord * client, const struct discord_ready * event )
{
	/* unsued */ event;

	struct discord_activity activities[] = {
	      {
	            .name    = "Quake II",
	            .type    = DISCORD_ACTIVITY_GAME,
	            .details = "q2admin-nxmod",
	            .url     = "https://fraglimit.nephatrine.net/",
	      },
	};
	struct discord_presence_update status = {
	      .activities =
	            &( struct discord_activities ){
	                  .size  = sizeof( activities ) / sizeof *activities,
	                  .array = activities,
	            },
	      .status = "idle",
	      .afk    = false,
	      .since  = discord_timestamp( client ),
	};
	discord_update_presence( client, &status );
}

// ============================
// Event: Bot Cycle
// ============================

static void q2d_on_bot_cycle( struct discord * client )
{
	queue_cmd_t * command = NULL;

	if( q2d_outgoing_queue->head )
		while( ( command = queue_try_pop( q2d_outgoing_queue ) ) )
		{
			q2d_discord_create_message_and_wait( client, q2d_bot.channel_id, command->msg );
			free( command );

			if( q2d_outgoing_queue->head == NULL ) break;
		}
	else if( queue_get_state( q2d_outgoing_queue ) == Q2D_STATE_CLOSING )
		discord_shutdown( client );
}

// ============================
// Event: Bot Commands
// ============================

static void q2d_on_command_say( struct discord * client, const struct discord_message * event )
{
	/* unused */ client;

	q2d_game_broadcast( event->channel_id, event->author, event->member, event->content );
}

static void q2d_on_command_rcon( struct discord * client, const struct discord_message * event )
{
	struct discord_create_message params = {
	      .content = q2d_game_command( event->channel_id, event->author, event->member, event->content ),
	};
	if( event->channel_id ) discord_create_message( client, event->channel_id, &params, NULL );
}

static void q2d_on_command_ping( struct discord * client, const struct discord_message * event )
{
	struct discord_create_message params = {
	      .content = q2d_game_ping( event->channel_id, event->author ),
	};
	if( event->channel_id ) discord_create_message( client, event->channel_id, &params, NULL );
}

static void q2d_on_bot_interaction( struct discord * client, const struct discord_interaction * event )
{
	if( event->type != DISCORD_INTERACTION_APPLICATION_COMMAND ) return;
	if( event->data == NULL ) return;

	json_char * arg1 = NULL;
	int         i;

	if( strcmp( event->data->name, "q2say" ) == 0 )
	{
		if( event->data->options == NULL ) return;

		for( i = 0; i < event->data->options->size; ++i )
			if( strcmp( event->data->options->array[i].name, "message" ) == 0 )
			{
				arg1 = event->data->options->array[i].value;
				break;
			}

		if( !arg1 ) return;

		q2d_game_broadcast( event->channel_id, event->member ? event->member->user : event->user, event->member, arg1 );

		struct discord_interaction_response params = {
		      .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
		      .data =
		            &( struct discord_interaction_callback_data ){
		                  .content = arg1,
		            },
		};
		discord_create_interaction_response( client, event->id, event->token, &params, NULL );
	}
	else if( strcmp( event->data->name, "q2rcon" ) == 0 )
	{
		if( event->data->options == NULL ) return;

		for( i = 0; i < event->data->options->size; ++i )
			if( strcmp( event->data->options->array[i].name, "command" ) == 0 )
			{
				arg1 = event->data->options->array[i].value;
				break;
			}

		if( !arg1 ) return;

		struct discord_interaction_response params = {
		      .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
		      .data =
		            &( struct discord_interaction_callback_data ){
		                  .content = q2d_game_command( event->channel_id, event->member ? event->member->user : event->user, event->member, arg1 ),
		            },
		};
		discord_create_interaction_response( client, event->id, event->token, &params, NULL );
	}
	else if( strcmp( event->data->name, "q2ping" ) == 0 )
	{
		struct discord_interaction_response params = {
		      .type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
		      .data =
		            &( struct discord_interaction_callback_data ){
		                  .content = q2d_game_ping( event->channel_id, event->member ? event->member->user : event->user ),
		            },
		};
		discord_create_interaction_response( client, event->id, event->token, &params, NULL );
	}
}

// ============================
// Bot Main Thread
// ============================

static void * q2d_main( void * arg )
{
	/* unsued */ arg;

	if( ( q2d_bot.client = q2d_discord_init( q2d_bot.token, q2d_bot.config ) ) )
	{
		if( q2d_bot.application_id ) q2d_discord_create_application_commands( q2d_bot.client, q2d_bot.application_id );
		q2d_bot.channel_id = q2d_discord_get_channel( q2d_bot.client, q2d_bot.channel_id );

		// register callbacks
		discord_set_on_ready( q2d_bot.client, &q2d_on_bot_ready );
		discord_set_on_command( q2d_bot.client, "ping", &q2d_on_command_ping );
		discord_set_on_command( q2d_bot.client, "rcon", &q2d_on_command_rcon );
		discord_set_on_command( q2d_bot.client, "say", &q2d_on_command_say );
		discord_set_on_idle( q2d_bot.client, &q2d_on_bot_cycle );
		discord_set_on_interaction_create( q2d_bot.client, &q2d_on_bot_interaction );

		queue_set_state_if( q2d_outgoing_queue, q2d_bot.channel_id ? Q2D_STATE_READY : Q2D_STATE_CLOSED, Q2D_STATE_UNINITIALIZED );

		// discord event loop
		discord_run( q2d_bot.client );

		queue_set_state( q2d_outgoing_queue, Q2D_STATE_CLOSED );
		q2d_discord_cleanup( q2d_bot.client );
		q2d_bot.client = NULL;
	}

	pthread_exit( NULL );
}

// =================================
// USED BY APPLICATION ITSELF
// These *can* reference game code.
// =================================

static pthread_t q2d_bot_thread;

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
	q2d_outgoing_queue = queue_construct();

	// This has been moved here from zb_init.
 	queue_push( q2d_outgoing_queue, "**[Q2Admin] === Open For Business ===**" );

	q2d_bot.client = NULL;

	q2d_bot.config[0] = q2d_bot.config[sizeof( q2d_bot.config ) - 1] = 0;
	q2d_bot.token[0] = q2d_bot.token[sizeof( q2d_bot.token ) - 1] = 0;

	q2d_bot.application_id = 0;
	q2d_bot.guild_id       = 0;
	q2d_bot.channel_id     = 0;
	q2d_bot.rcon_user_id   = 0;
	q2d_bot.rcon_role_id   = 0;

	q2d_bot.mirror_high = 0;
	q2d_bot.mirror_misc = 0;
	q2d_bot.mirror_chat = 0;

	cvar_t * d_bot_json  = gi.cvar( "d_bot_json", "q2discord.json", CVAR_ARCHIVE | CVAR_NOSET );
	cvar_t * d_bot_token = gi.cvar( "d_bot_token", "", CVAR_NOSET );

	cvar_t * d_application_id = gi.cvar( "d_application_id", "", CVAR_ARCHIVE | CVAR_NOSET );
	cvar_t * d_guild_id       = gi.cvar( "d_guild_id", "", CVAR_ARCHIVE | CVAR_NOSET );
	cvar_t * d_channel_id     = gi.cvar( "d_channel_id", "0", CVAR_ARCHIVE | CVAR_LATCH );
	cvar_t * d_rcon_user_id   = gi.cvar( "d_rcon_user_id", "", CVAR_ARCHIVE | CVAR_NOSET );
	cvar_t * d_rcon_role_id   = gi.cvar( "d_rcon_role_id", "", CVAR_ARCHIVE | CVAR_NOSET );

	cvar_t * d_mirror_high = gi.cvar( "d_mirror_high", "1", CVAR_ARCHIVE | CVAR_LATCH );
	cvar_t * d_mirror_misc = gi.cvar( "d_mirror_misc", "1", CVAR_ARCHIVE | CVAR_LATCH );
	cvar_t * d_mirror_chat = gi.cvar( "d_mirror_chat", "1", CVAR_ARCHIVE | CVAR_LATCH );

	if( d_bot_json->string )
	{
		strncpy( q2d_bot.config, d_bot_json->string, sizeof( q2d_bot.config ) - 1 );

		FILE * discord_file = q2a_fopen( q2d_bot.config, sizeof( q2d_bot.config ), "rt" );
		if( discord_file )
			fclose( discord_file );
		else
			q2d_bot.config[0] = 0;
	}
	if( d_bot_token->string ) strncpy( q2d_bot.token, d_bot_token->string, sizeof( q2d_bot.token ) - 1 );

	if( d_application_id->string ) q2d_bot.application_id = strtoull( d_application_id->string, NULL, 10 );
	if( d_guild_id->string ) q2d_bot.guild_id = strtoull( d_guild_id->string, NULL, 10 );
	if( d_channel_id->string ) q2d_bot.channel_id = strtoull( d_channel_id->string, NULL, 10 );
	if( d_rcon_user_id->string ) q2d_bot.rcon_user_id = strtoull( d_rcon_user_id->string, NULL, 10 );
	if( d_rcon_role_id->string ) q2d_bot.rcon_role_id = strtoull( d_rcon_role_id->string, NULL, 10 );

	if( d_mirror_high->string ) q2d_bot.mirror_high = strtol( d_mirror_high->string, NULL, 10 );
	if( d_mirror_misc->string ) q2d_bot.mirror_misc = strtol( d_mirror_misc->string, NULL, 10 );
	if( d_mirror_chat->string ) q2d_bot.mirror_chat = strtol( d_mirror_chat->string, NULL, 10 );

	pthread_create( &q2d_bot_thread, NULL, &q2d_main, NULL );
	queue_set_state( q2d_incoming_queue, Q2D_STATE_READY );
}

static void q2d_message_to_discord( char * msg )
{
	if( !msg ) return;

	size_t buflen  = strlen( msg ) + 1;
	char * command = (char *)malloc( buflen );

	snprintf( command, buflen, "%s", msg );
	queue_push( q2d_outgoing_queue, command );
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

	// This has been moved here from g_main.
	queue_push( q2d_outgoing_queue, "**[Q2Admin] === Closing Time ===**" );

	queue_set_state( q2d_incoming_queue, Q2D_STATE_CLOSED );
	queue_set_state_if( q2d_outgoing_queue, Q2D_STATE_CLOSING, ~Q2D_STATE_CLOSED );

	while( queue_get_state( q2d_outgoing_queue ) == Q2D_STATE_CLOSING && tries++ < 3 ) sleep( 1 );

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
