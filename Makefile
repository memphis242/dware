# dware - Diceware passphrase generator. See README.md. Requires GNU make.

PREFIX       ?= /usr/local
BINDIR       ?= $(PREFIX)/bin
CC           ?= cc
STRIP        ?= strip
CFLAGS       ?= -O2
DEBUG_CFLAGS ?= -O1 -g
LDFLAGS      ?=
JOBS         ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

BUILD    := build
SRC      := src/dware.c
WORDLIST := data/eff_large_wordlist.txt

WARNINGS := -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow \
            -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wcast-qual -Wwrite-strings \
            -Werror=implicit-function-declaration
# _FORTIFY_SOURCE needs an optimized build; glibc warns about it otherwise.
FORTIFY    := $(if $(filter -O1 -O2 -O3 -Os -Oz -Og -Ofast,$(CFLAGS)),-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3)
HARDENING  := -fstack-protector-strong -fPIE $(FORTIFY)
SANITIZERS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
# Apple's linker makes PIE executables by default and does not take ELF -z options.
ifeq ($(shell uname -s),Darwin)
LD_HARDENING :=
else
LD_HARDENING := -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack
endif

.PHONY: all debug test install uninstall clean

all: $(BUILD)/dware

$(BUILD):
	mkdir -p $@

$(BUILD)/wordlist.h: $(WORDLIST) $(WORDLIST).sha256 tools/gen-wordlist.sh | $(BUILD)
	sh tools/gen-wordlist.sh $(WORDLIST) $(WORDLIST).sha256 $@

$(BUILD)/dware: $(SRC) $(BUILD)/wordlist.h
	$(CC) $(WARNINGS) $(HARDENING) $(CFLAGS) -DNDEBUG -I$(BUILD) $(SRC) -o $@ $(LD_HARDENING) $(LDFLAGS)
	$(STRIP) $@

# Assertions enabled, under ASan and UBSan.
$(BUILD)/dware-debug: $(SRC) $(BUILD)/wordlist.h
	$(CC) $(WARNINGS) $(DEBUG_CFLAGS) $(SANITIZERS) -I$(BUILD) $(SRC) -o $@ $(LDFLAGS)

$(BUILD)/unit: tests/unit.c $(SRC) $(BUILD)/wordlist.h
	$(CC) $(WARNINGS) $(DEBUG_CFLAGS) $(SANITIZERS) -I$(BUILD) -Isrc tests/unit.c -o $@ $(LDFLAGS)

debug: $(BUILD)/dware-debug

test: $(BUILD)/dware $(BUILD)/dware-debug $(BUILD)/unit
	$(BUILD)/unit
	sh tests/test.sh $(BUILD)/dware $(WORDLIST) $(JOBS)
	sh tests/test.sh $(BUILD)/dware-debug $(WORDLIST) $(JOBS)

install: $(BUILD)/dware
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BUILD)/dware $(DESTDIR)$(BINDIR)/dware

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/dware

clean:
	rm -rf $(BUILD)
