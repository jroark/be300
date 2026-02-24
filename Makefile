CC      = clang
CFLAGS  = -std=c11 -Wall -Wextra -Isrc -g -O2 \
          $(shell pkg-config --cflags unicorn)
LDFLAGS = $(shell pkg-config --libs unicorn)

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
