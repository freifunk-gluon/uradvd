CFLAGS += -Wall -Wextra
VERSION = $(shell git rev-list --count HEAD)~$(shell git rev-parse --short=8 HEAD)

all: uradvd
uradvd: uradvd.o

uradvd.o: uradvd.c
	$(CC) $(CFLAGS) -DVERSION='"$(VERSION)"' -c uradvd.c

version:
	@echo "Version: $(VERSION)"

clean:
	rm -f uradvd *.o

.PHONY: clean version
