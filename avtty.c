#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define AVTTY_VERSION "0.1.0"
#define TAB_STOP 4
#define CTRL_(k) ((k) & 0x1f)
#define ESC 27
#define IS_CONT(b) ((((unsigned char)(b)) & 0xC0) == 0x80)

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
    struct termios orig; int raw;
} E;

static int last_typing = 0;
static const char *prompt_note = "";

struct abuf { char *b; int len, cap; };

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

static void refresh_screen(void) {
    get_size();
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
"  avtty                 empty, unnamed buffer\n"
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
    int create = 0, must_exist = 0, nums = 0, only_files = 0;

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
                if (a[1] == 'c') create = 1; else must_exist = 1;
                file = argv[++i];
                continue;
            }
            fprintf(stderr, "avtty: unknown option '%s' (try -h)\n", a);
            return 1;
        }
        if (file) { fputs("avtty: only one file at a time\n", stderr); return 1; }
        file = a;
    }

    struct stat st;
    int exists = file && stat(file, &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) {
        fprintf(stderr, "avtty: '%s' is a directory\n", file);
        return 1;
    }
    if (create) {
        if (exists) {
            fprintf(stderr, "avtty: '%s' already exists (use -o to open it)\n", file);
            return 1;
        }
        FILE *f = fopen(file, "wx");
        if (!f) { fprintf(stderr, "avtty: can't create '%s': %s\n", file, strerror(errno)); return 1; }
        fclose(f);
    } else if (must_exist && !exists) {
        fprintf(stderr, "avtty: '%s' does not exist (use -c to create it)\n", file);
        return 1;
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("avtty: needs an interactive terminal\n", stderr);
        return 1;
    }

    E.saved = 0;
    E.promptcol = -1;
    E.show_nums = nums;
    int new_buffer = 0;

    if (file) {
        E.filename = strdup(file);
        if (exists || create) {
            if (load_file(file) == -1) {
                fprintf(stderr, "avtty: can't read '%s': %s\n", file, strerror(errno));
                return 1;
            }
        } else {
            new_buffer = 1;
        }
    }
    if (E.nrows == 0) insert_row(0, "", 0);

    enable_raw();
    get_size();
    if (new_buffer) set_msg("New file: %s  (Ctrl-S to save)", file);
    else set_msg("Ctrl: S save | Q quit | Z undo | Y redo | F find | G goto | K cut line | N numbers");

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
        process_key(c);
        refresh_screen();
    }
}
