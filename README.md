# Q2Admin-NXMod

This is a fork of [Q2Admin](https://github.com/tastyspleen/q2admin-tsmod) with some compatibility tweaks and
experimental Discord support using the [Orca](https://github.com/cee-studio/orca) library.

[![Build Status](https://ci.nephatrine.net/api/badges/nephatrine/q2admin-nxmod/status.svg?ref=refs/heads/master)](https://ci.nephatrine.net/nephatrine/q2admin-nxmod)

## Discord Integration

**NOTE:** Feature is not supported for Windows builds.

The optional Discord integration passes many messages from Quake II to Discord automatically such as death
messages, client connections, and chat. There is currently no way to configure that more precisely.

Chat in the Discord channel is NOT automatically mirrored to Quake II outside of the `!say` command.

The following commands are implemented:

- `!say <message>`: Broadcasts a message to all players over in-game chat.
- `!rcon <command>`: Allows an whitelisted user or group to run Quake II server commands over Discord.
- `!ping`: Returns a *pong* from the  bot. Just there to test connectivity.

Note that in most cases, any `!rcon` commands that rely on output being shown to the user will most likely
not have that output mirrored to Discord.

## Building

The simplest way to build is to perform the following from the source directory.

```bash
mkdir build
cd build
cmake -GNinja ..
ninja orca
ninja
```

If you are on a platform where the Discord support is not functional, you can skip the `ninja orca` command.

## Installation

In the mod directory you want to install the proxy in, rename the original game module from `game<arch>.so`
to `game<arch>.real.so` and then copy the q2admin `game<arch>.so` module into the same directory.

Place the various configuration files either into the base game folder (or `basepath` folder) or into the
chosen mod directory.

## Configuration

The `q2discord.json` file contains the bot token and additional configuration for the bot itself - such as
the command prefix.

Additionally, the following in-game console variables control some functionality:

- `discord_json`: Name of json file to read. Defaults to "*q2discord.json*".
- `discord_channel`: A single Discord channel ID to act in and listen to.
- `discord_thread`: If set, create a thread in the channel with this name.
- `discord_rcuser`: A single Discord user ID to allow the !rcon command.
- `discord_rcgroup`: A single Discord role ID to allow the !rcon command.
- `discord_appid`: The Discord application ID to allow slash command registration.

These options control outgoing message mirroring from in-game to discord:

- `mirror_high`: Mirror HIGH messages such as player connections and server status.
- `mirror_misc`: Mirror MEDIUM messages such as death messages.
- `mirror_chat`: Mirror CHAT messages such as player chat.
- `mirror_unsafe`: Send messages without WITH_DISCORD_OUTGOING. May be unstable!

The standard Q2Admin configuration parameters are documented within the various configuration files.
