CFLAGS += -Wall -Wextra
VERSION = r$(shell git rev-list --count HEAD)-$(shell git rev-parse --short=8 HEAD)

all: uradvd
uradvd: uradvd.o

version:
	@echo "Version: $(VERSION)"

clean:
	rm -f uradvd *.o

.PHONY: clean version
