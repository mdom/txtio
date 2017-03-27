bin/txtio: src/txtio.c
	mkdir -p bin
	gcc -std=c99 -g -Wall -Wpedantic -lcurl -lsqlite3 -o bin/txtio src/*.c src/asprintf/*.c
