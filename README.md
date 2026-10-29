# dware

A tiny, dependency-free [Diceware](https://theworld.com/~reinhold/diceware.html)
passphrase generator in C23.

```console
$ dware
bruising idly settle stem ivy yippee
$ dware -n 8 -s . -e
relic.ebony.bluish.legwarmer.encode.animating.evasive.challenge
8 words x 12.92 bits = 103.4 bits of entropy
```

## Why

- **Small.** One ~90 KB binary that links only against libc. Peak RSS is about
  1.4 MB, which is the baseline cost of starting any C program on glibc.
- **No dependencies.** Building needs a C23 compiler, GNU make, `awk`, and
  `sha256sum` (or `shasum`). Nothing is downloaded at build or run time.
- **Auditable.** About 250 lines of C. The wordlist is pinned by sha256 and
  checked structurally before it is compiled in.

## Usage

```
dware [-n WORDS] [-s SEPARATOR] [-e]

  -n WORDS      number of words, 1 to 64 (default 6, about 77.5 bits)
  -s SEPARATOR  text placed between words, up to 16 bytes (default: a space)
  -e            also print the passphrase's entropy to stderr
  -h            show this help
  -V            show the version
```

Exit status is 0 on success, 1 if random bytes could not be read or output
could not be written, and 2 for invalid usage.

| Words | Entropy (bits) |
|------:|---------------:|
| 4     | 51.7           |
| 5     | 64.6           |
| 6     | 77.5           |
| 7     | 90.5           |
| 8     | 103.4          |

Each word adds log2(7776) ~= 12.92 bits. EFF recommends at least 6 words.

**Separators.** Words contain only `a-z`, plus a hyphen in four of them
(`drop-down`, `felt-tip`, `t-shirt`, `yo-yo`). A separator outside those
characters, such as the default space or `.`, keeps word boundaries
unambiguous. With `-s -`, `drop-down` and `drop` + `down` print the same;
the entropy lost is negligible, but the boundaries are no longer recoverable.

## Build and install

```sh
make                          # build/dware
make test                     # unit tests, plus behavioral tests of release and ASan/UBSan builds
make install PREFIX=~/.local  # or any prefix; default /usr/local
make uninstall PREFIX=~/.local
```

CI builds and tests with gcc and clang on Linux and with Apple clang on macOS.
On the BSDs, use `gmake`.

## How it works

- **Wordlist.** `data/eff_large_wordlist.txt` is the EFF large wordlist,
  pinned by `data/eff_large_wordlist.txt.sha256`. At build time
  `tools/gen-wordlist.sh` verifies the hash and that the file is the canonical
  7776-line list (dice codes 11111 to 66666 in order, words strictly
  ascending), then emits a fixed-width `char[7776][10]` table in read-only
  memory with no pointers to relocate.
- **Randomness.** One `getentropy(3)` call reads 4 bytes per word from the
  kernel CSPRNG. Each 32-bit draw becomes `draw % 7776` after rejecting the
  2560 values below 2^32 mod 7776, so every word is exactly equally likely.
  The threshold is checked with `static_assert`, and the redraw path is unit
  tested against a scripted entropy source.
- **Hygiene.** Core dumps are disabled at startup. The passphrase is assembled
  in a stack buffer, written with `write(2)` instead of through stdio buffers,
  and wiped along with the random draws before exit.

## License

Code: MIT, see [LICENSE](LICENSE).

The wordlist in `data/` is
[EFF's large wordlist](https://www.eff.org/dice) by the Electronic Frontier
Foundation, licensed under
[CC BY 3.0 US](https://creativecommons.org/licenses/by/3.0/us/).
