ifndef PS5_PAYLOAD_SDK
$(error Set PS5_PAYLOAD_SDK to your PS5 Payload SDK directory)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
TARGET=ps5-nfc-launcher.elf
SRC=src/main.c src/core.c src/ndef.c src/sony.c src/reader.c src/http.c src/games.c vendor/cJSON.c
CFLAGS=-O2 -Wall -Wextra -Werror -Wno-unreachable-code-generic-assoc -Isrc -Ivendor -fPIC
LDLIBS=-lSceSystemService -lSceUserService -lpthread -ldl
all: $(TARGET)
src/webui.h: web/index.html embed.py
	python3 embed.py
$(TARGET): $(SRC) src/webui.h $(wildcard src/*.h)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDLIBS)
clean:
	rm -f $(TARGET)
.PHONY: all clean
