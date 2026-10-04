#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
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

#define VERSION "0.3.0"
#define TABW 4
#define CTRL_(k) ((k) & 0x1f)
#define ESC 27
#define CONT(b) ((((unsigned char)(b)) & 0xC0) == 0x80)
#define RECENT_MAX 8
#define ITEM_W 24
#define NITEMS 7
#define HINT "Ctrl: S save | Q quit | Z undo | Y redo | F find | R replace | G goto | K cut"

#define C1 "18;34;156"
#define C2 "35;61;255"
#define C3 "93;182;250"
#define C4 "202;232;255"
#define C5 "245;246;251"
#define SGR(s) "\x1b[" s "m"
#define FG(c) SGR("38;2;" c)
#define RESET SGR("0")
#define SEL SGR("1;48;2;" C2 ";38;2;" C5)
#define BAR SGR("1;48;2;" C1 ";38;2;" C5)

enum Key {
    BACKSPACE = 127,
    ARROW_LEFT = 1000, ARROW_RIGHT, ARROW_UP, ARROW_DOWN,
    WORD_LEFT, WORD_RIGHT, DEL_KEY, HOME_KEY, END_KEY, PAGE_UP, PAGE_DOWN
};

typedef struct { char *s; int len; } Row;
typedef enum { INS, DEL, SPLIT, JOIN } OpType;
typedef struct { OpType t; int y, x, group; char c; } Op;
struct abuf { char *b; int len, cap; };

static struct {
    int cx, cy, rx, want, rowoff, coloff, srows, scols, rows;
    Row *row; int nrows, caprows;
    char *filename;
    int crlf, nums;
    char msg[160]; time_t msgtime;
    int promptcol;
    Op *ops; int nops, capops, cur, saved, group;
    int dash, page, sel, rsel, twocol, lang;
    char *recent[RECENT_MAX]; int nrecent;
    struct termios orig; int raw;
} E;

static int last_typing = 0, prompt_empty_ok = 0;
static const char *prompt_note = "";

static const struct { const char *label; char key; } items[NITEMS] = {
    { "Open File", 'o' }, { "New File", 'n' }, { "Create File", 'c' }, { "Recent Files", 'r' },
    { "Keys & Help", 'h' }, { "Line Numbers", 'l' }, { "Quit", 'q' }
};

static const char *help[] = {
    "Ctrl-S    save", "Ctrl-Q    quit", "Ctrl-Z    undo", "Ctrl-Y    redo",
    "Ctrl-F    find", "Ctrl-R    replace all", "Ctrl-G    go to line", "Ctrl-K    cut line",
    "Ctrl-N    line numbers", "Ctrl-A/E  line start/end", "Ctrl+Arrows  by word"
};

static void out(const char *s) { (void)!write(STDOUT_FILENO, s, strlen(s)); }

static void disable_raw(void) {
    if (!E.raw) return;
    E.raw = 0;
    out("\x1b[0m\x1b[?1049l");
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

static int read_key(void) {
    char c, q[5];
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 0) return 0;
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return 0;
        die("read");
    }
    if (c != ESC) return (unsigned char)c;
    if (read(STDIN_FILENO, &q[0], 1) != 1 || read(STDIN_FILENO, &q[1], 1) != 1) return ESC;
    if (q[0] == '[') {
        if (q[1] >= '0' && q[1] <= '9') {
            if (read(STDIN_FILENO, &q[2], 1) != 1) return ESC;
            if (q[2] == '~') {
                switch (q[1]) {
                    case '1': case '7': return HOME_KEY;
                    case '3': return DEL_KEY;
                    case '4': case '8': return END_KEY;
                    case '5': return PAGE_UP;
                    case '6': return PAGE_DOWN;
                }
            } else if (q[2] == ';') {
                if (read(STDIN_FILENO, &q[3], 1) != 1 || read(STDIN_FILENO, &q[4], 1) != 1) return ESC;
                if (q[4] == 'C') return WORD_RIGHT;
                if (q[4] == 'D') return WORD_LEFT;
            }
        } else {
            switch (q[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
    } else if (q[0] == 'O') {
        if (q[1] == 'H') return HOME_KEY;
        if (q[1] == 'F') return END_KEY;
    }
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

static void put(struct abuf *ab, int r, int c, const char *style, const char *s) {
    if (r < 1 || r >= E.srows) return;
    at(ab, r, c);
    ab_s(ab, style);
    ab_s(ab, s);
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
    "inline", "class", "public", "private", "protected", "new", "delete", "this", "try", "catch",
    "throw", "import", "from", "as", "def", "lambda", "pass", "with", "yield", "None", "True", "False",
    "null", "true", "false", "NULL", "function", "var", "let", "fn", "pub", "use", "impl", "match",
    "loop", "mut", "self", "async", "await", "package", "interface", "func", "defer", "then", "fi",
    "elif", "done", "in", "is", "not", "and", "or", "export", "local", NULL
};

static const char *const ty[] = {
    "int", "char", "void", "long", "short", "float", "double", "bool", "unsigned", "signed", "size_t",
    "ssize_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t",
    "int64_t", "FILE", "string", "str", "i32", "u32", "i64", "u64", "u8", "usize", "f32", "f64", NULL
};

static const char *const hlc[] = {
    RESET, SGR("3;38;2;" C2), SGR("1;38;2;" C3), FG(C4), SGR("1;38;2;" C5), SGR("1;38;2;" C2), FG(C3)
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
            memset(c + i, has(kw, s + i, j - i) ? 2 : has(ty, s + i, j - i) ? 6 : 0, j - i);
            i = j;
        } else i++;
    }
    return c;
}

static void draw_row(struct abuf *ab, const Row *r, int width, const char *cls) {
    int col = 0, lo = E.coloff, hi = lo + width, cur = 0;
    for (int i = 0; i < r->len && col < hi;) {
        unsigned char ch = r->s[i];
        if (cls[i] != cur && col >= lo) { ab_s(ab, hlc[(int)cls[i]]); cur = cls[i]; }
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
    if (cur) ab_s(ab, RESET);
}

static void fit_tail(char *dst, size_t n, const char *s, int maxw) {
    int skip = utf8w(s) - maxw;
    if (maxw < 1) { dst[0] = 0; return; }
    if (skip <= 0) { snprintf(dst, n, "%s", s); return; }
    for (skip++; *s && skip > 0; skip--) { s++; while (CONT(*s)) s++; }
    snprintf(dst, n, "…%s", s);
}

static const char *const bigl[6] = {
    " █████╗ " "██╗   ██╗" "████████╗" "████████╗" "██╗   ██╗",
    "██╔══██╗" "██║   ██║" "╚══██╔══╝" "╚══██╔══╝" "╚██╗ ██╔╝",
    "███████║" "██║   ██║" "   ██║   " "   ██║   " " ╚████╔╝ ",
    "██╔══██║" "╚██╗ ██╔╝" "   ██║   " "   ██║   " "  ╚██╔╝  ",
    "██║  ██║" " ╚████╔╝ " "   ██║   " "   ██║   " "   ██║   ",
    "╚═╝  ╚═╝" "  ╚═══╝  " "   ╚═╝   " "   ╚═╝   " "   ╚═╝   "
};

static void compact_row(char *b, int r) {
    static const char *const g[5][5] = {
        { " ### ", "#   #", "#####", "#   #", "#   #" },
        { "#   #", "#   #", "#   #", " # # ", "  #  " },
        { "#####", "  #  ", "  #  ", "  #  ", "  #  " },
        { "#####", "  #  ", "  #  ", "  #  ", "  #  " },
        { "#   #", " # # ", "  #  ", "  #  ", "  #  " }
    };
    b[0] = 0;
    for (int x = 0; x < 29; x++) {
        int l = x / 6, p = x % 6;
        strcat(b, p < 5 && r < 5 && g[l][r][p] == '#' ? "█" : p < 5 && r > 0 && g[l][r - 1][p] == '#' ? "▒" : " ");
    }
}

static void paint(struct abuf *ab, const char *s, int y, int w) {
    static const int pal[5][3] = { { 245, 246, 251 }, { 202, 232, 255 }, { 93, 182, 250 }, { 35, 61, 255 }, { 18, 34, 156 } };
    int x = 0, last = -1;
    for (; *s; x++) {
        int l = 1;
        while (CONT(s[l])) l++;
        if (*s != ' ') {
            int p = (x * 60 / w + y * 8) * 82 / 100 * 4, i = p / 100 > 3 ? 3 : p / 100, f = p - i * 100, c[3];
            int face = l == 3 && !memcmp(s, "█", 3);
            for (int k = 0; k < 3; k++) {
                c[k] = pal[i][k] + (pal[i + 1][k] - pal[i][k]) * f / 100;
                if (!face) c[k] = c[k] * 55 / 100;
            }
            if ((c[0] << 16 | c[1] << 8 | c[2]) != last) {
                char b[40];
                last = c[0] << 16 | c[1] << 8 | c[2];
                snprintf(b, sizeof b, "\x1b[38;2;%d;%d;%dm", c[0], c[1], c[2]);
                ab_s(ab, b);
            }
            ab_append(ab, s, l);
        } else ab_s(ab, " ");
        s += l;
    }
    ab_s(ab, RESET);
}

static void ctr(struct abuf *ab, int r, int x0, int iw, const char *style, const char *s) {
    put(ab, r, x0 + (iw - utf8w(s)) / 2, style, s);
    ab_s(ab, RESET);
}

static void item(struct abuf *ab, int r, int c, int i) {
    char t[48], k[4];
    int s = i == E.sel;
    snprintf(t, sizeof t, "%s%-19s", s ? " ▸ " : "   ",
             i == 5 ? (E.nums ? "Line Numbers: on" : "Line Numbers: off") : items[i].label);
    snprintf(k, sizeof k, "%c ", items[i].key);
    put(ab, r, c, s ? SEL : RESET, t);
    put(ab, r, c + 22, s ? SEL : FG(C3), k);
    ab_s(ab, RESET);
}

static void hline(char *b, const char *l, const char *r, int n) {
    strcpy(b, l);
    while (n-- > 0) strcat(b, "─");
    strcat(b, r);
}

static void dash_draw(struct abuf *ab) {
    int cols = E.scols, avail = E.srows - 1;
    for (int y = 1; y <= avail; y++) { at(ab, y, 1); ab_s(ab, "\x1b[K"); }
    if (cols < 40 || avail < 6) { put(ab, 1, 1, RESET, "avtty: terminal too small"); return; }

    int big = cols >= 56, lw = big ? 44 : 29;
    E.twocol = cols >= 62;
    int mr = E.twocol ? 4 : NITEMS, mw = E.twocol ? 2 * ITEM_W + 4 : ITEM_W;
    int iw = (lw > mw ? lw : mw) + 8, left = (cols - iw - 2) / 2 + 1, x0 = left + 1;
    int logo = E.page == 0 && avail >= mr + 14;
    int cnt = E.page == 0 ? mr : E.page == 1 ? (E.nrecent ? E.nrecent : 1) : (int)(sizeof help / sizeof *help);
    int n = E.page == 0 ? (logo ? 9 : 1) + cnt + 3 : cnt + 6;
    int top = (avail - n - 2) / 2 + 1, r;
    if (top < 1) top = 1;

    char ln[512];
    hline(ln, "╭", "╮", iw);
    put(ab, top, left, FG(C1), ln);
    for (int i = 1; i <= n; i++) {
        put(ab, top + i, left, FG(C1), "│");
        put(ab, top + i, left + iw + 1, FG(C1), "│");
    }
    hline(ln, "╰", "╯", iw);
    put(ab, top + n + 1, left, FG(C1), ln);
    ab_s(ab, RESET);

    if (E.page == 0) {
        r = top + 2;
        if (logo) {
            for (int k = 0; k < 6; k++) {
                char b[128];
                at(ab, r + k, x0 + (iw - lw) / 2);
                paint(ab, big ? bigl[k] : (compact_row(b, k), b), k, lw);
            }
            r += 8;
        }
        for (int k = 0; k < mr; k++) {
            item(ab, r + k, x0 + (iw - mw) / 2, k);
            if (E.twocol && k + 4 < NITEMS) item(ab, r + k, x0 + (iw - mw) / 2 + ITEM_W + 4, k + 4);
        }
        ctr(ab, r + mr + 1, x0, iw, FG(C2), "avtty " VERSION " • ↑↓←→ Enter");
        return;
    }

    ctr(ab, top + 2, x0, iw, SGR("1;38;2;" C4), E.page == 1 ? "Recent Files" : "Keys");
    r = top + 4;
    if (E.page == 2) {
        for (int i = 0; i < cnt; i++) put(ab, r + i, x0 + (iw - 24) / 2, RESET, help[i]);
        ctr(ab, r + cnt + 1, x0, iw, FG(C2), "Press any key to go back");
        return;
    }
    if (!E.nrecent) ctr(ab, r, x0, iw, FG(C2), "Nothing here yet");
    for (int i = 0; i < E.nrecent; i++) {
        char t[PATH_MAX + 8], s[PATH_MAX + 16], l[PATH_MAX + 32];
        const char *p = E.recent[i], *h = getenv("HOME");
        size_t hl = h ? strlen(h) : 0;
        int sel = i == E.rsel, cw = iw - 8;
        if (hl && !strncmp(p, h, hl) && (!p[hl] || p[hl] == '/')) snprintf(t, sizeof t, "~%s", p + hl);
        else snprintf(t, sizeof t, "%s", p);
        fit_tail(s, sizeof s, t, cw - 3);
        snprintf(l, sizeof l, "%s%s", sel ? " ▸ " : "   ", s);
        put(ab, r + i, x0 + 4, sel ? SEL : RESET, l);
        ab_rep(ab, " ", cw - 3 - utf8w(s));
        ab_s(ab, RESET);
    }
    ctr(ab, r + cnt + 1, x0, iw, FG(C2), "Enter or 1-8 open • Esc back");
}

static void refresh_screen(void) {
    get_size();
    struct abuf ab = { NULL, 0, 0 };
    ab_s(&ab, "\x1b[?25l" RESET);
    int gw = 0;

    if (E.dash) dash_draw(&ab);
    else {
        if (E.nums) {
            gw = snprintf(NULL, 0, "%d", E.nrows);
            gw = (gw < 3 ? 3 : gw) + 1;
            if (E.scols <= gw + 8) gw = 0;
        }
        int textw = E.scols - gw > 0 ? E.scols - gw : 1;
        if (E.cy < E.rowoff) E.rowoff = E.cy;
        if (E.cy >= E.rowoff + E.rows) E.rowoff = E.cy - E.rows + 1;
        E.rx = cx_to_rx(&E.row[E.cy], E.cx);
        if (E.rx < E.coloff) E.coloff = E.rx;
        if (E.rx >= E.coloff + textw) E.coloff = E.rx - textw + 1;

        int blk = 0;
        if (E.lang) for (int i = 0; i < E.rowoff && i < E.nrows; i++) hl(&E.row[i], &blk);
        for (int y = 0; y < E.rows; y++) {
            int fr = E.rowoff + y;
            at(&ab, y + 1, 1);
            if (fr < E.nrows) {
                if (gw) {
                    char num[96];
                    snprintf(num, sizeof num, FG(C2) "%*d " RESET, gw - 1, fr + 1);
                    ab_s(&ab, num);
                }
                draw_row(&ab, &E.row[fr], textw, hl(&E.row[fr], &blk));
            } else ab_s(&ab, FG(C1) "~" RESET);
            ab_s(&ab, "\x1b[K");
        }

        char left[256], right[64];
        int ln = snprintf(left, sizeof left, " avtty  %.100s%s", E.filename ? E.filename : "[No Name]",
                          is_dirty() ? " [modified]" : "");
        int rn = snprintf(right, sizeof right, "Ln %d/%d, Col %d ", E.cy + 1, E.nrows, E.rx + 1);
        if (ln > E.scols) ln = E.scols;
        at(&ab, E.rows + 1, 1);
        ab_s(&ab, BAR);
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
        at(&ab, E.cy - E.rowoff + 1, E.rx - E.coloff + gw + 1);
        ab_s(&ab, "\x1b[?25h");
    }
    (void)!write(STDOUT_FILENO, ab.b, ab.len);
    free(ab.b);
}

static int recent_path(char *o, size_t n) {
    const char *s = getenv("XDG_STATE_HOME"), *h = getenv("HOME");
    if (s && *s) snprintf(o, n, "%s/avtty/recent", s);
    else if (h && *h) snprintf(o, n, "%s/.local/state/avtty/recent", h);
    else return -1;
    return 0;
}

static int recent_read(char **o, int max) {
    char p[PATH_MAX], *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int c = 0;
    if (recent_path(p, sizeof p)) return 0;
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    while (c < max && (n = getline(&line, &cap, f)) != -1) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n > 0 && access(line, F_OK) == 0) o[c++] = strdup(line);
    }
    free(line);
    fclose(f);
    return c;
}

static void recent_add(const char *path) {
    char *real = realpath(path, NULL), *old[RECENT_MAX], p[PATH_MAX];
    if (!real) return;
    int n = recent_read(old, RECENT_MAX);
    if (recent_path(p, sizeof p) == 0) {
        for (char *s = p + 1; *s; s++)
            if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
        FILE *f = fopen(p, "w");
        if (f) {
            fprintf(f, "%s\n", real);
            for (int i = 0, w = 1; i < n && w < RECENT_MAX; i++)
                if (strcmp(old[i], real)) { fprintf(f, "%s\n", old[i]); w++; }
            fclose(f);
        }
    }
    for (int i = 0; i < n; i++) free(old[i]);
    free(real);
}

static int load_file(const char *fn) {
    FILE *f = fopen(fn, "rb");
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    if (!f) return -1;
    while ((n = getline(&line, &cap, f)) != -1) {
        if (n > 0 && line[n - 1] == '\n') {
            n--;
            if (n > 0 && line[n - 1] == '\r') { n--; if (!E.nrows) E.crlf = 1; }
        }
        insert_row(E.nrows, line, (int)n);
    }
    free(line);
    int err = ferror(f);
    fclose(f);
    if (!E.nrows) insert_row(0, "", 0);
    return err ? -1 : 0;
}

static int open_file_cmd(const char *file, int mode, char *err, size_t en) {
    struct stat st;
    int exists = stat(file, &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) { snprintf(err, en, "'%s' is a directory", file); return -1; }
    if (mode == 2) {
        if (exists) { snprintf(err, en, "'%s' already exists (use -o to open it)", file); return -1; }
        FILE *f = fopen(file, "wx");
        if (!f) { snprintf(err, en, "can't create '%s': %s", file, strerror(errno)); return -1; }
        fclose(f);
        exists = 1;
    } else if (mode == 1 && !exists) {
        snprintf(err, en, "'%s' does not exist (use -c to create it)", file);
        return -1;
    }
    free_rows();
    E.crlf = 0;
    if (!exists) insert_row(0, "", 0);
    else if (load_file(file) == -1) {
        snprintf(err, en, "can't read '%s': %s", file, strerror(errno));
        free_rows();
        return -1;
    }
    free(E.filename);
    E.filename = strdup(file);
    detect_lang();
    E.cx = E.cy = E.rowoff = E.coloff = E.nops = E.cur = E.saved = E.dash = 0;
    if (exists) { recent_add(file); set_msg(HINT); }
    else set_msg("New file: %s  (Ctrl-S to save)", file);
    return 0;
}

static char *prompt(const char *label, void (*cb)(const char *, int));

static int save_file(void) {
    if (!E.filename) {
        char *n = prompt("Save as (Esc to cancel): ", NULL);
        if (!n) { set_msg("Save cancelled"); return 0; }
        E.filename = n;
        detect_lang();
    }
    char *real = realpath(E.filename, NULL), tmp[PATH_MAX + 16];
    const char *target = real ? real : E.filename;
    snprintf(tmp, sizeof tmp, "%s.avtty.tmp", target);
    FILE *f = fopen(tmp, "wb");
    long total = 0;
    int ok = 0, e = errno;
    if (f) {
        if (!(E.nrows == 1 && !E.row[0].len))
            for (int i = 0; i < E.nrows; i++) {
                fwrite(E.row[i].s, 1, E.row[i].len, f);
                total += E.row[i].len + 1 + E.crlf;
                if (E.crlf) fputc('\r', f);
                fputc('\n', f);
            }
        ok = fflush(f) == 0 && !ferror(f);
        e = errno;
        ok = fclose(f) == 0 && ok;
        if (ok) {
            struct stat st;
            if (stat(target, &st) == 0) chmod(tmp, st.st_mode & 07777);
            if (rename(tmp, target)) { ok = 0; e = errno; }
        }
        if (!ok) unlink(tmp);
    }
    free(real);
    if (!ok) { set_msg("Can't save: %s", strerror(e)); return -1; }
    E.saved = E.cur;
    recent_add(E.filename);
    set_msg("%ld bytes written to %s", total, E.filename);
    return 0;
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
        } else if (c >= 32 && c < 256 && c != 127) {
            if (len + 2 > cap) buf = xrealloc(buf, cap *= 2);
            buf[len++] = (char)c;
            buf[len] = 0;
        }
        if (cb) cb(buf, c);
    }
}

static void find_cb(const char *q, int key) {
    static int last = -1, dir = 1;
    if (key == '\r' || key == ESC) { last = -1; dir = 1; return; }
    if (key == ARROW_RIGHT || key == ARROW_DOWN) dir = 1;
    else if (key == ARROW_LEFT || key == ARROW_UP) dir = -1;
    else { last = -1; dir = 1; }
    prompt_note = "";
    if (!*q) return;
    int cur = last == -1 ? E.cy - 1 : last;
    if (last != -1 && dir == 1 && E.cx < E.row[cur].len) {
        char *m = strstr(E.row[cur].s + E.cx + 1, q);
        if (m) { E.cx = (int)(m - E.row[cur].s); return; }
    }
    for (int i = 0; i < E.nrows; i++) {
        cur = (cur + dir + E.nrows) % E.nrows;
        char *m = strstr(E.row[cur].s, q);
        if (m) { last = E.cy = cur; E.cx = (int)(m - E.row[cur].s); return; }
    }
    prompt_note = "  [not found]";
}

static void find(void) {
    int cx = E.cx, cy = E.cy, ro = E.rowoff, co = E.coloff;
    char *q = prompt("Find (Esc cancel, arrows next/prev): ", find_cb);
    if (q) free(q);
    else { E.cx = cx; E.cy = cy; E.rowoff = ro; E.coloff = co; }
}

static void goto_line(void) {
    char *s = prompt("Go to line: ", NULL);
    if (!s) return;
    int n = atoi(s);
    free(s);
    E.cy = (n < 1 ? 1 : n > E.nrows ? E.nrows : n) - 1;
    E.cx = 0;
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

static void open_path(const char *label, int mode) {
    char err[256], *p = prompt(label, NULL);
    if (!p) return;
    const char *h = getenv("HOME");
    char *q = xrealloc(NULL, strlen(p) + (h ? strlen(h) : 0) + 1);
    if (p[0] == '~' && (p[1] == '/' || !p[1]) && h) sprintf(q, "%s%s", h, p + 1);
    else strcpy(q, p);
    if (open_file_cmd(q, mode, err, sizeof err)) set_msg("%s", err);
    free(q);
    free(p);
}

static void open_recent(int i) {
    char err[256];
    if (open_file_cmd(E.recent[i], 1, err, sizeof err)) set_msg("%s", err);
}

static void dash_activate(int i) {
    switch (i) {
    case 0: open_path("Open file: ", 0); break;
    case 1:
        free_rows();
        insert_row(0, "", 0);
        free(E.filename);
        E.filename = NULL;
        E.lang = E.cx = E.cy = E.dash = 0;
        set_msg("New buffer - Ctrl-S to name and save it");
        break;
    case 2: open_path("Create file: ", 2); break;
    case 3:
        for (int k = 0; k < E.nrecent; k++) free(E.recent[k]);
        E.nrecent = recent_read(E.recent, RECENT_MAX);
        E.rsel = 0;
        E.page = 1;
        break;
    case 4: E.page = 2; break;
    case 5: E.nums = !E.nums; break;
    case 6: exit(0);
    }
}

static void dash_key(int c) {
    if (E.page == 2) { E.page = 0; return; }
    if (E.page == 1) {
        if (c == ARROW_UP && E.nrecent) E.rsel = (E.rsel + E.nrecent - 1) % E.nrecent;
        else if (c == ARROW_DOWN && E.nrecent) E.rsel = (E.rsel + 1) % E.nrecent;
        else if (c == ESC || c == ARROW_LEFT || c == BACKSPACE || c == 'q') E.page = 0;
        else if (c == '\r' && E.nrecent) open_recent(E.rsel);
        else if (c >= '1' && c <= '8' && c - '1' < E.nrecent) open_recent(c - '1');
        return;
    }
    int col = E.sel / 4, row = E.sel % 4, sz = col ? NITEMS - 4 : 4;
    if (c == ARROW_UP) E.sel = E.twocol ? col * 4 + (row + sz - 1) % sz : (E.sel + NITEMS - 1) % NITEMS;
    else if (c == ARROW_DOWN) E.sel = E.twocol ? col * 4 + (row + 1) % sz : (E.sel + 1) % NITEMS;
    else if ((c == ARROW_LEFT || c == ARROW_RIGHT) && E.twocol) {
        int nsz = col ? 4 : NITEMS - 4;
        E.sel = (1 - col) * 4 + (row < nsz ? row : nsz - 1);
    } else if (c == '\r') dash_activate(E.sel);
    else if (c == CTRL_('q')) exit(0);
    else if (c > 32 && c < 127)
        for (int i = 0; i < NITEMS; i++)
            if (items[i].key == tolower(c)) { E.sel = i; dash_activate(i); return; }
}

static void newline(void) {
    Row *r = &E.row[E.cy];
    int ind = 0;
    while (ind < E.cx && (r->s[ind] == ' ' || r->s[ind] == '\t')) ind++;
    char *sp = xrealloc(NULL, ind + 1);
    memcpy(sp, r->s, ind);
    do_op(SPLIT, E.cy, E.cx, 0);
    for (int k = 0; k < ind; k++) do_op(INS, E.cy, k, sp[k]);
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

static void cut_line(void) {
    int y = E.cy;
    for (int x = E.row[y].len; x > 0; x--) do_op(DEL, y, x - 1, E.row[y].s[x - 1]);
    if (y + 1 < E.nrows) do_op(JOIN, y, 0, 0);
    else if (y > 0) do_op(JOIN, y - 1, E.row[y - 1].len, 0);
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

static void process_key(int c) {
    static int quit_pending = 0;
    int was_pending = quit_pending, vertical = 0, typed = 0;
    quit_pending = 0;

    switch (c) {
    case '\r': E.group++; newline(); break;
    case CTRL_('q'):
        if (is_dirty() && !was_pending) {
            set_msg("Unsaved changes! Ctrl-Q again to discard and quit, Ctrl-S to save.");
            quit_pending = 1;
            return;
        }
        exit(0);
    case CTRL_('s'): save_file(); break;
    case CTRL_('z'): undo(); break;
    case CTRL_('y'): redo(); break;
    case CTRL_('f'): find(); break;
    case CTRL_('r'): replace_all(); break;
    case CTRL_('g'): goto_line(); break;
    case CTRL_('k'): E.group++; cut_line(); break;
    case CTRL_('n'): E.nums = !E.nums; break;
    case CTRL_('c'): set_msg("Ctrl-Q quits avtty"); break;
    case BACKSPACE: case CTRL_('h'): E.group++; backspace(); break;
    case DEL_KEY: E.group++; delete_forward(); break;
    case ARROW_LEFT:
        if (E.cx > 0) { do E.cx--; while (E.cx > 0 && CONT(E.row[E.cy].s[E.cx])); }
        else if (E.cy > 0) { E.cy--; E.cx = E.row[E.cy].len; }
        break;
    case ARROW_RIGHT:
        if (E.cx < E.row[E.cy].len) { do E.cx++; while (E.cx < E.row[E.cy].len && CONT(E.row[E.cy].s[E.cx])); }
        else if (E.cy + 1 < E.nrows) { E.cy++; E.cx = 0; }
        break;
    case ARROW_UP: if (E.cy > 0) E.cy--; vertical = 1; break;
    case ARROW_DOWN: if (E.cy + 1 < E.nrows) E.cy++; vertical = 1; break;
    case PAGE_UP: E.cy = E.cy < E.rows ? 0 : E.cy - E.rows; vertical = 1; break;
    case PAGE_DOWN: E.cy = E.cy + E.rows >= E.nrows ? E.nrows - 1 : E.cy + E.rows; vertical = 1; break;
    case HOME_KEY: case CTRL_('a'): E.cx = 0; break;
    case END_KEY: case CTRL_('e'): E.cx = E.row[E.cy].len; break;
    case WORD_LEFT: word_move(-1); break;
    case WORD_RIGHT: word_move(1); break;
    default:
        if (c == '\t' || (c >= 32 && c < 256 && c != 127)) {
            if (!last_typing) E.group++;
            do_op(INS, E.cy, E.cx, (char)c);
            last_typing = !(c == ' ' || c == '\t');
            typed = 1;
        }
    }
    if (!typed) last_typing = 0;
    if (vertical) E.cx = rx_to_cx(&E.row[E.cy], E.want);
    else E.want = cx_to_rx(&E.row[E.cy], E.cx);
}

static void usage(void) {
    puts("avtty " VERSION " - a very tiny text yard\n\n"
         "  avtty             start screen\n"
         "  avtty <file>      open <file> (new buffer if it doesn't exist)\n"
         "  avtty -o <file>   open an existing file\n"
         "  avtty -c <file>   create a new file\n"
         "  avtty -n          start with line numbers on\n"
         "  avtty -h | -v     help | version");
}

int main(int argc, char **argv) {
    const char *file = NULL;
    int mode = 0;
    char err[256];

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h")) { usage(); return 0; }
        if (!strcmp(a, "-v")) { puts("avtty " VERSION); return 0; }
        if (!strcmp(a, "-n")) { E.nums = 1; continue; }
        if ((!strcmp(a, "-c") || !strcmp(a, "-o")) && i + 1 < argc && !file) {
            mode = a[1] == 'c' ? 2 : 1;
            file = argv[++i];
        } else if (a[0] != '-' && !file) file = a;
        else { fprintf(stderr, "avtty: bad argument '%s' (try -h)\n", a); return 1; }
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("avtty: needs an interactive terminal\n", stderr);
        return 1;
    }
    E.promptcol = -1;
    if (!file) E.dash = 1;
    else if (open_file_cmd(file, mode, err, sizeof err)) { fprintf(stderr, "avtty: %s\n", err); return 1; }

    enable_raw();
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
