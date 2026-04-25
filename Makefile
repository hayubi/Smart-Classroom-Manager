CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread
RAYLIB  = $(shell pkg-config --libs raylib 2>/dev/null || echo "-lraylib -lm -ldl -lpthread")
RAYINC  = $(shell pkg-config --cflags raylib 2>/dev/null || echo "")

TARGET     = classroom_sim
GUI_TARGET = classroom_gui

CORE_SRCS  = resources.c scheduler.c logger.c stats.c
CORE_OBJS  = $(CORE_SRCS:.c=.o)

all: $(TARGET) $(GUI_TARGET)

$(TARGET): main.o $(CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(GUI_TARGET): gui.o $(CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(RAYLIB) -lm

gui.o: gui.c
	$(CC) $(CFLAGS) $(RAYINC) -c gui.c -o gui.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o $(TARGET) $(GUI_TARGET) classroom_log.txt

.PHONY: all clean
