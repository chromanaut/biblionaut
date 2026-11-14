# Books for testing

Twenty-eight real EPUB files, kept here so that the tests have books that
nobody on this project made: 16 from Project Gutenberg, 7 from Standard
Ebooks, and 5 of the International Digital Publishing Forum's EPUB 3
samples. `tools/check-books.py corpus` runs the book tools over them and
compares what each tool finds with a reading of the same file by Python;
CTest runs it as `e2e.Corpus`.

**These files are not under this repository's MIT licence.** Each is passed
on here unchanged, under the terms it came with, which are below. Do not
edit one: a changed file is no longer what its terms cover, and
`SHA256SUMS` would no longer match it.

A book is here on one of two grounds:

- **It is in the public domain.** The books from Project Gutenberg and
  Standard Ebooks are, in the United States. They were chosen so that
  everyone who wrote, translated or illustrated them died more than
  seventy years ago, which puts them in the public domain in most other
  countries as well.
- **Its file states a licence that lets unchanged copies be passed on.**
  The IDPF samples do: they are recent works, under Creative Commons
  licences and the GNU Free Documentation License.

A file that carries a font which may not be passed on is not here, whatever
the terms of its text.

## From Project Gutenberg

> This eBook is for the use of anyone anywhere in the United States and
> most other parts of the world at no cost and with almost no restrictions
> whatsoever. You may copy it, give it away or re-use it under the terms of
> the Project Gutenberg License included with this eBook or online at
> [www.gutenberg.org](https://www.gutenberg.org). If you are not located in
> the United States, you will have to check the laws of the country where
> you are located before using this eBook.

That notice is in each of these files, with the full Project Gutenberg
License, which is also at <https://www.gutenberg.org/license>. The files
are in `gutenberg/`, as fetched on 2026-11-13 from Project Gutenberg's
mirror (`gutenberg.pglaf.org`, by rsync, which is how it asks programs to
fetch). `pgN-images-3.epub` is book N as EPUB 3, and `pgN-images.epub` as
EPUB 2; the page of book N is `https://www.gutenberg.org/ebooks/N`.
Project Gutenberg makes its files again from time to time, so the file of
the same name there today may not be the one here.

| File | Work | Language | Why it is here |
|---|---|---|---|
| `pg11-images-3.epub` | Lewis Carroll, *Alice's Adventures in Wonderland* | en | A small plain novel |
| `pg11-images.epub` | The same | en | The same book as EPUB 2: a table of contents in an NCX and no nav |
| `pg2701-images-3.epub` | Herman Melville, *Moby Dick; Or, The Whale* | en | A long novel in few, large documents |
| `pg4300-images-3.epub` | James Joyce, *Ulysses* | en | Paragraphs that run for pages |
| `pg28885-images-3.epub` | Lewis Carroll, *Alice's Adventures in Wonderland*, illustrated by Arthur Rackham | en | Pictures: 17 JPEG and 21 PNG |
| `pg22367-images-3.epub` | Franz Kafka, *Die Verwandlung* | de | German, in one document of 128 KB |
| `pg2229-images-3.epub` | Johann Wolfgang von Goethe, *Faust: Der Tragödie erster Teil* | de | A play in verse |
| `pg4650-images-3.epub` | Voltaire, *Candide, ou l'optimisme* | fr | French, in 36 short documents |
| `pg17489-images-3.epub` | Victor Hugo, *Les misérables*, Tome I | fr | A long novel in French |
| `pg1012-images-3.epub` | Dante Alighieri, *La Divina Commedia* | it | Verse |
| `pg227-images-3.epub` | Virgil, *Aeneidos* | la | Latin verse |
| `pg7000-images-3.epub` | *Kalevala* | fi | Finnish verse, with long words |
| `pg5316-images-3.epub` | Aleksandr Pushkin, *Красавице, которая нюхала табак* | ru | Cyrillic |
| `pg14741-images-3.epub` | Gavriil Derzhavin, *Духовные оды* | ru | Cyrillic verse |
| `pg26731-images-3.epub` | Sophocles, *Αντιγόνη* | el | A play in verse, in Greek |
| `pg24264-images-3.epub` | Cao Xueqin, *紅樓夢* | zh | Chinese, with paragraphs of up to 48,870 characters |

## From Standard Ebooks

Each of these files has a page headed "Uncopyright", which says that the
writing and artwork in it are believed to be in the public domain in the
United States, that they may still be in copyright in other countries, and
that the people who made the ebook dedicate their own work on it to the
public domain with the
[CC0 1.0 Universal Public Domain Dedication](https://creativecommons.org/publicdomain/zero/1.0/).

The files are in `standard-ebooks/`, as downloaded on 2026-11-13 from each
book's page, which is `https://standardebooks.org/ebooks/` and then the
path in the table. Standard Ebooks offers each book in more than one form:
these are the "compatible epub", and for one book the "advanced epub" as
well.

| File | Work | Its page | Why it is here |
|---|---|---|---|
| `jane-austen_pride-and-prejudice.epub` | Jane Austen, *Pride and Prejudice* | `jane-austen/pride-and-prejudice` | A plain novel in 65 documents |
| `jane-austen_pride-and-prejudice_advanced.epub` | The same | The same | The advanced form: an SVG cover, and a nav with no NCX |
| `william-shakespeare_hamlet.epub` | William Shakespeare, *Hamlet* | `william-shakespeare/hamlet` | A play in verse, with 20 tables |
| `anton-chekhov_three-sisters_julius-west.epub` | Anton Chekhov, *Three Sisters*, translated by Julius West | `anton-chekhov/three-sisters/julius-west` | A play in prose |
| `edwin-a-abbott_flatland.epub` | Edwin A. Abbott, *Flatland* | `edwin-a-abbott/flatland` | Diagrams within the text |
| `adam-smith_the-wealth-of-nations.epub` | Adam Smith, *The Wealth of Nations* | `adam-smith/the-wealth-of-nations` | 1,647 notes, 3,762 links within the book and 12 tables |
| `herman-melville_moby-dick.epub` | Herman Melville, *Moby Dick* | `herman-melville/moby-dick` | 145 documents; the text of Gutenberg's 2701, made into a book by other hands |

## From IDPF's EPUB 3 samples

Sample books that the International Digital Publishing Forum gathered to
show what EPUB 3 can do. The files are in `idpf-epub3-samples/`, as
downloaded on 2026-11-13 from release `20230704` of
<https://github.com/IDPF/epub3-samples>, whose own description says:
"Unless specified otherwise in the sample table, all samples are licensed
under CC-BY-SA 3.0". Each of these five also states its licence in the
file, and that statement is what is given here.

| File | Work | Its licence, as the file states it | Why it is here |
|---|---|---|---|
| `regime-anticancer-arabic.epub` | *Le Vrai Régime anti-cancer*, by Pr David Khayat, Nathalie Hutter-Lardeau and Marina Khalil Fayad; Hachette Antoine, 2012. An extract, in Arabic | [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/) | Arabic, read from right to left |
| `israelsailing.epub` | *מפליגים בישראל*, by אורי עידן (Uri Idan); הליקון ספרים (Helicon Books), 2012, <http://read.heliconbooks.com> | [CC BY-SA 2.5](https://creativecommons.org/licenses/by-sa/2.5/) | Hebrew, read from right to left, with its own fonts |
| `svg-in-spine.epub` | *SVG in Spine*, by the ePub Sample project | [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/) | A fixed layout, whose pages are SVG files and not XHTML |
| `moby-dick.epub` | Herman Melville, *Moby-Dick*, from the text of Project Gutenberg's book 2701, whose notice (above) is in the file | [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/) | Its own fonts, documents out of the reading order, and a nav with no NCX, in 144 documents |
| `linear-algebra.epub` | Robert A. Beezer, *A First Course in Linear Algebra*. © 2004 by Robert A. Beezer | [GNU Free Documentation License, Version 1.2](https://www.gnu.org/licenses/old-licenses/fdl-1.2.html), which is in the file in full | 10,281 formulas in MathML, and 505 tables |

The fonts in two of them are passed on with the files, under the fonts' own
terms: DejaVu Sans in `israelsailing.epub`, under the DejaVu Fonts licence,
and the STIX fonts in `moby-dick.epub`, under the STIX Font License, which
follows the SIL Open Font License.

## The files

`SHA256SUMS` has the SHA-256 of each file as it was fetched, in the form
`shasum -a 256 -c SHA256SUMS` reads. `tools/check-books.py` checks it too,
and that no EPUB is here without a line in it.
