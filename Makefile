.PHONY: all

BG80D_PATH=bg80d
USE_BG80D=1

OPT=-g
# OPT=-g -O2
LDFLAGS=-L/opt/local/lib
# LDFLAGS=-pg -L/opt/local/lib
LDLIBS=-lreadline -lao -lglfw -framework OpenGL -framework Cocoa -framework IOkit
CXXFLAGS=-Wall -I/opt/local/include -I$(BG80D_PATH) -DUSE_BG80D=$(USE_BG80D) -std=c++11 $(OPT) -fsigned-char -DGL_SILENCE_DEPRECATION
CFLAGS	+=	-fsigned-char

VPATH=$(BG80D_PATH)

all: emulator
# hex2bin hexinfo

emulator: emulator.o z80emu.o readhex.o gl_utility.o
	$(CXX) $(LDFLAGS) $^   -o $@ $(LDLIBS)

hexinfo: hexinfo.o readhex.o
	$(CC) hexinfo.o readhex.o -o hexinfo

hex2bin: hex2bin.o readhex.o
	$(CC) hex2bin.o readhex.o -o hex2bin

clean:
	rm emulator emulator.o z80emu.o readhex.o

immaculate: clean
	rm tables.h maketables

emulator.o: emulator.h z80emu.h bg80d.h

z80emu.o: z80emu.cpp z80emu.h instructions.h macros.h tables.h emulator.h

readhex.o: readhex.c readhex.h

hex2bin.o: hex2bin.c readhex.h

tables.h: maketables.c
	$(CC) -Wall $< -o maketables
	./maketables > $@
