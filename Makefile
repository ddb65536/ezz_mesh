CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -pedantic -g
PREFIX  ?= build
OBJDIR  := $(PREFIX)/obj
BINDIR  := $(PREFIX)/bin
INCLUDES:= -Iinclude

# OpenWrt: ensure STAGING_DIR is set so pkg-config can find libubus/libubox
UBUS_CFLAGS ?= $(shell pkg-config --cflags libubus 2>/dev/null)
UBUS_LIBS   ?= $(shell pkg-config --libs libubus 2>/dev/null)
UBOX_CFLAGS ?= $(shell pkg-config --cflags libubox 2>/dev/null)
UBOX_LIBS   ?= $(shell pkg-config --libs libubox 2>/dev/null)

LIB1905 := $(PREFIX)/libieee1905.a

LIB_SRC := src/ieee1905/ieee1905.c
LIB_OBJ := $(LIB_SRC:src/%.c=$(OBJDIR)/%.o)

APP_SRC := src/apps/ezz_controller.c src/apps/ezz_agent.c src/apps/ieee1905d.c
APP_OBJ := $(APP_SRC:src/%.c=$(OBJDIR)/%.o)
APPS    := $(BINDIR)/ezz_controller $(BINDIR)/ezz_agent $(BINDIR)/ieee1905d

.PHONY: all clean dirs

all: dirs $(LIB1905) $(APPS)

dirs:
	@mkdir -p $(OBJDIR)/ieee1905 $(OBJDIR)/apps $(BINDIR) $(PREFIX)

$(OBJDIR)/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) $(UBUS_CFLAGS) $(UBOX_CFLAGS) -c $< -o $@

$(LIB1905): $(LIB_OBJ)
	@mkdir -p $(PREFIX)
	ar rcs $@ $^

$(BINDIR)/ieee1905d: $(OBJDIR)/apps/ieee1905d.o $(LIB1905)
	$(CC) $(CFLAGS) $(INCLUDES) $^ $(UBUS_LIBS) $(UBOX_LIBS) -o $@

$(BINDIR)/ezz_controller: $(OBJDIR)/apps/ezz_controller.o
	$(CC) $(CFLAGS) $(INCLUDES) $^ $(UBUS_LIBS) $(UBOX_LIBS) -o $@

$(BINDIR)/ezz_agent: $(OBJDIR)/apps/ezz_agent.o
	$(CC) $(CFLAGS) $(INCLUDES) $^ $(UBUS_LIBS) $(UBOX_LIBS) -o $@

clean:
	rm -rf $(PREFIX)


