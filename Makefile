all:
	gcc -Wall -Wextra -g -o server server.c -lpthread -lrt
