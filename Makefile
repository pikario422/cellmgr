CC ?= gcc
AR ?= ar
STRIP ?= strip

TARGET ?= cellmgrd
PREFIX ?= /usr
SYSCONFDIR ?= /etc/cellmgr

SRC_DIR := src
BUILD_DIR := build
USE_BUNDLED_SQLITE ?= 0
SQLITE_DIR ?= third_party/sqlite

SRCS := $(wildcard $(SRC_DIR)/*.c)

ifeq ($(USE_BUNDLED_SQLITE),1)
SRCS += $(SQLITE_DIR)/sqlite3.c
CPPFLAGS += -I$(SQLITE_DIR) -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION
else
LDLIBS += -lsqlite3
endif

OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))

CFLAGS ?= -Os
CFLAGS += -std=c99 -Wall -Wextra -Wshadow -Wformat=2 -D_POSIX_C_SOURCE=200809L
CFLAGS += -ffunction-sections -fdata-sections
CPPFLAGS += -DSYSCONFDIR=\"$(SYSCONFDIR)\"
LDFLAGS += -Wl,--gc-sections
LDLIBS += -pthread

.PHONY: all clean install strip

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(SYSCONFDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -m 0644 config/cellmgrd.conf $(DESTDIR)$(SYSCONFDIR)/cellmgrd.conf
	install -m 0644 profiles/fm650.json $(DESTDIR)$(SYSCONFDIR)/fm650.json

strip: $(TARGET)
	$(STRIP) $(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
