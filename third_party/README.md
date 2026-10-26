# Third-party dependencies

Builds do not download these vendored files.

| Library | Source revision | Files and license |
| --- | --- | --- |
| [doctest 2.5.3](https://github.com/doctest/doctest/tree/2d0a9359a60c51affe2a9bebb1be1dca47868151) | `2d0a9359a60c51affe2a9bebb1be1dca47868151` (`v2.5.3` tag) | `doctest/doctest/doctest.h`, `doctest/LICENSE.txt` (MIT); test runner only, never linked into the linea library |
| [Lato 2.015](https://github.com/mikke89/RmlUi/tree/ba95ffe8bfb6370efb2cdcca927eaad4710c5413/Samples/assets), LatoLatin Regular | RmlUi's `ba95ffe8bfb6370efb2cdcca927eaad4710c5413` (`6.3` tag) | `../tests/fonts/LatoLatin-Regular.ttf`, `../tests/fonts/OFL.txt` (SIL Open Font License 1.1); test font only, never linked into the linea library |

The doctest files are unmodified copies of doctest's `doctest/doctest.h` and `LICENSE.txt` at that revision. The font
is an unmodified copy of RmlUi's `Samples/assets/LatoLatin-Regular.ttf`, and `OFL.txt` is the Lato section of that
directory's `LICENSE.txt`, its lines 4 to 109: Lato's copyright notice and the license. Their SHA-256 checksums, from
this directory:

```text
cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421  doctest/doctest/doctest.h
0fe0b331fa1513dcce8604ff1fa925f32d1cea17d8aeb1c2471fad40d291adc5  doctest/LICENSE.txt
d785334ac4e7810f571def986bbad41161f68ac385db8813f798bf04d71478e1  ../tests/fonts/LatoLatin-Regular.ttf
2765ffa5281ba8fd885b5df0d07a04f3e67e7328b64d23822b2730cb3b9465e8  ../tests/fonts/OFL.txt
```
