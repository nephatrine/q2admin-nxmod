# -------------------------------
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
# -------------------------------

cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(
	Q2Admin
	VERSION 1.18.0
	DESCRIPTION "Quake II Game Proxy Mod"
	HOMEPAGE_URL "https://code.nephatrine.net/nephatrine/q2admin-nxmod"
	LANGUAGES C)

# ==== Include NXBuild ====

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set(NX_INSTALL_IS_FLAT ON)

include(NXBuild)
include(NXProject)
include(NXFormat)
include(NXTarget)

# ==== Q2Admin Project ====

nx_project_begin(
	MAINTAINER "Daniel Wolf"
	CONTACT "nephatrine@gmail.com"
	SUPPORT "${PROJECT_HOMEPAGE_URL}/issues"
	VENDOR "NephNET")

set(Q2ADMIN_SOURCES
	"src/g_local.h"
	"src/g_main.c"
	"src/game.h"
	"src/q_shared.h"
	"src/zb_ban.c"
	"src/zb_checkvar.c"
	"src/zb_cmd.c"
	"src/zb_disable.c"
	"src/zb_flood.c"
	"src/zb_init.c"
	"src/zb_log.c"
	"src/zb_lrcon.c"
	"src/zb_msgqueue.c"
	"src/zb_spawn.c"
	"src/zb_util.c"
	"src/zb_vote.c"
	"src/zb_zbot.c"
	"src/zb_zbotcheck.c"
	"src/zbot.rc.in")

# ==== Q2Admin Configuration ====

if(COMMAND nx_default_flags)
	nx_default_flags()
endif()

unset(Q2ADMIN_DEPENDENCIES)
if(NX_TARGET_PLATFORM_WINDOWS)
	if(NX_HOST_COMPILER_MSVC)
		list(APPEND Q2ADMIN_SOURCES "src/regex.c" "src/regex.h")
	else()
		list(APPEND Q2ADMIN_DEPENDENCIES "regex")
	endif()
endif()

# Module Name

if(NOT DEFINED Q2ADMIN_NAME)
	set(Q2ADMIN_NAME "game")
	if(NX_TARGET_ARCHITECTURE_AMD64)
		if(NX_TARGET_PLATFORM_WINDOWS)
			set(Q2ADMIN_NAME "gamex64")
		else()
			set(Q2ADMIN_NAME "gamex86_64")
		endif()
	elseif(NX_TARGET_ARCHITECTURE_IA32)
		if(NX_TARGET_PLATFORM_WINDOWS)
			set(Q2ADMIN_NAME "gamex86")
		else()
			set(Q2ADMIN_NAME "gamei386")
		endif()
	elseif(NX_TARGET_ARCHITECTURE_ARMV7)
		set(Q2ADMIN_NAME "gamearm")
	elseif(NX_TARGET_ARCHITECTURE_ARM64)
		if(NX_TARGET_PLATFORM_WINDOWS)
			set(Q2ADMIN_NAME "gamearm64")
		else()
			set(Q2ADMIN_NAME "gameaarch64")
		endif()
	endif()
endif()
set(Q2ADMIN_DEFINES "GAMENAME=\"${Q2ADMIN_NAME}\"" "GAMEEXT=\"${CMAKE_SHARED_MODULE_SUFFIX}\"")

# Discord Support

set(bCanDiscord OFF)
if(NX_TARGET_PLATFORM_POSIX AND NX_TARGET_ARCHITECTURE_NATIVE)
	set(THREADS_PREFER_PTHREAD_FLAG ON)
	find_package(Threads)

	if(Threads_FOUND AND CMAKE_USE_PTHREADS_INIT)
		set(bCanDiscord ON)
	endif()
endif()

cmake_dependent_option(WITH_DISCORD "Enable Discord Support" ON "bCanDiscord" OFF)

# ==== Orca Target ====

unset(ORCA_DISCORD_INCLUDE_DIR)
unset(ORCA_DISCORD_LIBRARY)

if(WITH_DISCORD)
	list(APPEND Q2ADMIN_SOURCES "src/zb_discord.c" "src/zb_discord.h")
	list(APPEND Q2ADMIN_DEFINES "USE_DISCORD=1")

	include(ExternalProject)
	ExternalProject_Add(
		orca
		GIT_REPOSITORY "https://github.com/cee-studio/orca.git"
		GIT_TAG "ef7cc8d1eca1468c75fde7ba9f183b41c26ef1bc"
		GIT_SHALLOW ON
		GIT_PROGRESS ON
		CONFIGURE_COMMAND ""
		BUILD_COMMAND make discord github
		BUILD_IN_SOURCE ON
		INSTALL_COMMAND "${CMAKE_COMMAND}" -E env PREFIX="<INSTALL_DIR>" make install)

	ExternalProject_Get_Property(orca INSTALL_DIR)
	set(ORCA_DISCORD_INCLUDE_DIR "${INSTALL_DIR}/include")
	set(ORCA_DISCORD_LIBRARY "${INSTALL_DIR}/lib/libdiscord.a")
	set(ORCA_DISCORD_DEPENDENCIES "curl" "Threads::Threads")
endif()

# ==== Q2Admin Target ====

nx_target(
	Q2ADMIN_TARGETS "q2admin" SHARED_MODULE
	OUTPUT_NAME "${Q2ADMIN_NAME}"
	GENERATE_EXPORT "generated/g_export.h" Q2ADMIN
	GENERATE_VERSION "generated/g_version.h" Q2ADMIN
	DEFINES PRIVATE ${Q2ADMIN_DEFINES}
	DEPENDS PRIVATE "${ORCA_DISCORD_LIBRARY}" ${ORCA_DISCORD_DEPENDENCIES} ${CMAKE_DL_LIBS}
	FEATURES PRIVATE "c_std_99"
	INCLUDES PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated" "${ORCA_DISCORD_INCLUDE_DIR}"
	SOURCES PRIVATE ${Q2ADMIN_SOURCES})

if(WITH_DISCORD)
	add_dependencies("${Q2ADMIN_TARGETS}" "orca")
endif()

# ==== Project End ====

nx_format_clang(FILES "src/zb_discord.c" "src/zb_discord.h")
nx_project_end()
