# q2admin.so linux makefile

# This builds in the native mode of the current OS by default.

# Note that you might need to install the 32-bit libc package
# if it isn't already installed on your platform.
# Examples:
# sudo apt-get install ia32-libs
# sudo apt-get install libc6-dev-i386
# On Ubuntu 16.x use sudo apt install libc6-dev-i386

WITH_DISCORD:=yes

# Stole This From Yamagi

# Detect the OS
ifdef SystemRoot
Q2A_OSTYPE ?= Windows
else
Q2A_OSTYPE ?= $(shell uname -s)
endif

# Special case for MinGW
ifneq (,$(findstring MINGW,$(Q2A_OSTYPE)))
Q2A_OSTYPE := Windows
endif

# Detect the architecture
ifeq ($(Q2A_OSTYPE), Windows)
ifdef MINGW_CHOST
ifeq ($(MINGW_CHOST), x86_64-w64-mingw32)
Q2A_ARCH ?= x86_64
else # i686-w64-mingw32
Q2A_ARCH ?= i386
endif
else # windows, but MINGW_CHOST not defined
ifdef PROCESSOR_ARCHITEW6432
# 64 bit Windows
Q2A_ARCH ?= $(PROCESSOR_ARCHITEW6432)
else
# 32 bit Windows
Q2A_ARCH ?= $(PROCESSOR_ARCHITECTURE)
endif
endif # windows but MINGW_CHOST not defined
else
ifneq ($(Q2A_OSTYPE), Darwin)
# Normalize some abiguous Q2A_ARCH strings
Q2A_ARCH ?= $(shell uname -m | sed -e 's/i.86/i386/' -e 's/amd64/x86_64/' -e 's/^arm.*/arm/')
else
Q2A_ARCH ?= $(shell uname -m)
endif
endif

# On Windows / MinGW $(CC) is undefined by default.
ifeq ($(Q2A_OSTYPE),Windows)
CC ?= gcc
endif

# Detect the compiler
ifeq ($(shell $(CC) -v 2>&1 | grep -c "clang version"), 1)
COMPILER := clang
COMPILERVER := $(shell $(CC)  -dumpversion | sed -e 's/\.\([0-9][0-9]\)/\1/g' -e 's/\.\([0-9]\)/0\1/g' -e 's/^[0-9]\{3,4\}$$/&00/')
else ifeq ($(shell $(CC) -v 2>&1 | grep -c -E "(gcc version|gcc-Version)"), 1)
COMPILER := gcc
COMPILERVER := $(shell $(CC)  -dumpversion | sed -e 's/\.\([0-9][0-9]\)/\1/g' -e 's/\.\([0-9]\)/0\1/g' -e 's/^[0-9]\{3,4\}$$/&00/')
else
COMPILER := unknown
endif

# ASAN includes DEBUG
ifdef ASAN
DEBUG=1
endif

# UBSAN includes DEBUG
ifdef UBSAN
DEBUG=1
endif

# ----------

# Base CFLAGS. These may be overridden by the environment.
# Highest supported optimizations are -O2, higher levels
# will likely break this crappy code.
ifdef DEBUG
CFLAGS ?= -O0 -g -Wall -pipe
ifdef ASAN
override CFLAGS += -fsanitize=address -DUSE_SANITIZER
endif
ifdef UBSAN
override CFLAGS += -fsanitize=undefined -DUSE_SANITIZER
endif
else
CFLAGS ?= -O2 -Wall -pipe -fomit-frame-pointer
endif

# Always needed are:
#  -fno-strict-aliasing since the source doesn't comply
#   with strict aliasing rules and it's next to impossible
#   to get it there...
#  -fwrapv for defined integer wrapping. MSVC6 did this
#   and the game code requires it.
#  -fvisibility=hidden to keep symbols hidden. This is
#   mostly best practice and not really necessary.
override CFLAGS += -std=gnu99 -fno-strict-aliasing -fwrapv -fvisibility=hidden

# -MMD to generate header dependencies. Unsupported by
#  the Clang shipped with OS X.
ifneq ($(Q2A_OSTYPE), Darwin)
override CFLAGS += -MMD
endif

# OS X architecture.
ifeq ($(Q2A_OSTYPE), Darwin)
override CFLAGS += -arch $(Q2A_ARCH)
endif

# ----------

# ARM needs a sane minimum architecture. We need the `yield`
# opcode, arm6k is the first iteration that supports it. arm6k
# is also the first Raspberry PI generation and older hardware
# is likely too slow to run the game. We're not enforcing the
# minimum architecture, but if you're build for something older
# like arm5 the `yield` opcode isn't compiled in and the game
# (especially q2ded) will consume more CPU time than necessary.
ifeq ($(Q2A_ARCH), arm)
CFLAGS += -march=armv6k
endif

# ----------

# Switch of some annoying warnings.
ifeq ($(COMPILER), clang)
	# -Wno-missing-braces because otherwise clang complains
	#  about totally valid 'vec3_t bla = {0}' constructs.
	override CFLAGS += -Wno-missing-braces
else ifeq ($(COMPILER), gcc)
	# GCC 8.0 or higher.
	ifeq ($(shell test $(COMPILERVER) -ge 80000; echo $$?),0)
	    # -Wno-format-truncation and -Wno-format-overflow
		# because GCC spams about 50 false positives.
		override CFLAGS += -Wno-format-truncation -Wno-format-overflow
	endif
endif

# ----------

# Defines the operating system and architecture
override CFLAGS += -DOSTYPE=\"$(Q2A_OSTYPE)\" -DARCH=\"$(Q2A_ARCH)\"

# ----------

# For reproduceable builds, look here for details:
# https://reproducible-builds.org/specs/source-date-epoch/
ifdef SOURCE_DATE_EPOCH
override CFLAGS += -DBUILD_DATE=\"$(shell date --utc --date="@${SOURCE_DATE_EPOCH}" +"%b %_d %Y" | sed -e 's/ /\\ /g')\"
endif

# ----------

# Using the default x87 float math on 32bit x86 causes rounding trouble
# -ffloat-store could work around that, but the better solution is to
# just enforce SSE - every x86 CPU since Pentium3 supports that
# and this should even improve the performance on old CPUs
ifeq ($(Q2A_ARCH), i386)
override CFLAGS += -msse -mfpmath=sse
endif

# Force SSE math on x86_64. All sane compilers should do this
# anyway, just to protect us from broken Linux distros.
ifeq ($(Q2A_ARCH), x86_64)
override CFLAGS += -mfpmath=sse
endif

# Disable floating-point expression contraction. While this shouldn't be
# a problem for C (only for C++) better be safe than sorry. See
# https://gcc.gnu.org/bugzilla/show_bug.cgi?id=100839 for details.
ifeq ($(COMPILER), gcc)
override CFLAGS += -ffp-contract=off
endif

# ----------

# Base include path.
ifeq ($(Q2A_OSTYPE),Linux)
INCLUDE ?= -I/usr/include
else ifeq ($(Q2A_OSTYPE),FreeBSD)
INCLUDE ?= -I/usr/local/include
else ifeq ($(Q2A_OSTYPE),NetBSD)
INCLUDE ?= -I/usr/pkg/include
else ifeq ($(Q2A_OSTYPE),OpenBSD)
INCLUDE ?= -I/usr/local/include
else ifeq ($(Q2A_OSTYPE),Windows)
INCLUDE ?= -I/usr/include
endif

# ----------

# Base LDFLAGS. This is just the library path.
ifeq ($(Q2A_OSTYPE),Linux)
LDFLAGS ?= -L/usr/lib
else ifeq ($(Q2A_OSTYPE),FreeBSD)
LDFLAGS ?= -L/usr/local/lib
else ifeq ($(Q2A_OSTYPE),NetBSD)
LDFLAGS ?= -L/usr/pkg/lib -Wl,-R/usr/pkg/lib
else ifeq ($(Q2A_OSTYPE),OpenBSD)
LDFLAGS ?= -L/usr/local/lib
else ifeq ($(Q2A_OSTYPE),Windows)
LDFLAGS ?= -L/usr/lib
endif

# It's a shared library.
override LDFLAGS += -shared

# Link address sanitizer if requested.
ifdef ASAN
override LDFLAGS += -fsanitize=address
endif

# Link undefined behavior sanitizer if requested.
ifdef UBSAN
override LDFLAGS += -fsanitize=undefined
endif

# Required libraries.
ifeq ($(Q2A_OSTYPE),Linux)
ifeq ($(WITH_DISCORD),yes)
override CFLAGS += -DUSE_DISCORD
LDLIBS ?= -ldiscord -lcurl -lcrypto -lpthread -lm -ldl -rdynamic
else
LDLIBS ?= -lm -ldl -rdynamic
endif
else ifeq ($(Q2A_OSTYPE),FreeBSD)
LDLIBS ?= -lm
else ifeq ($(Q2A_OSTYPE),NetBSD)
LDLIBS ?= -lm
else ifeq ($(Q2A_OSTYPE),OpenBSD)
LDLIBS ?= -lm
else ifeq ($(Q2A_OSTYPE),Windows)
LDLIBS ?= -static-libgcc
else ifeq ($(Q2A_OSTYPE), Darwin)
LDLIBS ?= -arch $(Q2A_ARCH)
else ifeq ($(Q2A_OSTYPE), Haiku)
LDLIBS ?= -lm
else ifeq ($(Q2A_OSTYPE), SunOS)
LDLIBS ?= -lm
endif

# ASAN and UBSAN must not be linked
# with --no-undefined. OSX and OpenBSD
# don't support it at all.
ifndef ASAN
ifndef UBSAN
ifneq ($(Q2A_OSTYPE), Darwin)
ifneq ($(Q2A_OSTYPE), OpenBSD)
override LDFLAGS += -Wl,--no-undefined
endif
endif
endif
endif

# ----------

# When make is invoked by "make VERBOSE=1" print
# the compiler and linker commands.
ifdef VERBOSE
Q :=
else
Q := @
endif

# ----------

# Phony targets
.PHONY : all q2admin

# ----------

# Builds everything
all: q2admin

# ----------

# Print config values
config:
	@echo "Build configuration"
	@echo "============================"
	@echo "Q2A_ARCH     = $(Q2A_ARCH) COMPILER = $(COMPILER)"
	@echo "WITH_DISCORD = $(WITH_DISCORD)"
	@echo "============================"
	@echo ""

# ----------

# Cleanup
clean:
	@echo "===> CLEAN"
	${Q}rm -Rf build release/*

cleanall:
	@echo "===> CLEAN"
	${Q}rm -Rf build release

# ----------

ifeq ($(Q2A_OSTYPE), Windows)
q2admin:
	@echo "===> Building game.dll"
	${Q}mkdir -p release
	$(MAKE) release/game.dll
else ifeq ($(Q2A_OSTYPE), Darwin)
q2admin:
	@echo "===> Building game.dylib"
	${Q}mkdir -p release
	$(MAKE) release/game.dylib
else
q2admin:
	@echo "===> Building game.so"
	${Q}mkdir -p release
	$(MAKE) release/game.so

release/game.so : CFLAGS += -fPIC
endif

build/%.o: %.c
	@echo "===> CC $<"
	${Q}mkdir -p $(@D)
	${Q}$(CC) -c $(CFLAGS) -o $@ $<
	
# ----------

Q2A_OBJS_ = \
	g_main.o \
	zb_spawn.o \
	zb_vote.o \
	zb_ban.o \
	zb_cmd.o \
	zb_flood.o \
	zb_init.o \
	zb_log.o \
	zb_lrcon.o \
	zb_msgqueue.o \
	zb_util.o \
	zb_zbot.o \
	zb_zbotcheck.o \
	zb_disable.o \
	zb_checkvar.o \
	zb_discord.o

# ----------

# Rewrite paths to our object directory
Q2A_OBJS = $(patsubst %,build/%,$(Q2A_OBJS_))

# ----------

# Generate header dependencies
Q2A_DEPS = $(Q2A_OBJS:.o=.d)

# Suck header dependencies in
-include $(Q2A_DEPS)

# ----------

# release/quake2
ifeq ($(Q2A_OSTYPE), Windows)
release/game.dll : $(Q2A_OBJS) icon
	@echo "===> LD $@"
	${Q}$(CC) -o $@ $(Q2A_OBJS) $(LDFLAGS) $(LDLIBS)
else ifeq ($(Q2A_OSTYPE), Darwin)
release/game.dylib : $(Q2A_OBJS)
	@echo "===> LD $@"
	${Q}$(CC) -o $@ $(Q2A_OBJS) $(LDFLAGS) $(LDLIBS)
else
release/game.so : $(Q2A_OBJS)
	@echo "===> LD $@"
	${Q}$(CC) -o $@ $(Q2A_OBJS) $(LDFLAGS) $(LDLIBS)
endif

# ----------
