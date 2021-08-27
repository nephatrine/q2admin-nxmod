/*
Copyright (C) 2000 Shane Powell

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

//
// q2admin
//
// g_main.c
//
// copyright 2000 Shane Powell
//

#include "g_local.h"

#if defined(WIN32)
#include <windows.h>
#elif defined(__GNUC__)
#define GCC_VERSION (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#include <dlfcn.h>
#endif

#if defined(WIN32)
HINSTANCE hdll;
#define DLLNAME       "game.real.dll"
#define DLLNAMEMODDIR "game.real.dll"
#elif defined(__GNUC__)
void *hdll = NULL;
#define DLLNAME       "game.real.so"
#endif

typedef game_export_t  *GAMEAPI (game_import_t *import);

// from 3zb2-zigflag
cvar_t *basepath;
cvar_t *savepath;

char  zbot_teststring1[] = ZBOT_TESTSTRING1;
char  zbot_teststring_test1[] = ZBOT_TESTSTRING_TEST1;
char  zbot_teststring_test2[] = ZBOT_TESTSTRING_TEST2;
char  zbot_teststring_test3[] = ZBOT_TESTSTRING_TEST3;
char  zbot_testchar1;
char  zbot_testchar2;

qboolean soloadlazy;

void ShutdownGame (void)
{
	INITPERFORMANCE(1);
	INITPERFORMANCE(2);
	
	if (!dllloaded) return;

//*** UPDATE START ***
	if (whois_details)
	{
		whois_write_file();
		gi.TagFree (whois_details);
	}
//*** UPDATE END ***

	if (q2adminrunmode)
		{
			STARTPERFORMANCE(1);
			logEvent(LT_SERVEREND, 0, NULL, NULL, 0, 0.0);
			STARTPERFORMANCE(2);
		}
		
	// reset the password just in case something has gone wrong...
	lrcon_reset_rcon_password(0, 0, 0);
	dllglobals->Shutdown();
	
	if (q2adminrunmode)
		{
			STOPPERFORMANCE(2, "mod->ShutdownGame", 0, NULL);
		}
	gi.dprintf ("==== Shutdown (Q2Admin) ====\n");
		
#if defined(WIN32)
	FreeLibrary(hdll);
#elif defined(__GNUC__)
	dlclose(hdll);
#endif
	
	dllloaded = FALSE;
	
	if (q2adminrunmode)
		{
			STOPPERFORMANCE(1, "q2admin->ShutdownGame", 0, NULL);
		}
}

/*
=================
GetGameAPI
 
Returns a pointer to the structure with all entry points
and global variables
=================
*/
Q2_DLL_EXPORTED game_export_t *GetGameAPI(game_import_t *import)
{
	GAMEAPI *getapi;
#ifdef __GNUC__
	int loadtype;
#endif
	
	unsigned int i;// UPDATE
	dllloaded = FALSE;
	gi = *import;
	
	import->bprintf = bprintf_internal;
	import->cprintf = cprintf_internal;
	import->dprintf = dprintf_internal;
	import->AddCommandString = AddCommandString_internal;
	//import->Pmove = Pmove_internal;
	import->linkentity = linkentity_internal;
	import->unlinkentity = unlinkentity_internal;
	
	globals.Init = InitGame;
	globals.Shutdown = ShutdownGame;
	globals.SpawnEntities = SpawnEntities;
	
	globals.WriteGame = WriteGame;
	globals.ReadGame = ReadGame;
	globals.WriteLevel = WriteLevel;
	globals.ReadLevel = ReadLevel;
	
	globals.ClientThink = ClientThink;
	globals.ClientConnect = ClientConnect;
	globals.ClientUserinfoChanged = ClientUserinfoChanged;
	globals.ClientDisconnect = ClientDisconnect;
	globals.ClientBegin = ClientBegin;
	globals.ClientCommand = ClientCommand;
	
	globals.RunFrame = G_RunFrame;
	
	globals.ServerCommand = ServerCommand;
	
	// from 3zb2-zigflag
	basepath = gi.cvar("basepath", DEFAULTPATH, CVAR_NOSET);
	savepath = gi.cvar("savepath", DEFAULTPATH, CVAR_NOSET);
	
	port = gi.cvar("port", "", 0);
	rcon_password = gi.cvar("rcon_password", "", 0) ; // UPDATE
	q2admintxt = gi.cvar("q2admintxt", "", 0);
	q2adminbantxt = gi.cvar("q2adminbantxt", "", 0);
	
	gamedir = gi.cvar ("game", "baseq2", 0);
	q2a_strcpy(moddir, gamedir->string);
	
	if (moddir[0] == 0)
		{
			q2a_strcpy(moddir, "baseq2");
		}
		
	for (i=0;i<PRIVATE_COMMANDS;i++)
		{
			private_commands[i].command[0] = 0;
		}
		
//*** UPDATE START ***
	q2a_strcpy(client_msg,DEFAULTQ2AMSG);
//*** UPDATE END ***
	q2a_strcpy(zbotuserdisplay, DEFAULTUSERDISPLAY);
	q2a_strcpy(timescaleuserdisplay, DEFAULTTSDISPLAY);
	q2a_strcpy(hackuserdisplay, DEFAULTHACKDISPLAY);
	q2a_strcpy(skincrashmsg, DEFAULTSKINCRASHMSG);
	q2a_strcpy(defaultreconnectmessage, DEFAULTRECONNECTMSG);
	q2a_strcpy(defaultBanMsg, DEFAULTBANMSG);
	q2a_strcpy(nameChangeFloodProtectMsg, DEFAULTFLOODMSG);
	q2a_strcpy(skinChangeFloodProtectMsg, DEFAULTSKINFLOODMSG);
	q2a_strcpy(defaultChatBanMsg, DEFAULTCHABANMSG);
	q2a_strcpy(chatFloodProtectMsg, DEFAULTCHATFLOODMSG);
	q2a_strcpy(clientVoteCommand, DEFAULTVOTECOMMAND);
	q2a_strcpy(cl_pitchspeed_kickmsg, DEFAULTCL_PITCHSPEED_KICKMSG);
	q2a_strcpy(cl_anglespeedkey_kickmsg, DEFAULTCL_ANGLESPEEDKEY_KICKMSG);
	q2a_strcpy(lockoutmsg, DEFAULTLOCKOUTMSG);
	
	adminpassword[0] = 0;
	customServerCmd[0] = 0;
	customClientCmd[0] = 0;
	customClientCmdConnect[0] = 0;
	customServerCmdConnect[0] = 0;
	
	readCfgFiles();
	
	if (q2adminrunmode)
		{
			loadLogList();
		}
		
	// setup zbot test strings
	srand( (unsigned)time( NULL ) );
	i = random(); i = random(); i = random(); i = random(); //QW: not sure why these are needed
	zbot_teststring1[7] = zbot_teststring_test1[7] = '0' + (int)(9.9 * random());
	zbot_teststring1[8] = zbot_teststring_test1[8] = '0' + (int)(9.9 * random());
	zbot_teststring_test2[3] = '0' + (int)(9.9 * random());
	zbot_teststring_test2[4] = '0' + (int)(9.9 * random());
	zbot_testchar1 = '0' + (int)(9.9 * random());
	zbot_testchar2 = '0' + (int)(9.9 * random());
	
#if defined(WIN32)
	if (quake2dirsupport)
		{
			sprintf(dllname, "%s/%s", moddir, DLLNAME);
		}
	else
		{
			sprintf(dllname, "%s/%s", moddir, DLLNAMEMODDIR);
		}
	
	hdll = LoadLibrary(dllname);
#elif defined(__GNUC__)
	loadtype = soloadlazy ? RTLD_LAZY : RTLD_NOW;
	sprintf(dllname, "%s/%s", moddir, DLLNAME);
	hdll = dlopen(dllname, loadtype);
#endif
	
	if (hdll == NULL)
		{
			// try the baseq2 directory...
			sprintf(dllname, "baseq2/%s", DLLNAME);
			
#if defined(WIN32)
			hdll = LoadLibrary(dllname);
#elif defined(__GNUC__)
			hdll = dlopen(dllname, loadtype);
#endif
			
#if defined(WIN32)
			if (quake2dirsupport)
				{
					sprintf(dllname, "%s/%s", moddir, DLLNAME);
				}
			else
				{
					sprintf(dllname, "%s/%s", moddir, DLLNAMEMODDIR);
				}
#elif defined(__GNUC__)
			sprintf(dllname, "%s/%s", moddir, DLLNAME);
#endif
			
			if (hdll == NULL)
				{
#if defined(WIN32)
				unsigned long stat = GetLastError();
				gi.dprintf("Unable to load DLL %s, error %u.\n", dllname, stat);
					return &globals;
#else
				gi.dprintf("Unable to load DLL %s.\n", dllname);
					return &globals;
#endif
			}
			else
				{
					gi.dprintf ("Unable to load DLL %s, loading baseq2 DLL.\n", dllname);
				}
		}
		
#if defined(WIN32)
	getapi = (GAMEAPI *)GetProcAddress (hdll, "GetGameAPI");
#elif defined(__GNUC__)
	getapi = (GAMEAPI *)dlsym(hdll, "GetGameAPI");
#endif
	
	if (getapi == NULL)
		{
#if defined(WIN32)
			FreeLibrary(hdll);
#elif defined(__GNUC__)
			dlclose(hdll);
#endif
			
			gi.dprintf ("No \"GetGameApi\" entry in DLL %s.\n", dllname);
			return &globals;
		}
		
	dllglobals = (*getapi)(import);
	dllloaded = TRUE;
	copyDllInfo();
	import->cprintf = gi.cprintf;
	
	if (q2adminrunmode)
		{
			logEvent(LT_SERVERSTART, 0, NULL, NULL, 0, 0.0);
		}
		
	return &globals;
}
