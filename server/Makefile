all: server

server: server.c
	gcc -o server -g -Wall -fsanitize=address -Wvla server.c -pthread
