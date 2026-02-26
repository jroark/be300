CC      = clang
# Prefer the pip-installed unicorn 2.1.4 over the apt 2.0.1 package.
# PKG_CONFIG_PATH is set to pick up /usr/local/lib/pkgconfig/unicorn.pc first.
LOCAL_UNICORN_DIR := $(CURDIR)/third_party/unicorn
LINUX_UNICORN_LIBDIR := $(CURDIR)/third_party/unicorn-linux/lib
ifneq ($(wildcard $(LINUX_UNICORN_LIBDIR)/libunicorn.so),)
DEFAULT_UNICORN_LIBDIR := $(LINUX_UNICORN_LIBDIR)
else ifneq ($(wildcard $(LOCAL_UNICORN_DIR)/libunicorn.dylib),)
DEFAULT_UNICORN_LIBDIR := $(LOCAL_UNICORN_DIR)
else
DEFAULT_UNICORN_LIBDIR := $(shell pkg-config --variable=libdir unicorn)
endif
UNICORN_LIBDIR ?= $(DEFAULT_UNICORN_LIBDIR)

CFLAGS  = -std=c11 -Wall -Wextra -Isrc -g -O2 \
          $(shell pkg-config --cflags unicorn)
LDFLAGS = -L$(UNICORN_LIBDIR) -lunicorn \
          -Wl,-rpath,$(UNICORN_LIBDIR)

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
