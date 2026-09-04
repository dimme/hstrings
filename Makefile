# CC, CFLAGS, CPPFLAGS, LDFLAGS, LDLIBS and PREFIX may all be overridden from
# the environment or on the command line, e.g.
#     make CC=clang CFLAGS="-O1 -g -fsanitize=address"
# WARNINGS is kept separate so that overriding CFLAGS does not silently turn
# the warnings and the language standard back off.
CFLAGS   ?= -O2
WARNINGS ?= -Wall -Wextra -std=c11 -pedantic
THREADS  ?= -pthread
CPPFLAGS ?=
LDFLAGS  ?=
LDLIBS   ?=

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DESTDIR ?=

TARGET = hstrings

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CPPFLAGS) $(WARNINGS) $(THREADS) $(CFLAGS) $(LDFLAGS) $< -o $@ $(LDLIBS)

test check: $(TARGET)
	./tests/run_tests.sh ./$(TARGET)

install: $(TARGET)
	install -D -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all test check install uninstall clean
