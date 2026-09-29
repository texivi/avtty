# AVTTY

*A Very Tiny Text Yard*

AVTTY is a small Unix terminal text editor written in C.

The project is focused on keeping the source as small as theoretically possible while still functioning as a basic text editor.

The entire editor is **9 lines**.

It is aimed at Unix users, C programmers, and people who prefer minimal software. Its small footprint makes it suitable for minimal systems.

## Installation

```sh
git clone https://github.com/texivi/avtty
cd avtty
chmod +x install.sh
./install.sh
```

## Usage

Open an existing file:

```sh
avtty <filename>
```

**Ctrl-S** saves the file.

**Ctrl-Q** quits AVTTY.
