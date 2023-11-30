GCC = g++
CFLAGS = -Wall -Wextra

all: orca orca_client

orca: orca.o
	$(GCC) $(CFLAGS) orca.o -o orca

orca_client: orca_client.o
	$(GCC) $(CFLAGS) orca_client.o -o orca_client

%.o: %.c
	$(GCC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o orca orca_client
