CC ?= gcc
CFLAGS ?= -shared -fPIC -O2 -Wall -Wextra -Werror -pthread
TARGET ?= usage-metrics.so

.PHONY: all clean

all: $(TARGET)

ui_assets.h: ui.html gen_ui_header.py
	python3 gen_ui_header.py

$(TARGET): plugin.c ui_assets.h
	$(CC) $(CFLAGS) plugin.c -o $@

clean:
	rm -f $(TARGET) ui_assets.h
