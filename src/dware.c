/*
 * dware - print a Diceware passphrase drawn from the EFF large wordlist.
 *
 * Each word is chosen uniformly from 7776 (6^5) entries using the kernel CSPRNG,
 * giving log2(7776) ~= 12.92 bits of entropy per word. The wordlist is compiled
 * in as a fixed-width read-only table with no pointers to relocate. Core dumps
 * are disabled, the passphrase is assembled in one stack buffer, written with
 * write(2) (bypassing stdio buffers), and wiped along with the random draws
 * before exit.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef __STDC_WANT_LIB_EXT1__
#define __STDC_WANT_LIB_EXT1__ 1 /* memset_s on Apple platforms */
#endif

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <unistd.h>

#include "wordlist.h"

#define DWARE_VERSION "0.1.0"

enum {
    DEFAULT_WORDS = 6,
    MAX_WORDS = 64,
    MAX_SEP_LEN = 16,
    /* getentropy(3) serves at most this many bytes per call. */
    GETENTROPY_MAX = 256,
    /* Per word. A single redraw happens with probability 2560 / 2^32. */
    MAX_REDRAWS = 64,
    OUT_CAP = MAX_WORDS * (WORDLIST_WIDTH - 1) + (MAX_WORDS - 1) * MAX_SEP_LEN + 1,
};

enum { STATUS_OK = 0, STATUS_RUNTIME_ERROR = 1, STATUS_USAGE_ERROR = 2 };

/* Draws in [0, REJECT_BELOW) are discarded so that draw % WORDLIST_COUNT is uniform:
 * the remaining 2^32 - (2^32 mod WORDLIST_COUNT) values split evenly across words. */
static const uint32_t REJECT_BELOW = (uint32_t)(0u - (uint32_t)WORDLIST_COUNT) % WORDLIST_COUNT;

/* log2(7776) = 5 * log2(6) */
static const double BITS_PER_WORD = 12.92481250360578;

static_assert(WORDLIST_COUNT == 7776, "a Diceware list has exactly 6^5 entries");
static_assert(sizeof wordlist / sizeof wordlist[0] == WORDLIST_COUNT, "wordlist table size mismatch");
static_assert(WORDLIST_WIDTH >= 2, "wordlist entries must hold at least one character");
static_assert(MAX_WORDS * sizeof(uint32_t) <= GETENTROPY_MAX, "one getentropy() call must cover every draw");
static_assert((uint32_t)(0u - (uint32_t)WORDLIST_COUNT) % WORDLIST_COUNT == (UINT64_C(1) << 32) % WORDLIST_COUNT,
              "REJECT_BELOW must equal 2^32 mod WORDLIST_COUNT");
static_assert((UINT64_C(1) << 32) % WORDLIST_COUNT == 2560, "rejection threshold for 7776 words");

struct options {
    size_t words;
    const char *sep;
    size_t sep_len;
    bool show_entropy;
};

enum parse_result { PARSE_RUN, PARSE_EXIT_OK, PARSE_EXIT_USAGE };

/* Zeroes memory in a way the compiler may not elide as a dead store. */
static void secure_zero(void *const buf, const size_t len)
{
#if defined(__APPLE__)
    memset_s(buf, len, 0, len);
#elif defined(__NetBSD__)
    explicit_memset(buf, 0, len);
#else
    explicit_bzero(buf, len);
#endif
}

static void disable_core_dumps(void)
{
    const struct rlimit none = {.rlim_cur = 0, .rlim_max = 0};
    if (setrlimit(RLIMIT_CORE, &none) != 0) {
        fprintf(stderr, "dware: warning: cannot disable core dumps: %s\n", strerror(errno));
    }
}

static void print_usage(FILE *const stream)
{
    fprintf(stream,
            "usage: dware [-n WORDS] [-s SEPARATOR] [-e]\n"
            "\n"
            "Print a Diceware passphrase of words drawn uniformly from the EFF large wordlist.\n"
            "\n"
            "  -n WORDS      number of words, 1 to %d (default %d, about %.1f bits)\n"
            "  -s SEPARATOR  text placed between words, up to %d bytes (default: a space)\n"
            "  -e            also print the passphrase's entropy to stderr\n"
            "  -h            show this help\n"
            "  -V            show the version\n",
            MAX_WORDS, DEFAULT_WORDS, (double)DEFAULT_WORDS * BITS_PER_WORD, MAX_SEP_LEN);
}

/* Flushes stdout, reporting whether everything printed to it was written. */
static bool flush_stdout(void)
{
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "dware: cannot write to stdout: %s\n", strerror(errno));
        return false;
    }
    return true;
}

static bool parse_words(const char *const arg, size_t *const words)
{
    char *end = nullptr;
    errno = 0;
    const long value = strtol(arg, &end, 10);
    if (end == arg || *end != '\0' || errno == ERANGE || value < 1 || value > MAX_WORDS) {
        fprintf(stderr, "dware: -n expects a whole number from 1 to %d, got '%s'\n", MAX_WORDS, arg);
        return false;
    }
    *words = (size_t)value;
    return true;
}

static enum parse_result parse_options(const int argc, char *argv[], struct options *const opts)
{
    int opt = 0;
    while ((opt = getopt(argc, argv, "n:s:ehV")) != -1) {
        switch (opt) {
        case 'n':
            if (!parse_words(optarg, &opts->words)) {
                return PARSE_EXIT_USAGE;
            }
            break;
        case 's': {
            const size_t len = strlen(optarg);
            if (len > MAX_SEP_LEN) {
                fprintf(stderr, "dware: -s accepts at most %d bytes, got %zu\n", MAX_SEP_LEN, len);
                return PARSE_EXIT_USAGE;
            }
            opts->sep = optarg;
            opts->sep_len = len;
            break;
        }
        case 'e':
            opts->show_entropy = true;
            break;
        case 'h':
            print_usage(stdout);
            return PARSE_EXIT_OK;
        case 'V':
            puts("dware " DWARE_VERSION);
            return PARSE_EXIT_OK;
        default:
            /* getopt() has already described the bad option on stderr. */
            fputs("Try 'dware -h' for usage.\n", stderr);
            return PARSE_EXIT_USAGE;
        }
    }
    if (optind != argc) {
        fprintf(stderr, "dware: unexpected argument '%s'\nTry 'dware -h' for usage.\n", argv[optind]);
        return PARSE_EXIT_USAGE;
    }
    return PARSE_RUN;
}

static bool fill_random(void *const buf, const size_t len)
{
    assert(len <= GETENTROPY_MAX);
    if (getentropy(buf, len) != 0) {
        fprintf(stderr, "dware: cannot read random bytes from the kernel: %s\n", strerror(errno));
        return false;
    }
    return true;
}

/* Maps a uniform 32-bit draw to a uniform word index, redrawing the rare biased values
 * in place so that every random byte stays in the caller's (wiped) buffer. */
static bool pick_index(uint32_t *const draw, size_t *const index)
{
    for (int redraws = 0; *draw < REJECT_BELOW; ++redraws) {
        if (redraws == MAX_REDRAWS) {
            fprintf(stderr, "dware: the random source returned %d consecutive out-of-range values; refusing to continue\n",
                    MAX_REDRAWS);
            return false;
        }
        if (!fill_random(draw, sizeof *draw)) {
            return false;
        }
    }
    *index = *draw % WORDLIST_COUNT;
    assert(*index < WORDLIST_COUNT);
    return true;
}

static bool write_all(const int fd, const char *buf, size_t len)
{
    while (len > 0) {
        const ssize_t written = write(fd, buf, len);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            fprintf(stderr, "dware: cannot write the passphrase: %s\n",
                    written < 0 ? strerror(errno) : "write made no progress");
            return false;
        }
        assert((size_t)written <= len);
        buf += written;
        len -= (size_t)written;
    }
    return true;
}

/* Builds "word<sep>word...\n" into out. Returns the length, or 0 on failure (already reported). */
static size_t build_passphrase(const struct options *const opts, char out[static OUT_CAP], uint32_t draws[static MAX_WORDS])
{
    assert(opts->words >= 1 && opts->words <= MAX_WORDS);
    assert(opts->sep_len <= MAX_SEP_LEN);

    if (!fill_random(draws, opts->words * sizeof draws[0])) {
        return 0;
    }
    size_t len = 0;
    for (size_t i = 0; i < opts->words; ++i) {
        size_t index = 0;
        if (!pick_index(&draws[i], &index)) {
            return 0;
        }
        const char *const word = wordlist[index];
        const size_t word_len = strnlen(word, WORDLIST_WIDTH);
        assert(word_len >= 1 && word_len < WORDLIST_WIDTH);
        if (i > 0) {
            assert(len + opts->sep_len <= OUT_CAP);
            memcpy(out + len, opts->sep, opts->sep_len);
            len += opts->sep_len;
        }
        assert(len + word_len <= OUT_CAP);
        memcpy(out + len, word, word_len);
        len += word_len;
    }
    assert(len < OUT_CAP);
    out[len++] = '\n';
    return len;
}

int main(int argc, char *argv[])
{
    disable_core_dumps();

    struct options opts = {.words = DEFAULT_WORDS, .sep = " ", .sep_len = 1, .show_entropy = false};
    switch (parse_options(argc, argv, &opts)) {
    case PARSE_RUN:
        break;
    case PARSE_EXIT_OK:
        return flush_stdout() ? STATUS_OK : STATUS_RUNTIME_ERROR;
    case PARSE_EXIT_USAGE:
        return STATUS_USAGE_ERROR;
    }

    char out[OUT_CAP] = {0};
    uint32_t draws[MAX_WORDS] = {0};
    const size_t len = build_passphrase(&opts, out, draws);
    const bool ok = len > 0 && write_all(STDOUT_FILENO, out, len);
    secure_zero(out, sizeof out);
    secure_zero(draws, sizeof draws);
    if (!ok) {
        return STATUS_RUNTIME_ERROR;
    }

    /* stderr is unwritable if this fails, so the exit status is the only report left. */
    if (opts.show_entropy && fprintf(stderr, "%zu words x %.2f bits = %.1f bits of entropy\n", opts.words,
                                     BITS_PER_WORD, (double)opts.words * BITS_PER_WORD) < 0) {
        return STATUS_RUNTIME_ERROR;
    }
    return STATUS_OK;
}
