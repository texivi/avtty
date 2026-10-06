#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define VERSION "0.5.0"
#define TABW 4
#define CTRL_(k) ((k) & 0x1f)
#define ESC 27
#define ALT(c) (2000 + (c))
#define CONT(b) ((((unsigned char)(b)) & 0xC0) == 0x80)
#define MAXREC 8
#define HINT "Ctrl: S save | Q quit | Z undo | Y redo | F find | R replace | G goto | K cut"

#define SGR(s) "\x1b[" s "m"
#define RESET SGR("0")
#define GRAY SGR("38;5;244")
#define TXT SGR("38;5;252")
#define TITLE SGR("1;38;5;117")
#define SEL SGR("1;48;5;238;38;5;117")
#define SELBG SGR("48;2;40;52;87")

enum Key {
    BACKSPACE = 127,
    ARROW_LEFT = 1000, ARROW_RIGHT, ARROW_UP, ARROW_DOWN,
    WORD_LEFT, WORD_RIGHT, DEL_KEY, HOME_KEY, END_KEY, PAGE_UP, PAGE_DOWN,
    BACKTAB, TOP_KEY, BOT_KEY, MOVE_UP, MOVE_DOWN
};

typedef struct { char *s; int len; } Row;
typedef enum { INS, DEL, SPLIT, JOIN } OpType;
typedef struct { OpType t; int y, x, group; char c; } Op;
struct abuf { char *b; int len, cap; };

static struct {
    int cx, cy, rx, want, rowoff, coloff, srows, scols, rows;
    Row *row; int nrows, caprows;
    char *filename, *clip, *lastq;
    int nums, lang, promptcol, sel_on, ay, ax, wrap, clip_line;
    char msg[160]; time_t msgtime;
    Op *ops; int nops, capops, cur, saved, group;
    int dash, page, sel, rsel, twocol;
    char *recent[MAXREC]; int nrecent;
    struct termios orig; int raw;
} E;

static int last_typing, prompt_empty_ok, prompt_paths, find_x, find_y;
static const char *prompt_note = "";

static const struct { const char *label; char key; } items[6] = {
    { "Open File", 'o' }, { "New File", 'n' }, { "Create File", 'c' },
    { "Recent Files", 'r' }, { "Keys & Help", 'h' }, { "Quit", 'q' }
};

struct kd { const char *k, *d; };

static const struct { const char *title; int n; struct kd i[5]; } keys[6] = {
    { "File", 4, { { "Ctrl-S", "save" }, { "Ctrl-O", "open file" }, { "Ctrl-Z / Y", "undo / redo" }, { "Ctrl-Q", "quit" } } },
    { "Search", 4, { { "Ctrl-F", "find" }, { "Alt-N / P", "next / prev" }, { "Alt-S", "this word" }, { "Ctrl-R", "replace all" } } },
    { "Move", 5, { { "Ctrl-G", "go to line" }, { "Ctrl-Home/End", "top / bottom" }, { "Home / End", "start / end" },
                   { "Ctrl-B", "match bracket" }, { "Ctrl-←/→", "word jump" } } },
    { "Select", 5, { { "Ctrl-V", "select" }, { "Alt-A", "select all" }, { "Ctrl-C", "copy" }, { "Ctrl-K", "cut" }, { "Ctrl-U", "paste" } } },
    { "Lines", 5, { { "Ctrl-D", "duplicate" }, { "Alt-↑/↓", "move line" }, { "Ctrl-/", "comment" },
                    { "Tab / S-Tab", "indent/dedent" }, { "Ctrl-W", "delete word" } } },
    { "View", 3, { { "Ctrl-N", "line numbers" }, { "Alt-W", "soft wrap" }, { "Ctrl-L", "center" } } }
};

static void out(const char *s) { (void)!write(STDOUT_FILENO, s, strlen(s)); }

static void disable_raw(void) {
    if (!E.raw) return;
    E.raw = 0;
    out(RESET "\x1b[?1049l");
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig);
}

static void die(const char *s) {
    int e = errno;
    disable_raw();
    errno = e;
    perror(s);
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { disable_raw(); fputs("avtty: out of memory\n", stderr); exit(1); }
    return q;
}

static void enable_raw(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig) == -1) die("tcgetattr");
    struct termios t = E.orig;
    t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= ~(OPOST);
    t.c_cflag |= CS8;
    t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) == -1) die("tcsetattr");
    E.raw = 1;
    atexit(disable_raw);
    out("\x1b[?1049h");
}

static void get_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) { E.srows = 24; E.scols = 80; }
    else { E.srows = ws.ws_row; E.scols = ws.ws_col; }
    E.rows = E.srows - 2 < 1 ? 1 : E.srows - 2;
}

static void center(void) {
    get_size();
    E.rowoff = E.cy > E.rows / 2 ? E.cy - E.rows / 2 : 0;
}

static int read_key(void) {
    static const struct { const char *s; int k; } esc[] = {
        { "[A", ARROW_UP }, { "[B", ARROW_DOWN }, { "[C", ARROW_RIGHT }, { "[D", ARROW_LEFT },
        { "[H", HOME_KEY }, { "[F", END_KEY }, { "OH", HOME_KEY }, { "OF", END_KEY },
        { "[1~", HOME_KEY }, { "[7~", HOME_KEY }, { "[4~", END_KEY }, { "[8~", END_KEY },
        { "[3~", DEL_KEY }, { "[5~", PAGE_UP }, { "[6~", PAGE_DOWN },
        { "[1;5C", WORD_RIGHT }, { "[1;5D", WORD_LEFT }, { "[1;3C", WORD_RIGHT }, { "[1;3D", WORD_LEFT },
        { "[Z", BACKTAB }, { "[1;5H", TOP_KEY }, { "[1;5F", BOT_KEY }, { "[1;3A", MOVE_UP }, { "[1;3B", MOVE_DOWN }
    };
    char c, q[8] = "";
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) {
        if (n < 0 && errno != EAGAIN && errno != EINTR) die("read");
        return 0;
    }
    if (c != ESC) return (unsigned char)c;
    if (read(STDIN_FILENO, q, 1) != 1) return ESC;
    if (q[0] != '[' && q[0] != 'O') return ALT((unsigned char)q[0]);
    for (int i = 1; i < 7 && read(STDIN_FILENO, q + i, 1) == 1; i++)
        if (isalpha((unsigned char)q[i]) || q[i] == '~') break;
    for (size_t i = 0; i < sizeof esc / sizeof *esc; i++)
        if (!strcmp(q, esc[i].s)) return esc[i].k;
    return ESC;
}

static void insert_row(int at, const char *s, int len) {
    if (E.nrows == E.caprows) {
        E.caprows = E.caprows ? E.caprows * 2 : 64;
        E.row = xrealloc(E.row, sizeof(Row) * E.caprows);
    }
    memmove(&E.row[at + 1], &E.row[at], sizeof(Row) * (E.nrows - at));
    E.row[at].s = xrealloc(NULL, len + 1);
    memcpy(E.row[at].s, s, len);
    E.row[at].s[len] = 0;
    E.row[at].len = len;
    E.nrows++;
}

static void free_rows(void) {
    for (int i = 0; i < E.nrows; i++) free(E.row[i].s);
    E.nrows = 0;
}

static void apply(OpType t, int y, int x, char c) {
    Row *r = &E.row[y];
    switch (t) {
    case INS:
        r->s = xrealloc(r->s, r->len + 2);
        memmove(r->s + x + 1, r->s + x, r->len - x + 1);
        r->s[x] = c;
        r->len++;
        E.cy = y; E.cx = x + 1;
        break;
    case DEL:
        memmove(r->s + x, r->s + x + 1, r->len - x);
        r->len--;
        E.cy = y; E.cx = x;
        break;
    case SPLIT:
        insert_row(y + 1, r->s + x, r->len - x);
        r = &E.row[y];
        r->len = x;
        r->s[x] = 0;
        E.cy = y + 1; E.cx = 0;
        break;
    case JOIN: {
        Row *b = &E.row[y + 1];
        r->s = xrealloc(r->s, r->len + b->len + 1);
        memcpy(r->s + r->len, b->s, b->len + 1);
        r->len += b->len;
        free(b->s);
        memmove(b, b + 1, sizeof(Row) * (E.nrows - y - 2));
        E.nrows--;
        E.cy = y; E.cx = x;
        break;
    }
    }
}

static void do_op(OpType t, int y, int x, char c) {
    apply(t, y, x, c);
    if (E.cur < E.nops) E.nops = E.cur;
    if (E.saved > E.cur) E.saved = -1;
    if (E.nops == E.capops) {
        E.capops = E.capops ? E.capops * 2 : 256;
        E.ops = xrealloc(E.ops, sizeof(Op) * E.capops);
    }
    E.ops[E.nops++] = (Op){ t, y, x, E.group, c };
    E.cur = E.nops;
}

static int is_dirty(void) { return E.cur != E.saved; }

static void set_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.msg, sizeof E.msg, fmt, ap);
    va_end(ap);
    E.msgtime = time(NULL);
}

static void undo(void) {
    if (!E.cur) { set_msg("Nothing to undo"); return; }
    int g = E.ops[E.cur - 1].group;
    while (E.cur > 0 && E.ops[E.cur - 1].group == g) {
        Op *o = &E.ops[--E.cur];
        apply(o->t ^ 1, o->y, o->x, o->c);
    }
}

static void redo(void) {
    if (E.cur == E.nops) { set_msg("Nothing to redo"); return; }
    int g = E.ops[E.cur].group;
    while (E.cur < E.nops && E.ops[E.cur].group == g) {
        Op *o = &E.ops[E.cur++];
        apply(o->t, o->y, o->x, o->c);
    }
}

static void ab_append(struct abuf *ab, const char *s, int len) {
    if (ab->len + len > ab->cap) {
        ab->cap = (ab->len + len) * 2;
        ab->b = xrealloc(ab->b, ab->cap);
    }
    memcpy(ab->b + ab->len, s, len);
    ab->len += len;
}

static void ab_s(struct abuf *ab, const char *s) { ab_append(ab, s, (int)strlen(s)); }
static void ab_rep(struct abuf *ab, const char *s, int n) { while (n-- > 0) ab_s(ab, s); }

static void at(struct abuf *ab, int r, int c) {
    char b[24];
    ab_append(ab, b, snprintf(b, sizeof b, "\x1b[%d;%dH", r, c));
}

static int put(struct abuf *ab, int r, int c, const char *style, const char *s) {
    if (r < 1 || r >= E.srows) return 0;
    at(ab, r, c);
    ab_s(ab, style);
    ab_s(ab, s);
    return 1;
}

static int utf8w(const char *s) {
    int w = 0;
    for (; *s; s++) if (!CONT(*s)) w++;
    return w;
}

static int cx_to_rx(const Row *r, int cx) {
    int col = 0;
    for (int i = 0; i < cx && i < r->len; i++) {
        if (r->s[i] == '\t') col += TABW - col % TABW;
        else if (!CONT(r->s[i])) col++;
    }
    return col;
}

static int rx_to_cx(const Row *r, int rx) {
    int col = 0, i = 0;
    while (i < r->len && col < rx) {
        if (r->s[i] == '\t') { col += TABW - col % TABW; i++; }
        else { col++; i++; while (i < r->len && CONT(r->s[i])) i++; }
    }
    return i;
}


static const char *const kw[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return",
    "goto", "sizeof", "typedef", "struct", "union", "enum", "static", "const", "extern", "volatile",
    "inline", "class", "public", "private", "new", "delete", "this", "try", "catch", "throw",
    "import", "from", "as", "def", "lambda", "pass", "with", "yield",
    "function", "var", "let", "fn", "pub", "use", "impl", "match",
    "self", "async", "await", "package", "func", "defer", "then", "fi", "elif", "done", "in", "is",
    "not", "and", "or", "export", "local", NULL
};

static const char *const cn[] = { "None", "True", "False", "null", "true", "false", "NULL", "nil", "undefined", NULL };

static const char *const ty[] = {
    "int", "char", "void", "long", "short", "float", "double", "bool", "unsigned", "signed",
    "size_t", "ssize_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t",
    "int32_t", "int64_t", "FILE", "string", "str", "i32", "u32", "i64", "u64", "u8", "usize", NULL
};

static const char *const hlc[] = {
    RESET, SGR("3;38;2;108;112;134"), SGR("38;2;187;154;247"), SGR("38;2;158;206;106"),
    SGR("38;2;255;158;100"), SGR("38;2;224;108;128"), SGR("38;2;224;175;104"),
    SGR("38;2;122;162;247"), SGR("38;2;115;218;202")
};

static int has(const char *const *l, const char *w, size_t n) {
    for (; *l; l++) if (strlen(*l) == n && !strncmp(*l, w, n)) return 1;
    return 0;
}

static void detect_lang(void) {
    static const char *const clike[] = { "c", "h", "cpp", "cc", "hpp", "js", "ts", "java", "go", "rs", "cs", "swift", "kt", "css", "php", NULL };
    static const char *const hashy[] = { "py", "sh", "bash", "zsh", "rb", "pl", "yml", "yaml", "toml", "conf", "mk", "cfg", "ini", "r", "Makefile", "makefile", "Dockerfile", NULL };
    const char *f = E.filename, *b = f ? strrchr(f, '/') : NULL, *e = f ? strrchr(f, '.') : NULL;
    E.lang = 0;
    if (!f) return;
    b = b ? b + 1 : f;
    e = e && e > b ? e + 1 : b;
    if (has(clike, e, strlen(e))) E.lang = 1;
    else if (has(hashy, e, strlen(e)) || has(hashy, b, strlen(b))) E.lang = 2;
}

static char *hl(const Row *r, int *blk) {
    static char *c;
    static int cap;
    const char *s = r->s;
    int n = r->len, i = 0, hash = E.lang == 2;
    if (n + 1 > cap) { cap = n + 64; c = xrealloc(c, cap); }
    memset(c, 0, n + 1);
    while (E.lang && i < n) {
        unsigned char ch = s[i];
        if (*blk) {
            c[i] = 1;
            if (ch == '*' && i + 1 < n && s[i + 1] == '/') { c[++i] = 1; *blk = 0; }
            i++;
        } else if (hash ? ch == '#' : (ch == '/' && i + 1 < n && s[i + 1] == '/')) {
            memset(c + i, 1, n - i);
            break;
        } else if (!hash && ch == '/' && i + 1 < n && s[i + 1] == '*') {
            *blk = 1; c[i] = c[i + 1] = 1; i += 2;
        } else if (!hash && ch == '#') {
            int j = 0;
            while (j < i && (s[j] == ' ' || s[j] == '\t')) j++;
            memset(c + i, j == i ? 5 : 0, n - i);
            break;
        } else if (ch == '"' || ch == '\'' || ch == '`') {
            int j = i + 1;
            while (j < n && s[j] != ch) j += s[j] == '\\' ? 2 : 1;
            j = j < n ? j + 1 : n;
            memset(c + i, 3, j - i);
            i = j;
        } else if (isdigit(ch) && (!i || !(isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_'))) {
            int j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_')) j++;
            memset(c + i, 4, j - i);
            i = j;
        } else if (isalpha(ch) || ch == '_') {
            int j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            int prop = i > 0 && (s[i - 1] == '.' || (i > 1 && s[i - 1] == '>' && s[i - 2] == '-'));
            memset(c + i, has(kw, s + i, j - i) ? 2 : has(cn, s + i, j - i) ? 4 : has(ty, s + i, j - i) ? 6
                   : j < n && s[j] == '(' ? 7 : prop ? 8 : 0, j - i);
            i = j;
        } else i++;
    }
    return c;
}

static int sel_range(int *sy, int *sx, int *ey, int *ex) {
    if (!E.sel_on || (E.ay == E.cy && E.ax == E.cx)) return 0;
    int fwd = E.ay < E.cy || (E.ay == E.cy && E.ax < E.cx);
    *sy = fwd ? E.ay : E.cy; *sx = fwd ? E.ax : E.cx;
    *ey = fwd ? E.cy : E.ay; *ex = fwd ? E.cx : E.ax;
    return 1;
}

static int rowh(int y, int tw) { return cx_to_rx(&E.row[y], E.row[y].len) / tw + 1; }

static void draw_row(struct abuf *ab, int y, int lo, int width, const char *cls) {
    const Row *r = &E.row[y];
    int col = 0, hi = lo + width, cur = 0, curs = 0, sy, sx, ey, ex;
    int has = sel_range(&sy, &sx, &ey, &ex) && y >= sy && y <= ey;
    int s0 = has && y == sy ? sx : 0, e0 = has ? (y == ey ? ex : r->len) : 0;
    for (int i = 0; i < r->len && col < hi;) {
        unsigned char ch = r->s[i];
        int in = i >= s0 && i < e0;
        if (col >= lo && (cls[i] != cur || in != curs)) {
            ab_s(ab, RESET);
            if (cls[i]) ab_s(ab, hlc[(int)cls[i]]);
            if (in) ab_s(ab, SELBG);
            cur = cls[i];
            curs = in;
        }
        if (ch == '\t') {
            for (int k = TABW - col % TABW; k > 0; k--, col++)
                if (col >= lo && col < hi) ab_s(ab, " ");
            i++;
        } else {
            int l = 1, bad = ch < 32 || ch == 127;
            if (!bad) while (i + l < r->len && CONT(r->s[i + l])) l++;
            if (col >= lo && col < hi) ab_append(ab, bad ? "?" : r->s + i, bad ? 1 : l);
            col++; i += l;
        }
    }
    if (has && y < ey && col >= lo && col < hi) { ab_s(ab, RESET SELBG " "); curs = 1; }
    if (cur || curs) ab_s(ab, RESET);
}

static void ctr(struct abuf *ab, int r, int w, const char *style, const char *s) {
    put(ab, r, 1 + (w - utf8w(s)) / 2, style, s);
    ab_s(ab, RESET);
}

static void item(struct abuf *ab, int r, int c, int i) {
    char t[48];
    int s = i == E.sel;
    snprintf(t, sizeof t, "%s%-19s", s ? " ▸ " : "   ", items[i].label);
    put(ab, r, c, s ? SEL : TXT, t);
    snprintf(t, sizeof t, "%c ", items[i].key);
    put(ab, r, c + 22, s ? SEL : GRAY, t);
    ab_s(ab, RESET);
}

static void logo_row(struct abuf *ab, int r, int sc) {
    static const char *const g[5][5] = {
        { " ### ", "#   #", "#####", "#   #", "#   #" },
        { "#   #", "#   #", "#   #", " # # ", "  #  " },
        { "#####", "  #  ", "  #  ", "  #  ", "  #  " },
        { "#####", "  #  ", "  #  ", "  #  ", "  #  " },
        { "#   #", " # # ", "  #  ", "  #  ", "  #  " }
    };
    static const char *const col[5] = {
        SGR("38;5;117"), SGR("38;5;111"), SGR("38;5;105"), SGR("38;5;99"), SGR("38;5;93")
    };
    ab_s(ab, col[r]);
    for (int l = 0; l < 5; l++) {
        for (int p = 0; p < 5; p++) ab_rep(ab, g[l][r][p] == '#' ? "█" : " ", sc);
        if (l < 4) ab_rep(ab, " ", sc);
    }
    ab_s(ab, RESET);
}

static void box(struct abuf *ab, int r, int c, int g) {
    int n = keys[g].n;
    if (put(ab, r, c, GRAY, "╭─ ")) {
        ab_s(ab, TITLE);
        ab_s(ab, keys[g].title);
        ab_s(ab, GRAY " ");
        ab_rep(ab, "─", 27 - utf8w(keys[g].title));
        ab_s(ab, "╮");
    }
    for (int i = 0; i < n; i++)
        if (put(ab, r + 1 + i, c, GRAY, "│ ")) {
            ab_s(ab, SGR("38;5;117"));
            ab_s(ab, keys[g].i[i].k);
            ab_rep(ab, " ", 14 - utf8w(keys[g].i[i].k));
            ab_s(ab, TXT);
            ab_s(ab, keys[g].i[i].d);
            ab_rep(ab, " ", 14 - utf8w(keys[g].i[i].d));
            ab_s(ab, GRAY " │");
        }
    if (put(ab, r + 1 + n, c, GRAY, "╰")) {
        ab_rep(ab, "─", 30);
        ab_s(ab, "╯");
    }
    ab_s(ab, RESET);
}

static void dash_draw(struct abuf *ab) {
    int cols = E.scols, avail = E.srows - 1;
    for (int y = 1; y <= avail; y++) { at(ab, y, 1); ab_s(ab, "\x1b[K"); }
    if (cols < 40 || avail < 8) { put(ab, 1, 1, RESET, "avtty: terminal too small"); return; }

    int sc = cols >= 64 ? 2 : 1, lw = 29 * sc, per = cols >= 62 ? 2 : 1, mr = 6 / per, hper = cols >= 68 ? 2 : 1;
    int logo = E.page == 0 && avail >= mr + 13, h[2] = { 0, 0 }, n, r;
    for (int g = 0; g < 6; g++) h[hper == 2 && g > 2] += keys[g].n + 2;
    E.twocol = per == 2;
    n = E.page == 0 ? (logo ? 6 : 0) + mr + 5 : E.page == 1 ? (E.nrecent ? E.nrecent : 1) + 4 : (h[0] > h[1] ? h[0] : h[1]) + 2;
    int top = (avail - n) / 2 + 1;
    if (top < 1) top = 1;
    r = top;

    if (E.page == 0) {
        if (logo) {
            for (int k = 0; k < 5; k++) { at(ab, r + k, 1 + (cols - lw) / 2); logo_row(ab, k, sc); }
            r += 6;
        }
        ctr(ab, r, cols, TXT, "A very tiny text yard");
        r += 2;
        int mx = 1 + (cols - (per == 2 ? 52 : 24)) / 2;
        for (int k = 0; k < mr; k++) {
            item(ab, r + k, mx, k);
            if (per == 2) item(ab, r + k, mx + 28, k + 3);
        }
        ctr(ab, r + mr + 1, cols, GRAY, "Part of the Texivi Software Suite (TSS)");
        ctr(ab, r + mr + 2, cols, GRAY, "avtty " VERSION " • ↑↓←→ move • Enter select");
        return;
    }

    if (E.page == 2) {
        int x = 1 + (cols - (hper == 2 ? 66 : 32)) / 2, y[2] = { top, top };
        for (int g = 0; g < 6; g++) {
            int col = hper == 2 && g > 2;
            box(ab, y[col], x + col * 34, g);
            y[col] += keys[g].n + 2;
        }
        ctr(ab, top + n - 1, cols, GRAY, "Press any key to go back");
        return;
    }

    ctr(ab, top, cols, TITLE, "Recent Files");
    r = top + 2;
    if (!E.nrecent) ctr(ab, r, cols, GRAY, "Nothing here yet");
    int cw = cols < 60 ? cols - 4 : 56;
    for (int i = 0; i < E.nrecent; i++) {
        char t[PATH_MAX + 8], l[PATH_MAX + 32];
        const char *p = E.recent[i], *hm = getenv("HOME");
        size_t hl = hm ? strlen(hm) : 0;
        int sel = i == E.rsel;
        if (hl && !strncmp(p, hm, hl) && (!p[hl] || p[hl] == '/')) snprintf(t, sizeof t, "~%s", p + hl);
        else snprintf(t, sizeof t, "%s", p);
        snprintf(l, sizeof l, "%s%.*s", sel ? " ▸ " : "   ", cw - 3, t);
        if (put(ab, r + i, 1 + (cols - cw) / 2, sel ? SEL : TXT, l)) ab_rep(ab, " ", cw - utf8w(l));
        ab_s(ab, RESET);
    }
    ctr(ab, r + (E.nrecent ? E.nrecent : 1) + 1, cols, GRAY, "Enter or 1-8 open • Esc back");
}

static void refresh_screen(void) {
    get_size();
    struct abuf ab = { NULL, 0, 0 };
    ab_s(&ab, "\x1b[?25l" RESET);
    int gw = 0, crow = 0, ccol = 0;

    if (E.dash) dash_draw(&ab);
    else {
        if (E.nums) {
            gw = snprintf(NULL, 0, "%d", E.nrows);
            gw = (gw < 3 ? 3 : gw) + 1;
            if (E.scols <= gw + 8) gw = 0;
        }
        int textw = E.scols - gw > 0 ? E.scols - gw : 1, blk = 0;
        E.rx = cx_to_rx(&E.row[E.cy], E.cx);
        if (E.cy < E.rowoff) E.rowoff = E.cy;
        if (E.wrap) {
            E.coloff = 0;
            if (E.cy - E.rowoff >= E.rows) E.rowoff = E.cy - E.rows + 1;
            for (;;) {
                crow = E.rx / textw;
                for (int y = E.rowoff; y < E.cy; y++) crow += rowh(y, textw);
                if (crow < E.rows || E.rowoff >= E.cy) break;
                E.rowoff++;
            }
            ccol = E.rx % textw;
        } else {
            if (E.cy >= E.rowoff + E.rows) E.rowoff = E.cy - E.rows + 1;
            if (E.rx < E.coloff) E.coloff = E.rx;
            if (E.rx >= E.coloff + textw) E.coloff = E.rx - textw + 1;
            crow = E.cy - E.rowoff;
            ccol = E.rx - E.coloff;
        }
        if (E.lang) for (int i = 0; i < E.rowoff && i < E.nrows; i++) hl(&E.row[i], &blk);

        for (int y = 0, fr = E.rowoff; y < E.rows; fr++) {
            if (fr >= E.nrows) { at(&ab, ++y, 1); ab_s(&ab, GRAY "~" RESET "\x1b[K"); continue; }
            char *cls = hl(&E.row[fr], &blk);
            int h = E.wrap ? rowh(fr, textw) : 1;
            for (int k = 0; k < h && y < E.rows; k++) {
                at(&ab, ++y, 1);
                if (gw) {
                    char num[96];
                    if (k) snprintf(num, sizeof num, "%*s", gw, "");
                    else snprintf(num, sizeof num, GRAY "%*d " RESET, gw - 1, fr + 1);
                    ab_s(&ab, num);
                }
                draw_row(&ab, fr, E.wrap ? k * textw : E.coloff, textw, cls);
                ab_s(&ab, "\x1b[K");
            }
        }

        char left[256], right[64];
        int ln = snprintf(left, sizeof left, " avtty  %.100s%s%s", E.filename ? E.filename : "[No Name]",
                          is_dirty() ? " [modified]" : "", E.sel_on ? " [select]" : "");
        int rn = snprintf(right, sizeof right, "Ln %d/%d, Col %d ", E.cy + 1, E.nrows, E.rx + 1);
        if (ln > E.scols) ln = E.scols;
        at(&ab, E.rows + 1, 1);
        ab_s(&ab, "\x1b[7m");
        ab_append(&ab, left, ln);
        for (; ln < E.scols; ln++) {
            if (E.scols - ln == rn) { ab_append(&ab, right, rn); break; }
            ab_s(&ab, " ");
        }
        ab_s(&ab, RESET);
    }

    at(&ab, E.srows, 1);
    ab_s(&ab, RESET "\x1b[K");
    if (E.msg[0] && (time(NULL) - E.msgtime < 5 || E.promptcol >= 0)) {
        int ml = (int)strlen(E.msg);
        ab_append(&ab, E.msg, ml > E.scols ? E.scols : ml);
    }
    if (E.promptcol >= 0) {
        at(&ab, E.srows, E.promptcol + 1 > E.scols ? E.scols : E.promptcol + 1);
        ab_s(&ab, "\x1b[?25h");
    } else if (!E.dash) {
        at(&ab, crow + 1, ccol + gw + 1);
        ab_s(&ab, "\x1b[?25h");
    }
    (void)!write(STDOUT_FILENO, ab.b, ab.len);
    free(ab.b);
}

static int recent_path(char *o, size_t n) {
    const char *h = getenv("HOME");
    if (!h || !*h) return -1;
    snprintf(o, n, "%s/.avtty_recent", h);
    return 0;
}

static int recent_read(char **o, int *ln, int max) {
    char p[PATH_MAX], *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int c = 0;
    if (recent_path(p, sizeof p)) return 0;
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    while (c < max && (n = getline(&line, &cap, f)) != -1) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char *colon = strrchr(line, ':');
        int l = 0;
        if (colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1)) { l = atoi(colon + 1); *colon = 0; }
        if (*line && access(line, F_OK) == 0) {
            if (ln) ln[c] = l;
            o[c++] = strdup(line);
        }
    }
    free(line);
    fclose(f);
    return c;
}

static void recent_add(const char *path, int line) {
    char *real = realpath(path, NULL), *old[MAXREC], p[PATH_MAX];
    int oln[MAXREC];
    if (!real) return;
    int n = recent_read(old, oln, MAXREC);
    FILE *f = recent_path(p, sizeof p) ? NULL : fopen(p, "w");
    if (f) {
        fprintf(f, "%s:%d\n", real, line);
        for (int i = 0, w = 1; i < n && w < MAXREC; i++)
            if (strcmp(old[i], real)) { fprintf(f, "%s:%d\n", old[i], oln[i] ? oln[i] : 1); w++; }
        fclose(f);
    }
    for (int i = 0; i < n; i++) free(old[i]);
    free(real);
}

static int recent_line(const char *path) {
    char *real = realpath(path, NULL), *o[MAXREC];
    int l[MAXREC], n = recent_read(o, l, MAXREC), r = 0;
    for (int i = 0; i < n; i++) {
        if (real && !strcmp(o[i], real)) r = l[i];
        free(o[i]);
    }
    free(real);
    return r;
}

static void remember(void) {
    if (!E.dash && E.filename && E.nrows) recent_add(E.filename, E.cy + 1);
}

static void load_stream(FILE *f) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) != -1) {
        if (n > 0 && line[n - 1] == '\n') n--;
        insert_row(E.nrows, line, (int)n);
    }
    free(line);
}

static int open_file_cmd(const char *file, int mode, char *err, size_t en) {
    struct stat st;
    int exists = stat(file, &st) == 0;
    FILE *f = NULL;
    if (exists && S_ISDIR(st.st_mode)) { snprintf(err, en, "'%s' is a directory", file); return -1; }
    if (mode == 2 && exists) { snprintf(err, en, "'%s' already exists (use -o to open it)", file); return -1; }
    if (mode == 1 && !exists) { snprintf(err, en, "'%s' does not exist (use -c to create it)", file); return -1; }
    if (mode == 2 || exists) {
        f = fopen(file, mode == 2 ? "wx" : "rb");
        if (!f) { snprintf(err, en, "can't %s '%s': %s", mode == 2 ? "create" : "read", file, strerror(errno)); return -1; }
    }
    remember();
    free_rows();
    if (exists) load_stream(f);
    if (f) fclose(f);
    if (!E.nrows) insert_row(0, "", 0);
    free(E.filename);
    E.filename = strdup(file);
    detect_lang();
    E.cx = E.cy = E.rowoff = E.coloff = E.nops = E.cur = E.saved = E.dash = E.sel_on = 0;
    if (f) {
        int ln = recent_line(file);
        if (ln > 0) E.cy = ln > E.nrows ? E.nrows - 1 : ln - 1;
        recent_add(file, E.cy + 1);
        center();
        set_msg(HINT);
    } else set_msg("New file: %s  (Ctrl-S to save)", file);
    return 0;
}

static char *prompt(const char *label, void (*cb)(const char *, int));

static char *ask_path(const char *label) {
    prompt_paths = 1;
    char *p = prompt(label, NULL);
    prompt_paths = 0;
    return p;
}

static int save_file(void) {
    if (!E.filename) {
        char *n = ask_path("Save as (Esc to cancel): ");
        if (!n) { set_msg("Save cancelled"); return 0; }
        E.filename = n;
        detect_lang();
    }
    FILE *f = fopen(E.filename, "wb");
    long total = 0;
    int bad;
    if (!f) { set_msg("Can't save: %s", strerror(errno)); return -1; }
    if (!(E.nrows == 1 && !E.row[0].len))
        for (int i = 0; i < E.nrows; i++) {
            fwrite(E.row[i].s, 1, E.row[i].len, f);
            fputc('\n', f);
            total += E.row[i].len + 1;
        }
    bad = ferror(f);
    bad |= fclose(f) != 0;
    if (bad) { set_msg("Can't save: %s", strerror(errno)); return -1; }
    E.saved = E.cur;
    recent_add(E.filename, E.cy + 1);
    set_msg("%ld bytes written to %s", total, E.filename);
    return 0;
}

static void complete(char **buf, size_t *cap, size_t *len) {
    glob_t g;
    char pat[PATH_MAX];
    snprintf(pat, sizeof pat, "%s*", *buf);
    prompt_note = "";
    if (glob(pat, GLOB_MARK | GLOB_TILDE, NULL, &g) == 0) {
        size_t n = strlen(g.gl_pathv[0]);
        for (size_t i = 1; i < g.gl_pathc; i++) {
            size_t k = 0;
            while (k < n && g.gl_pathv[i][k] == g.gl_pathv[0][k]) k++;
            n = k;
        }
        if (n >= *len) {
            if (n + 2 > *cap) *buf = xrealloc(*buf, *cap = n + 64);
            memcpy(*buf, g.gl_pathv[0], n);
            (*buf)[*len = n] = 0;
        }
        if (g.gl_pathc > 1) {
            static char note[48];
            snprintf(note, sizeof note, "  (%zu matches)", g.gl_pathc);
            prompt_note = note;
        }
        globfree(&g);
    }
}

static char *prompt(const char *label, void (*cb)(const char *, int)) {
    size_t cap = 64, len = 0;
    char *buf = xrealloc(NULL, cap);
    buf[0] = 0;
    prompt_note = "";
    for (;;) {
        set_msg("%s%s%s", label, buf, prompt_note);
        E.promptcol = utf8w(label) + utf8w(buf);
        refresh_screen();
        int c = read_key();
        if (!c) continue;
        if (c == DEL_KEY || c == BACKSPACE || c == CTRL_('h')) {
            if (len) { do len--; while (len && CONT(buf[len])); buf[len] = 0; }
        } else if (c == ESC || (c == '\r' && (len || prompt_empty_ok))) {
            E.promptcol = -1; E.msg[0] = 0; prompt_note = "";
            if (cb) cb(buf, c);
            if (c == ESC) { free(buf); return NULL; }
            return buf;
        } else if (c == '\t' && prompt_paths) complete(&buf, &cap, &len);
        else if (c >= 32 && c < 256 && c != 127) {
            if (len + 2 > cap) buf = xrealloc(buf, cap *= 2);
            buf[len++] = (char)c;
            buf[len] = 0;
        }
        if (cb) cb(buf, c);
    }
}

static int search(const char *q, int dir, int incl) {
    for (int i = 0; i <= E.nrows; i++) {
        int y = ((E.cy + dir * i) % E.nrows + E.nrows) % E.nrows, lo = 0, hi = E.row[y].len;
        const char *s = E.row[y].s, *m = NULL;
        if (i == 0) { if (dir > 0) lo = E.cx + !incl; else hi = E.cx - 1; }
        if (i == E.nrows) { if (dir > 0) hi = E.cx; else lo = E.cx; }
        for (const char *p = strstr(s, q); p && p - s <= hi; p = strstr(p + 1, q))
            if (p - s >= lo) { m = p; if (dir > 0) break; }
        if (m) {
            E.cy = y;
            E.cx = (int)(m - s);
            if (E.cy < E.rowoff || E.cy >= E.rowoff + E.rows) center();
            return 1;
        }
    }
    return 0;
}

static void find_cb(const char *q, int key) {
    if (key == '\r' || key == ESC) return;
    int dir = key == ARROW_RIGHT || key == ARROW_DOWN ? 1 : key == ARROW_LEFT || key == ARROW_UP ? -1 : 0;
    prompt_note = "";
    if (!*q) return;
    if (!dir) { E.cx = find_x; E.cy = find_y; }
    if (!search(q, dir ? dir : 1, !dir)) prompt_note = "  [not found]";
}

static void find(void) {
    int ro = E.rowoff, co = E.coloff;
    find_x = E.cx;
    find_y = E.cy;
    char *q = prompt("Find (Esc cancel, arrows next/prev): ", find_cb);
    if (q) { free(E.lastq); E.lastq = q; }
    else { E.cx = find_x; E.cy = find_y; E.rowoff = ro; E.coloff = co; }
}

static void goto_line(void) {
    char *s = prompt("Go to line: ", NULL);
    if (!s) return;
    int n = atoi(s);
    free(s);
    E.cy = (n < 1 ? 1 : n > E.nrows ? E.nrows : n) - 1;
    E.cx = 0;
    center();
}

static void replace_all(void) {
    char *f = prompt("Replace: ", NULL), *t;
    if (!f) return;
    prompt_empty_ok = 1;
    t = prompt("Replace with: ", NULL);
    prompt_empty_ok = 0;
    if (!t) { free(f); set_msg("Replace cancelled"); return; }
    int fl = (int)strlen(f), tl = (int)strlen(t), cnt = 0, cx = E.cx, cy = E.cy;
    E.group++;
    for (int y = 0; y < E.nrows; y++) {
        int pos = 0;
        char *m;
        while ((m = strstr(E.row[y].s + pos, f))) {
            int x = (int)(m - E.row[y].s);
            for (int k = 0; k < fl; k++) do_op(DEL, y, x, E.row[y].s[x]);
            for (int k = 0; k < tl; k++) do_op(INS, y, x + k, t[k]);
            pos = x + tl;
            cnt++;
        }
    }
    E.cy = cy < E.nrows ? cy : E.nrows - 1;
    E.cx = cx < E.row[E.cy].len ? cx : E.row[E.cy].len;
    if (cnt) set_msg("Replaced %d occurrence%s of '%.40s'", cnt, cnt == 1 ? "" : "s", f);
    else set_msg("No matches for '%.60s'", f);
    free(f);
    free(t);
}

static void open_prompt(const char *label, int mode) {
    char err[256], *q, *p = ask_path(label);
    const char *h = getenv("HOME");
    if (!p) return;
    q = xrealloc(NULL, strlen(p) + (h ? strlen(h) : 0) + 1);
    if (p[0] == '~' && (p[1] == '/' || !p[1]) && h) sprintf(q, "%s%s", h, p + 1);
    else strcpy(q, p);
    if (open_file_cmd(q, mode, err, sizeof err)) set_msg("%s", err);
    free(q);
    free(p);
}

static void dash_activate(int i) {
    switch (i) {
    case 0: open_prompt("Open file: ", 0); break;
    case 1:
        free_rows();
        insert_row(0, "", 0);
        free(E.filename);
        E.filename = NULL;
        E.lang = E.cx = E.cy = E.dash = 0;
        set_msg("New buffer - Ctrl-S to name and save it");
        break;
    case 2: open_prompt("Create file: ", 2); break;
    case 3:
        for (int k = 0; k < E.nrecent; k++) free(E.recent[k]);
        E.nrecent = recent_read(E.recent, NULL, MAXREC);
        E.rsel = 0;
        E.page = 1;
        break;
    case 4: E.page = 2; break;
    case 5: exit(0);
    }
}

static void dash_key(int c) {
    if (E.page == 2) { E.page = 0; return; }
    if (E.page == 1) {
        if (c == ARROW_UP && E.nrecent) E.rsel = (E.rsel + E.nrecent - 1) % E.nrecent;
        else if (c == ARROW_DOWN && E.nrecent) E.rsel = (E.rsel + 1) % E.nrecent;
        else if (c == ESC || c == ARROW_LEFT || c == BACKSPACE || c == 'q') E.page = 0;
        else if (c == '\r' || (c >= '1' && c <= '8')) {
            char err[256];
            int i = c == '\r' ? E.rsel : c - '1';
            if (i < E.nrecent && open_file_cmd(E.recent[i], 1, err, sizeof err)) set_msg("%s", err);
        }
        return;
    }
    int col = E.sel / 3, row = E.sel % 3;
    if (c == ARROW_UP) E.sel = E.twocol ? col * 3 + (row + 2) % 3 : (E.sel + 5) % 6;
    else if (c == ARROW_DOWN) E.sel = E.twocol ? col * 3 + (row + 1) % 3 : (E.sel + 1) % 6;
    else if ((c == ARROW_LEFT || c == ARROW_RIGHT) && E.twocol) E.sel = (E.sel + 3) % 6;
    else if (c == '\r') dash_activate(E.sel);
    else if (c == CTRL_('q')) exit(0);
    else if (c > 32 && c < 127)
        for (int i = 0; i < 6; i++)
            if (items[i].key == tolower(c)) { E.sel = i; dash_activate(i); return; }
}

static void put_line(int at, const char *s) {
    int y = at < E.nrows ? at : at - 1;
    do_op(SPLIT, y, at < E.nrows ? 0 : E.row[y].len, 0);
    for (int k = 0; s[k]; k++) do_op(INS, at, k, s[k]);
}

static void ins_line(int y, const char *s) {
    char *c = strdup(s);
    int cx = E.cx;
    E.group++;
    put_line(y, c);
    free(c);
    E.cy = y + 1;
    E.cx = cx < E.row[E.cy].len ? cx : E.row[E.cy].len;
}

static void newline(void) {
    Row *r = &E.row[E.cy];
    int ind = 0, op = E.cx > 0 ? r->s[E.cx - 1] : 0, nx = E.cx < r->len ? r->s[E.cx] : 0;
    while (ind < E.cx && (r->s[ind] == ' ' || r->s[ind] == '\t')) ind++;
    char *sp = xrealloc(NULL, ind + 1);
    memcpy(sp, r->s, ind);
    int deeper = op == '{' || op == '(' || op == '[' || (E.lang == 2 && op == ':');
    const char *extra = !deeper ? "" : (ind && sp[0] == ' ') || E.lang == 2 ? "    " : "\t";
    do_op(SPLIT, E.cy, E.cx, 0);
    for (int k = 0; k < ind; k++) do_op(INS, E.cy, k, sp[k]);
    for (int k = 0; extra[k]; k++) do_op(INS, E.cy, ind + k, extra[k]);
    if (deeper && nx && strchr("}])", nx)) {
        int y = E.cy, x = E.cx;
        do_op(SPLIT, y, x, 0);
        for (int k = 0; k < ind; k++) do_op(INS, y + 1, k, sp[k]);
        E.cy = y;
        E.cx = x;
    }
    free(sp);
}

static void backspace(void) {
    if (E.cx > 0) {
        for (int x = E.cx; x > 0;) {
            char b = E.row[E.cy].s[--x];
            do_op(DEL, E.cy, x, b);
            if (!CONT(b)) break;
        }
    } else if (E.cy > 0) do_op(JOIN, E.cy - 1, E.row[E.cy - 1].len, 0);
}

static void delete_forward(void) {
    if (E.cx < E.row[E.cy].len) {
        do do_op(DEL, E.cy, E.cx, E.row[E.cy].s[E.cx]);
        while (E.cx < E.row[E.cy].len && CONT(E.row[E.cy].s[E.cx]));
    } else if (E.cy + 1 < E.nrows) do_op(JOIN, E.cy, E.row[E.cy].len, 0);
}

static void del_row(int y) {
    for (int x = E.row[y].len; x > 0; x--) do_op(DEL, y, x - 1, E.row[y].s[x - 1]);
    if (y + 1 < E.nrows) do_op(JOIN, y, 0, 0);
    else if (y > 0) do_op(JOIN, y - 1, E.row[y - 1].len, 0);
}

static void move_line(int dir) {
    int y = E.cy, cx = E.cx;
    if (y + dir < 0 || y + dir >= E.nrows) return;
    char *t = strdup(E.row[y + dir].s);
    E.group++;
    del_row(y + dir);
    put_line(y, t);
    free(t);
    E.cy = y + dir;
    E.cx = cx < E.row[E.cy].len ? cx : E.row[E.cy].len;
}

static void after_lines(int sel, int sy, int ey, int cy, int cx, int d) {
    if (sel) { E.ay = sy; E.ax = 0; E.cy = ey; E.cx = E.row[ey].len; return; }
    E.cy = cy;
    E.cx = cx + d < 0 ? 0 : cx + d > E.row[cy].len ? E.row[cy].len : cx + d;
}

static void reindent(int dir) {
    int sy, sx, ey, ex, sel = sel_range(&sy, &sx, &ey, &ex), cy = E.cy, cx = E.cx, d = 0;
    if (!sel) sy = ey = cy;
    E.group++;
    for (int y = sy; y <= ey; y++) {
        Row *r = &E.row[y];
        int n = 0;
        if (dir > 0) {
            if (r->len) { do_op(INS, y, 0, '\t'); if (y == cy) d = 1; }
            continue;
        }
        if (r->len && r->s[0] == '\t') n = 1;
        else while (n < TABW && n < r->len && r->s[n] == ' ') n++;
        for (int k = 0; k < n; k++) do_op(DEL, y, 0, r->s[0]);
        if (y == cy) d = -n;
    }
    after_lines(sel, sy, ey, cy, cx, d);
}

static void toggle_comment(void) {
    const char *p = E.lang == 1 ? "// " : E.lang == 2 ? "# " : NULL;
    int sy, sx, ey, ex, sel = sel_range(&sy, &sx, &ey, &ex), cy = E.cy, cx = E.cx, d = 0, all = 1, any = 0, pl;
    if (!p) { set_msg("No comment style for this file type"); return; }
    if (!sel) sy = ey = cy;
    pl = (int)strlen(p);
    for (int y = sy, i; y <= ey; y++) {
        const char *s = E.row[y].s;
        for (i = 0; s[i] == ' ' || s[i] == '\t'; i++);
        if (i == E.row[y].len) continue;
        any = 1;
        if (strncmp(s + i, p, pl - 1)) all = 0;
    }
    if (!any) return;
    E.group++;
    for (int y = sy, i; y <= ey; y++) {
        const char *s = E.row[y].s;
        for (i = 0; s[i] == ' ' || s[i] == '\t'; i++);
        if (i == E.row[y].len) continue;
        if (all) {
            int n = pl - 1 + (s[i + pl - 1] == ' ');
            for (int k = 0; k < n; k++) do_op(DEL, y, i, E.row[y].s[i]);
            if (y == cy) d = -n;
        } else {
            for (int k = 0; k < pl; k++) do_op(INS, y, i + k, p[k]);
            if (y == cy) d = pl;
        }
    }
    after_lines(sel, sy, ey, cy, cx, d);
}

static int has_sel(void) {
    int a, b, c, d;
    return sel_range(&a, &b, &c, &d);
}

static void del_sel(void) {
    int sy, sx, ey, ex;
    if (!sel_range(&sy, &sx, &ey, &ex)) return;
    E.group++;
    E.cy = ey;
    E.cx = ex;
    while (E.cy > sy || E.cx > sx) backspace();
    E.sel_on = 0;
}

static void copy_sel(int cut) {
    int sy, sx, ey, ex;
    free(E.clip);
    if (sel_range(&sy, &sx, &ey, &ex)) {
        size_t n = 1;
        for (int y = sy; y <= ey; y++) n += E.row[y].len + 1;
        char *p = E.clip = xrealloc(NULL, n);
        for (int y = sy; y <= ey; y++) {
            int a = y == sy ? sx : 0, b = y == ey ? ex : E.row[y].len;
            memcpy(p, E.row[y].s + a, b - a);
            p += b - a;
            if (y < ey) *p++ = '\n';
        }
        *p = 0;
        E.clip_line = 0;
        if (cut) del_sel();
    } else {
        E.clip = strndup(E.row[E.cy].s, E.row[E.cy].len);
        E.clip_line = 1;
        if (cut) { E.group++; del_row(E.cy); }
    }
    set_msg(cut ? "Cut %zu bytes" : "Copied %zu bytes", strlen(E.clip));
}

static void paste(void) {
    if (!E.clip) { set_msg("Nothing to paste (Ctrl-C or Ctrl-K first)"); return; }
    if (E.clip_line && !has_sel()) { ins_line(E.cy, E.clip); return; }
    if (has_sel()) del_sel();
    else E.group++;
    for (const char *p = E.clip; *p; p++) {
        if (*p == '\n') do_op(SPLIT, E.cy, E.cx, 0);
        else do_op(INS, E.cy, E.cx, *p);
    }
}

static void search_word(void) {
    Row *r = &E.row[E.cy];
    int a = E.cx, b = E.cx;
    while (a > 0 && (isalnum((unsigned char)r->s[a - 1]) || r->s[a - 1] == '_')) a--;
    while (b < r->len && (isalnum((unsigned char)r->s[b]) || r->s[b] == '_')) b++;
    if (a == b) { set_msg("No word under cursor"); return; }
    free(E.lastq);
    E.lastq = strndup(r->s + a, b - a);
    E.cx = a;
    search(E.lastq, 1, 0);
    set_msg("/%s  (Alt-N / Alt-P for next / prev)", E.lastq);
}

static void unpair(void) {
    const char *s = E.row[E.cy].s;
    if (E.lang && E.cx > 0 && E.cx < E.row[E.cy].len) {
        char a = s[E.cx - 1], b = s[E.cx];
        if ((a == '(' && b == ')') || (a == '[' && b == ']') || (a == '{' && b == '}') || (a == '"' && b == '"'))
            do_op(DEL, E.cy, E.cx, b);
    }
}

static void type_char(int c) {
    static const char open[] = "([{\"", close[] = ")]}\"";
    const Row *r = &E.row[E.cy];
    int nx = E.cx < r->len ? r->s[E.cx] : 0, pv = E.cx ? r->s[E.cx - 1] : 0;
    const char *o = E.lang && c > 0 && c < 128 ? strchr(open, c) : NULL;
    if (E.lang && c > 0 && c < 128 && strchr(close, c) && nx == c) { E.cx++; return; }
    do_op(INS, E.cy, E.cx, (char)c);
    if (o && (c != '"' || (!isalnum((unsigned char)pv) && pv != '"' && pv != '\\')) && (!nx || strchr(" \t)]};,", nx))) {
        do_op(INS, E.cy, E.cx, close[o - open]);
        E.cx--;
    }
}

static void word_move(int dir) {
    Row *r = &E.row[E.cy];
    if (dir < 0 ? E.cx == 0 : E.cx >= r->len) {
        if (dir < 0 && E.cy > 0) { E.cy--; E.cx = E.row[E.cy].len; }
        if (dir > 0 && E.cy + 1 < E.nrows) { E.cy++; E.cx = 0; }
        return;
    }
    for (int pass = 0; pass < 2; pass++) {
        int want_space = dir < 0 ? pass == 0 : pass == 1;
        while (dir < 0 ? E.cx > 0 : E.cx < r->len) {
            if ((isspace((unsigned char)r->s[E.cx + (dir < 0 ? -1 : 0)]) != 0) != want_space) break;
            E.cx += dir;
        }
    }
}

static void word_delete(void) {
    int to = E.cx;
    if (has_sel()) { del_sel(); return; }
    if (!to) return;
    word_move(-1);
    int from = E.cx;
    E.cx = to;
    E.group++;
    while (E.cx > from) backspace();
}

static void match_bracket(void) {
    static const char b[] = "([{)]}";
    char ch = E.cx < E.row[E.cy].len ? E.row[E.cy].s[E.cx] : 0, *p = ch ? strchr(b, ch) : NULL;
    if (!p) { set_msg("Not on a bracket"); return; }
    int y = E.cy, x = E.cx, d = 0, i = (int)(p - b), dir = i < 3 ? 1 : -1;
    char match = b[(i + 3) % 6];
    for (;;) {
        x += dir;
        while (x < 0 || x >= E.row[y].len) {
            y += dir;
            if (y < 0 || y >= E.nrows) { set_msg("No matching bracket"); return; }
            x = dir > 0 ? 0 : E.row[y].len - 1;
        }
        char k = E.row[y].s[x];
        if (k == ch) d++;
        else if (k == match) {
            if (!d) { E.cy = y; E.cx = x; return; }
            d--;
        }
    }
}

static void process_key(int c) {
    static int quit_pending = 0;
    int was_pending = quit_pending, move = 0, vertical = 0, typed = 0;
    quit_pending = 0;

    switch (c) {
    case '\r':
        if (has_sel()) del_sel();
        else E.group++;
        newline();
        break;
    case CTRL_('q'):
        if (is_dirty() && !was_pending) {
            set_msg("Unsaved changes! Ctrl-Q again to discard and quit, Ctrl-S to save.");
            quit_pending = 1;
            return;
        }
        exit(0);
    case CTRL_('s'): save_file(); break;
    case CTRL_('o'):
        if (is_dirty()) set_msg("Unsaved changes - save first (Ctrl-S)");
        else open_prompt("Open file: ", 0);
        break;
    case CTRL_('z'): undo(); break;
    case CTRL_('y'): redo(); break;
    case CTRL_('f'): find(); break;
    case CTRL_('r'): replace_all(); break;
    case CTRL_('g'): goto_line(); break;
    case CTRL_('l'): center(); break;
    case CTRL_('v'):
        E.sel_on = !E.sel_on;
        E.ay = E.cy;
        E.ax = E.cx;
        move = 1;
        if (E.sel_on) set_msg("Select: move to extend - Ctrl-C copy, Ctrl-K cut, Esc cancel");
        break;
    case ALT('a'):
        E.sel_on = move = 1;
        E.ay = E.ax = 0;
        E.cy = E.nrows - 1;
        E.cx = E.row[E.cy].len;
        break;
    case CTRL_('c'): copy_sel(0); break;
    case CTRL_('k'): copy_sel(1); break;
    case CTRL_('u'): paste(); break;
    case CTRL_('d'): ins_line(E.cy, E.row[E.cy].s); break;
    case CTRL_('w'): word_delete(); break;
    case CTRL_('b'): match_bracket(); break;
    case CTRL_('n'): E.nums = !E.nums; break;
    case CTRL_('_'): case ALT('/'): move = has_sel(); toggle_comment(); break;
    case BACKTAB: move = has_sel(); reindent(-1); break;
    case MOVE_UP: case ALT('k'): move_line(-1); break;
    case MOVE_DOWN: case ALT('j'): move_line(1); break;
    case ALT('s'): search_word(); break;
    case ALT('w'):
        E.wrap = !E.wrap;
        E.coloff = 0;
        set_msg("Soft wrap %s", E.wrap ? "on" : "off");
        break;
    case ALT('n'): case ALT('p'):
        if (!E.lastq) set_msg("No previous search (Ctrl-F)");
        else if (!search(E.lastq, c == ALT('n') ? 1 : -1, 0)) set_msg("'%.40s' not found", E.lastq);
        break;
    case BACKSPACE: case CTRL_('h'):
        if (has_sel()) del_sel();
        else { E.group++; unpair(); backspace(); }
        break;
    case DEL_KEY:
        if (has_sel()) del_sel();
        else { E.group++; delete_forward(); }
        break;
    case ARROW_LEFT: move = 1;
        if (E.cx > 0) { do E.cx--; while (E.cx > 0 && CONT(E.row[E.cy].s[E.cx])); }
        else if (E.cy > 0) { E.cy--; E.cx = E.row[E.cy].len; }
        break;
    case ARROW_RIGHT: move = 1;
        if (E.cx < E.row[E.cy].len) { do E.cx++; while (E.cx < E.row[E.cy].len && CONT(E.row[E.cy].s[E.cx])); }
        else if (E.cy + 1 < E.nrows) { E.cy++; E.cx = 0; }
        break;
    case ARROW_UP: move = vertical = 1; if (E.cy > 0) E.cy--; break;
    case ARROW_DOWN: move = vertical = 1; if (E.cy + 1 < E.nrows) E.cy++; break;
    case PAGE_UP: move = vertical = 1; E.cy = E.cy < E.rows ? 0 : E.cy - E.rows; break;
    case PAGE_DOWN: move = vertical = 1; E.cy = E.cy + E.rows >= E.nrows ? E.nrows - 1 : E.cy + E.rows; break;
    case TOP_KEY: case ALT('<'): move = 1; E.cy = E.cx = 0; break;
    case BOT_KEY: case ALT('>'): move = 1; E.cy = E.nrows - 1; E.cx = E.row[E.cy].len; break;
    case HOME_KEY: case CTRL_('a'): {
        int i = 0;
        move = 1;
        while (i < E.row[E.cy].len && (E.row[E.cy].s[i] == ' ' || E.row[E.cy].s[i] == '\t')) i++;
        E.cx = E.cx == i ? 0 : i;
        break;
    }
    case END_KEY: case CTRL_('e'): move = 1; E.cx = E.row[E.cy].len; break;
    case WORD_LEFT: move = 1; word_move(-1); break;
    case WORD_RIGHT: move = 1; word_move(1); break;
    default:
        if (c == '\t' && has_sel()) { move = 1; reindent(1); }
        else if (c == '\t' || (c >= 32 && c < 256 && c != 127)) {
            if (has_sel()) { del_sel(); last_typing = 1; }
            if (!last_typing) E.group++;
            type_char(c);
            last_typing = !(c == ' ' || c == '\t');
            typed = 1;
        }
    }
    if (!typed) last_typing = 0;
    if (!move) E.sel_on = 0;
    if (vertical) E.cx = rx_to_cx(&E.row[E.cy], E.want);
    else E.want = cx_to_rx(&E.row[E.cy], E.cx);
}

static int is_system(const char *p) {
    static const char *const sys[] = { "/bin", "/boot", "/dev", "/etc", "/lib", "/lib32", "/lib64", "/proc", "/sbin", "/sys", "/usr", "/var", "/system", NULL };
    char t[PATH_MAX], par[PATH_MAX], full[PATH_MAX], out[PATH_MAX * 2], *b;
    const char *pre = getenv("PREFIX");
    size_t n = strlen(p);
    snprintf(t, sizeof t, "%s", p);
    while (n > 1 && t[n - 1] == '/') t[--n] = 0;
    if (!strcmp(t, "/")) return 1;
    b = strrchr(t, '/');
    if (b) { snprintf(par, sizeof par, "%.*s", b == t ? 1 : (int)(b - t), t); b++; }
    else { snprintf(par, sizeof par, "."); b = t; }
    if (!realpath(par, full)) return 0;
    snprintf(out, sizeof out, "%s%s%s", full, strcmp(full, "/") ? "/" : "", b);
    if (strrchr(out, '/') == out) return 1;
    for (int i = 0; sys[i]; i++) {
        size_t l = strlen(sys[i]);
        if (!strncmp(out, sys[i], l) && (!out[l] || out[l] == '/')) return 1;
    }
    if (pre && *pre && strlen(pre) > 1) {
        size_t l = strlen(pre);
        if (!strncmp(out, pre, l) && (!out[l] || out[l] == '/')) return 1;
    }
    return 0;
}

static int delete_files(int n, char **v) {
    int force = 0, cnt = 0, rc = 0;
    struct stat st;
    for (int i = 0; i < n; i++) {
        if (!strcmp(v[i], "--no-preserve-root")) force = 1;
        else cnt++;
    }
    if (!cnt) { fputs("usage: avtty -d [--no-preserve-root] file...\n", stderr); return 1; }
    for (int i = 0; i < n && !force; i++)
        if (strcmp(v[i], "--no-preserve-root") && is_system(v[i])) {
            fprintf(stderr, "avtty: refusing to delete system path '%s'\n"
                            "avtty: use avtty -d --no-preserve-root to override\n", v[i]);
            return 1;
        }
    for (int i = 0; i < n; i++) {
        if (!strcmp(v[i], "--no-preserve-root")) continue;
        if (lstat(v[i], &st)) { fprintf(stderr, "avtty: cannot delete '%s': %s\n", v[i], strerror(errno)); rc = 1; }
        else if (S_ISDIR(st.st_mode)) { fprintf(stderr, "avtty: '%s' is a directory (avtty -d only deletes files)\n", v[i]); rc = 1; }
        else if (unlink(v[i])) { fprintf(stderr, "avtty: cannot delete '%s': %s\n", v[i], strerror(errno)); rc = 1; }
        else printf("removed '%s'\n", v[i]);
    }
    return rc;
}

static void usage(void) {
    puts("avtty " VERSION " - a very tiny text yard\n"
         "Part of the Texivi Software Suite (TSS)\n\n"
         "usage: avtty [+line] [file]   open file (new buffer if it doesn't exist)\n"
         "       avtty -o file          open an existing file\n"
         "       avtty -c file          create a new file\n"
         "       avtty -d file...       delete files (never directories)\n"
         "       cmd | avtty            edit piped text, then save it with Ctrl-S\n"
         "       avtty -h | -v          help | version\n\n"
         "avtty -d refuses system paths such as /bin or /etc/passwd and anything under\n"
         "/usr, /var and similar. To override: avtty -d --no-preserve-root file\n\n"
         "Keys:");
    for (int g = 0; g < 6; g++) {
        printf("\n  %s\n", keys[g].title);
        for (int i = 0; i < keys[g].n; i++)
            printf("    %s%*s %s\n", keys[g].i[i].k, 14 - utf8w(keys[g].i[i].k), "", keys[g].i[i].d);
    }
    puts("\nAlso:\n"
         "    Ctrl-A / E = Home / End, Alt-J / K = move line, Alt-/ = comment (when Ctrl-/ is unavailable)\n"
         "    Alt-< / Alt-> = top / bottom, Esc cancels a selection or a prompt\n"
         "    Tab completes file names in Open / Save prompts, ~ expands to your home\n"
         "    Code files get syntax colours, auto-pairs ()[]{}\"\" and smart indent after { ( [\n"
         "    The cursor line of recent files is remembered in ~/.avtty_recent");
}

int main(int argc, char **argv) {
    const char *file = NULL;
    int mode = 0, line = 0, piped = 0;
    char err[256];

    if (argc > 1 && !strcmp(argv[1], "-d")) return delete_files(argc - 2, argv + 2);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h")) { usage(); return 0; }
        if (!strcmp(a, "-v")) { puts("avtty " VERSION " - Texivi Software Suite (TSS)"); return 0; }
        if (a[0] == '+' && isdigit((unsigned char)a[1])) line = atoi(a + 1);
        else if ((!strcmp(a, "-c") || !strcmp(a, "-o")) && i + 1 < argc && !file) {
            mode = a[1] == 'c' ? 2 : 1;
            file = argv[++i];
        } else if (a[0] != '-' && !file) file = a;
        else { fprintf(stderr, "avtty: bad argument '%s' (try -h)\n", a); return 1; }
    }
    if (!isatty(STDIN_FILENO)) {
        if (!file) { load_stream(stdin); piped = 1; }
        int t = open("/dev/tty", O_RDWR);
        if (t < 0 || dup2(t, STDIN_FILENO) < 0) { fputs("avtty: no terminal available\n", stderr); return 1; }
        close(t);
    }
    if (!isatty(STDOUT_FILENO)) { fputs("avtty: stdout is not a terminal\n", stderr); return 1; }

    E.promptcol = -1;
    if (file) {
        if (open_file_cmd(file, mode, err, sizeof err)) { fprintf(stderr, "avtty: %s\n", err); return 1; }
    } else if (piped) {
        if (!E.nrows) insert_row(0, "", 0);
        set_msg("Read from stdin - Ctrl-S to save it somewhere");
    } else E.dash = 1;
    if (line && !E.dash) {
        E.cy = line > E.nrows ? E.nrows - 1 : line - 1;
        center();
    }

    enable_raw();
    atexit(remember);
    refresh_screen();
    for (;;) {
        int rs = E.srows, cs = E.scols, had_msg = E.msg[0] != 0;
        get_size();
        int c = read_key();
        if (!c) {
            int expired = had_msg && time(NULL) - E.msgtime >= 5;
            if (expired) E.msg[0] = 0;
            if (expired || rs != E.srows || cs != E.scols) refresh_screen();
            continue;
        }
        if (E.dash) dash_key(c);
        else process_key(c);
        refresh_screen();
    }
}
