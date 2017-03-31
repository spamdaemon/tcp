
.phony: all

all: bin/bridge


bin/bridge: obj/bridge.o
	@mkdir -p "$(@D)";
	@gcc "$<" -o "$@";

obj/bridge.o: src/bridge.c
	@mkdir -p "$(@D)";
	@gcc -c -g "$<" -o "$@";

clean:
	@rm -rf obj bin libs;
