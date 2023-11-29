GCC = g++
CFLAGS = -Wall -Wextra

SRC = orca.cpp
OBJ = $(SRC:.cpp=.o)
TARGET = orca

all: $(TARGET)

%.o: %.c
	$(GCC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJ)
	$(GCC) $(CFLAGS) $(OBJ) -o $(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)
