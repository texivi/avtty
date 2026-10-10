#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define VERSION "0.12.0"
#define TABW 4
#define CTRL_(k) ((k) & 0x1f)
#define ESC 27
#define ALT(c) (2000 + (c))
#define CONT(b) ((((unsigned char)(b)) & 0xC0) == 0x80)
#define MAXREC 8

#define SGR(s) "\x1b[" s "m"
#define RESET SGR("0")
/* every colour below is one of the terminal's own 16 ANSI colours, so the user's theme drives it;
   the few backgrounds that need to differ subtly from the page are derived from the theme at startup */
#define GRAY SGR("90")
#define TXT SGR("39")
#define TITLE SGR("1;36")
#define MATCH SGR("30;43")
#define MATCHC SGR("1;30;103")
#define GUT SGR("90")
#define CHIP_EDIT SGR("1;30;46")
#define CHIP_SEL SGR("1;30;45")
#define CURNUM SGR("1;39")
#define KEYC SGR("1;36")
#define MSGC SGR("33")
#define GUIDEC SGR("90")
#define BRK SGR("1;7")
#define SBT SGR("34")
#define SBK SGR("90")
#define ICONC SGR("36")
#define LABC SGR("39")
#define KEYO SGR("33")
#define HINTC SGR("94")
static struct { char sel[64], selbg[48], curbg[48], rowbg[48], seldim[64], selkey[64], barc[64], barn[64], bard[64], barr[64]; } th;
#define SEL th.sel
#define SELBG th.selbg
#define CURBG th.curbg
#define SELDIM th.seldim
#define BARC th.barc
#define BARN th.barn
#define BARD th.bard
#define BARR th.barr
#define NKEYS 6
#define NELEM(a) ((int)(sizeof a / sizeof *a))

enum Key {
    BACKSPACE = 127,
    ARROW_LEFT = 1000, ARROW_RIGHT, ARROW_UP, ARROW_DOWN,
    WORD_LEFT, WORD_RIGHT, DEL_KEY, HOME_KEY, END_KEY, PAGE_UP, PAGE_DOWN,
    BACKTAB, TOP_KEY, BOT_KEY, MOVE_UP, MOVE_DOWN, PASTE_ON, PASTE_OFF, PARA_UP, PARA_DOWN, F1_KEY, MOUSE_KEY
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
    char msg[160]; long long msgtime; int sticky;
    Op *ops; int nops, capops, cur, saved, group;
    int dash, page, sel, rsel, twocol;
    int ind, pasting, hlon, hl_from, guides, crlf, nonl, kpage, knp;
    char *recent[MAXREC]; int rline[MAXREC]; int nrecent;
    struct termios orig; int raw;
} E;

static int last_typing, prompt_empty_ok, prompt_paths, find_x, find_y;
static const char *prompt_note = "", *live_q;
static volatile sig_atomic_t got_sig;
static void rescue(void);
static void (*overlay)(struct abuf *);
static int ov_r, ov_c, mb_y = -1, mb_x = -1, use_icons = 1, use_mouse = 1, mx_, my_, greet = 1, mas_vis, mon;

static const struct { const char *label; char key; const char *hint; } items[8] = {
    { "Open File", 'o', "open an existing file - tab to complete" },
    { "Find File", 'f', "fuzzy-find any file in this folder (Ctrl-P)" },
    { "Find Text", 't', "search inside any file (Alt-F)" },
    { "Create File", 'c', "start a brand new file right now" },
    { "Scratch Buffer", 's', "a blank page - nothing is saved until you say so" },
    { "Recent Files", 'r', "jump back in right where you left off" },
    { "Keys & Help", 'h', "every shortcut, neatly grouped (or just ask me)" },
    { "Quit", 'q', "see you next time!" }
};

struct kd { const char *k, *d; };

static const struct kd bar[] = {
    { "^S", "save" }, { "^Q", "quit" }, { "^F", "find" }, { "^P", "files" }, { "^Z", "undo" },
    { "^R", "replace" }, { "^G", "goto" }, { "^K", "cut" }, { "^U", "paste" }
};

static const struct kd sbar[] = {
    { "^C", "copy" }, { "^K", "cut" }, { "Tab", "indent" }, { "^/", "comment" }, { "Esc", "cancel" }
};

static const struct { const char *title; int n; struct kd i[8]; } keys[NKEYS] = {
    { "File", 5, { { "Ctrl-S", "save" }, { "Ctrl-O / P", "open / find" }, { "Alt-F", "find text" },
                   { "Ctrl-T", "start screen" }, { "Ctrl-Q", "quit" } } },
    { "Search", 4, { { "Ctrl-F", "find" }, { "Alt-N / P", "next / prev" }, { "Alt-S", "this word" }, { "Ctrl-R", "replace y/n/a" } } },
    { "Move", 6, { { "Ctrl-G", "go to line" }, { "Ctrl-Home/End", "top / bottom" }, { "Ctrl-←/→", "word jump" },
                   { "Ctrl-↑/↓", "paragraph" }, { "PgUp / PgDn", "page" }, { "Ctrl-B", "match bracket" } } },
    { "View", 3, { { "Ctrl-N", "line numbers" }, { "Alt-W", "soft wrap" }, { "Alt-I", "indent guides" } } },
    { "Select", 4, { { "Ctrl-V", "select mode" }, { "Alt-A", "select all" }, { "Alt-L", "select line" },
                     { "Ctrl-C/K/U", "copy/cut/paste" } } },
    { "Edit", 8, { { "Ctrl-Z / Y", "undo / redo" }, { "Ctrl-D", "dup line" }, { "Alt-↑/↓", "move line" }, { "Alt-Enter", "line below" },
                   { "Ctrl-W/Alt-D", "del word ←/→" }, { "Ctrl-/", "comment" }, { "Tab / S-Tab", "indent/dedent" }, { "Alt-T", "trim spaces" } } }
};

static void out(const char *s) { (void)!write(STDOUT_FILENO, s, strlen(s)); }

static void disable_raw(void) {
    if (!E.raw) return;
    E.raw = 0;
    out(RESET "\x1b[?1000l\x1b[?1006l\x1b[?2004l\x1b[?25h\x1b[?1049l");
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
    out("\x1b[?1049h\x1b[?2004h");
}

static int parse_rgb(const char *p, int *c) {
    for (int i = 0; i < 3; i++) {
        int v = 0, n = 0;
        for (; isxdigit((unsigned char)*p); p++, n++) v = v * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (*p | 32) - 'a' + 10);
        if (!n || n > 4) return 0;
        c[i] = n == 1 ? v * 17 : n == 2 ? v : n == 3 ? v >> 4 : v >> 8;
        if (i < 2 && *p++ != '/') return 0;
    }
    return 1;
}

static int mixc(int a, int b, int t) { return a + (b - a) * t / 100; }

static void bgsgr(char *o, size_t n, int r, int g, int b) {
    const char *ct = getenv("COLORTERM");
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) snprintf(o, n, "\x1b[48;2;%d;%d;%dm", r, g, b);
    else if (mx - mn < 24) {
        int k = ((r + g + b) / 3 - 8) / 10;
        snprintf(o, n, "\x1b[48;5;%dm", 232 + (k < 0 ? 0 : k > 23 ? 23 : k));
    } else snprintf(o, n, "\x1b[48;5;%dm", 16 + 36 * ((r * 5 + 127) / 255) + 6 * ((g * 5 + 127) / 255) + (b * 5 + 127) / 255);
}

/* ask the terminal for its background/foreground (OSC 11/10, DA1 as end marker) and derive the few shades we need */
static void theme_init(void) {
    char buf[512], bs[40], sb[40], cb[40], bb[40], *p;
    int n = 0, bg[3], fg[3], hb = 0, hf = 0, tries = 0, dk;
    const char *tm = getenv("TERM"), *fgs;
    if (tm && (!strcmp(tm, "linux") || !strcmp(tm, "dumb"))) goto fallback;
    out("\x1b]11;?\x1b\\\x1b]10;?\x1b\\\x1b[c");
    while (n < (int)sizeof buf - 1 && tries < 6) {
        struct pollfd pf = { STDIN_FILENO, POLLIN, 0 };
        ssize_t k;
        if (poll(&pf, 1, 120) <= 0) { tries++; continue; }
        if ((k = read(STDIN_FILENO, buf + n, sizeof buf - 1 - n)) <= 0) break;
        n += (int)k;
        buf[n] = 0;
        if ((p = strstr(buf, "\x1b[?")) && strchr(p, 'c')) break;
    }
    buf[n] = 0;
    if ((p = strstr(buf, "]11;rgb:"))) hb = parse_rgb(p + 8, bg);
    if ((p = strstr(buf, "]10;rgb:"))) hf = parse_rgb(p + 8, fg);
    if (!hb) goto fallback;
    dk = (bg[0] * 3 + bg[1] * 6 + bg[2]) / 10 < 128;
    if (!hf) for (int i = 0; i < 3; i++) fg[i] = dk ? 215 : 45;
    {
        int acc[3] = { 70, 110, 200 };
        char t[40];
        bgsgr(cb, sizeof cb, mixc(bg[0], fg[0], 7), mixc(bg[1], fg[1], 7), mixc(bg[2], fg[2], 7));
        bgsgr(bs, sizeof bs, mixc(bg[0], fg[0], 15), mixc(bg[1], fg[1], 15), mixc(bg[2], fg[2], 15));
        bgsgr(bb, sizeof bb, mixc(bg[0], fg[0], 13), mixc(bg[1], fg[1], 13), mixc(bg[2], fg[2], 13));
        bgsgr(sb, sizeof sb, mixc(bg[0], acc[0], 32), mixc(bg[1], acc[1], 32), mixc(bg[2], acc[2], 32));
        (void)t;
    }
    fgs = "90";
    goto build;
fallback:
    snprintf(cb, sizeof cb, "%s", "");
    snprintf(bs, sizeof bs, "\x1b[100m");
    snprintf(bb, sizeof bb, "\x1b[100m");
    snprintf(sb, sizeof sb, "\x1b[44m");
    fgs = "39";
build:
    snprintf(th.sel, sizeof th.sel, "%s" SGR("1;36"), bs);
    snprintf(th.selbg, sizeof th.selbg, "%s", sb);
    snprintf(th.curbg, sizeof th.curbg, "%s", cb);
    snprintf(th.rowbg, sizeof th.rowbg, "%s", bs);
    snprintf(th.seldim, sizeof th.seldim, "%s\x1b[%sm", bs, fgs);
    snprintf(th.selkey, sizeof th.selkey, "%s" SGR("1;33"), bs);
    snprintf(th.barc, sizeof th.barc, "%s" SGR("39"), bb);
    snprintf(th.barn, sizeof th.barn, "%s" SGR("1;39"), bb);
    snprintf(th.bard, sizeof th.bard, "%s" SGR("33"), bb);
    snprintf(th.barr, sizeof th.barr, "%s\x1b[%sm", bb, fgs);
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
        { "[200~", PASTE_ON }, { "[201~", PASTE_OFF }, { "[1;5A", PARA_UP }, { "[1;5B", PARA_DOWN }, { "[Z", BACKTAB }, { "[1;5H", TOP_KEY }, { "[1;5F", BOT_KEY }, { "[1;3A", MOVE_UP }, { "[1;3B", MOVE_DOWN },
        { "OP", F1_KEY }, { "[11~", F1_KEY }
    };
    char c, q[24] = "";
    if (got_sig) rescue();
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) {
        if (got_sig || (n < 0 && errno == EIO)) rescue();
        if (n < 0 && errno != EAGAIN && errno != EINTR) die("read");
        return 0;
    }
    if (c != ESC) return (unsigned char)c;
    if (read(STDIN_FILENO, q, 1) != 1) return ESC;
    if (q[0] == ']') {
        char p = 0;
        while (read(STDIN_FILENO, &c, 1) == 1 && c != 7 && !(p == ESC && c == '\\')) p = c;
        return 0;
    }
    if (q[0] != '[' && q[0] != 'O') return ALT((unsigned char)q[0]);
    for (int i = 1; i < 22 && read(STDIN_FILENO, q + i, 1) == 1; i++)
        if (isalpha((unsigned char)q[i]) || q[i] == '~') break;
    if (q[0] == '[' && q[1] == '<') {
        int b, x, y;
        char f;
        if (sscanf(q + 2, "%d;%d;%d%c", &b, &x, &y, &f) != 4) return 0;
        if (f == 'M' && b == 0) { mx_ = x; my_ = y; return MOUSE_KEY; }
        if (f == 'M' && b == 64) return ARROW_UP;
        if (f == 'M' && b == 65) return ARROW_DOWN;
        return 0;
    }
    for (size_t i = 0; i < sizeof esc / sizeof *esc; i++)
        if (!strcmp(q, esc[i].s)) return esc[i].k;
    return q[1] == '?' || q[1] == '>' ? 0 : ESC;
}

/* block until a key arrives; returns 0 only if the terminal was resized, so callers redraw on demand, not on a timer */
static int wait_key(void) {
    int rs = E.srows, cs = E.scols, c;
    while (!(c = read_key())) {
        get_size();
        if (rs != E.srows || cs != E.scols) return 0;
    }
    return c;
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
    E.hl_from = E.crlf = E.nonl = 0;
}

static void apply(OpType t, int y, int x, char c) {
    Row *r = &E.row[y];
    if (y < E.hl_from) E.hl_from = y;
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
    E.hlon = 0;
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

static int toast_ms = 1000;   /* how long avtta / messages stay up; AVTTY_TOAST_MS overrides */
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.msg, sizeof E.msg, fmt, ap);
    va_end(ap);
    /* errors and anything that waits on you stay until your next key; plain confirmations ("saved") vanish fast */
    E.sticky = strcasestr(E.msg, "unsaved") || strcasestr(E.msg, "couldn't") || strcasestr(E.msg, "can't") ||
               strcasestr(E.msg, "bigger window") || strcasestr(E.msg, "replace?") || strcasestr(E.msg, "save first");
    E.msgtime = now_ms() + (E.sticky ? 3600000 : 0);
}

static void undo(void) {
    if (!E.cur) { set_msg("nothing to undo yet."); return; }
    int g = E.ops[E.cur - 1].group;
    while (E.cur > 0 && E.ops[E.cur - 1].group == g) {
        Op *o = &E.ops[--E.cur];
        apply(o->t ^ 1, o->y, o->x, o->c);
    }
}

static void redo(void) {
    if (E.cur == E.nops) { set_msg("nothing to redo."); return; }
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
    RESET, SGR("3;90"), SGR("35"), SGR("32"), SGR("91"), SGR("31"), SGR("33"), SGR("34"), SGR("36")
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
    E.hl_from = 0;
    if (!f) return;
    b = b ? b + 1 : f;
    e = e && e > b ? e + 1 : b;
    if (has(clike, e, strlen(e))) E.lang = 1;
    else if (has(hashy, e, strlen(e)) || has(hashy, b, strlen(b))) E.lang = 2;
}

static void infer_indent(void) {
    int tabs = 0, sp = 0, w = 0;
    for (int i = 0; i < E.nrows && i < 300; i++) {
        const char *s = E.row[i].s;
        int n = 0;
        if (s[0] == '\t') tabs++;
        else {
            while (s[n] == ' ') n++;
            if (n >= 2 && n < E.row[i].len) { sp++; if (!w || n < w) w = n; }
        }
    }
    if (tabs > sp) E.ind = 0;
    else if (sp) E.ind = w > 8 ? 4 : w;
    else {
        const char *f = E.filename;
        size_t l = f ? strlen(f) : 0;
        E.ind = E.lang == 2 ? 4 : 0;
        if (f && (strstr(f, "akefile") || (l > 3 && !strcmp(f + l - 3, ".mk")))) E.ind = 0;
    }
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

static const char *hq(void) {
    if (live_q && *live_q) return live_q;
    return E.hlon && E.lastq && *E.lastq ? E.lastq : NULL;
}

static const char *ffind(const char *h, const char *q) {
    for (const char *c = q; *c; c++)
        if (isupper((unsigned char)*c)) return strstr(h, q);
    return strcasestr(h, q);
}

static void nextm(const char *s, const char *q, int from, int *ms, int *me) {
    const char *p = ffind(s + from, q);
    *ms = p ? (int)(p - s) : -1;
    *me = p ? *ms + (int)strlen(q) : -1;
}

static int row_lead(int y) {
    const Row *r = &E.row[y];
    int i = 0;
    while (i < r->len && (r->s[i] == ' ' || r->s[i] == '\t')) i++;
    return i == r->len ? -1 : cx_to_rx(r, i);
}

/* columns of indent guides to draw on row y; blank rows borrow from the nearest code above and below */
static int guide_w(int y) {
    int a = row_lead(y), up = -1, dn = -1;
    if (a >= 0) return a;
    for (int k = y - 1; k >= 0 && k >= y - 30 && up < 0; k--) up = row_lead(k);
    for (int k = y + 1; k < E.nrows && k <= y + 30 && dn < 0; k++) dn = row_lead(k);
    return up < 0 || dn < 0 ? 0 : up < dn ? up : dn;
}

static void guide(struct abuf *ab, const char *bg) {
    ab_s(ab, RESET);
    ab_s(ab, bg);
    ab_s(ab, GUIDEC "│" RESET);
    ab_s(ab, bg);
}

static void draw_row(struct abuf *ab, int y, int lo, int width, const char *cls, const char *bg, int gd) {
    const Row *r = &E.row[y];
    const char *q = hq();
    int col = 0, hi = lo + width, cur = 0, curs = 0, curm = 0, curb = 0, drawn = 0, sy, sx, ey, ex, ms = -1, me = -1;
    int has = sel_range(&sy, &sx, &ey, &ex) && y >= sy && y <= ey, lb = 0, step = E.ind ? E.ind : TABW;
    int s0 = has && y == sy ? sx : 0, e0 = has ? (y == ey ? ex : r->len) : 0;
    while (lb < r->len && (r->s[lb] == ' ' || r->s[lb] == '\t')) lb++;
    if (q) nextm(r->s, q, 0, &ms, &me);
    ab_s(ab, RESET);
    ab_s(ab, bg);
    for (int i = 0; i < r->len && col < hi;) {
        unsigned char ch = r->s[i];
        int in = i >= s0 && i < e0, m, bm;
        while (ms >= 0 && i >= me) nextm(r->s, q, me, &ms, &me);
        m = ms >= 0 && i >= ms && i < me ? (y == E.cy && ms == E.cx ? 2 : 1) : 0;
        bm = mb_y >= 0 && ((y == E.cy && i == E.cx) || (y == mb_y && i == mb_x));
        if (col >= lo && (cls[i] != cur || in != curs || m != curm || bm != curb)) {
            ab_s(ab, RESET);
            ab_s(ab, bg);
            if (cls[i]) ab_s(ab, hlc[(int)cls[i]]);
            if (in) ab_s(ab, SELBG);
            if (m) ab_s(ab, m == 2 ? MATCHC : MATCH);
            if (bm) ab_s(ab, BRK);
            cur = cls[i];
            curs = in;
            curm = m;
            curb = bm;
        }
        if (ch == '\t') {
            for (int k = TABW - col % TABW, first = 1; k > 0; k--, col++, first = 0)
                if (col >= lo && col < hi) {
                    if (first && gd > 0 && i < lb && col < gd && col % step == 0 && !in && !m) { guide(ab, bg); cur = curs = curm = curb = -1; }
                    else ab_s(ab, " ");
                    drawn++;
                }
            i++;
        } else if (ch == ' ' && gd > 0 && i < lb && col < gd && col % step == 0 && col >= lo && col < hi && !in && !m) {
            guide(ab, bg);
            cur = curs = curm = curb = -1;
            drawn++; col++; i++;
        } else {
            int l = 1, bad = ch < 32 || ch == 127;
            if (!bad) while (i + l < r->len && CONT(r->s[i + l])) l++;
            if (col >= lo && col < hi) { ab_append(ab, bad ? "?" : r->s + i, bad ? 1 : l); drawn++; }
            col++; i += l;
        }
    }
    if (has && y < ey && col >= lo && col < hi) { ab_s(ab, RESET), ab_s(ab, SELBG), ab_s(ab, " "); drawn++; }
    if (lb == r->len && gd > col) {
        ab_s(ab, RESET);
        ab_s(ab, bg);
        for (; col < gd && col < hi; col++)
            if (col >= lo) { if (col % step == 0) guide(ab, bg); else ab_s(ab, " "); drawn++; }
    }
    ab_s(ab, RESET);
    if (bg[0] && drawn < width) { ab_s(ab, bg); ab_rep(ab, " ", width - drawn); ab_s(ab, RESET); }
}

static void ctr(struct abuf *ab, int r, int w, const char *style, const char *s) {
    int c0 = 1 + (w - utf8w(s)) / 2;
    put(ab, r, c0 < 1 ? 1 : c0, style, s);
    ab_s(ab, RESET);
}

static const char *const icons[8] = { "\uf07c", "\uf002", "\uf0f6", "\uf15b", "\uf044", "\uf1da", "\uf11c", "\uf08b" };

static void item(struct abuf *ab, int r, int c, int i) {
    char t[48];
    int s = i == E.sel;
    const char *bg = s ? th.rowbg : "";
    if (!put(ab, r, c, bg, s ? " ▸ " : "   ")) return;
    ab_s(ab, ICONC);
    ab_s(ab, use_icons ? icons[i] : "•");
    ab_s(ab, " ");
    snprintf(t, sizeof t, "%-17s", items[i].label);
    ab_s(ab, s ? SGR("1;36") : LABC);
    ab_s(ab, t);
    snprintf(t, sizeof t, "%c ", items[i].key);
    ab_s(ab, KEYO);
    ab_s(ab, t);
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
    static const char *const col[5] = { SGR("96"), SGR("94"), SGR("94"), SGR("95"), SGR("35") };
    ab_s(ab, col[r]);
    for (int l = 0; l < 5; l++) {
        for (int p = 0; p < 5; p++) ab_rep(ab, g[l][r][p] == '#' ? "█" : " ", sc);
        if (l < 4) ab_rep(ab, " ", sc);
    }
    ab_s(ab, RESET);
}

static int kmax = 1 << 20;

static void box(struct abuf *ab, int r, int c, int g) {
    int n = keys[g].n;
    if (r <= kmax && put(ab, r, c, GRAY, "┌─ ")) {
        ab_s(ab, TITLE);
        ab_s(ab, keys[g].title);
        ab_s(ab, GRAY " ");
        ab_rep(ab, "─", 27 - utf8w(keys[g].title));
        ab_s(ab, "┐");
    }
    for (int i = 0; i < n; i++)
        if (r + 1 + i <= kmax && put(ab, r + 1 + i, c, GRAY, "│ ")) {
            ab_s(ab, KEYC);
            ab_s(ab, keys[g].i[i].k);
            ab_rep(ab, " ", 14 - utf8w(keys[g].i[i].k));
            ab_s(ab, TXT);
            ab_s(ab, keys[g].i[i].d);
            ab_rep(ab, " ", 14 - utf8w(keys[g].i[i].d));
            ab_s(ab, GRAY " │");
        }
    if (r + 1 + n <= kmax && put(ab, r + 1 + n, c, GRAY, "└")) {
        ab_rep(ab, "─", 30);
        ab_s(ab, "┘");
    }
    ab_s(ab, RESET);
}


/* ---- avtta (a very tiny text assistant): 12x8 pixels drawn with half blocks, colours come from the terminal theme ---- */
#define CY "\001"
#define OR "\002"
enum { P_IDLE, P_WAVE, P_HAPPY, P_CHEER, P_WORRY, P_CONFUSED, P_SEARCH, P_READ, P_THINK, P_IDEA, P_PEN, P_SLEEP, P_LOOK_UP, P_SHY, P_SAD };
#define NPOSE 15
static const char *const spr[NPOSE][10] = {
    { "................", "...LLLLLLLLLL...", "...BBBBBBBBBB...", "...BBEKBBEKBB...", "...BBKKBBKKBB...", "...BBKKBBKKBB...", "...BQBBMMBBQB...", "...BBBBBBBBBB...", "...DDDDDDDDDD...", "....DD....DD...." },
    { "................", "...LLLLLLLLLL...", "...BBBBBBBBBB.BB", "...BBEKBBEKBB.B.", "...BBKKBBKKBBB..", "...BBKKBBKKBBB..", "..BBQBBMMBBQB...", "...BBBBBBBBBB...", "...DDDDDDDDDD...", "....DD....DD...." },
    { "................", "...LLLLLLLLLL...", "...BBBBBBBBBB...", "...BBBBBBBBBB...", "...BBKKBBKKBB...", "...BKBBKKBBKB...", "...BQBMMMMBQB...", "...BQBBMMBBQB...", "...DDDDDDDDDD...", "....DD....DD...." },
    { ".Y......Y.....Y.", "...LLLLLLLLLL...", "BB.BBBBBBBBBB.BB", ".B.BBBBBBBBBB.B.", "..BBBKKBBKKBBB..", "..BBKBBKKBBKBB..", "...BQBMMMMBQB...", "...BQBBMMBBQB...", "...DDDDDDDDDD...", "....DD....DD...." },
    { "................", "...LLLLLLLLLL...", "...BBBKBBKBBB...", "...BBEKBBEKBBC..", "...BBKKBBKKBBC..", "...BBKKBBKKBB...", "...BQBMBMBBQB...", "...BBBBMBMBBB...", "...DDDDDDDDDD...", "....DD....DD...." },
    { ".............CC.", "...LLLLLLLLLL.C.", "...BBBBBBBBBBC..", "...BBEKBBBBBB...", "...BBKKBBKKBBC..", "...BBKKBBKKBB...", "...BQBBBBBBQB...", "...BBBBMMMBBB...", "...DDDDDDDDDD...", "....DD....DD...." },
    { "................", "...LLLLLLLLLL...", "...BBBBBBBBBB...", "...BBBEKBBEKBCCC", "...BBBKKBBKKBCWC", "...BBBKKBBKKBCCC", "...BQBBMMBBQBD..", "...BBBBBBBBBB...", "...DDDDDDDDDD...", "....DD....DD...." },
    { "................", "...LLLLLLLLLL...", "...BBBBBBBBBB...", "...BBEKBBEKBB...", "...BBKKBBKKBB...", "...BBKKBBKKBB...", "..BBQWWWWWWQBB..", "...BBWCCWCWBB...", "...DDWCWCCWDD...", "....DD....DD...." },
    { ".............WWW", "...LLLLLLLLLLWWW", "...BBBEKBBEKB.W.", "...BBBKKBBKKBW..", "...BBBKKBBKKB...", "...BBBBBBBBBB...", "...BQBBBBBBQB...", "...BBBBMMMBBB...", "...DDDDDDDDDD...", "....DD....DD...." },
    { "..............Y.", "...LLLLLLLLLLYYY", "...BBEKBBEKBBYYY", "...BBKKBBKKBB.S.", "...BBKKBBKKBB...", "...BBBBBBBBBB...", "...BQBBMMBBQB...", "...BBBBBBBBBB...", "...DDDDDDDDDD...", "....DD....DD...." },
    { "................", "...LLLLLLLLLL...", "...BBBBBBBBBB.P.", "...BBBEKBBEKB.Y.", "...BBBKKBBKKB.Y.", "...BBBKKBBKKB.Y.", "...BQBBMMBBQBBT.", "...BBBBBBBBBB.G.", "...DDDDDDDDDD...", "....DD....DD...." },
    { "...............C", "...LLLLLLLLLL...", "...BBBBBBBBBBCC.", "...BBBBBBBBBB.C.", "...BBBBBBBBBBCC.", "...BKKKKKKKKB...", "...BQBBBMBBQB...", "...BBBBBBBBBB...", "...DDDDDDDDDD...", "....DD....DD...." },
    { "................", "...LLLLLLLLLL...", "...BBEKBBEKBB...", "...BBKKBBKKBB...", "...BBKKBBKKBB...", "...BBBBBBBBBB...", "...BQBBMMBBQB...", "...BBBBBBBBBB...", "...DDDDDDDDDD...", "....DD....DD...." },
    { "................", "...LLLLLLLLLL...", "...BBBBBBBBBB...", "...BBBBBBBBBB...", "...BEKBBEKBBB...", "...BKKBBKKBBB...", "...BQKBBMKBQB...", "...BQBBBBBBQB...", "...DDDDDDDDDD...", "....DD....DD...." },
    { "................", "...LLLLLLLLLL...", "...BBKBBBBKBB...", "...BBEKBBEKBB...", "...BBKKBBKKBB...", "...BBKKBBKKBB...", "...BBCBMMBBBB...", "...BBBMBBMBBB...", "...DDDDDDDDDD...", "....DD....DD...." },
};

/* body uses the terminal's own blues so it follows the theme; face and props use fixed 256-colour codes (1000+n),
   because themes remap ANSI black/white and the eyes would vanish */
static int pcode(char c) {
    switch (c) {
    case 'B': return 94; case 'D': return 34; case 'L': return 96;
    case 'K': return 1233; case 'E': return 1231; case 'M': return 1203; case 'Q': return 1217;
    case 'C': return 36; case 'W': return 1231; case 'Y': return 93; case 'P': return 91;
    case 'S': return 1250; case 'T': return 1223; case 'G': return 1238;
    }
    return 0;
}

static void pc(char *o, int code, int bg) {
    if (code >= 1000) snprintf(o, 24, "%d;5;%d", bg ? 48 : 38, code - 1000);
    else snprintf(o, 24, "%d", code + (bg ? 10 : 0));
}

static void mascot_draw(struct abuf *ab, int r, int c, int pose) {
    char t[80], fa[24], fb[24], bb[24];
    for (int k = 0; k < 5; k++) {
        at(ab, r + k, 1);
        ab_s(ab, RESET);
        ab_rep(ab, " ", c - 1);
        for (int x = 0; x < 16; x++) {
            int a = pcode(spr[pose][2 * k][x]), b = pcode(spr[pose][2 * k + 1][x]);
            pc(fa, a, 0); pc(fb, b, 0); pc(bb, b, 1);
            if (!a && !b) ab_s(ab, RESET " ");
            else if (a == b) { snprintf(t, sizeof t, "\x1b[0;%sm█", fa); ab_s(ab, t); }
            else if (!b) { snprintf(t, sizeof t, "\x1b[0;%sm▀", fa); ab_s(ab, t); }
            else if (!a) { snprintf(t, sizeof t, "\x1b[0;%sm▄", fb); ab_s(ab, t); }
            else { snprintf(t, sizeof t, "\x1b[0;%s;%sm▀", fa, bb); ab_s(ab, t); }
        }
        ab_s(ab, RESET " ");
    }
}

/* one bubble line; \001 toggles cyan, \002 toggles orange */
static void bline(struct abuf *ab, const char *s, int w) {
    int col = 0, st = 0;
    ab_s(ab, TXT);
    for (; *s && col < w; ) {
        if (*s == 1 || *s == 2) {
            st = st == *s ? 0 : *s;
            ab_s(ab, st == 1 ? KEYC : st == 2 ? KEYO : TXT);
            s++;
            continue;
        }
        int l = 1;
        while (CONT(s[l])) l++;
        ab_append(ab, s, l);
        s += l;
        col++;
    }
    ab_s(ab, RESET);
    if (col < w) ab_rep(ab, " ", w - col);
}

static int vw(const char *s) {
    int w = 0;
    for (; *s; s++) if (!CONT(*s) && *s != 1 && *s != 2) w++;
    return w;
}

static int panel_maxl = 4;

static int wrap_into(const char *s, int w, char out[4][220]) {
    int nl = 0;
    const char *p = s;
    while (*p && nl < panel_maxl) {
        while (*p && *p != '\n' && nl < panel_maxl) {
            int col = 0;
            const char *q = p, *sp = NULL, *end;
            while (*q && *q != '\n') {
                if (*q == 1 || *q == 2) { q++; continue; }
                int l = 1;
                while (CONT(q[l])) l++;
                if (col >= w) break;
                if (*q == ' ') sp = q;
                col++;
                q += l;
            }
            end = (*q && *q != '\n' && sp) ? sp : q;
            if (end == p) end = q > p ? q : p + 1;
            int len = (int)(end - p) < 219 ? (int)(end - p) : 219;
            memcpy(out[nl], p, len);
            out[nl++][len] = 0;
            p = end;
            while (*p == ' ') p++;
        }
        if (*p == '\n') p++;
    }
    return nl;
}

/* avtta bottom-left, speech bubble to its right; base is the last row it uses. full = bubble spans the screen */
static void panel(struct abuf *ab, int base, int pose, const char *text, int full) {
    char ln[4][220];
    int bx = 21, mw = E.scols - bx, nl = wrap_into(text, mw - 4, ln), bw = 16, top;
    if (nl < 1) { nl = 1; ln[0][0] = 0; }
    if (full) bw = mw;
    else {
        for (int i = 0; i < nl; i++) if (vw(ln[i]) + 4 > bw) bw = vw(ln[i]) + 4;
        if (bw > mw) bw = mw;
    }
    mascot_draw(ab, base - 4, 3, pose);
    top = base - nl - 1;
    put(ab, top, bx, GRAY, "┌");
    ab_rep(ab, "─", bw - 2);
    ab_s(ab, "┐");
    for (int i = 0; i < nl; i++) {
        put(ab, top + 1 + i, bx, GRAY, "│ ");
        bline(ab, ln[i], bw - 4);
        ab_s(ab, GRAY " │" RESET);
    }
    put(ab, base, bx, GRAY, "└");
    ab_rep(ab, "─", bw - 2);
    ab_s(ab, "┘");
    put(ab, base - 1, bx - 1, GRAY, "◀");
    ab_s(ab, RESET);
}

/* which pose fits what avtta is saying */
static int msg_pose(const char *m) {
    if (strcasestr(m, "couldn't save")) return P_SAD;
    if (strcasestr(m, "find") && (strcasestr(m, "couldn't") || strcasestr(m, "can't") || strcasestr(m, "don't"))) return P_CONFUSED;
    if (strcasestr(m, "unsaved") || strcasestr(m, "couldn't") || strcasestr(m, "can't") || strcasestr(m, "bigger")) return P_WORRY;
    if (strcasestr(m, "nothing") || strcasestr(m, "don't") || strcasestr(m, "isn't") || strcasestr(m, "there's no")) return P_CONFUSED;
    if (strcasestr(m, "saved")) return P_CHEER;
    if (strcasestr(m, "searching")) return P_SEARCH;
    if (strcasestr(m, "scratch") || strcasestr(m, "stdin") || strcasestr(m, "new file")) return P_PEN;
    if (strcasestr(m, "select mode")) return P_LOOK_UP;
    if (strcasestr(m, "cancel") || strcasestr(m, "not saving")) return P_SHY;
    if (strcasestr(m, "opened") || strcasestr(m, "copied") || strcasestr(m, "cut ") || strcasestr(m, "replaced") ||
        strcasestr(m, "trimmed") || strcasestr(m, "jumped") || strcasestr(m, "tidy")) return P_HAPPY;
    return P_IDLE;
}

static const signed char ipose[8] = { P_LOOK_UP, P_SEARCH, P_SEARCH, P_PEN, P_THINK, P_READ, P_READ, P_SLEEP };

/* two fixed columns of boxes; on narrow terminals one column that scrolls */
static const signed char kc2[2][3] = { { 0, 1, 2 }, { 3, 4, 5 } }, kc3[3][3] = { { 0, 3, -1 }, { 1, 2, -1 }, { 4, 5, -1 } };

static void dash_draw(struct abuf *ab) {
    int cols = E.scols, avail = E.srows - 1;
    for (int y = 1; y <= avail; y++) { at(ab, y, 1); ab_s(ab, "\x1b[K"); }
    if (cols < 40 || avail < 8) { put(ab, 1, 1, RESET, "avtty: terminal too small"); return; }

    int sc = cols >= 64 ? 2 : 1, lw = 29 * sc, per = cols >= 62 ? 2 : 1, mr = per == 2 ? 4 : 8, hper = cols >= 68 ? 2 : 1;
    int mas0 = E.page == 0 && cols >= 44 && avail - 6 >= mr + 5, ca = mas0 ? avail - 6 : avail;
    int logo = E.page == 0 && ca >= mr + (mas0 ? 12 : 14), hm = 0, n, r, tall = 0, nc = cols >= 106 ? 3 : 2;
    int ml = E.msg[0] && E.promptcol < 0 && now_ms() - E.msgtime < toast_ms;
    mas_vis = 0;
    for (int c = 0; c < nc; c++) {
        int hh = 0;
        for (int k = 0; k < 3; k++) { int g = nc == 3 ? kc3[c][k] : kc2[c][k]; if (g >= 0) hh += keys[g].n + 2; }
        if (hh > hm) hm = hh;
    }
    for (int g = 0; g < NKEYS; g++) tall += keys[g].n + 2;
    E.twocol = per == 2;
    n = E.page == 0 ? (logo ? 6 : 0) + mr + (mas0 ? 5 : 6) : E.page == 1 ? (E.nrecent ? E.nrecent : 1) + 4 : hper == 2 ? hm + 2 : tall + 2;
    int top = (ca - n) / 2 + 1;
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
            if (per == 2) item(ab, r + k, mx + 28, k + 4);
        }
        {
            int f = r + mr + (mas0 ? 1 : 2);
            if (!mas0) ctr(ab, r + mr + 1, cols, HINTC, items[E.sel].hint);
            ctr(ab, f, cols, GRAY, E.nrows ? "Esc back to file • avtty " VERSION : "↑↓ move • Enter select • avtty " VERSION);
            ctr(ab, f + 1, cols, GRAY, "Part of the Texivi Software Suite");
        }
        if (mas0) {
            char t[300];
            if (ml) snprintf(t, sizeof t, "%s", E.msg);
            else if (greet) snprintf(t, sizeof t, "hey, i'm " OR "avtta" OR " - a very tiny text assistant. pick something above, or ask me about any shortcut!");
            else snprintf(t, sizeof t, "%s\nclick me or press " OR "/" OR " and ask about any shortcut.", items[E.sel].hint);
            panel(ab, avail, ml ? msg_pose(E.msg) : greet ? P_WAVE : ipose[E.sel], t, 1);
            mas_vis = 1;
        }
        return;
    }

    if (E.page == 2) {
        int x = 1 + (cols - (hper == 2 ? nc * 34 - 2 : 32)) / 2;
        int kmas = cols >= 44 && avail >= (hper == 2 ? hm + 7 : 18), lim = kmas ? avail - 6 : avail - 1, mxs;
        char t[300];
        mxs = hper == 2 ? 0 : tall + 2 - lim;
        E.knp = mxs < 0 ? 0 : mxs;
        if (E.kpage > E.knp) E.kpage = E.knp;
        kmax = lim;
        if (hper == 2) {
            int tp = kmas ? (avail - 6 - hm) / 2 + 1 : top;
            if (tp < 1) tp = 1;
            for (int c = 0; c < nc; c++)
                for (int k = 0, y = tp; k < 3; k++) {
                    int g = nc == 3 ? kc3[c][k] : kc2[c][k];
                    if (g < 0) break;
                    box(ab, y, x + c * 34, g);
                    y += keys[g].n + 2;
                }
            if (!kmas) ctr(ab, top + n - 1, cols, GRAY, "press any key to go back • / to ask me");
        } else {
            int y = (mxs > 0 ? 1 : top) - E.kpage;
            for (int g = 0; g < NKEYS; g++) { box(ab, y, x, g); y += keys[g].n + 2; }
            if (!kmas) {
                at(ab, avail, 1);
                ab_s(ab, "\x1b[K");
                ctr(ab, avail, cols, GRAY, E.knp ? "↑↓ scroll • press any key to go back" : "press any key to go back");
            }
        }
        kmax = 1 << 20;
        if (kmas) {
            if (ml) snprintf(t, sizeof t, "%s", E.msg);
            else snprintf(t, sizeof t, "here are all my shortcuts, grouped by what they do. press any key to go back%s\nlooking for one? click me or press " OR "/" OR " and just ask.", E.knp ? " (↑↓ scrolls)" : "");
            panel(ab, avail, ml ? msg_pose(E.msg) : P_READ, t, 1);
            mas_vis = 1;
        }
        return;
    }

    ctr(ab, top, cols, TITLE, "Recent Files");
    r = top + 2;
    if (!E.nrecent) ctr(ab, r, cols, GRAY, "Nothing here yet - open a file first");
    int cw = cols < 60 ? cols - 4 : 56;
    for (int i = 0; i < E.nrecent; i++) {
        char t[PATH_MAX + 8], sfx[16], num[8];
        const char *p = E.recent[i], *hm = getenv("HOME"), *sp = t, *d;
        size_t hl = hm ? strlen(hm) : 0;
        int sel = i == E.rsel, trunc = 0, maxw, w, dl;
        if (hl && !strncmp(p, hm, hl) && (!p[hl] || p[hl] == '/')) snprintf(t, sizeof t, "~%s", p + hl);
        else snprintf(t, sizeof t, "%s", p);
        snprintf(sfx, sizeof sfx, ":%d", E.rline[i] > 0 ? E.rline[i] : 1);
        maxw = cw - 7 - (int)strlen(sfx);
        if (utf8w(t) > maxw) {
            sp = t + strlen(t) - (maxw - 1);
            while (CONT(*sp)) sp++;
            trunc = 1;
        }
        d = strrchr(sp, '/');
        dl = d ? (int)(d - sp) + 1 : 0;
        w = 6 + trunc + utf8w(sp) + (int)strlen(sfx) + 1;
        if (!put(ab, r + i, 1 + (cols - cw) / 2, sel ? SEL : TXT, sel ? " ▸ " : "   ")) continue;
        snprintf(num, sizeof num, "%d ", i + 1);
        ab_s(ab, sel ? th.selkey : KEYO);
        ab_s(ab, num);
        ab_s(ab, sel ? SELDIM : GRAY);
        if (trunc) ab_s(ab, "…");
        ab_append(ab, sp, dl);
        ab_s(ab, sel ? SEL : CURNUM);
        ab_s(ab, sp + dl);
        ab_s(ab, sel ? SEL : RESET);
        ab_rep(ab, " ", cw - w);
        ab_s(ab, sel ? SELDIM : GRAY);
        ab_s(ab, sfx);
        ab_s(ab, sel ? " " RESET : RESET);
    }
    ctr(ab, r + (E.nrecent ? E.nrecent : 1) + 1, cols, GRAY, "Enter or 1-8 open • Esc back");
}

static int find_match(int maxrows, int *my, int *mx) {
    static const char b[] = "([{)]}";
    char ch = E.cx < E.row[E.cy].len ? E.row[E.cy].s[E.cx] : 0, *p = ch ? strchr(b, ch) : NULL;
    if (!p) return 0;
    int y = E.cy, x = E.cx, d = 0, i = (int)(p - b), dir = i < 3 ? 1 : -1;
    char match = b[(i + 3) % 6];
    for (;;) {
        x += dir;
        while (x < 0 || x >= E.row[y].len) {
            y += dir;
            if (y < 0 || y >= E.nrows || abs(y - E.cy) > maxrows) return 0;
            x = dir > 0 ? 0 : E.row[y].len - 1;
        }
        char k = E.row[y].s[x];
        if (k == ch) d++;
        else if (k == match) {
            if (!d) { *my = y; *mx = x; return 1; }
            d--;
        }
    }
}

static void seg(struct abuf *ab, int *w, const char *style, const char *t) {
    ab_s(ab, style);
    ab_s(ab, t);
    *w += utf8w(t);
}

static void status_bar(struct abuf *ab) {
    char right[128], name[PATH_MAX + 8];
    const char *base = E.filename ? E.filename : "[No Name]", *nm = base;
    int w = 0, sc = E.scols, rn, avail, pad, tr = 0;
    int pct = E.nrows > 1 ? E.cy * 100 / (E.nrows - 1) : 100;
    if (sc >= 72) {
        char ind[16];
        if (E.ind) snprintf(ind, sizeof ind, "Spaces:%d", E.ind);
        else snprintf(ind, sizeof ind, "Tabs");
        snprintf(right, sizeof right, "%s%s  %s  Ln %d/%d  Col %d  %d%% ", E.crlf ? "CRLF  " : "",
                 E.lang == 1 ? "C-like" : E.lang == 2 ? "Script" : "Text", ind, E.cy + 1, E.nrows, E.rx + 1, pct);
    } else snprintf(right, sizeof right, "Ln %d, Col %d ", E.cy + 1, E.rx + 1);
    rn = utf8w(right);
    at(ab, E.rows + 1, 1);
    ab_s(ab, RESET); ab_s(ab, BARC);
    seg(ab, &w, E.sel_on ? CHIP_SEL : CHIP_EDIT, E.sel_on ? " SELECT " : " EDIT ");
    seg(ab, &w, BARC, " ");
    avail = sc - w - 3 - rn;
    if (avail < 8) { rn = 0; right[0] = 0; avail = sc - w - 3; }
    if (avail < 1) avail = 1;
    if (utf8w(base) > avail) {
        nm = base + strlen(base) - (avail - 1);
        while (CONT(*nm)) nm++;
        tr = 1;
    }
    snprintf(name, sizeof name, "%s%s", tr ? "…" : "", nm);
    seg(ab, &w, BARN, name);
    if (is_dirty()) seg(ab, &w, BARD, " ●");
    pad = sc - w - rn;
    ab_s(ab, BARC);
    if (pad > 0) ab_rep(ab, " ", pad);
    if (rn) seg(ab, &w, BARR, right);
    ab_s(ab, RESET);
}

static void hint_bar(struct abuf *ab, const struct kd *h, int n) {
    int w = 1;
    ab_s(ab, " ");
    for (int i = 0; i < n; i++) {
        int need = utf8w(h[i].k) + 1 + utf8w(h[i].d) + (i ? 2 : 0);
        if (w + need > E.scols) break;
        if (i) ab_s(ab, "  ");
        ab_s(ab, KEYO);
        ab_s(ab, h[i].k);
        ab_s(ab, RESET GRAY " ");
        ab_s(ab, h[i].d);
        w += need;
    }
    ab_s(ab, RESET);
}

static int *bs;
static int bscap;

/* in the editor avtta only shows up for the big moments: saving, quitting, errors; copy/cut/jump/etc just get a plain line */
static int avtta_worthy(void) {
    return E.sticky || strcasestr(E.msg, "saved") || strcasestr(E.msg, "new file") ||
           strcasestr(E.msg, "scratch") || strcasestr(E.msg, "stdin");
}

static void refresh_screen(void) {
    get_size();
    struct abuf ab = { NULL, 0, 0 };
    ab_s(&ab, "\x1b[?25l" RESET);
    if ((E.dash && use_mouse) != mon) {
        mon = E.dash && use_mouse;
        ab_s(&ab, mon ? "\x1b[?1000h\x1b[?1006h" : "\x1b[?1000l\x1b[?1006l");
    }
    int gw = 0, crow = 0, ccol = 0;

    if (E.dash) dash_draw(&ab);
    else {
        if (E.nums) {
            gw = snprintf(NULL, 0, "%d", E.nrows);
            gw = (gw < 3 ? 3 : gw) + 2;
            if (E.scols <= gw + 8) gw = 0;
        }
        int sbw = E.nrows > E.rows && E.scols > gw + 20, myy, mxx;
        int textw = E.scols - gw - sbw > 0 ? E.scols - gw - sbw : 1, blk = 0;
        mb_y = mb_x = -1;
        if (find_match(300, &myy, &mxx)) { mb_y = myy; mb_x = mxx; }
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
        if (E.lang == 1) {
            /* bs[i] = block-comment state at the start of row i, valid for i <= hl_from */
            int i, b;
            if (E.nrows + 2 > bscap) { bscap = E.nrows * 2 + 16; bs = xrealloc(bs, sizeof *bs * bscap); }
            bs[0] = 0;
            if (E.hl_from > E.nrows) E.hl_from = E.nrows;
            i = E.hl_from < E.rowoff ? E.hl_from : E.rowoff;
            b = bs[i];
            for (; i < E.rowoff && i < E.nrows; i++) {
                if (memchr(E.row[i].s, '/', E.row[i].len)) hl(&E.row[i], &b);
                bs[i + 1] = b;
            }
            blk = b;
            if (E.rowoff > E.hl_from) E.hl_from = E.rowoff;
        }

        for (int y = 0, fr = E.rowoff; y < E.rows; fr++) {
            if (fr >= E.nrows) { at(&ab, ++y, 1); ab_s(&ab, GUT "~" RESET "\x1b[K"); continue; }
            char *cls = hl(&E.row[fr], &blk);
            int h = E.wrap ? rowh(fr, textw) : 1;
            const char *bg = fr == E.cy && !E.sel_on ? CURBG : "";
            for (int k = 0; k < h && y < E.rows; k++) {
                at(&ab, ++y, 1);
                if (gw) {
                    char num[96];
                    if (k) snprintf(num, sizeof num, "%*s", gw, "");
                    else snprintf(num, sizeof num, "%s%*d  " RESET, fr == E.cy ? CURNUM : GUT, gw - 2, fr + 1);
                    ab_s(&ab, num);
                }
                draw_row(&ab, fr, E.wrap ? k * textw : E.coloff, textw, cls, bg, E.guides ? guide_w(fr) : 0);
                ab_s(&ab, "\x1b[K");
            }
        }
        if (sbw) {
            int th = E.rows * E.rows / E.nrows, range = E.nrows - E.rows, top;
            if (th < 1) th = 1;
            top = range > 0 ? (int)((long)E.rowoff * (E.rows - th) / range) : 0;
            if (top < 0) top = 0;
            if (top > E.rows - th) top = E.rows - th;
            for (int k = 0; k < E.rows; k++) {
                at(&ab, k + 1, E.scols);
                ab_s(&ab, k >= top && k < top + th ? SBT "█" RESET : SBK "│" RESET);
            }
        }
        status_bar(&ab);
    }

    at(&ab, E.srows, 1);
    ab_s(&ab, RESET "\x1b[K");
    int msg_live = E.msg[0] && (now_ms() - E.msgtime < toast_ms || E.promptcol >= 0);
    int toast = msg_live && E.promptcol < 0 && ((!E.dash && !overlay && E.scols >= 44 && E.rows >= 10 && avtta_worthy()) || (E.dash && mas_vis));
    if (msg_live && !toast) {
        int ml = (int)strlen(E.msg);
        ab_s(&ab, E.promptcol >= 0 ? TXT : MSGC);
        ab_append(&ab, E.msg, ml > E.scols ? E.scols : ml);
        ab_s(&ab, RESET);
    } else if (!E.dash) {
        if (E.sel_on) hint_bar(&ab, sbar, NELEM(sbar));
        else hint_bar(&ab, bar, NELEM(bar));
    }
    if (toast && !E.dash) {
        panel_maxl = 2;
        panel(&ab, crow >= E.rows / 2 ? 5 : E.rows, msg_pose(E.msg), E.msg, 0);
        panel_maxl = 4;
    }
    if (overlay) {
        overlay(&ab);
        at(&ab, ov_r, ov_c);
        ab_s(&ab, "\x1b[?25h");
    } else if (E.promptcol >= 0) {
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
    int first = 1;
    while ((n = getline(&line, &cap, f)) != -1) {
        int nl = n > 0 && line[n - 1] == '\n';
        if (nl) n--;
        if (first) { E.crlf = nl && n > 0 && line[n - 1] == '\r'; first = 0; }
        if (E.crlf && nl && n > 0 && line[n - 1] == '\r') n--;
        E.nonl = !nl;
        insert_row(E.nrows, line, (int)n);
    }
    free(line);
}

static void reset_view(void) {
    E.cx = E.cy = E.rowoff = E.coloff = E.nops = E.cur = E.saved = E.dash = E.sel_on = 0;
    E.hl_from = E.hlon = E.pasting = 0;
    infer_indent();
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
    reset_view();
    if (f) {
        int ln = recent_line(file);
        if (ln > 0) E.cy = ln > E.nrows ? E.nrows - 1 : ln - 1;
        recent_add(file, E.cy + 1);
        center();
        set_msg("opened %.60s - %d line%s.", file, E.nrows, E.nrows == 1 ? "" : "s");
    } else set_msg("new file: %s. press ctrl-s to save it.", file);
    return 0;
}

static char *prompt(const char *label, void (*cb)(const char *, int));

static char *ask_path(const char *label) {
    prompt_paths = 1;
    char *p = prompt(label, NULL);
    prompt_paths = 0;
    return p;
}

static char *expand(const char *p) {
    const char *h = getenv("HOME");
    if (p[0] == '~' && (p[1] == '/' || !p[1]) && h) {
        char *q = xrealloc(NULL, strlen(h) + strlen(p));
        sprintf(q, "%s%s", h, p + 1);
        return q;
    }
    return strdup(p);
}

static int write_rows(FILE *f, long *total) {
    long t = 0;
    if (!(E.nrows == 1 && !E.row[0].len))
        for (int i = 0; i < E.nrows; i++) {
            fwrite(E.row[i].s, 1, E.row[i].len, f);
            t += E.row[i].len;
            if (i < E.nrows - 1 || !E.nonl) {
                if (E.crlf) { fputc('\r', f); t++; }
                fputc('\n', f);
                t++;
            }
        }
    if (total) *total = t;
    return ferror(f) ? -1 : 0;
}

static int save_direct(const char *path, long *total) {
    FILE *f = fopen(path, "wb");
    int bad;
    if (!f) return -1;
    bad = write_rows(f, total);
    bad |= fclose(f) != 0;
    return bad ? -1 : 0;
}

/* write to a temp file next to the target, fsync, rename over it: a crash or full disk can't eat the original */
static int save_atomic(const char *path, long *total) {
    struct stat st, ls;
    char real[PATH_MAX], tmp[PATH_MAX * 2 + 32], dir[PATH_MAX];
    const char *base;
    char *sl;
    int exists = stat(path, &st) == 0, fd, bad, e;
    FILE *f;
    mode_t um;
    if (exists ? (!S_ISREG(st.st_mode) || st.st_nlink > 1 || st.st_uid != geteuid())
               : lstat(path, &ls) == 0)
        return save_direct(path, total);
    if (!realpath(path, real)) snprintf(real, sizeof real, "%s", path);
    sl = strrchr(real, '/');
    if (sl) { snprintf(dir, sizeof dir, "%.*s", sl == real ? 1 : (int)(sl - real), real); base = sl + 1; }
    else { snprintf(dir, sizeof dir, "."); base = real; }
    snprintf(tmp, sizeof tmp, "%s/.%s.avtty-XXXXXX", dir, base);
    fd = mkstemp(tmp);
    if (fd < 0) return save_direct(path, total);
    um = umask(0);
    umask(um);
    if (fchmod(fd, exists ? (st.st_mode & 07777) : (0666 & ~um))) {}
    if (exists) (void)!fchown(fd, st.st_uid, st.st_gid);
    f = fdopen(fd, "wb");
    if (!f) { e = errno; close(fd); unlink(tmp); errno = e; return -1; }
    bad = write_rows(f, total);
    bad |= fflush(f) != 0;
    bad |= fsync(fd) != 0;
    bad |= fclose(f) != 0;
    if (bad || rename(tmp, real)) {
        e = errno;
        unlink(tmp);
        errno = e;
        return -1;
    }
    return 0;
}

static void rescue(void) {
    char p[PATH_MAX + 8], *where = NULL;
    if (E.nrows && is_dirty()) {
        snprintf(p, sizeof p, "%s.save", E.filename ? E.filename : "avtty");
        FILE *f = fopen(p, "wb");
        if (f) {
            write_rows(f, NULL);
            if (!fclose(f)) where = p;
        }
    }
    disable_raw();
    if (where) fprintf(stderr, "avtty: terminal went away - your text was saved to %s\n", where);
    exit(1);
}

static int save_file(void) {
    long total = 0;
    if (!E.filename) {
        char *n = ask_path("Save as (Esc to cancel): ");
        if (!n) { set_msg("ok, not saving."); return 0; }
        E.filename = expand(n);
        free(n);
        detect_lang();
    }
    if (save_atomic(E.filename, &total)) { set_msg("i couldn't save that: %s", strerror(errno)); return -1; }
    E.saved = E.cur;
    recent_add(E.filename, E.cy + 1);
    set_msg("saved %s - %ld bytes.", E.filename, total);
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
        int c = wait_key();
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
        for (const char *p = ffind(s, q); p && p - s <= hi; p = ffind(p + 1, q))
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
    live_q = q;
    if (!*q) return;
    if (!dir) { E.cx = find_x; E.cy = find_y; }
    if (!search(q, dir ? dir : 1, !dir)) prompt_note = "  [not found]";
}

static void find(void) {
    int ro = E.rowoff, co = E.coloff;
    find_x = E.cx;
    find_y = E.cy;
    char *q = prompt("Find (Esc cancel, arrows next/prev): ", find_cb);
    live_q = NULL;
    if (q) { free(E.lastq); E.lastq = q; E.hlon = 1; }
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
    if (!t) { free(f); set_msg("replace cancelled."); return; }
    int fl = (int)strlen(f), tl = (int)strlen(t), cnt = 0, skip = 0, all = 0, stop = 0, cx = E.cx, cy = E.cy;
    free(E.lastq);
    E.lastq = strdup(f);
    E.group++;
    for (int y = 0; y < E.nrows && !stop; y++) {
        int pos = 0;
        char *m;
        while (!stop && (m = (char *)ffind(E.row[y].s + pos, f))) {
            int x = (int)(m - E.row[y].s);
            if (!all) {
                int k;
                E.cy = y; E.cx = x; E.hlon = 1;
                if (E.cy < E.rowoff || E.cy >= E.rowoff + E.rows) center();
                set_msg("replace?  y yes • n skip • a all • q stop");
                E.promptcol = utf8w(E.msg);
                do { refresh_screen(); k = wait_key(); } while (!k);
                E.promptcol = -1;
                E.msg[0] = 0;
                if (k == 'n' || k == ' ') { pos = x + fl; skip++; continue; }
                if (k == 'q' || k == ESC) { stop = 1; break; }
                if (k == 'a') all = 1;
                else if (k != 'y' && k != '\r') continue;
            }
            for (int k = 0; k < fl; k++) do_op(DEL, y, x, E.row[y].s[x]);
            for (int k = 0; k < tl; k++) do_op(INS, y, x + k, t[k]);
            pos = x + tl;
            cnt++;
        }
    }
    E.cy = cy < E.nrows ? cy : E.nrows - 1;
    E.cx = cx < E.row[E.cy].len ? cx : E.row[E.cy].len;
    if (cnt) set_msg("replaced %d, skipped %d. ctrl-z undoes it all.", cnt, skip);
    else set_msg("i couldn't find '%.60s' to replace.", f);
    free(f);
    free(t);
}

static void open_prompt(const char *label, int mode) {
    char err[256], *q, *p = ask_path(label);
    if (!p) return;
    q = expand(p);
    if (open_file_cmd(q, mode, err, sizeof err)) set_msg("%s", err);
    free(q);
    free(p);
}

static char **ff_list;
static int ff_n, ff_nres, ff_sel, ff_top, ff_qlen, pk_text, ntr;
static struct { int idx, score; } *ff_res;
static struct tres { int fi, line, col; char *snip; } *tr;
static struct { char *d; size_t n; } *tf;
static char ff_q[128];

static void ff_scan(const char *dir, const char *rel, int depth) {
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) && ff_n < 8000) {
        char pth[PATH_MAX], rl[PATH_MAX];
        int isdir, isreg;
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "node_modules")) continue;
        snprintf(pth, sizeof pth, "%s/%s", dir, e->d_name);
        snprintf(rl, sizeof rl, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        isdir = e->d_type == DT_DIR;
        isreg = e->d_type == DT_REG;
        if (e->d_type == DT_LNK || e->d_type == DT_UNKNOWN) {
            struct stat st;
            isdir = 0;
            isreg = stat(pth, &st) == 0 && S_ISREG(st.st_mode);
        }
        if (isdir && depth < 6) ff_scan(pth, rl, depth + 1);
        else if (isreg) {
            ff_list = xrealloc(ff_list, sizeof *ff_list * (ff_n + 1));
            ff_list[ff_n++] = strdup(rl);
        }
    }
    closedir(d);
}

static int fz_match(const char *s, const char *q, int *pos) {
    int sc = 0, qi = 0, last = -2;
    const char *sl = strrchr(s, '/');
    int base = sl ? (int)(sl - s) + 1 : 0;
    for (int i = 0; s[i] && q[qi]; i++)
        if (tolower((unsigned char)s[i]) == tolower((unsigned char)q[qi])) {
            sc += 1 + (i == last + 1 ? 3 : 0) + (i == 0 || strchr("/_-. ", s[i - 1]) ? 4 : 0) + (i >= base ? 2 : 0);
            if (pos) pos[qi] = i;
            last = i;
            qi++;
        }
    return q[qi] ? -1 : sc - (int)strlen(s) / 8;
}

static int ff_cmp(const void *a, const void *b) {
    const int *x = a, *y = b;
    if (x[1] != y[1]) return y[1] - x[1];
    size_t lx = strlen(ff_list[x[0]]), ly = strlen(ff_list[y[0]]);
    return lx != ly ? (lx < ly ? -1 : 1) : strcmp(ff_list[x[0]], ff_list[y[0]]);
}

static void ff_filter(void) {
    ff_nres = 0;
    for (int i = 0; i < ff_n; i++) {
        int sc = ff_qlen ? fz_match(ff_list[i], ff_q, NULL) : 0;
        if (sc >= 0) { ff_res[ff_nres].idx = i; ff_res[ff_nres].score = sc; ff_nres++; }
    }
    qsort(ff_res, ff_nres, sizeof *ff_res, ff_cmp);
    ff_sel = ff_top = 0;
}

/* Find Text: every file was read once up front, each keystroke just rescans the buffers */
static void tx_filter(void) {
    for (int i = 0; i < ntr; i++) free(tr[i].snip);
    ntr = ff_nres = ff_sel = ff_top = 0;
    if (!ff_qlen) return;
    for (int f = 0; f < ff_n && ntr < 1000; f++) {
        const char *d = tf[f].d, *end, *lp, *p;
        int ln = 1;
        if (!d) continue;
        end = d + tf[f].n;
        lp = d;
        for (p = ffind(d, ff_q); p && ntr < 1000; p = lp < end ? ffind(lp, ff_q) : NULL) {
            const char *ls = p, *le = memchr(p, '\n', end - p), *ss, *c;
            int sl;
            while (ls > lp && ls[-1] != '\n') ls--;
            for (c = lp; c < ls; c++) if (*c == '\n') ln++;
            if (!le) le = end;
            for (ss = ls; ss < le && (*ss == ' ' || *ss == '\t'); ss++);
            sl = (int)(le - ss) > 200 ? 200 : (int)(le - ss);
            while (sl > 0 && ss + sl < le && CONT(ss[sl])) sl--;
            tr[ntr].fi = f;
            tr[ntr].line = ln;
            tr[ntr].col = (int)(p - ls);
            tr[ntr].snip = strndup(ss, sl);
            ntr++;
            ln++;
            lp = le < end ? le + 1 : end;
        }
    }
    ff_nres = ntr;
}

static void pk_filter(void) { if (pk_text) tx_filter(); else ff_filter(); }

static int ff_nl(void) { return E.srows - 8 < 1 ? 1 : E.srows - 8 > 10 ? 10 : E.srows - 8; }

static void ff_draw(struct abuf *ab) {
    int w = E.scols - 4 < 72 ? E.scols - 4 : 72, nl = ff_nl(), x = 1 + (E.scols - w) / 2, y0 = 2, iw = w - 2;
    char info[48];
    int pos[128];
    const char *tt = pk_text ? "Find Text" : "Find File";
    ov_r = y0 + 1;
    ov_c = x + 4 + utf8w(ff_q);
    if (E.scols < 30 || E.srows < 10) return;
    if (put(ab, y0, x, GRAY, "┌─ ")) {
        ab_s(ab, TITLE);
        ab_s(ab, tt);
        ab_s(ab, GRAY " ");
        ab_rep(ab, "─", w - 5 - utf8w(tt));
        ab_s(ab, "┐" RESET);
    }
    put(ab, y0 + 1, x, GRAY, "│ ");
    ab_s(ab, KEYC "❯ " TXT);
    ab_s(ab, ff_q);
    ab_rep(ab, " ", iw - 4 - utf8w(ff_q));
    ab_s(ab, GRAY " │" RESET);
    put(ab, y0 + 2, x, GRAY, "├");
    ab_rep(ab, "─", iw);
    ab_s(ab, "┤" RESET);
    for (int k = 0; k < nl; k++) {
        int i = ff_top + k, sel = i == ff_sel;
        const char *base = sel ? th.rowbg : "", *sp;
        put(ab, y0 + 3 + k, x, GRAY, "│");
        if (i >= ff_nres) {
            const char *msg = pk_text && !ff_qlen && !k ? "   search inside any file" : "";
            ab_s(ab, GRAY);
            ab_s(ab, msg);
            ab_rep(ab, " ", iw - utf8w(msg));
            ab_s(ab, GRAY "│" RESET);
            continue;
        }
        if (pk_text) {
            const struct tres *t = &tr[i];
            char head[PATH_MAX + 16];
            const char *hp = head, *sn = t->snip;
            int avail = iw - 3, hl, used = 3, rem, ms = -1, me = -1, lm = -1, cl = 0, trc = 0;
            snprintf(head, sizeof head, "%s:%d", ff_list[t->fi], t->line);
            hl = utf8w(head);
            if (hl > avail * 55 / 100) {
                hp = head + (hl - avail * 55 / 100 + 1);
                while (CONT(*hp)) hp++;
                trc = 1;
            }
            nextm(sn, ff_q, 0, &ms, &me);
            ab_s(ab, base);
            ab_s(ab, sel ? KEYC " ▸ " : "   ");
            ab_s(ab, GRAY);
            if (trc) ab_s(ab, "…");
            ab_s(ab, sel ? SELDIM : GRAY);
            ab_s(ab, hp);
            used += utf8w(hp) + trc + 2;
            ab_s(ab, "  ");
            rem = iw - used - 1;
            for (int b = 0; sn[b] && cl < rem;) {
                int l = 1, m;
                while (CONT(sn[b + l])) l++;
                while (ms >= 0 && b >= me) nextm(sn, ff_q, me, &ms, &me);
                m = ms >= 0 && b >= ms && b < me;
                if (m != lm) {
                    ab_s(ab, RESET);
                    ab_s(ab, base);
                    ab_s(ab, m ? SGR("1;33") : sel ? SGR("1;39") : TXT);
                    lm = m;
                }
                if (sn[b] == '\t' || (unsigned char)sn[b] < 32) ab_s(ab, " ");
                else ab_append(ab, sn + b, l);
                cl++;
                b += l;
            }
            used += cl;
            ab_s(ab, RESET);
            ab_s(ab, base);
            ab_rep(ab, " ", iw - used);
            ab_s(ab, RESET GRAY "│" RESET);
            continue;
        }
        const char *path = ff_list[ff_res[i].idx];
        int off = 0, avail = iw - 3, len = (int)strlen(path), lastsl = -1, used;
        memset(pos, -1, sizeof pos);
        if (ff_qlen) fz_match(path, ff_q, pos);
        if (len > avail) off = len - (avail - 1);
        sp = strrchr(path, '/');
        lastsl = sp ? (int)(sp - path) : -1;
        ab_s(ab, base);
        ab_s(ab, sel ? KEYC " ▸ " : "   ");
        used = 3;
        if (off) { ab_s(ab, RESET); ab_s(ab, base); ab_s(ab, GRAY "…"); used++; }
        for (int b = off, ls = -1; b < len; b++) {
            int mt = 0, st;
            if (CONT(path[b])) { ab_append(ab, path + b, 1); continue; }
            for (int q = 0; q < ff_qlen && pos[q] >= 0; q++) if (pos[q] == b) mt = 1;
            st = mt ? 2 : b <= lastsl ? 0 : 1;
            if (st != ls) {
                ab_s(ab, RESET);
                ab_s(ab, base);
                ab_s(ab, st == 2 ? SGR("1;33") : st == 0 ? (sel ? SELDIM : GRAY) : (sel ? SGR("1;39") : TXT));
                ls = st;
            }
            ab_append(ab, path + b, 1);
            used++;
        }
        ab_s(ab, RESET);
        ab_s(ab, base);
        ab_rep(ab, " ", iw - used);
        ab_s(ab, RESET GRAY "│" RESET);
    }
    if (pk_text) snprintf(info, sizeof info, ntr >= 1000 ? "1000+ matches" : "%d matches", ntr);
    else snprintf(info, sizeof info, "%d/%d", ff_nres, ff_n);
    if (put(ab, y0 + 3 + nl, x, GRAY, "└─ ")) {
        int hw = w >= 52 ? 33 : 0;
        if (hw) ab_s(ab, "↑↓ move • Enter open • Esc close ");
        int fill = w - 3 - hw - (int)strlen(info) - 4;
        ab_rep(ab, "─", fill < 0 ? 0 : fill);
        ab_s(ab, " ");
        ab_s(ab, info);
        ab_s(ab, " ─┘" RESET);
    }
}

static char *pick_run(int text, int *line, int *col) {
    char *res = NULL;
    size_t tot = 0;
    ff_n = 0;
    ff_scan(".", "", 0);
    if (!ff_n) { set_msg("i don't see any files here."); return NULL; }
    pk_text = text;
    ff_res = xrealloc(NULL, sizeof *ff_res * ff_n);
    if (text) {
        tr = xrealloc(NULL, sizeof *tr * 1000);
        tf = xrealloc(NULL, sizeof *tf * ff_n);
        for (int i = 0; i < ff_n; i++) {
            struct stat st;
            FILE *f;
            char *d;
            size_t n;
            tf[i].d = NULL;
            tf[i].n = 0;
            if (stat(ff_list[i], &st) || st.st_size > 1000000 || tot + st.st_size > 40000000) continue;
            if (!(f = fopen(ff_list[i], "rb"))) continue;
            d = xrealloc(NULL, st.st_size + 1);
            n = fread(d, 1, st.st_size, f);
            fclose(f);
            d[n] = 0;
            if (memchr(d, 0, n < 8000 ? n : 8000)) { free(d); continue; }
            tf[i].d = d;
            tf[i].n = n;
            tot += n;
        }
    }
    ff_q[0] = 0;
    ff_qlen = 0;
    pk_filter();
    overlay = ff_draw;
    for (;;) {
        int c, nl = ff_nl();
        refresh_screen();
        c = wait_key();
        if (!c) continue;
        if (c == ESC) break;
        if (c == '\r') {
            if (ff_nres && text) {
                res = strdup(ff_list[tr[ff_sel].fi]);
                if (line) *line = tr[ff_sel].line;
                if (col) *col = tr[ff_sel].col;
            } else if (ff_nres) res = strdup(ff_list[ff_res[ff_sel].idx]);
            break;
        }
        if (c == ARROW_UP || c == CTRL_('p') || c == CTRL_('k')) { if (ff_sel > 0) ff_sel--; }
        else if (c == ARROW_DOWN || c == CTRL_('n') || c == CTRL_('j')) { if (ff_sel + 1 < ff_nres) ff_sel++; }
        else if (c == PAGE_UP) ff_sel = ff_sel > nl ? ff_sel - nl : 0;
        else if (c == PAGE_DOWN) ff_sel = ff_sel + nl < ff_nres ? ff_sel + nl : (ff_nres ? ff_nres - 1 : 0);
        else if (c == BACKSPACE || c == CTRL_('h')) {
            if (ff_qlen) { do ff_qlen--; while (ff_qlen && CONT(ff_q[ff_qlen])); ff_q[ff_qlen] = 0; pk_filter(); }
        } else if (c >= 32 && c < 256 && c != 127 && ff_qlen < 120) {
            ff_q[ff_qlen++] = (char)c;
            ff_q[ff_qlen] = 0;
            pk_filter();
        }
        if (ff_sel < ff_top) ff_top = ff_sel;
        if (ff_sel >= ff_top + nl) ff_top = ff_sel - nl + 1;
    }
    overlay = NULL;
    if (text) {
        for (int i = 0; i < ntr; i++) free(tr[i].snip);
        for (int i = 0; i < ff_n; i++) free(tf[i].d);
        free(tr);
        free(tf);
        tr = NULL;
        tf = NULL;
        ntr = 0;
    }
    for (int i = 0; i < ff_n; i++) free(ff_list[i]);
    free(ff_list);
    free(ff_res);
    ff_list = NULL;
    ff_res = NULL;
    ff_n = 0;
    return res;
}

static void pick_open(void) {
    char err[256], *p;
    if (!E.dash && is_dirty()) { set_msg("you have unsaved changes - save first with ctrl-s."); return; }
    p = pick_run(0, NULL, NULL);
    if (p) {
        if (open_file_cmd(p, 1, err, sizeof err)) set_msg("%s", err);
        free(p);
    }
}

static void pick_text_open(void) {
    char err[256], *p, *q;
    int ln = 1, col = 0;
    if (!E.dash && is_dirty()) { set_msg("you have unsaved changes - save first with ctrl-s."); return; }
    p = pick_run(1, &ln, &col);
    if (!p) return;
    q = strdup(ff_q);
    if (open_file_cmd(p, 1, err, sizeof err)) set_msg("%s", err);
    else {
        E.cy = ln > E.nrows ? E.nrows - 1 : ln - 1;
        E.cx = col < E.row[E.cy].len ? col : E.row[E.cy].len;
        center();
        free(E.lastq);
        E.lastq = q;
        q = NULL;
        E.hlon = 1;
        set_msg("jumped to %s:%d.", p, ln);
    }
    free(q);
    free(p);
}

/* ---- ask avtta: plain-words search through every shortcut ---- */
static const struct { const char *a, *b; } syn[] = {
    { "exit", "quit" }, { "close", "quit" }, { "leave", "quit" }, { "yank", "copy" }, { "put", "paste" }, { "delete", "cut" },
    { "remove", "cut" }, { "search", "find" }, { "seek", "find" }, { "goto", "go to" }, { "jump", "go to" }, { "rename", "save" },
    { "duplicate", "dup" }, { "clone", "dup" }, { "wrap", "wrap" }, { "spaces", "indent" }, { "tab", "indent" }, { "numbers", "line numbers" },
    { "top", "top" }, { "bottom", "bottom" }, { "word", "word" }, { "whitespace", "trim" }, { "redo", "undo" }, { "help", "ask me" }
};
static const char *const stopw[] = { "how", "do", "i", "to", "a", "the", "can", "my", "what", "is", "where", "key", "keys", "shortcut",
                                     "shortcuts", "for", "of", "in", "it", "an", "and", "me", "you", "with", "on", "use", "does", "that" };
static const struct { const char *g, *k, *d, *a; } extra[] = {
    { "Prompts", "Tab", "complete a path", "autocomplete" }, { "Prompts", "Esc", "cancel / close", "exit escape" },
    { "Finder", "↑ / ↓", "move through results", "" }, { "Finder", "Enter", "open the result", "" },
    { "Help", "F1 / Alt-H", "ask avtta about shortcuts", "help" }, { "Start screen", "/ or click avtta", "ask avtta", "help" },
    { "Start screen", "o f t c s r h q", "menu shortcuts", "" }, { "Edit", "Ctrl-A / Ctrl-E", "line start / end", "home end" }
};
struct hit { const char *g, *k, *d; };

static int ask_find(const char *q, struct hit *h, int max) {
    char lq[96], *tok[8], hay[256];
    int nt = 0, best = 0, tot = 0;
    snprintf(lq, sizeof lq, "%s", q);
    for (char *p = lq; *p; p++) *p = tolower((unsigned char)*p);
    for (char *p = strtok(lq, " ,.?!"); p && nt < 8; p = strtok(NULL, " ,.?!")) {
        int sw = 0;
        for (int i = 0; i < NELEM(stopw); i++) if (!strcmp(p, stopw[i])) sw = 1;
        if (!sw) tok[nt++] = p;
    }
    for (int pass = 0; pass < 2; pass++) {
        int ng = 0;
        for (int g = 0; g < NKEYS + 1; g++) {
            int n = g < NKEYS ? keys[g].n : NELEM(extra);
            for (int i = 0; i < n; i++) {
                const char *gt = g < NKEYS ? keys[g].title : extra[i].g, *k = g < NKEYS ? keys[g].i[i].k : extra[i].k,
                           *d = g < NKEYS ? keys[g].i[i].d : extra[i].d, *a = g < NKEYS ? "" : extra[i].a;
                int sc = 0;
                snprintf(hay, sizeof hay, "%s %s %s %s", gt, k, d, a);
                for (char *p = hay; *p; p++) *p = tolower((unsigned char)*p);
                for (int t = 0; t < nt; t++) {
                    int ok = strstr(hay, tok[t]) != NULL;
                    for (int y = 0; !ok && y < NELEM(syn); y++) if (!strcmp(tok[t], syn[y].a) && strstr(hay, syn[y].b)) ok = 1;
                    sc += ok;
                }
                if (pass == 0 && sc > best) best = sc;
                if (pass == 1 && sc && sc == best) { if (tot < max) { h[tot].g = gt; h[tot].k = k; h[tot].d = d; } tot++; }
                ng++;
            }
        }
        (void)ng;
    }
    return nt ? tot : 0;
}

static char mtext[512];
static int mpose, mbase;
static int mfull = 1;
static void mas_overlay(struct abuf *ab) { panel(ab, mbase, mpose, mtext, mfull); }
static int mas_room(void) { return E.scols >= 44 && (E.dash ? E.srows - 1 : E.rows) >= 9; }

static void ask_modal(void) {
    char q[64] = "";
    int ql = 0;
    if (!mas_room()) { set_msg("i need a bigger window to help - try making it larger."); return; }
    greet = 0;
    mfull = 1;
    overlay = mas_overlay;
    for (;;) {
        struct hit h[3];
        int n = ql ? ask_find(q, h, 3) : 0, shown = n > 3 ? 2 : n, o;
        mbase = E.dash ? E.srows - 1 : E.rows;
        o = snprintf(mtext, sizeof mtext, CY "ask avtta" CY " > %s", q);
        if (!ql) snprintf(mtext + o, sizeof mtext - o, "\nwhat are you trying to do? try " OR "undo" OR ", " OR "save" OR " or " OR "jump to a line" OR ".\nesc to close");
        else if (!n) snprintf(mtext + o, sizeof mtext - o, "\nhmm, i don't know a shortcut for that. try " OR "save" OR ", " OR "undo" OR " or " OR "find" OR "?");
        else {
            for (int i = 0; i < shown; i++)
                o += snprintf(mtext + o, sizeof mtext - o, "\n" CY "%s" CY "%*s %s  (%s)", h[i].k, 15 - utf8w(h[i].k), "", h[i].d, h[i].g);
            if (n > shown) snprintf(mtext + o, sizeof mtext - o, "\n...and %d more - add another word to narrow it down", n - shown);
        }
        mpose = !ql ? P_THINK : n ? P_IDEA : P_CONFUSED;
        { int nl = 1; for (const char *p = mtext; *p; p++) if (*p == '\n') nl++; ov_r = mbase - (nl > 4 ? 4 : nl); }
        ov_c = 21 + 2 + 12 + utf8w(q);
        if (ov_c > E.scols) ov_c = E.scols;
        refresh_screen();
        int c = wait_key();
        if (!c) continue;
        if (c == ESC || c == '\r') break;
        if (c == BACKSPACE || c == CTRL_('h')) { if (ql) { do ql--; while (ql && CONT(q[ql])); q[ql] = 0; } }
        else if (c >= 32 && c < 256 && c != 127 && ql < 40) { q[ql++] = (char)c; q[ql] = 0; }
    }
    overlay = NULL;
}

/* returns 1 to quit, 0 to keep editing, -1 if there is no room for the dialog */
static int quit_dialog(void) {
    const char *nm = E.filename ? E.filename : "this buffer";
    if (!mas_room()) return -1;
    overlay = mas_overlay;
    mfull = 0;
    mbase = E.rows;
    mpose = P_WORRY;
    snprintf(mtext, sizeof mtext, "wait! %.60s has unsaved changes.\n" OR "s" OR " save and quit    " OR "d" OR " discard and quit\n" OR "c" OR " keep editing", nm);
    ov_r = mbase;
    ov_c = E.scols;
    int c;
    do { refresh_screen(); c = wait_key(); } while (!c);
    overlay = NULL;
    if (c == 's' || c == 'S') { save_file(); return is_dirty() ? 0 : 1; }
    return c == 'd' || c == 'D' || c == CTRL_('q');
}

static void dash_activate(int i) {
    switch (i) {
    case 0: open_prompt("Open file: ", 0); break;
    case 1: pick_open(); break;
    case 2: pick_text_open(); break;
    case 3: open_prompt("Create file: ", 2); break;
    case 4:
        remember();
        free_rows();
        insert_row(0, "", 0);
        free(E.filename);
        E.filename = NULL;
        E.lang = 0;
        reset_view();
        set_msg("this is a scratch buffer - ctrl-s to name and save it.");
        break;
    case 5:
        for (int k = 0; k < E.nrecent; k++) free(E.recent[k]);
        E.nrecent = recent_read(E.recent, E.rline, MAXREC);
        E.rsel = 0;
        E.page = 1;
        break;
    case 6: E.page = 2; E.kpage = 0; break;
    case 7: exit(0);
    }
}

static void dash_key(int c) {
    if (E.page == 2) {
        int pg = E.srows > 8 ? E.srows - 4 : 4;
        if (E.knp && (c == ARROW_DOWN || c == PAGE_DOWN)) E.kpage = c == ARROW_DOWN ? E.kpage + 1 : E.kpage + pg;
        else if (E.knp && (c == ARROW_UP || c == PAGE_UP)) E.kpage = c == ARROW_UP ? E.kpage - 1 : E.kpage - pg;
        else if (E.knp && (c == HOME_KEY || c == TOP_KEY)) E.kpage = 0;
        else if (E.knp && (c == END_KEY || c == BOT_KEY)) E.kpage = E.knp;
        else if (c == '/' || c == '?' || c == F1_KEY || c == ALT('h')) ask_modal();
        else if (c == MOUSE_KEY) { if (mas_vis && my_ >= E.srows - 6 && my_ <= E.srows - 1) ask_modal(); }
        else E.page = 0;
        if (E.kpage < 0) E.kpage = 0;
        return;
    }
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
    int col = E.sel / 4, row = E.sel % 4;
    greet = 0;
    if (c == MOUSE_KEY) { if (mas_vis && my_ >= E.srows - 6 && my_ <= E.srows - 1) ask_modal(); }
    else if (c == '/' || c == '?' || c == F1_KEY || c == ALT('h')) ask_modal();
    else if (c == ESC && E.nrows) E.dash = 0;
    else if (c == CTRL_('p')) pick_open();
    else if (c == ALT('f')) pick_text_open();
    else if (c == ARROW_UP) E.sel = E.twocol ? col * 4 + (row + 3) % 4 : (E.sel + 7) % 8;
    else if (c == ARROW_DOWN) E.sel = E.twocol ? col * 4 + (row + 1) % 4 : (E.sel + 1) % 8;
    else if ((c == ARROW_LEFT || c == ARROW_RIGHT) && E.twocol) E.sel = (E.sel + 4) % 8;
    else if (c == '\r') dash_activate(E.sel);
    else if (c == CTRL_('q')) exit(0);
    else if (c > 32 && c < 127)
        for (int i = 0; i < 8; i++)
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
    char ex[9] = "";
    const char *extra = ex;
    if (deeper) {
        if (E.ind) { memset(ex, ' ', E.ind); ex[E.ind] = 0; }
        else strcpy(ex, "\t");
    }
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
            if (r->len) {
                int k = E.ind ? E.ind : 1;
                while (k--) do_op(INS, y, 0, E.ind ? ' ' : '\t');
                if (y == cy) d = E.ind ? E.ind : 1;
            }
            continue;
        }
        if (r->len && r->s[0] == '\t') n = 1;
        else while (n < (E.ind ? E.ind : TABW) && n < r->len && r->s[n] == ' ') n++;
        for (int k = 0; k < n; k++) do_op(DEL, y, 0, r->s[0]);
        if (y == cy) d = -n;
    }
    after_lines(sel, sy, ey, cy, cx, d);
}

static void toggle_comment(void) {
    const char *p = E.lang == 1 ? "// " : E.lang == 2 ? "# " : NULL;
    int sy, sx, ey, ex, sel = sel_range(&sy, &sx, &ey, &ex), cy = E.cy, cx = E.cx, d = 0, all = 1, any = 0, pl;
    if (!p) { set_msg("i don't know the comment style for this file type."); return; }
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

static void osc52(const char *s) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(s), i, k = 0;
    char *o;
    if (!n || n > 74000) return;
    o = xrealloc(NULL, n / 3 * 4 + 32);
    for (i = 0; i + 2 < n; i += 3) {
        unsigned v = (unsigned char)s[i] << 16 | (unsigned char)s[i + 1] << 8 | (unsigned char)s[i + 2];
        o[k++] = t[v >> 18]; o[k++] = t[v >> 12 & 63]; o[k++] = t[v >> 6 & 63]; o[k++] = t[v & 63];
    }
    if (i < n) {
        unsigned v = (unsigned char)s[i] << 16 | (i + 1 < n ? (unsigned char)s[i + 1] << 8 : 0);
        o[k++] = t[v >> 18]; o[k++] = t[v >> 12 & 63];
        o[k++] = i + 1 < n ? t[v >> 6 & 63] : '='; o[k++] = '=';
    }
    o[k] = 0;
    out("\x1b]52;c;");
    out(o);
    out("\x07");
    free(o);
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
    osc52(E.clip);
    set_msg(cut ? "cut %zu bytes." : "copied %zu bytes.", strlen(E.clip));
}

static void paste(void) {
    if (!E.clip) { set_msg("nothing to paste yet - copy or cut something first."); return; }
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
    if (a == b) { set_msg("there's no word under the cursor."); return; }
    free(E.lastq);
    E.lastq = strndup(r->s + a, b - a);
    E.cx = a;
    E.hlon = 1;
    search(E.lastq, 1, 0);
    set_msg("searching for '%.50s'. alt-n / alt-p jump between matches.", E.lastq);
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
    int y, x;
    char ch = E.cx < E.row[E.cy].len ? E.row[E.cy].s[E.cx] : 0;
    if (!ch || !strchr("([{)]}", ch)) { set_msg("the cursor isn't on a bracket."); return; }
    if (!find_match(E.nrows, &y, &x)) { set_msg("i can't find the matching bracket."); return; }
    E.cy = y;
    E.cx = x;
}

static void word_delete_fwd(void) {
    int y = E.cy, x = E.cx;
    if (has_sel()) { del_sel(); return; }
    word_move(1);
    if (E.cy == y && E.cx == x) return;
    E.group++;
    while (E.cy > y || E.cx > x) backspace();
}

static void trim_ws(void) {
    int n = 0, cy = E.cy, cx = E.cx;
    E.group++;
    for (int y = 0; y < E.nrows; y++) {
        const Row *r = &E.row[y];
        if (!r->len || (r->s[r->len - 1] != ' ' && r->s[r->len - 1] != '\t')) continue;
        n++;
        while (E.row[y].len && (E.row[y].s[E.row[y].len - 1] == ' ' || E.row[y].s[E.row[y].len - 1] == '\t'))
            do_op(DEL, y, E.row[y].len - 1, E.row[y].s[E.row[y].len - 1]);
    }
    E.cy = cy;
    E.cx = cx < E.row[cy].len ? cx : E.row[cy].len;
    set_msg(n ? "trimmed trailing spaces on %d line%s." : "no trailing spaces - already tidy.", n, n == 1 ? "" : "s");
}

static void process_key(int c) {
    static int quit_pending = 0;
    int was_pending = quit_pending, move = 0, vertical = 0, typed = 0;
    quit_pending = 0;

    if (E.pasting && c != PASTE_OFF) {
        if (c == '\r' || c == '\n') do_op(SPLIT, E.cy, E.cx, 0);
        else if (c == '\t' || (c >= 32 && c < 256 && c != 127)) do_op(INS, E.cy, E.cx, (char)c);
        return;
    }
    switch (c) {
    case PASTE_ON:
        if (has_sel()) del_sel();
        E.sel_on = 0;
        E.group++;
        E.pasting = 1;
        return;
    case PASTE_OFF:
        E.pasting = 0;
        last_typing = 0;
        E.want = cx_to_rx(&E.row[E.cy], E.cx);
        return;
    case ESC: E.hlon = 0; break;
    case ALT('f'): pick_text_open(); break;
    case F1_KEY: case ALT('h'): ask_modal(); break;
    case ALT('d'): word_delete_fwd(); break;
    case ALT('t'): trim_ws(); break;
    case ALT('l'):
        E.sel_on = move = 1;
        E.ay = E.cy;
        E.ax = 0;
        if (E.cy + 1 < E.nrows) { E.cy++; E.cx = 0; }
        else E.cx = E.row[E.cy].len;
        break;
    case ALT('\r'):
        E.sel_on = 0;
        E.group++;
        E.cx = E.row[E.cy].len;
        newline();
        break;
    case PARA_UP: case PARA_DOWN: {
        int d = c == PARA_DOWN ? 1 : -1, y = E.cy;
        move = 1;
        while (y + d >= 0 && y + d < E.nrows && !E.row[y + d].len) y += d;
        while (y + d >= 0 && y + d < E.nrows && E.row[y + d].len) y += d;
        if (y + d >= 0 && y + d < E.nrows) y += d;
        E.cy = y;
        E.cx = 0;
        break;
    }
    case CTRL_('p'): pick_open(); break;
    case ALT('i'): E.guides = !E.guides; break;
    case CTRL_('t'):
        if (is_dirty()) set_msg("you have unsaved changes - save first with ctrl-s.");
        else { remember(); E.dash = 1; E.page = 0; E.sel_on = 0; }
        break;
    case '\r':
        if (has_sel()) del_sel();
        else E.group++;
        newline();
        break;
    case CTRL_('q'):
        if (is_dirty()) {
            int r = quit_dialog();
            if (r == 0) return;
            if (r < 0 && !was_pending) {
                set_msg("unsaved changes! ctrl-q again to discard and quit, ctrl-s to save.");
                quit_pending = 1;
                return;
            }
        }
        exit(0);
    case CTRL_('s'): save_file(); break;
    case CTRL_('o'):
        if (is_dirty()) set_msg("you have unsaved changes - save first with ctrl-s.");
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
        if (E.sel_on) set_msg("select mode: move to extend, then ctrl-c to copy or ctrl-k to cut. esc cancels.");
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
        set_msg("soft wrap is %s.", E.wrap ? "on" : "off");
        break;
    case ALT('n'): case ALT('p'):
        E.hlon = 1;
        if (!E.lastq) set_msg("nothing to search for yet - press ctrl-f.");
        else if (!search(E.lastq, c == ALT('n') ? 1 : -1, 0)) set_msg("i can't find '%.40s'.", E.lastq);
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
            if (c == '\t' && E.ind) {
                int k = E.ind - cx_to_rx(&E.row[E.cy], E.cx) % E.ind;
                while (k--) type_char(' ');
            } else type_char(c);
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
    for (int g = 0; g < NKEYS; g++) {
        printf("\n  %s\n", keys[g].title);
        for (int i = 0; i < keys[g].n; i++)
            printf("    %s%*s %s\n", keys[g].i[i].k, 14 - utf8w(keys[g].i[i].k), "", keys[g].i[i].d);
    }
    puts("\nAlso:\n"
         "    Ctrl-A / E = Home / End, Alt-J / K = move line, Alt-/ = comment (when Ctrl-/ is unavailable)\n"
         "    Alt-< / Alt-> = top / bottom, Esc cancels a selection or a prompt\n"
         "    F1 / Alt-H = ask avtta (a very tiny text assistant) about any shortcut (start screen: click avtta, or press /)\n"
         "    Tab to complete file names in Open / Save prompts, ~ expands to your home\n"
         "    Code files get syntax colours, auto-pairs ()[]{}\"\" and smart indent after { ( [\n"
         "    The cursor line of recent files is remembered in ~/.avtty_recent");
}

static void on_sig(int n) { got_sig = n; }

static void install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
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
        E.saved = -1;
        infer_indent();
        set_msg("i read this from stdin - ctrl-s to save it somewhere.");
    } else E.dash = 1;
    if (line && !E.dash) {
        E.cy = line > E.nrows ? E.nrows - 1 : line - 1;
        center();
    }

    E.nums = E.guides = 1;
    { const char *tt = getenv("AVTTY_TOAST_MS"); if (tt && atoi(tt) > 0) toast_ms = atoi(tt); }
    install_signals();
    {
        const char *tm = getenv("TERM");
        use_mouse = !getenv("AVTTY_NOMOUSE");
        use_icons = !getenv("AVTTY_NOICONS") && !(tm && !strcmp(tm, "linux"));
    }
    enable_raw();
    theme_init();
    atexit(remember);
    refresh_screen();
    for (int idle = 0;;) {
        int rs = E.srows, cs = E.scols, had_msg = E.msg[0] != 0;
        get_size();
        int c = read_key();
        if (!c) {
            int expired = had_msg && now_ms() - E.msgtime >= toast_ms;
            if (E.pasting) {
                if (++idle >= 20) { E.pasting = 0; refresh_screen(); }
                continue;
            }
            if (expired) E.msg[0] = 0;
            if (expired || rs != E.srows || cs != E.scols) refresh_screen();
            continue;
        }
        idle = 0;
        if (E.sticky) { E.msg[0] = 0; E.sticky = 0; }
        if (E.dash) {
            if (c == PASTE_ON) E.pasting = 1;
            else if (c == PASTE_OFF) E.pasting = 0;
            else if (!E.pasting) dash_key(c);
        } else process_key(c);
        if (E.pasting) continue;
        refresh_screen();
    }
}
