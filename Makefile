.PHONY: all

BG80D_PATH=bg80d
USE_BG80D=1

# OPT=-g
OPT=-g -O2
LDFLAGS=
LDFLAGS_GLFW=-L/opt/local/lib
LDFLAGS_SDL=-L/opt/local/lib
LDLIBS=-lreadline
LDLIBS_GLFW=-lreadline -lao -lglfw -framework OpenGL -framework Cocoa -framework IOkit
LDLIBS_SDL=-lreadline -lSDL2 -framework OpenGL -framework Cocoa -framework IOkit
CXXFLAGS=-Wall -I/opt/local/include -I$(BG80D_PATH) -DUSE_BG80D=$(USE_BG80D) -std=c++17 $(OPT) -fsigned-char -DGL_SILENCE_DEPRECATION
CFLAGS	+=	-fsigned-char

VPATH=$(BG80D_PATH)

all: emulator emulator_terminal emulator_sdl
# hex2bin hexinfo

OBJECTS_GLFW = emulator.o readhex.o coleco_platform_glfw.o gl_utility.o
OBJECTS_SDL = emulator.o coleco_platform_sdl.o
OBJECTS_TERMINAL = emulator.o coleco_platform_template.o

emulator: $(OBJECTS_GLFW)
	$(CXX) $(LDFLAGS_GLFW) $^   -o $@ $(LDLIBS_GLFW)

emulator_terminal: $(OBJECTS_TERMINAL)
	$(CXX) $(LDFLAGS) $^   -o $@ $(LDLIBS)

emulator_sdl: $(OBJECTS_SDL)
	$(CXX) $(LDFLAGS_SDL) $^   -o $@ $(LDLIBS_SDL)

hexinfo: hexinfo.o readhex.o
	$(CC) hexinfo.o readhex.o -o hexinfo

hex2bin: hex2bin.o readhex.o
	$(CC) hex2bin.o readhex.o -o hex2bin

clean:
	rm emulator $(OBJECTS_GLFW) emulator_terminal $(OBJECTS_TERMINAL) emulator_sdl $(OBJECTS_SDL)

immaculate: clean
	rm tables.h maketables

emulator.o: emulator.h bg80d.h coleco_platform.h tms9918.h

coleco_platform_glfw.o: coleco_platform.h tms9918.h
coleco_platform_empty.o: coleco_platform.h tms9918.h
coleco_platform_sdl.o: coleco_platform.h tms9918.h

readhex.o: readhex.c readhex.h

hex2bin.o: hex2bin.c readhex.h

tables.h: maketables.c
	$(CC) -Wall $< -o maketables
	./maketables > $@

coleco.js: emulator.cpp coleco_platform_sdl.cpp emulator.h coleco_platform.h tms9918.h
	em++ -Wall -I/opt/local/include -Ibg80d -DUSE_BG80D=1 -std=c++17 -g -O3 -fsigned-char --preload-file OurColeco@/ --preload-file others@/ emulator.cpp coleco_platform_sdl.cpp -s USE_SDL=2 -s WASM=1 -s ASSERTIONS=1 -o coleco.js
