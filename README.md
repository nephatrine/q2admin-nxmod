# Q2Admin-NXMod

This is a fork of [Q2Admin](https://github.com/tastyspleen/q2admin-tsmod) with some compatibility tweaks and
experimental Discord support using the [Concord](https://github.com/Cogmasters/concord) library.

[![Build Status](https://ci.nephatrine.net/api/badges/nephatrine/q2admin-nxmod/status.svg?ref=refs/heads/master)](https://ci.nephatrine.net/nephatrine/q2admin-nxmod)

## Discord Integration

**NOTE:** Feature is not supported for Windows builds.

The optional Discord integration passes many messages from Quake II to Discord automatically such as death
messages, client connections, and chat. There is currently no way to configure that more precisely.

Chat in the Discord channel is NOT automatically mirrored to Quake II outside of the say commands.

The following prefix commands are implemented if the bot has MESSAGE_CONTENT intent:

- `!say <message>`: Broadcasts a message to all players over in-game chat.
- `!rcon <command>`: Allows an whitelisted user or group to run Quake II server commands over Discord.
- `!ping`: Returns a *pong* from the  bot. Just there to test connectivity.

If passed an application id, the bot registers these as the following slash commands:

- `/q2say`
- `/q2rcon`
- `/q2ping`

Note that in most cases, any "rcon" commands that rely on output being shown to the user will most likely
not have that output mirrored to Discord, but instead the server terminal as they work the same way as
typing the command at the terminal rather than using the in-game rcon commands.

## Building

The simplest way to build is to perform the following from the source directory.

```bash
mkdir build
cd build
cmake -GNinja ..
ninja concord
ninja
```

If you are on a platform where the Discord support is not functional, you can skip the `ninja concord` command.

## Installation

In the mod directory you want to install the proxy in, rename the original game module from `game<arch>.so`
to `game<arch>.real.so` and then copy the q2admin `game<arch>.so` module into the same directory.

Place the various configuration files either into the base game folder (or `basepath` folder) or into the
chosen mod directory.

## Configuration

The `q2discord.json` file contains the bot token and additional configuration for the bot itself - such as
the command prefix and bot token.

Additionally, the following in-game console variables control some functionality:

- `d_bot_json`: Name of json file to read. Defaults to "*q2discord.json*".
- `d_application_id`: The Discord application ID to allow slash command registration.
- `d_channel_id`: A single Discord channel ID to act in and listen to.
- `d_rcon_user_id`: A single Discord user ID to allow the (q2)rcon command.
- `d_rcon_role_id`: A single Discord role ID to allow the (q2)rcon command.

These options control outgoing message mirroring from in-game to discord:

- `d_mirror_high`: Mirror HIGH messages such as player connections and server status.
- `d_mirror_misc`: Mirror MEDIUM messages such as death messages.
- `d_mirror_chat`: Mirror CHAT messages such as player chat.

The standard Q2Admin configuration parameters are documented within the various configuration files.
