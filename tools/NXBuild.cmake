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

if(NOT COMMAND nx_project_begin)
	include(FetchContent)
	FetchContent_Declare(
		nxbuild
		GIT_REPOSITORY "https://code.nephatrine.net/nephatrine/nxbuild-cmake.git"
		GIT_TAG "master"
		GIT_SHALLOW ON)
	if(NOT nxbuild_POPULATED)
		FetchContent_Populate(nxbuild)
	endif()
	list(APPEND CMAKE_MODULE_PATH "${nxbuild_SOURCE_DIR}/tools")
endif()
