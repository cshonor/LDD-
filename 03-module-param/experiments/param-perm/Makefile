CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
TARGET   = param_probe

all: $(TARGET)

$(TARGET): param_probe.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
