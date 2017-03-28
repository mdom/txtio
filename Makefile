bin/txtio: src/txtio.c
	mkdir -p bin
	gcc -std=c99 -g -Wall -Wpedantic -lcurl -lsqlite3 -D_POSIX_C_SOURCE=200809L -o bin/txtio src/*.c src/uthash/*.h
