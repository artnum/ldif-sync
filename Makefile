DEBUG ?= 1

NAME=ldif-sync
PKG_PACKAGES=libxxhash

ifeq ($(strip $(NAME)),)
	$(error NAME is empty)
endif

ifneq ($(strip $(PKG_PACKAGES)),)
    PKG_CFLAGS := $(shell pkg-config --cflags $(PKG_PACKAGES))
    PKG_LIBS   := $(shell pkg-config --libs $(PKG_PACKAGES))
endif

ifeq ($(DEBUG), 1)
	CFLAGS :=-Wall -Wextra -pedantic -ggdb -O0 -fno-omit-frame-pointer $(PKG_CFLAGS)
	LIBS :=-ggdb $(PKG_LIBS)
else
	CFLAGS :=-O2 -march=native -DNDEBUG $(PKG_CFLAGS)
	LIBS :=$(PKG_LIBS)
endif

SRCFILES=$(wildcard src/*.c)
OBJFILES=$(addprefix build/, $(addsuffix .o,$(basename $(notdir $(SRCFILES)))))
RM=rm
CC=gcc


all: $(NAME)

$(NAME): $(OBJFILES)
	$(CC) $^ -o $(NAME) $(LIBS)


build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@ $(LIBS)

build:
	mkdir -p $@

backend: build
	mkdir -p build/$@

clean:
	$(RM) $(wildcard $(OBJFILES) $(NAME))
