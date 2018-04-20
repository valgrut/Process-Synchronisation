PROJ=proj2
CC=gcc
FLAGS=-std=gnu99 -Wall -Wextra -Werror -pedantic

proj2: proj2.c
	$(CC) $(FLAGS) $(PROJ).c -o $(PROJ) -lpthread -lrt

clean:
	rm -f *.o
