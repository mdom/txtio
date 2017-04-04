CC = c99
CFLAGS = -Wall -Wpedantic
LDLIBS = -lcurl -lsqlite3

txtio: src/*.c src/uthash/*.h
	$(CC) $(CFLAGS) $(LDLIBS) -D_POSIX_C_SOURCE=200809L -o txtio $^
