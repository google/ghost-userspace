GCC = g++
CFLAGS = -Wall -Wextra

SRC = orc.cpp
OBJ = $(SRC:.cpp=.o)
TARGET = orc 

all: $(TARGET)

%.o: %.c
	$(GCC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJ)
	$(GCC) $(CFLAGS) $(OBJ) -o $(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)
