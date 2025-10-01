CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c99 -O2 -g

.PHONY: all clean
all: task_manager

task_manager: main.o
	$(CC) $(CFLAGS) -o $@ $^

main.o: main.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o task_manager
