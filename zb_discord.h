#ifndef ZB_DISCORD_H
#define ZB_DISCORD_H 1

#ifdef USE_DISCORD
void q2d_initialize();
void q2d_shutdown();
void q2d_message_to_discord2( int level, const char * s );
void q2d_process_game_queue();
#endif

#endif
