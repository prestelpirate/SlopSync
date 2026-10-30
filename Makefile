# radicalesync - J-Pilot plugin for two-way sync with Radicale
JPILOT_SRC ?= $(HOME)/Downloads/jpilot-2.1.0
PLUGIN_DIR ?= $(HOME)/.jpilot/plugins

PKGS    = gtk+-3.0 gio-2.0 libical
CFLAGS ?= -O2 -g
CFLAGS += -fPIC -Wall -Wno-deprecated-declarations \
          -Icompat -I$(JPILOT_SRC) $(shell pkg-config --cflags $(PKGS))
LIBS    = $(shell pkg-config --libs $(PKGS)) -lpisock

OBJS = plugin.o sync.o http.o ical.o vcard.o

all: libradicalesync.so

libradicalesync.so: $(OBJS)
	$(CC) -shared -o $@ $(OBJS) $(LIBS)

%.o: %.c rs.h
	$(CC) $(CFLAGS) -c $< -o $@

install: libradicalesync.so
	mkdir -p $(PLUGIN_DIR)
	install -m 0644 libradicalesync.so $(PLUGIN_DIR)/

clean:
	rm -f *.o *.so test/*.o test/rstest

.PHONY: all install clean check

# End-to-end test against a Radicale server (see test/run.sh)
test/rstest: sync.o http.o ical.o vcard.o test/stubs.c test/rstest.c
	$(CC) $(CFLAGS) -I. -Itest -o $@ test/rstest.c test/stubs.c sync.o http.o ical.o vcard.o $(LIBS)
