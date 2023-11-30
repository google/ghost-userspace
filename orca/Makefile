GCC = g++
CFLAGS = -Wall -Wextra

all: orca orca_client

orca: $@.o
	$(GCC) $(CFLAGS) $^ -o $@

orca_client: $@.o
	$(GCC) $(CFLAGS) $^ -o $@

%.o: %.c
	$(GCC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
