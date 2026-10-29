/*
 * Unit tests for dware's internals. src/dware.c is compiled in directly, with
 * getentropy() swapped for a scripted source, so the rare rejection and
 * failure paths run deterministically and the passphrase buffer can be filled
 * to exactly its worst case under ASan.
 */
#define _DEFAULT_SOURCE 1
#define __STDC_WANT_LIB_EXT1__ 1

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

enum { SCRIPT_CAP = 256 };

/* The draws the next getentropy() calls return, one uint32_t at a time. */
static struct {
    uint32_t draws[SCRIPT_CAP];
    size_t len;
    size_t next;
    bool fail;
} script;

static void script_set(const uint32_t *const draws, const size_t len)
{
    assert(len <= SCRIPT_CAP);
    assert(len == 0 || draws != nullptr);
    if (len > 0) {
        memcpy(script.draws, draws, len * sizeof draws[0]);
    }
    script.len = len;
    script.next = 0;
    script.fail = false;
}

static int scripted_getentropy(void *const buf, const size_t len)
{
    if (script.fail) {
        errno = EIO;
        return -1;
    }
    assert(len % sizeof(uint32_t) == 0);
    for (size_t off = 0; off < len; off += sizeof(uint32_t)) {
        assert(script.next < script.len && "test consumed more draws than it scripted");
        memcpy((char *)buf + off, &script.draws[script.next++], sizeof(uint32_t));
    }
    return 0;
}

#define getentropy scripted_getentropy
#define main dware_main
int dware_main(int argc, char *argv[]);
#include "dware.c"
#undef main
#undef getentropy

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_pick_index(void)
{
    size_t index = SIZE_MAX;

    uint32_t draw = REJECT_BELOW;
    script_set(nullptr, 0);
    CHECK(pick_index(&draw, &index) && index == REJECT_BELOW % WORDLIST_COUNT && script.next == 0);

    draw = UINT32_MAX;
    CHECK(pick_index(&draw, &index) && index == UINT32_MAX % WORDLIST_COUNT);

    draw = 2 * WORDLIST_COUNT - 1;
    CHECK(pick_index(&draw, &index) && index == WORDLIST_COUNT - 1);

    /* Rejected draws are replaced in place until one is in range. */
    draw = 0;
    script_set((const uint32_t[]){REJECT_BELOW - 1, REJECT_BELOW + 5}, 2);
    CHECK(pick_index(&draw, &index) && index == REJECT_BELOW + 5 && script.next == 2 && draw == REJECT_BELOW + 5);

    /* A source stuck below the threshold is refused after MAX_REDRAWS redraws. */
    static const uint32_t zeros[MAX_REDRAWS] = {0};
    draw = 0;
    script_set(zeros, MAX_REDRAWS);
    CHECK(!pick_index(&draw, &index) && script.next == MAX_REDRAWS);

    draw = 0;
    script_set(nullptr, 0);
    script.fail = true;
    CHECK(!pick_index(&draw, &index));
}

static void test_build_passphrase(void)
{
    size_t longest = 0;
    for (size_t i = 0; i < WORDLIST_COUNT; ++i) {
        if (strnlen(wordlist[i], WORDLIST_WIDTH) == WORDLIST_WIDTH - 1) {
            longest = i;
            break;
        }
    }
    CHECK(strnlen(wordlist[longest], WORDLIST_WIDTH) == WORDLIST_WIDTH - 1);

    /* Longest word, longest separator, most words: exactly OUT_CAP bytes. */
    uint32_t longest_draws[MAX_WORDS] = {0};
    for (size_t i = 0; i < MAX_WORDS; ++i) {
        longest_draws[i] = (uint32_t)(longest + WORDLIST_COUNT);
    }
    script_set(longest_draws, MAX_WORDS);
    const struct options widest = {.words = MAX_WORDS, .sep = "0123456789abcdef", .sep_len = MAX_SEP_LEN};
    static_assert(sizeof "0123456789abcdef" - 1 == MAX_SEP_LEN);
    char out[OUT_CAP] = {0};
    uint32_t draws[MAX_WORDS] = {0};
    CHECK(build_passphrase(&widest, out, draws) == OUT_CAP && out[OUT_CAP - 1] == '\n');

    /* One word, and a rejected first draw that is redrawn. */
    script_set((const uint32_t[]){0, 7 + WORDLIST_COUNT}, 2);
    const struct options one = {.words = 1, .sep = " ", .sep_len = 1};
    const size_t len = build_passphrase(&one, out, draws);
    const size_t word_len = strnlen(wordlist[7], WORDLIST_WIDTH);
    CHECK(len == word_len + 1 && memcmp(out, wordlist[7], word_len) == 0 && out[word_len] == '\n');

    script_set(nullptr, 0);
    script.fail = true;
    CHECK(build_passphrase(&one, out, draws) == 0);
}

int main(void)
{
    fputs("unit: failure-path tests print the dware: errors below on purpose\n", stderr);
    test_pick_index();
    test_build_passphrase();
    if (failures != 0) {
        fprintf(stderr, "unit: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("unit: all %d checks passed\n", checks);
    return 0;
}
