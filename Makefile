CC = clang++
CFLAGS = -Wall -Wextra -O2 -Iinclude/
LDFLAGS = -lX11

SRC = src/main.cpp src/core/krka.cpp src/core/log.cpp
OUT = krkawm

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(OUT)
