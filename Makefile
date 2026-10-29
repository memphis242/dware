# dware - Diceware passphrase generator. See README.md.

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
CC      ?= cc
CFLAGS  ?= -O2
LDFLAGS ?=
JOBS    ?= $(shell nproc 2>/dev/null || echo 4)

BUILD    := build
SRC      := src/dware.c
WORDLIST := data/eff_large_wordlist.txt

WARNINGS := -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow \
            -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wcast-qual -Wwrite-strings \
            -Werror=implicit-function-declaration
HARDENING    := -fstack-protector-strong -fPIE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3
LD_HARDENING := -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack
SANITIZERS   := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

.PHONY: all debug test install uninstall clean

all: $(BUILD)/dware

$(BUILD):
	mkdir -p $@

$(BUILD)/wordlist.h: $(WORDLIST) $(WORDLIST).sha256 tools/gen-wordlist.sh | $(BUILD)
	sh tools/gen-wordlist.sh $(WORDLIST) $(WORDLIST).sha256 $@

$(BUILD)/dware: $(SRC) $(BUILD)/wordlist.h
	$(CC) $(WARNINGS) $(HARDENING) $(CFLAGS) -DNDEBUG -I$(BUILD) $(SRC) -o $@ $(LD_HARDENING) $(LDFLAGS) -s

# Assertions enabled, under ASan and UBSan.
$(BUILD)/dware-debug: $(SRC) $(BUILD)/wordlist.h
	$(CC) $(WARNINGS) -O1 -g $(SANITIZERS) -I$(BUILD) $(SRC) -o $@ $(LDFLAGS)

debug: $(BUILD)/dware-debug

test: $(BUILD)/dware $(BUILD)/dware-debug
	sh tests/test.sh $(BUILD)/dware $(WORDLIST) $(JOBS)
	sh tests/test.sh $(BUILD)/dware-debug $(WORDLIST) $(JOBS)

install: $(BUILD)/dware
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BUILD)/dware $(DESTDIR)$(BINDIR)/dware

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/dware

clean:
	rm -rf $(BUILD)
