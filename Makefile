CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g

SRC_DIR = src
BUILD   = build
TARGET  = $(BUILD)/server

SRCS = \
	main.c \
	$(SRC_DIR)/net.c \
	$(SRC_DIR)/lobby.c \
	$(SRC_DIR)/protocol.c \
	$(SRC_DIR)/game.c \
	$(SRC_DIR)/log.c

OBJS = $(SRCS:%.c=$(BUILD)/%.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c $< -o $@

clean:
	rm -rf $(BUILD)
