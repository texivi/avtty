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

#define AVTTY_VERSION "0.2.0"
#define TAB_STOP 4
#define CTRL_(k) ((k) & 0x1f)
#define ESC 27
#define IS_CONT(b) ((((unsigned char)(b)) & 0xC0) == 0x80)
#define RECENT_MAX 10
#define RECENT_SHOWN 8
#define ITEM_W 24
#define NITEMS 7
#define DASH_MAXLINES 48
#define KEYS_HINT "Ctrl: S save | Q quit | Z undo | Y redo | F find | G goto | K cut line | N numbers"

enum Key {
    BACKSPACE = 127,
    ARROW_LEFT = 1000, ARROW_RIGHT, ARROW_UP, ARROW_DOWN,
    WORD_LEFT, WORD_RIGHT,
    DEL_KEY, HOME_KEY, END_KEY, PAGE_UP, PAGE_DOWN
};

typedef struct { char *s; int len; } Row;

typedef enum { OP_INS, OP_DEL, OP_SPLIT, OP_JOIN } OpType;
typedef struct { OpType t; int y, x; char c; int group; } Op;

static struct {
    int cx, cy;
    int rx;
    int want;
    int rowoff, coloff;
    int screenrows, screencols, rows;
    Row *row; int nrows, caprows;
    char *filename;
    int crlf;
    int show_nums;
    char msg[160]; time_t msgtime;
    int promptcol;
    Op *ops; int nops, capops;
    int cur;
    int saved;
    int group;
    int dash, dpage, dsel, rsel, twocol;
    char *recent[RECENT_MAX]; int nrecent;
    struct termios orig; int raw;
} E;

static int last_typing = 0;
static const char *prompt_note = "";

struct abuf { char *b; int len, cap; };

static const struct { const char *label; char key; } dash_items[NITEMS] = {
    { "Open File", 'o' }, { "New File", 'n' },
    { "Create File", 'c' }, { "Recent Files", 'r' },
    { "Keys & Help", 'h' }, { "Line Numbers", 'l' },
    { "Quit", 'q' }
};

static void disable_raw(void) {
    if (!E.raw) return;
    E.raw = 0;
    (void)!write(STDOUT_FILENO, "\x1b[?1049l", 8);
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
    (void)!write(STDOUT_FILENO, "\x1b[?1049h", 8);
}

static void get_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        E.screenrows = 24; E.screencols = 80;
    } else {
        E.screenrows = ws.ws_row; E.screencols = ws.ws_col;
    }
    E.rows = E.screenrows - 2;
    if (E.rows < 1) E.rows = 1;
}

static int read_key(void) {
    char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 0) return 0;
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return 0;
        die("read");
    }
    if (c != ESC) return (unsigned char)c;

    char seq[5];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return ESC;
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return ESC;
    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, &seq[2], 1) != 1) return ESC;
            if (seq[2] == '~') {
                switch (seq[1]) {
                    case '1': case '7': return HOME_KEY;
                    case '3': return DEL_KEY;
                    case '4': case '8': return END_KEY;
                    case '5': return PAGE_UP;
                    case '6': return PAGE_DOWN;
                }
            } else if (seq[2] == ';') {
                if (read(STDIN_FILENO, &seq[3], 1) != 1) return ESC;
                if (read(STDIN_FILENO, &seq[4], 1) != 1) return ESC;
                if (seq[3] == '5' || seq[3] == '3') {
                    if (seq[4] == 'C') return WORD_RIGHT;
                    if (seq[4] == 'D') return WORD_LEFT;
                }
            }
        } else {
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
    } else if (seq[0] == 'O') {
        if (seq[1] == 'H') return HOME_KEY;
        if (seq[1] == 'F') return END_KEY;
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
    switch (t) {
    case OP_INS: {
        Row *r = &E.row[y];
        r->s = xrealloc(r->s, r->len + 2);
        memmove(r->s + x + 1, r->s + x, r->len - x + 1);
        r->s[x] = c;
        r->len++;
        E.cy = y; E.cx = x + 1;
        break;
    }
    case OP_DEL: {
        Row *r = &E.row[y];
        memmove(r->s + x, r->s + x + 1, r->len - x);
        r->len--;
        E.cy = y; E.cx = x;
        break;
    }
    case OP_SPLIT: {
        Row *r = &E.row[y];
        insert_row(y + 1, r->s + x, r->len - x);
        r = &E.row[y];
        r->len = x;
        r->s[x] = 0;
        E.cy = y + 1; E.cx = 0;
        break;
    }
    case OP_JOIN: {
        Row *a = &E.row[y], *b = &E.row[y + 1];
        a->s = xrealloc(a->s, a->len + b->len + 1);
        memcpy(a->s + a->len, b->s, b->len + 1);
        a->len += b->len;
        free(b->s);
        memmove(&E.row[y + 1], &E.row[y + 2], sizeof(Row) * (E.nrows - y - 2));
        E.nrows--;
        E.cy = y; E.cx = x;
        break;
    }
    }
}

static OpType inverse(OpType t) {
    switch (t) {
        case OP_INS: return OP_DEL;
        case OP_DEL: return OP_INS;
        case OP_SPLIT: return OP_JOIN;
        default: return OP_SPLIT;
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
    E.ops[E.nops++] = (Op){ t, y, x, c, E.group };
    E.cur = E.nops;
}

static void group_new(void) { E.group++; }
static int  is_dirty(void)  { return E.cur != E.saved; }

static void set_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.msg, sizeof E.msg, fmt, ap);
    va_end(ap);
    E.msgtime = time(NULL);
}

static void undo(void) {
    if (E.cur == 0) { set_msg("Nothing to undo"); return; }
    int g = E.ops[E.cur - 1].group;
    while (E.cur > 0 && E.ops[E.cur - 1].group == g) {
        Op *o = &E.ops[--E.cur];
        apply(inverse(o->t), o->y, o->x, o->c);
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

static void ab_spaces(struct abuf *ab, int n) {
    while (n-- > 0) ab_append(ab, " ", 1);
}

static void ab_rep(struct abuf *ab, const char *s, int n) {
    int l = (int)strlen(s);
    while (n-- > 0) ab_append(ab, s, l);
}

static int utf8w(const char *s) {
    int w = 0;
    for (; *s; s++) if (!IS_CONT(*s)) w++;
    return w;
}

static int cx_to_rx(const Row *r, int cx) {
    int col = 0;
    for (int i = 0; i < cx && i < r->len; i++) {
        unsigned char ch = r->s[i];
        if (ch == '\t') col += TAB_STOP - (col % TAB_STOP);
        else if (!IS_CONT(ch)) col++;
    }
    return col;
}

static int rx_to_cx(const Row *r, int rx) {
    int col = 0, i = 0;
    while (i < r->len && col < rx) {
        unsigned char ch = r->s[i];
        if (ch == '\t') { col += TAB_STOP - (col % TAB_STOP); i++; }
        else { col++; i++; while (i < r->len && IS_CONT(r->s[i])) i++; }
    }
    return i;
}

static int gutter_width(void) {
    if (!E.show_nums) return 0;
    int d = 3, n = E.nrows;
    while (n >= 1000) { d++; n /= 10; }
    int w = d + 1;
    return (E.screencols > w + 8) ? w : 0;
}

static void draw_row(struct abuf *ab, const Row *r, int width) {
    int col = 0, lo = E.coloff, hi = E.coloff + width;
    for (int i = 0; i < r->len && col < hi;) {
        unsigned char ch = r->s[i];
        if (ch == '\t') {
            int n = TAB_STOP - (col % TAB_STOP);
            for (int k = 0; k < n; k++, col++)
                if (col >= lo && col < hi) ab_append(ab, " ", 1);
            i++;
        } else if (ch < 32 || ch == 127) {
            if (col >= lo && col < hi) ab_append(ab, "?", 1);
            col++; i++;
        } else {
            int l = 1;
            while (i + l < r->len && IS_CONT(r->s[i + l])) l++;
            if (col >= lo && col < hi) ab_append(ab, r->s + i, l);
            col++; i += l;
        }
    }
}

static void scroll(int textw) {
    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.rows) E.rowoff = E.cy - E.rows + 1;
    E.rx = cx_to_rx(&E.row[E.cy], E.cx);
    if (E.rx < E.coloff) E.coloff = E.rx;
    if (E.rx >= E.coloff + textw) E.coloff = E.rx - textw + 1;
}

struct dline { char b[1024]; int n, w; };

static void dl_clear(struct dline *l) { l->n = 0; l->w = 0; l->b[0] = 0; }

static void dl_add(struct dline *l, const char *s, int vis) {
    int len = (int)strlen(s);
    if (l->n + len < (int)sizeof l->b) {
        memcpy(l->b + l->n, s, len + 1);
        l->n += len;
    }
    l->w += vis;
}

static void dl_esc(struct dline *l, const char *s)  { dl_add(l, s, 0); }
static void dl_text(struct dline *l, const char *s) { dl_add(l, s, utf8w(s)); }
static void dl_pad(struct dline *l, int n)          { while (n-- > 0) dl_add(l, " ", 1); }

static void fit_tail(char *dst, size_t n, const char *s, int maxw) {
    int w = utf8w(s);
    if (maxw < 1) { dst[0] = 0; return; }
    if (w <= maxw) { snprintf(dst, n, "%s", s); return; }
    int skip = w - (maxw - 1);
    while (*s && skip > 0) {
        s++;
        while (IS_CONT(*s)) s++;
        skip--;
    }
    snprintf(dst, n, "\xE2\x80\xA6%s", s);
}

static void push_center(struct dline *L, int *nl, int iw, const struct dline *t) {
    if (*nl >= DASH_MAXLINES) return;
    struct dline *d = &L[(*nl)++];
    dl_clear(d);
    int pad = (iw - t->w) / 2;
    if (pad < 0) pad = 0;
    dl_pad(d, pad);
    dl_add(d, t->b, t->w);
    dl_pad(d, iw - d->w);
}

static void push_blank(struct dline *L, int *nl, int iw) {
    struct dline t;
    dl_clear(&t);
    push_center(L, nl, iw, &t);
}

static void dl_item(struct dline *t, int idx, int sel) {
    char lab[32];
    const char *l = dash_items[idx].label;
    if (idx == 5) {
        snprintf(lab, sizeof lab, "Line Numbers: %s", E.show_nums ? "on" : "off");
        l = lab;
    }
    char key[2] = { dash_items[idx].key, 0 };
    dl_esc(t, sel ? "\x1b[48;5;238m\x1b[1;38;5;117m" : "\x1b[38;5;252m");
    dl_text(t, sel ? " \xE2\x96\xB8 " : "   ");
    dl_text(t, l);
    dl_pad(t, 19 - utf8w(l));
    dl_esc(t, sel ? "\x1b[0;48;5;238;38;5;250m" : "\x1b[38;5;244m");
    dl_text(t, key);
    dl_text(t, " ");
    dl_esc(t, "\x1b[m");
}

static void dl_footer(struct dline *L, int *nl, int iw, const char *text) {
    struct dline t;
    dl_clear(&t);
    dl_esc(&t, "\x1b[38;5;244m");
    dl_text(&t, text);
    dl_esc(&t, "\x1b[m");
    push_center(L, nl, iw, &t);
}

static void dl_title(struct dline *L, int *nl, int iw, const char *text) {
    struct dline t;
    dl_clear(&t);
    dl_esc(&t, "\x1b[1;38;5;117m");
    dl_text(&t, text);
    dl_esc(&t, "\x1b[m");
    push_center(L, nl, iw, &t);
}

static void build_logo(struct dline *L, int *nl, int iw, int scale) {
    static const char *glyph[5][5] = {
        { " ### ", "#   #", "#####", "#   #", "#   #" },
        { "#   #", "#   #", "#   #", " # # ", "  #  " },
        { "#####", "  #  ", "  #  ", "  #  ", "  #  " },
        { "#####", "  #  ", "  #  ", "  #  ", "  #  " },
        { "#   #", " # # ", "  #  ", "  #  ", "  #  " }
    };
    static const char *col[5] = {
        "\x1b[1;38;5;117m", "\x1b[1;38;5;111m", "\x1b[1;38;5;105m",
        "\x1b[1;38;5;99m", "\x1b[1;38;5;93m"
    };
    for (int r = 0; r < 5; r++) {
        struct dline t;
        dl_clear(&t);
        dl_esc(&t, col[r]);
        for (int g = 0; g < 5; g++) {
            for (int c = 0; c < 5; c++)
                for (int s = 0; s < scale; s++)
                    dl_add(&t, glyph[g][r][c] == '#' ? "\xE2\x96\x88" : " ", 1);
            if (g < 4) dl_pad(&t, scale);
        }
        dl_esc(&t, "\x1b[m");
        push_center(L, nl, iw, &t);
    }
}

static void build_menu_page(struct dline *L, int *nl, int iw, int avail, int scale) {
    int menurows = E.twocol ? 4 : NITEMS;
    int show_logo = avail >= menurows + 13;
    push_blank(L, nl, iw);
    if (show_logo) {
        build_logo(L, nl, iw, scale);
        push_blank(L, nl, iw);
        push_blank(L, nl, iw);
    }
    for (int r = 0; r < menurows; r++) {
        struct dline t;
        dl_clear(&t);
        dl_item(&t, r, E.dsel == r);
        if (E.twocol) {
            dl_pad(&t, 4);
            if (r + 4 < NITEMS) dl_item(&t, r + 4, E.dsel == r + 4);
            else dl_pad(&t, ITEM_W);
        }
        push_center(L, nl, iw, &t);
    }
    push_blank(L, nl, iw);
    dl_footer(L, nl, iw, "avtty " AVTTY_VERSION " \xE2\x80\xA2 \xE2\x86\x91\xE2\x86\x93\xE2\x86\x90\xE2\x86\x92 move \xE2\x80\xA2 Enter select");
    push_blank(L, nl, iw);
}

static void build_recent_page(struct dline *L, int *nl, int iw) {
    int cw = iw - 8;
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    push_blank(L, nl, iw);
    dl_title(L, nl, iw, "Recent Files");
    push_blank(L, nl, iw);
    if (E.nrecent == 0) dl_footer(L, nl, iw, "Nothing here yet - open or create a file first");
    for (int i = 0; i < E.nrecent; i++) {
        const char *path = E.recent[i];
        const char *slash = strrchr(path, '/');
        const char *name = slash ? slash + 1 : path;
        char dir[PATH_MAX], dshow[PATH_MAX], nshow[PATH_MAX];
        int dlen = slash ? (int)(slash - path) : 0;
        snprintf(dir, sizeof dir, "%.*s", dlen, path);
        if (hl && !strncmp(dir, home, hl) && (dir[hl] == 0 || dir[hl] == '/')) {
            memmove(dir + 1, dir + hl, strlen(dir + hl) + 1);
            dir[0] = '~';
        }
        fit_tail(nshow, sizeof nshow, name, cw - 6);
        int remain = cw - 3 - utf8w(nshow) - 2;
        if (remain >= 4) fit_tail(dshow, sizeof dshow, dir, remain);
        else dshow[0] = 0;

        int sel = (i == E.rsel);
        struct dline t;
        dl_clear(&t);
        dl_esc(&t, sel ? "\x1b[48;5;238m\x1b[1;38;5;117m" : "\x1b[38;5;252m");
        dl_text(&t, sel ? " \xE2\x96\xB8 " : "   ");
        dl_text(&t, nshow);
        dl_esc(&t, sel ? "\x1b[0;48;5;238;38;5;244m" : "\x1b[38;5;244m");
        dl_text(&t, "  ");
        dl_text(&t, dshow);
        dl_pad(&t, cw - t.w);
        dl_esc(&t, "\x1b[m");
        push_center(L, nl, iw, &t);
    }
    push_blank(L, nl, iw);
    dl_footer(L, nl, iw, "Enter or 1-8 open \xE2\x80\xA2 Esc back");
    push_blank(L, nl, iw);
}

static void build_help_page(struct dline *L, int *nl, int iw) {
    static const char *pairs[][2] = {
        { "Ctrl-S  save", "Ctrl-Q  quit" },
        { "Ctrl-Z  undo", "Ctrl-Y  redo" },
        { "Ctrl-F  find", "Ctrl-G  go to line" },
        { "Ctrl-K  cut line", "Ctrl-N  line numbers" },
        { "Ctrl-A  line start", "Ctrl-E  line end" },
        { "Ctrl+Arrows  word jump", "PgUp/PgDn  page" }
    };
    push_blank(L, nl, iw);
    dl_title(L, nl, iw, "Keys");
    push_blank(L, nl, iw);
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        struct dline t;
        dl_clear(&t);
        dl_esc(&t, "\x1b[38;5;252m");
        dl_text(&t, pairs[i][0]);
        if (E.twocol) {
            dl_pad(&t, 26 - utf8w(pairs[i][0]));
            dl_text(&t, pairs[i][1]);
            push_center(L, nl, iw, &t);
        } else {
            push_center(L, nl, iw, &t);
            struct dline u;
            dl_clear(&u);
            dl_esc(&u, "\x1b[38;5;252m");
            dl_text(&u, pairs[i][1]);
            push_center(L, nl, iw, &u);
        }
    }
    push_blank(L, nl, iw);
    dl_footer(L, nl, iw, "Press any key to go back");
    push_blank(L, nl, iw);
}

static void draw_dashboard(void) {
    static struct dline L[DASH_MAXLINES];
    struct abuf ab = { NULL, 0, 0 };
    ab_append(&ab, "\x1b[?25l\x1b[H", 9);

    int cols = E.screencols, avail = E.screenrows - 1;
    int small = cols < 40 || avail < 6;
    int iw = 0, total = 0, leftpad = 0, nl = 0;

    if (!small) {
        int scale = cols >= 70 ? 2 : 1;
        E.twocol = cols >= 62;
        int lw = 29 * scale;
        int menuw = E.twocol ? 2 * ITEM_W + 4 : ITEM_W;
        iw = (lw > menuw ? lw : menuw) + 8;
        if (E.dpage == 1) build_recent_page(L, &nl, iw);
        else if (E.dpage == 2) build_help_page(L, &nl, iw);
        else build_menu_page(L, &nl, iw, avail, scale);
        total = nl + 2;
        leftpad = (cols - (iw + 2)) / 2;
    }
    int toppad = (avail - total) / 2;
    if (toppad < 0) toppad = 0;

    for (int y = 0; y < avail; y++) {
        if (small) {
            if (y == 0) ab_append(&ab, "avtty: terminal too small", 25);
        } else {
            int idx = y - toppad;
            if (idx >= 0 && idx < total) {
                ab_spaces(&ab, leftpad);
                ab_append(&ab, "\x1b[38;5;60m", 10);
                if (idx == 0 || idx == total - 1) {
                    ab_append(&ab, idx == 0 ? "\xE2\x95\xAD" : "\xE2\x95\xB0", 3);
                    ab_rep(&ab, "\xE2\x94\x80", iw);
                    ab_append(&ab, idx == 0 ? "\xE2\x95\xAE" : "\xE2\x95\xAF", 3);
                    ab_append(&ab, "\x1b[m", 3);
                } else {
                    ab_append(&ab, "\xE2\x94\x82\x1b[m", 6);
                    ab_append(&ab, L[idx - 1].b, L[idx - 1].n);
                    ab_append(&ab, "\x1b[38;5;60m\xE2\x94\x82\x1b[m", 14);
                }
            }
        }
        ab_append(&ab, "\x1b[K\r\n", 5);
    }

    ab_append(&ab, "\x1b[K", 3);
    if (E.msg[0] && (time(NULL) - E.msgtime < 5 || E.promptcol >= 0)) {
        int ml = (int)strlen(E.msg);
        if (ml > cols) ml = cols;
        ab_append(&ab, E.msg, ml);
    }
    if (E.promptcol >= 0) {
        char buf[48];
        int c = E.promptcol + 1;
        if (c > cols) c = cols;
        int n = snprintf(buf, sizeof buf, "\x1b[%d;%dH\x1b[?25h", E.screenrows, c);
        ab_append(&ab, buf, n);
    }
    (void)!write(STDOUT_FILENO, ab.b, ab.len);
    free(ab.b);
}

static void refresh_screen(void) {
    get_size();
    if (E.dash) { draw_dashboard(); return; }
    int gw = gutter_width();
    int textw = E.screencols - gw;
    if (textw < 1) textw = 1;
    scroll(textw);

    struct abuf ab = { NULL, 0, 0 };
    ab_append(&ab, "\x1b[?25l\x1b[H", 9);

    for (int y = 0; y < E.rows; y++) {
        int fr = E.rowoff + y;
        if (fr < E.nrows) {
            if (gw) {
                char num[32];
                int n = snprintf(num, sizeof num, "\x1b[90m%*d \x1b[m", gw - 1, fr + 1);
                ab_append(&ab, num, n);
            }
            draw_row(&ab, &E.row[fr], textw);
        } else {
            if (gw) ab_append(&ab, "\x1b[90m", 5);
            ab_append(&ab, "~", 1);
            if (gw) ab_append(&ab, "\x1b[m", 3);
        }
        ab_append(&ab, "\x1b[K\r\n", 5);
    }

    char left[256], right[64];
    int ln = snprintf(left, sizeof left, " avtty  %.100s%s",
                      E.filename ? E.filename : "[No Name]",
                      is_dirty() ? " [modified]" : "");
    int rn = snprintf(right, sizeof right, "Ln %d/%d, Col %d ",
                      E.cy + 1, E.nrows, E.rx + 1);
    if (ln > E.screencols) ln = E.screencols;
    ab_append(&ab, "\x1b[7m", 4);
    ab_append(&ab, left, ln);
    while (ln < E.screencols) {
        if (E.screencols - ln == rn) { ab_append(&ab, right, rn); break; }
        ab_append(&ab, " ", 1);
        ln++;
    }
    ab_append(&ab, "\x1b[m\r\n", 5);

    ab_append(&ab, "\x1b[K", 3);
    if (E.msg[0] && (time(NULL) - E.msgtime < 5 || E.promptcol >= 0)) {
        int ml = (int)strlen(E.msg);
        if (ml > E.screencols) ml = E.screencols;
        ab_append(&ab, E.msg, ml);
    }

    char buf[48];
    int n;
    if (E.promptcol >= 0) {
        int c = E.promptcol + 1;
        if (c > E.screencols) c = E.screencols;
        n = snprintf(buf, sizeof buf, "\x1b[%d;%dH", E.screenrows, c);
    } else {
        n = snprintf(buf, sizeof buf, "\x1b[%d;%dH",
                     E.cy - E.rowoff + 1, E.rx - E.coloff + gw + 1);
    }
    ab_append(&ab, buf, n);
    ab_append(&ab, "\x1b[?25h", 6);
    (void)!write(STDOUT_FILENO, ab.b, ab.len);
    free(ab.b);
}

static int xdg_path(char *out, size_t n, const char *env, const char *fallback, const char *rel) {
    const char *base = getenv(env);
    const char *home = getenv("HOME");
    if (base && *base) snprintf(out, n, "%s/%s", base, rel);
    else if (home && *home) snprintf(out, n, "%s/%s/%s", home, fallback, rel);
    else return -1;
    return 0;
}

static void mkdir_parents(const char *file) {
    char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s", file);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    }
}

static int recent_path(char *out, size_t n) {
    return xdg_path(out, n, "XDG_STATE_HOME", ".local/state", "avtty/recent");
}

static int recent_read(char **out, int max) {
    char p[PATH_MAX];
    if (recent_path(p, sizeof p)) return 0;
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int c = 0;
    while (c < max && (n = getline(&line, &cap, f)) != -1) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n > 0 && access(line, F_OK) == 0) out[c++] = strdup(line);
    }
    free(line);
    fclose(f);
    return c;
}

static void recent_add(const char *path) {
    char *real = realpath(path, NULL);
    if (!real) return;
    char *old[RECENT_MAX];
    int n = recent_read(old, RECENT_MAX);
    char p[PATH_MAX];
    if (recent_path(p, sizeof p) == 0) {
        mkdir_parents(p);
        FILE *f = fopen(p, "w");
        if (f) {
            fprintf(f, "%s\n", real);
            int w = 1;
            for (int i = 0; i < n && w < RECENT_MAX; i++) {
                if (strcmp(old[i], real)) { fprintf(f, "%s\n", old[i]); w++; }
            }
            fclose(f);
        }
    }
    for (int i = 0; i < n; i++) free(old[i]);
    free(real);
}

static int load_file(const char *fn) {
    FILE *f = fopen(fn, "rb");
    if (!f) return -1;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int first = 1;
    while ((n = getline(&line, &cap, f)) != -1) {
        if (n > 0 && line[n - 1] == '\n') {
            n--;
            if (n > 0 && line[n - 1] == '\r') { n--; if (first) E.crlf = 1; }
        }
        insert_row(E.nrows, line, (int)n);
        first = 0;
    }
    free(line);
    int err = ferror(f);
    fclose(f);
    if (E.nrows == 0) insert_row(0, "", 0);
    return err ? -1 : 0;
}

static int open_file_cmd(const char *file, int mode, char *err, size_t en) {
    struct stat st;
    int exists = stat(file, &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) {
        snprintf(err, en, "'%s' is a directory", file);
        return -1;
    }
    if (mode == 2) {
        if (exists) {
            snprintf(err, en, "'%s' already exists (use -o to open it)", file);
            return -1;
        }
        FILE *f = fopen(file, "wx");
        if (!f) {
            snprintf(err, en, "can't create '%s': %s", file, strerror(errno));
            return -1;
        }
        fclose(f);
        exists = 1;
    } else if (mode == 1 && !exists) {
        snprintf(err, en, "'%s' does not exist (use -c to create it)", file);
        return -1;
    }
    free_rows();
    E.crlf = 0;
    if (exists) {
        if (load_file(file) == -1) {
            snprintf(err, en, "can't read '%s': %s", file, strerror(errno));
            free_rows();
            return -1;
        }
    } else {
        insert_row(0, "", 0);
    }
    free(E.filename);
    E.filename = strdup(file);
    E.cx = E.cy = E.rowoff = E.coloff = 0;
    E.nops = E.cur = E.saved = 0;
    E.dash = 0;
    if (exists) {
        recent_add(file);
        set_msg(KEYS_HINT);
    } else {
        set_msg("New file: %s  (Ctrl-S to save)", file);
    }
    return 0;
}

static char *prompt(const char *label, void (*cb)(const char *, int));

static int save_file(void) {
    if (!E.filename) {
        char *n = prompt("Save as (Esc to cancel): ", NULL);
        if (!n) { set_msg("Save cancelled"); return 0; }
        E.filename = n;
    }

    char *real = realpath(E.filename, NULL);
    const char *target = real ? real : E.filename;
    size_t tl = strlen(target) + 16;
    char *tmp = xrealloc(NULL, tl);
    snprintf(tmp, tl, "%s.avtty.tmp", target);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        set_msg("Can't save: %s", strerror(errno));
        free(tmp); free(real);
        return -1;
    }
    long total = 0;
    int empty = (E.nrows == 1 && E.row[0].len == 0);
    if (!empty) {
        for (int i = 0; i < E.nrows; i++) {
            fwrite(E.row[i].s, 1, E.row[i].len, f);
            total += E.row[i].len + 1;
            if (E.crlf) { fputc('\r', f); total++; }
            fputc('\n', f);
        }
    }
    int ok = (fflush(f) == 0 && !ferror(f) && fsync(fileno(f)) == 0);
    int saved_errno = errno;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        struct stat st;
        if (stat(target, &st) == 0) chmod(tmp, st.st_mode & 07777);
        if (rename(tmp, target) != 0) { ok = 0; saved_errno = errno; }
    }
    if (!ok) {
        unlink(tmp);
        set_msg("Can't save: %s", strerror(saved_errno));
        free(tmp); free(real);
        return -1;
    }
    free(tmp); free(real);
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

        int col = 0;
        for (const char *p = label; *p; p++) if (!IS_CONT(*p)) col++;
        for (size_t i = 0; i < len; i++) if (!IS_CONT(buf[i])) col++;
        E.promptcol = col;
        refresh_screen();
        int c = read_key();
        if (c == 0) continue;
        if (c == DEL_KEY || c == BACKSPACE || c == CTRL_('h')) {
            if (len > 0) { do { len--; } while (len > 0 && IS_CONT(buf[len])); buf[len] = 0; }
        } else if (c == ESC) {
            E.promptcol = -1; E.msg[0] = 0; prompt_note = "";
            if (cb) cb(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (len > 0) {
                E.promptcol = -1; E.msg[0] = 0; prompt_note = "";
                if (cb) cb(buf, c);
                return buf;
            }
        } else if (c >= 32 && c < 256 && c != 127) {
            if (len + 2 > cap) { cap *= 2; buf = xrealloc(buf, cap); }
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

    int cur = (last == -1) ? E.cy - 1 : last;
    if (last != -1 && dir == 1) {
        Row *r = &E.row[cur];
        if (E.cx < r->len) {
            char *m = strstr(r->s + E.cx + 1, q);
            if (m) { E.cx = (int)(m - r->s); return; }
        }
    }
    for (int i = 0; i < E.nrows; i++) {
        cur += dir;
        if (cur < 0) cur = E.nrows - 1;
        else if (cur >= E.nrows) cur = 0;
        char *m = strstr(E.row[cur].s, q);
        if (m) { last = cur; E.cy = cur; E.cx = (int)(m - E.row[cur].s); return; }
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
    if (n < 1) n = 1;
    if (n > E.nrows) n = E.nrows;
    E.cy = n - 1;
    E.cx = 0;
}

static char *expand_home(const char *p) {
    const char *home = getenv("HOME");
    if (p[0] == '~' && (p[1] == '/' || p[1] == 0) && home) {
        size_t n = strlen(home) + strlen(p);
        char *r = xrealloc(NULL, n + 1);
        snprintf(r, n + 1, "%s%s", home, p + 1);
        return r;
    }
    return strdup(p);
}

static void free_recent(void) {
    for (int i = 0; i < E.nrecent; i++) free(E.recent[i]);
    E.nrecent = 0;
}

static void open_recent(int i) {
    char err[256];
    if (open_file_cmd(E.recent[i], 1, err, sizeof err) != 0) set_msg("%s", err);
}

static void dash_prompt_open(const char *label, int mode) {
    char err[256];
    char *p = prompt(label, NULL);
    if (!p) return;
    char *q = expand_home(p);
    if (open_file_cmd(q, mode, err, sizeof err) != 0) set_msg("%s", err);
    free(q);
    free(p);
}

static void dash_activate(int i) {
    switch (i) {
    case 0: dash_prompt_open("Open file: ", 0); break;
    case 1:
        free_rows();
        insert_row(0, "", 0);
        free(E.filename);
        E.filename = NULL;
        E.cx = E.cy = 0;
        E.dash = 0;
        set_msg("New buffer - Ctrl-S to name and save it");
        break;
    case 2: dash_prompt_open("Create file: ", 2); break;
    case 3:
        free_recent();
        E.nrecent = recent_read(E.recent, RECENT_SHOWN);
        E.rsel = 0;
        E.dpage = 1;
        break;
    case 4: E.dpage = 2; break;
    case 5: E.show_nums = !E.show_nums; break;
    case 6: exit(0);
    }
}

static void recent_key(int c) {
    switch (c) {
    case ARROW_UP:
        if (E.nrecent) E.rsel = (E.rsel + E.nrecent - 1) % E.nrecent;
        return;
    case ARROW_DOWN:
        if (E.nrecent) E.rsel = (E.rsel + 1) % E.nrecent;
        return;
    case ESC: case ARROW_LEFT: case BACKSPACE: case 'q':
        E.dpage = 0;
        return;
    case '\r':
        if (E.nrecent) open_recent(E.rsel);
        return;
    }
    if (c >= '1' && c <= '8' && c - '1' < E.nrecent) open_recent(c - '1');
}

static void dash_key(int c) {
    if (E.dpage == 2) { E.dpage = 0; return; }
    if (E.dpage == 1) { recent_key(c); return; }
    int col = E.dsel / 4, row = E.dsel % 4;
    int sz = col == 0 ? 4 : NITEMS - 4;
    switch (c) {
    case ARROW_UP:
        if (E.twocol) E.dsel = col * 4 + (row + sz - 1) % sz;
        else E.dsel = (E.dsel + NITEMS - 1) % NITEMS;
        return;
    case ARROW_DOWN:
        if (E.twocol) E.dsel = col * 4 + (row + 1) % sz;
        else E.dsel = (E.dsel + 1) % NITEMS;
        return;
    case ARROW_LEFT: case ARROW_RIGHT:
        if (E.twocol) {
            int nc = 1 - col;
            int nsz = nc == 0 ? 4 : NITEMS - 4;
            E.dsel = nc * 4 + (row < nsz ? row : nsz - 1);
        }
        return;
    case '\r':
        dash_activate(E.dsel);
        return;
    case CTRL_('q'):
        exit(0);
    }
    if (c > 32 && c < 127) {
        int k = tolower(c);
        for (int i = 0; i < NITEMS; i++) {
            if (dash_items[i].key == k) { E.dsel = i; dash_activate(i); return; }
        }
    }
}

static void insert_char(int c) {
    do_op(OP_INS, E.cy, E.cx, (char)c);
}

static void insert_newline(void) {
    group_new();
    Row *r = &E.row[E.cy];
    int ind = 0;
    while (ind < E.cx && (r->s[ind] == ' ' || r->s[ind] == '\t')) ind++;
    char *indent = xrealloc(NULL, ind + 1);
    memcpy(indent, r->s, ind);
    do_op(OP_SPLIT, E.cy, E.cx, 0);
    for (int k = 0; k < ind; k++) do_op(OP_INS, E.cy, k, indent[k]);
    free(indent);
}

static void backspace(void) {
    group_new();
    if (E.cx > 0) {
        int x = E.cx;
        while (x > 0) {
            char b = E.row[E.cy].s[x - 1];
            do_op(OP_DEL, E.cy, x - 1, b);
            x--;
            if (!IS_CONT(b)) break;
        }
    } else if (E.cy > 0) {
        do_op(OP_JOIN, E.cy - 1, E.row[E.cy - 1].len, 0);
    }
}

static void delete_forward(void) {
    group_new();
    Row *r = &E.row[E.cy];
    if (E.cx < r->len) {
        do {
            do_op(OP_DEL, E.cy, E.cx, E.row[E.cy].s[E.cx]);
        } while (E.cx < E.row[E.cy].len && IS_CONT(E.row[E.cy].s[E.cx]));
    } else if (E.cy + 1 < E.nrows) {
        do_op(OP_JOIN, E.cy, r->len, 0);
    }
}

static void cut_line(void) {
    group_new();
    int y = E.cy;
    for (int x = E.row[y].len; x > 0; x--)
        do_op(OP_DEL, y, x - 1, E.row[y].s[x - 1]);
    if (y + 1 < E.nrows)      do_op(OP_JOIN, y, 0, 0);
    else if (y > 0)           do_op(OP_JOIN, y - 1, E.row[y - 1].len, 0);
}

static void move_word_left(void) {
    if (E.cx == 0) {
        if (E.cy > 0) { E.cy--; E.cx = E.row[E.cy].len; }
        return;
    }
    const char *s = E.row[E.cy].s;
    while (E.cx > 0 && isspace((unsigned char)s[E.cx - 1])) E.cx--;
    while (E.cx > 0 && !isspace((unsigned char)s[E.cx - 1])) E.cx--;
}

static void move_word_right(void) {
    Row *r = &E.row[E.cy];
    if (E.cx >= r->len) {
        if (E.cy + 1 < E.nrows) { E.cy++; E.cx = 0; }
        return;
    }
    while (E.cx < r->len && !isspace((unsigned char)r->s[E.cx])) E.cx++;
    while (E.cx < r->len && isspace((unsigned char)r->s[E.cx])) E.cx++;
}

static void process_key(int c) {
    static int quit_pending = 0;
    int was_pending = quit_pending;
    quit_pending = 0;
    int vertical = 0, typed = 0;

    switch (c) {
    case '\r':
        insert_newline();
        break;

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
    case CTRL_('g'): goto_line(); break;
    case CTRL_('k'): cut_line(); break;
    case CTRL_('n'): E.show_nums = !E.show_nums; break;
    case CTRL_('c'): set_msg("Ctrl-Q quits avtty"); break;

    case BACKSPACE:
    case CTRL_('h'):
        backspace();
        break;
    case DEL_KEY:
        delete_forward();
        break;

    case ARROW_LEFT:
        if (E.cx > 0) {
            E.cx--;
            while (E.cx > 0 && IS_CONT(E.row[E.cy].s[E.cx])) E.cx--;
        } else if (E.cy > 0) {
            E.cy--; E.cx = E.row[E.cy].len;
        }
        break;
    case ARROW_RIGHT: {
        Row *r = &E.row[E.cy];
        if (E.cx < r->len) {
            E.cx++;
            while (E.cx < r->len && IS_CONT(r->s[E.cx])) E.cx++;
        } else if (E.cy + 1 < E.nrows) {
            E.cy++; E.cx = 0;
        }
        break;
    }
    case ARROW_UP:
        if (E.cy > 0) E.cy--;
        vertical = 1;
        break;
    case ARROW_DOWN:
        if (E.cy + 1 < E.nrows) E.cy++;
        vertical = 1;
        break;
    case PAGE_UP:
        E.cy -= E.rows;
        if (E.cy < 0) E.cy = 0;
        vertical = 1;
        break;
    case PAGE_DOWN:
        E.cy += E.rows;
        if (E.cy >= E.nrows) E.cy = E.nrows - 1;
        vertical = 1;
        break;
    case HOME_KEY: case CTRL_('a'): E.cx = 0; break;
    case END_KEY:  case CTRL_('e'): E.cx = E.row[E.cy].len; break;
    case WORD_LEFT:  move_word_left(); break;
    case WORD_RIGHT: move_word_right(); break;

    default:
        if (c == '\t' || (c >= 32 && c < 256 && c != 127)) {
            if (!last_typing) group_new();
            insert_char(c);
            last_typing = !(c == ' ' || c == '\t');
            typed = 1;
        }
        break;
    }

    if (!typed) last_typing = 0;
    if (vertical) E.cx = rx_to_cx(&E.row[E.cy], E.want);
    else E.want = cx_to_rx(&E.row[E.cy], E.cx);
}

static void usage(FILE *o) {
    fputs(
"avtty - a very tiny text yard " AVTTY_VERSION "\n"
"\n"
"Usage:\n"
"  avtty                 start screen (menu: open, new, recent files...)\n"
"  avtty <file>          open <file> (new buffer if it doesn't exist yet)\n"
"  avtty -o <file>       open an existing file\n"
"  avtty -c <file>       create a new file (fails if it already exists)\n"
"  avtty -n [...]        start with line numbers on\n"
"  avtty -h | -v         show help | version\n"
"\n"
"Keys:\n"
"  Ctrl-S save        Ctrl-Q quit        Ctrl-Z undo       Ctrl-Y redo\n"
"  Ctrl-F find        Ctrl-G go to line  Ctrl-K cut line   Ctrl-N line numbers\n"
"  Arrows, Home/End (Ctrl-A/Ctrl-E), PgUp/PgDn, Del, Backspace,\n"
"  Ctrl+Left/Right to jump by word\n", o);
}

int main(int argc, char **argv) {
    const char *file = NULL;
    int mode = 0, nums = 0, only_files = 0;
    char err[256];

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!only_files && a[0] == '-' && a[1]) {
            if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
            if (!strcmp(a, "-v") || !strcmp(a, "--version")) { puts("avtty " AVTTY_VERSION); return 0; }
            if (!strcmp(a, "-n")) { nums = 1; continue; }
            if (!strcmp(a, "--")) { only_files = 1; continue; }
            if (!strcmp(a, "-c") || !strcmp(a, "-o")) {
                if (i + 1 >= argc) { fprintf(stderr, "avtty: %s needs a filename\n", a); return 1; }
                if (file) { fputs("avtty: only one file at a time\n", stderr); return 1; }
                mode = (a[1] == 'c') ? 2 : 1;
                file = argv[++i];
                continue;
            }
            fprintf(stderr, "avtty: unknown option '%s' (try -h)\n", a);
            return 1;
        }
        if (file) { fputs("avtty: only one file at a time\n", stderr); return 1; }
        file = a;
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("avtty: needs an interactive terminal\n", stderr);
        return 1;
    }

    E.promptcol = -1;
    E.show_nums = nums;

    if (file) {
        if (open_file_cmd(file, mode, err, sizeof err) != 0) {
            fprintf(stderr, "avtty: %s\n", err);
            return 1;
        }
    } else {
        E.dash = 1;
    }

    enable_raw();
    get_size();

    refresh_screen();
    for (;;) {
        int rs = E.screenrows, cs = E.screencols;
        int msg_was_visible = E.msg[0] != 0;
        get_size();
        int c = read_key();
        if (c == 0) {
            int expired = msg_was_visible && time(NULL) - E.msgtime >= 5;
            if (expired) E.msg[0] = 0;
            if (expired || rs != E.screenrows || cs != E.screencols) refresh_screen();
            continue;
        }
        if (E.dash) dash_key(c);
        else process_key(c);
        refresh_screen();
    }
}
