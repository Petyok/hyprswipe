CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra $(shell pkg-config --cflags libevdev)
LIBS     = $(shell pkg-config --libs libevdev)

BINS = test_swipe mousegest

all: $(BINS)

test_swipe: test_swipe.c vtouchpad.c vtouchpad.h
	$(CC) $(CFLAGS) -o $@ test_swipe.c vtouchpad.c $(LIBS)

mousegest: mousegest.c vtouchpad.c vtouchpad.h
	$(CC) $(CFLAGS) -o $@ mousegest.c vtouchpad.c $(LIBS)

PREFIX ?= /usr/local

install: mousegest
	install -m755 mousegest $(PREFIX)/bin/mousegest

uninstall:
	rm -f $(PREFIX)/bin/mousegest

clean:
	rm -f $(BINS)

.PHONY: all clean install uninstall
