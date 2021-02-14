FLAGS = -lpthread -o mcp

mcp: mcp.c
	gcc $(FLAGS) mcp.c

install: mcp
	sudo mv mcp /usr/bin

clean:
	sudo rm /usr/bin/mcp
