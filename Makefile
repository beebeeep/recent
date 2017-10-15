TARGET=recent
LIBS=-lpcre

CC ?= gcc
CFLAGS += -std=gnu99 -Wall -pedantic -g -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700
INSTALL ?= install

OBJECTS = $(patsubst %.c, %.o, $(wildcard src/*.c))
HEADERS = $(wildcard *.h)

.PHONY: default all clean

default: $(TARGET)

all: default

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

.PRECIOUS: $(TARGET) $(OBJECTS)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) $(LIBS) -o $@

install: $(TARGET)
	$(INSTALL) -D -m 755 $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)
	$(INSTALL) -D -m 644 contrib/timestamps.conf $(DESTDIR)/etc/$(TARGET)/timestamps.conf

clean:
	rm -f src/*.o
	rm -f $(TARGET)
