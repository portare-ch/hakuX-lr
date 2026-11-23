# Open system font

`hakux-open.xtf.gz` is the console's system font for a hard disk that has none:
the core writes it to `C:\Xbox.xtf` and `C:\XBox Book.xtf` (the same glyphs
under the two names the original dashboard's fonts have), with
`LICENSE_roboto_mono` beside them as `C:\RobotoMono-LICENSE.txt`. Programs that
draw with the console's fonts need them; a game disc's Xbox Live updater
(`update.xbe`) is one.

It is Roboto Mono Regular, triangulated into the dashboard's vector font
format (XTF). No Microsoft font is part of it.

| | |
| --- | --- |
| Source | `assets/RobotoMono-Regular.ttf` of [hakux-lr-dash](https://github.com/portare-ch/hakux-lr-dash), SHA-256 `e4bf45837847d4abfa5a502b807a83393aa138727bdb9ea9c93fb17d53208d95` |
| Licence | SIL Open Font License 1.1 (`LICENSE_roboto_mono`) |
| Glyphs | 875, in 70 ranges |
| `Xbox.xtf` (uncompressed, name `xbox`) | 1,084,952 bytes, SHA-256 `24e3488d3e831a1be6dc2768b1ff057f1165b4876ab26e47bde77571ccf600bf` |
| Compressed | `gzip -9n` |

`C:\XBox Book.xtf` differs from it in the 32-byte name at offset 8 only (`Xbox Book`).
