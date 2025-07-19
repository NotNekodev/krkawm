CC = clang++
CFLAGS = -Wall -Wextra -O2 -Iinclude $(shell pkg-config --cflags imlib2)
LDFLAGS = -lX11 -lXrandr $(shell pkg-config --libs imlib2) -lm

SRC = src/main.cpp src/core/krka.cpp src/core/log.cpp src/core/displays.cpp
OUT = krkawm

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(OUT)
