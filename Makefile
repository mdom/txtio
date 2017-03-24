bin/txtio: src/txtio.c
	mkdir -p bin
	gcc -std=c99 -Wall -Wpedantic -Werror -lcurl -o bin/txtio src/txtio.c
