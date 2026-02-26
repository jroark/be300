CC      ?= clang
PKG_CONFIG ?= pkg-config
UNAME_S := $(shell uname -s)

# Detect whether pkg-config is available and has a unicorn entry.  Guarding the
# command avoids noisy errors on hosts where pkg-config is absent (e.g. a clean
# macOS install).
HAVE_PKG_CONFIG := $(shell if command -v $(PKG_CONFIG) >/dev/null 2>&1; then echo 1; fi)
ifeq ($(HAVE_PKG_CONFIG),1)
UNICORN_PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags unicorn 2>/dev/null)
UNICORN_PKG_LIBDIR := $(shell $(PKG_CONFIG) --variable=libdir unicorn 2>/dev/null)
endif

# Prefer the pip-installed unicorn 2.1.4 over the apt 2.0.1 package.
# PKG_CONFIG_PATH is set to pick up /usr/local/lib/pkgconfig/unicorn.pc first.
LOCAL_UNICORN_DIR := $(CURDIR)/third_party/unicorn
LINUX_UNICORN_LIBDIR := $(CURDIR)/third_party/unicorn-linux/lib
LINUX_UNICORN_INCLUDEDIR := $(CURDIR)/third_party/unicorn-linux/include
LOCAL_UNICORN_INCLUDEDIR := $(LOCAL_UNICORN_DIR)/include

DEFAULT_UNICORN_LIBDIR :=
DEFAULT_UNICORN_INCLUDEDIR :=

ifeq ($(UNAME_S),Darwin)
  ifneq ($(wildcard $(LOCAL_UNICORN_DIR)/libunicorn.dylib),)
    DEFAULT_UNICORN_LIBDIR := $(LOCAL_UNICORN_DIR)
    DEFAULT_UNICORN_INCLUDEDIR := $(LOCAL_UNICORN_INCLUDEDIR)
  else ifneq ($(UNICORN_PKG_LIBDIR),)
    DEFAULT_UNICORN_LIBDIR := $(UNICORN_PKG_LIBDIR)
  endif
else
  ifneq ($(wildcard $(LINUX_UNICORN_LIBDIR)/libunicorn.so),)
    DEFAULT_UNICORN_LIBDIR := $(LINUX_UNICORN_LIBDIR)
    DEFAULT_UNICORN_INCLUDEDIR := $(LINUX_UNICORN_INCLUDEDIR)
  else ifneq ($(wildcard $(LOCAL_UNICORN_DIR)/libunicorn.dylib),)
    DEFAULT_UNICORN_LIBDIR := $(LOCAL_UNICORN_DIR)
    DEFAULT_UNICORN_INCLUDEDIR := $(LOCAL_UNICORN_INCLUDEDIR)
  else ifneq ($(UNICORN_PKG_LIBDIR),)
    DEFAULT_UNICORN_LIBDIR := $(UNICORN_PKG_LIBDIR)
  endif
endif

UNICORN_LIBDIR ?= $(DEFAULT_UNICORN_LIBDIR)
UNICORN_INCLUDEDIR ?= $(DEFAULT_UNICORN_INCLUDEDIR)

ifneq ($(strip $(UNICORN_PKG_CFLAGS)),)
UNICORN_EXTRA_CFLAGS := $(UNICORN_PKG_CFLAGS)
else ifneq ($(strip $(UNICORN_INCLUDEDIR)),)
UNICORN_EXTRA_CFLAGS := -I$(UNICORN_INCLUDEDIR)
else
UNICORN_EXTRA_CFLAGS :=
endif

ifneq ($(strip $(UNICORN_LIBDIR)),)
UNICORN_EXTRA_LDFLAGS := -L$(UNICORN_LIBDIR) -Wl,-rpath,$(UNICORN_LIBDIR)
else
UNICORN_EXTRA_LDFLAGS :=
endif

CFLAGS  = -std=c11 -Wall -Wextra -Isrc -g -O2 $(UNICORN_EXTRA_CFLAGS)
LDFLAGS = $(UNICORN_EXTRA_LDFLAGS) -lunicorn

OBJS = build/main.o \
       build/machine.o \
       build/bus.o \
       build/loader.o \
       build/macc.o \
       build/hw/bcu.o \
       build/hw/cmu.o \
       build/hw/pmu.o \
       build/hw/icu.o \
       build/hw/siu.o \
       build/hw/rtc.o \
       build/hw/gpio.o

TARGET = be300

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $^ $(LDFLAGS) -o $@

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/hw/%.o: src/hw/%.c
	@mkdir -p build/hw
	$(CC) $(CFLAGS) -c $< -o $@

# Test binary links everything except main.o
TEST_OBJS = $(filter-out build/main.o, $(OBJS))

test: $(TEST_OBJS) tests/test_basic.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o build/test_basic
	./build/test_basic

clean:
	rm -rf build $(TARGET)
