CC = gcc
CFLAGS = -Wall -Wextra -O2
TARGET = battery-monitor

all: $(TARGET)

$(TARGET): battery-monitor.c
	$(CC) $(CFLAGS) -o $(TARGET) battery-monitor.c

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

.PHONY: all clean install
