# Kinds of face

A book names its fonts, and the device has only its own. So of a
`font-family` value the reader of CSS keeps one thing: which kind of face
it asks for. `epub/css.h` has it as `CssFamilyClass`, one of five:

| Kind | What it is |
|---|---|
| `serif` | A face with serifs |
| `sans` | One without |
| `mono` | One whose letters are all of a width |
| `script` | A hand, or a face drawn after one |
| `other` | Something was named, and nothing known of it says which of those it is: a face of pictures, letters drawn for show, or a name that is not known |

With that a book's distinctions can be kept when its typefaces cannot: a
passage it sets in another kind of face from the text around it is still
set apart. Nothing sets text in a face by it yet.

**A value is the kind of the first of its names that says**, as a reader
takes the first face it has. `"Courier New", serif` is `mono`. A name
that says nothing is passed over for the next, and a value none of whose
names says is `other`.

**A name says in one of two ways.**

- It is one of CSS's own words for a kind of face: `serif`, `sans-serif`,
  `monospace`, `cursive`, `fantasy` (which is `other`), and the newer
  `ui-serif`, `ui-sans-serif`, `ui-monospace`, `system-ui` and `math`.
- A name that is known is found in it. Books write one face's name in
  many ways (`Palatino`, `"Palatino LT Std"`, `PalatinoLTStd-Medium`),
  so a known name counts wherever it is found among the name's letters,
  with the spaces, digits and marks between them left out and capitals
  made small: `garamond` is found in `AGaramondPro-Italic-0855`. The
  words for a kind of face count the same way (`sans` in `DejaVuSans`,
  `mono` in `FreeFontMono`).

Three things keep that from going wrong:

- **The longest name found is taken**, and of two as long the later. So
  `Ubuntu` is `sans` and `Ubuntu Mono` is `mono`, `Century Schoolbook` is
  `serif` and `Century Gothic` is `sans`, and `Sans Serif` is `sans`.
- **A short name counts only as a whole word.** `arial` is found in
  `Arial`, `Arial-R` and `Arial Narrow`, and not in `Aerial` or
  `Material Icons`.
- **A maker's name says nothing, and hides what is inside it.** `mono` is
  not taken from `Monotype Corsiva`, which is a `script` by its
  `corsiva`; and `Monotype Sorts` says nothing at all.

**What is known** is 229 names: the faces the books name most, the ones a
computer or a reading device is likely to have, and the words for a kind.
A name that is not among them is not classed, and that is all that
happens: the value goes by its other names, or is `other`.

**What is looked at**: of each name its first 128 bytes, the first 64
letters among those, and for whole words the first 16. Only Latin's
letters are letters here, so a name in another alphabet says nothing. The
work for a value is in proportion to its length, times the names that are
known; and a declaration in a sheet is 512 bytes at the most.

**Where it is kept.** The reader tells of a `font-family` with its names
and its kind. The table of a book's rules keeps the kind, in a byte, and
none of the names; so a cascade gives the kind for an element, from a rule
or from the element's own `style` ([A book's rules, and what they come
to](css-cascade.md)).

`bn-css family` says what a value comes to, name by name:

```
./build/host/tools/css/bn-css family '"Shift Light", "Courier New", serif'
```

```
N shift light: -
N courier new: mono name
N serif: serif keyword
class mono
```

Each name (`N`) is followed by its kind and by what said so, `keyword` or
`name`, or by `-` if nothing did; the last line is the value's kind.
`bn-css family --names` prints every name that is known after its kind,
with `word` after one that counts only as a whole word and `nothing` for a
maker's name. It begins:

```
serif academyengraved
sans agencyfb
sans akzidenz
serif albertus
serif alegreya
sans alegreyasans
sans andika
mono anonymous
serif antiqua
sans antonio
serif archer
sans arial word
sans arialmt
```

`bn-css book` counts the `font-family` values in a book's sheets: how many
there are, how many a keyword classed, a known name, or nothing, and how
many came to each kind. With `--dropped` it names each value that nothing
classed. For the IDPF sample `israelsailing.epub`, which is in `corpus/`,
with the three lines for what was passed over left out:

```
ok cc2dddb0     36    108      9 OEBPS/default.css
css_families=32 css_families_keyword=0 css_families_name=31 css_families_unclassed=1 css_families_serif=31 css_families_sans=0 css_families_mono=0 css_families_script=0 css_families_other=1
css_sheets=1 css_failed=0 css_bytes=5463 css_rules=36 css_declarations=108 css_dropped=9
css_table_bytes=1549 css_table_names=36 css_table_nodes=0 css_table_rules=36 css_table_selectors=36 css_table_left_out=0 css_table_sheets_left_out=0
unclassed 1 family: time new romans
```

`tools/check-books.py --css-report` adds those up over a folder of books,
with the values nothing classed. Over `corpus/`:

```
Kinds of face: 61 font-family values in the sheets of 20 books:
  classed by a keyword 22 (36.1%), by a known name 38 (62.3%), by nothing 1 (1.6%) in 1 books
  they come to: serif 37, sans 6, mono 17, script 0, other 1
  the values nothing classed, the 1 in most books of 1:
           1  in    1 books  time new romans
```

**How it is tested.** The unit tests say what a name comes to, written
every way a book writes one, and that every name that is known is found
by itself. An end-to-end test (`tests/e2e/css_tool.py`) works the same
thing out a second time in Python, another way round: it asks the tool
for the names that are known, which are a list of facts, and shares
nothing else. The two have to agree on 1,500 values made at random out of
those names, of names that are not known and of things that are no name
at all, and on the count of a book's values. `tools/check-books.py` holds
them to each other on the sheets of real books. The fuzz target of the
reader works each value out a name at a time and holds the reader to the
first that says.
