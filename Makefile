CC = gcc
CFLAGS = -Wall -Wextra -O2
TARGET = hstrings
PREFIX = /usr/local

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) $< -o $@

install: $(TARGET)
	install -D -m 0755 $(TARGET) $(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean
