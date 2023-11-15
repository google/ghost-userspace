GCC = g++
CFLAGS = -Wall -Wextra

SRC = main.cpp
OBJ = $(SRC:.cpp=.o)
TARGET = orchestrator

all: $(TARGET)

%.o: %.c
	$(GCC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJ)
	$(GCC) $(CFLAGS) $(OBJ) -o $(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)
