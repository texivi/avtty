#define _DEFAULT_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
    char *buf;
    size_t cap, lo, hi;
} Text;

typedef struct {
    int type;
    size_t pos, len;
    char *data;
} Op;

typedef struct {
    Op *v;
    size_t len, cap;
} Stack;

static struct termios oldt;
static int raw_on;
static Text text;
static Stack undo, redo;
static size_t cur, top_line, left_col;
static int dirty;
static size_t saved_undo;
static const char *name;

static size_t right_len(void) { return text.cap - text.hi; }
static size_t file_len(void) { return text.lo + right_len(); }
static char at(size_t p) {
    return p < text.lo ? text.buf[p] : text.buf[text.hi + p - text.lo];
}

static void die(const char *s) {
    if (raw_on) tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
    fprintf(stderr, "avtty: %s\n", s);
    exit(1);
}

static void raw_off(void) {
    if (raw_on) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
        raw_on = 0;
    }
}

static void on_sig(int sig) {
    (void)sig;
    raw_off();
    _exit(128 + sig);
}

static void raw_on_now(void) {
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &oldt) < 0 || tcgetattr(STDIN_FILENO, &t) < 0)
        die("cannot read terminal settings");
    t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= ~(OPOST);
    t.c_cflag |= CS8;
    t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) < 0)
        die("cannot enter raw mode");
    raw_on = 1;
}

static void stack_clear(Stack *s) {
    for (size_t i = 0; i < s->len; i++) free(s->v[i].data);
    s->len = 0;
}

static void stack_push(Stack *s, int type, size_t pos, const char *data, size_t len) {
    if (s->len == s->cap) {
        size_t n = s->cap ? s->cap * 2 : 32;
        Op *v = realloc(s->v, n * sizeof(*v));
        if (!v) die("out of memory");
        s->v = v;
        s->cap = n;
    }
    s->v[s->len].type = type;
    s->v[s->len].pos = pos;
    s->v[s->len].len = len;
    s->v[s->len].data = NULL;
    if (len) {
        s->v[s->len].data = malloc(len);
        if (!s->v[s->len].data) die("out of memory");
        memcpy(s->v[s->len].data, data, len);
    }
    s->len++;
}

static void move_gap(size_t p) {
    if (p < text.lo) {
        size_t n = text.lo - p;
        memmove(text.buf + text.hi - n, text.buf + p, n);
        text.lo = p;
        text.hi -= n;
    } else if (p > text.lo) {
        size_t n = p - text.lo;
        memmove(text.buf + text.lo, text.buf + text.hi, n);
        text.lo += n;
        text.hi += n;
    }
}

static void gap_need(size_t n) {
    size_t gap = text.hi - text.lo;
    if (gap >= n) return;
    size_t need = file_len() + n;
    size_t cap = text.cap ? text.cap : 1024;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) die("file too large");
        cap *= 2;
    }
    char *b = malloc(cap);
    if (!b) die("out of memory");
    memcpy(b, text.buf, text.lo);
    size_t r = right_len();
    memcpy(b + cap - r, text.buf + text.hi, r);
    free(text.buf);
    text.buf = b;
    text.hi = cap - r;
    text.cap = cap;
}

static void raw_insert(size_t p, const char *s, size_t n) {
    move_gap(p);
    gap_need(n);
    memcpy(text.buf + text.lo, s, n);
    text.lo += n;
}

static void raw_delete(size_t p, size_t n) {
    move_gap(p);
    text.hi += n;
}

static void edit_insert(size_t p, const char *s, size_t n) {
    raw_insert(p, s, n);
    stack_clear(&redo);
    stack_push(&undo, 1, p, s, n);
    dirty = 1;
}

static void edit_delete(size_t p, size_t n) {
    char *s = malloc(n);
    if (!s) die("out of memory");
    for (size_t i = 0; i < n; i++) s[i] = at(p + i);
    raw_delete(p, n);
    stack_clear(&redo);
    stack_push(&undo, 0, p, s, n);
    free(s);
    dirty = 1;
}

static void undo_one(void) {
    if (!undo.len) return;
    Op *o = &undo.v[undo.len - 1];
    if (o->type == 1) raw_delete(o->pos, o->len);
    else raw_insert(o->pos, o->data, o->len);
    stack_push(&redo, o->type, o->pos, o->data, o->len);
    cur = o->pos;
    undo.len--;
    free(o->data);
    o->data = NULL;
    dirty = undo.len != saved_undo;
}

static void redo_one(void) {
    if (!redo.len) return;
    Op *o = &redo.v[redo.len - 1];
    if (o->type == 1) raw_insert(o->pos, o->data, o->len);
    else raw_delete(o->pos, o->len);
    stack_push(&undo, o->type, o->pos, o->data, o->len);
    cur = o->type == 1 ? o->pos + o->len : o->pos;
    redo.len--;
    free(o->data);
    o->data = NULL;
    dirty = 1;
}

static int line_col(size_t p, size_t *line, size_t *col) {
    size_t l = 0, c = 0;
    for (size_t i = 0; i < p; i++) {
        if (at(i) == '\n') l++, c = 0;
        else c++;
    }
    *line = l;
    *col = c;
    return 0;
}

static size_t line_start(size_t line) {
    size_t l = 0;
    for (size_t i = 0; i < file_len(); i++) {
        if (l == line) return i;
        if (at(i) == '\n') l++;
    }
    return file_len();
}

static size_t line_end(size_t p) {
    while (p < file_len() && at(p) != '\n') p++;
    return p;
}

static void term_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0 || !ws.ws_row || !ws.ws_col) {
        *rows = 24; *cols = 80;
    } else {
        *rows = ws.ws_row; *cols = ws.ws_col;
    }
}

static void adjust_view(void) {
    size_t line, col;
    int rows, cols;
    line_col(cur, &line, &col);
    term_size(&rows, &cols);
    size_t body = rows > 2 ? (size_t)rows - 2 : 1;
    if (line < top_line) top_line = line;
    if (line >= top_line + body) top_line = line - body + 1;
    if (col < left_col) left_col = col;
    if (col >= left_col + (size_t)(cols ? cols - 1 : 1))
        left_col = col - (cols ? cols - 1 : 1) + 1;
}

static void draw(void) {
    int rows, cols;
    term_size(&rows, &cols);
    adjust_view();
    size_t body = rows > 2 ? (size_t)rows - 2 : 1;

    printf("\033[H\033[2J");
    size_t p = line_start(top_line);
    for (size_t r = 0; r < body; r++) {
        printf("\033[%zu;1H", r + 1);
        size_t q = p, col = 0;
        while (q < file_len() && at(q) != '\n' && col < left_col) q++, col++;
        size_t shown = 0;
        while (q < file_len() && at(q) != '\n' && shown < (size_t)cols) {
            unsigned char c = (unsigned char)at(q++);
            if (c == '\t') {
                int n = 4 - ((int)(left_col + shown) % 4);
                while (n-- > 0 && shown < (size_t)cols) putchar(' '), shown++;
            } else if (c >= 32 && c != 127) {
                putchar(c); shown++;
            } else {
                putchar('?'); shown++;
            }
        }
        printf("\033[K");
        while (p < file_len() && at(p) != '\n') p++;
        if (p < file_len() && at(p) == '\n') p++;
        else if (p >= file_len()) break;
    }

    printf("\033[%d;1H\033[K", rows - 1);
    printf("%s  %zu bytes%s", name ? name : "[No Name]", file_len(), dirty ? "  [modified]" : "");
    printf("\033[%d;1H\033[K", rows);
    printf("Ctrl-S save  Ctrl-Q quit  Ctrl-F find  Ctrl-G goto  Ctrl-Z undo  Ctrl-Y redo");

    size_t line, col;
    line_col(cur, &line, &col);
    size_t x = col >= left_col ? col - left_col + 1 : 1;
    size_t y = line >= top_line ? line - top_line + 1 : 1;
    if (x > (size_t)cols) x = cols;
    if (y > body) y = body;
    printf("\033[%zu;%zuH", y, x);
    fflush(stdout);
}

static void move_vertical(int dir) {
    size_t line, col;
    line_col(cur, &line, &col);
    if (dir < 0) {
        if (!line) return;
        size_t start = line_start(line - 1);
        size_t end = line_end(start);
        size_t n = end - start;
        cur = start + (col < n ? col : n);
    } else {
        size_t start = line_start(line);
        size_t end = line_end(start);
        if (end >= file_len()) return;
        start = end + 1;
        end = line_end(start);
        size_t n = end - start;
        cur = start + (col < n ? col : n);
    }
}

static int read_key(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c != 27) return c;
    unsigned char a, b;
    if (read(STDIN_FILENO, &a, 1) != 1) return 27;
    if (a == '[' && read(STDIN_FILENO, &b, 1) == 1) {
        if (b == 'A') return 1001;
        if (b == 'B') return 1002;
        if (b == 'C') return 1003;
        if (b == 'D') return 1004;
        if (b == 'H') return 1005;
        if (b == 'F') return 1006;
        if (b == '3') { read(STDIN_FILENO, &b, 1); if (b == '~') return 1007; }
    }
    return 27;
}

static void prompt(const char *label, char *out, size_t cap) {
    size_t n = 0;
    int rows, cols;
    term_size(&rows, &cols);
    printf("\033[%d;1H\033[K%s", rows, label);
    fflush(stdout);
    for (;;) {
        int k = read_key();
        if (k == '\r' || k == '\n') break;
        if (k == 27 || k == 3) { out[0] = 0; return; }
        if ((k == 127 || k == 8) && n) n--;
        else if (k >= 32 && k < 127 && n + 1 < cap) out[n++] = (char)k;
        out[n] = 0;
        printf("\033[%d;1H\033[K%s%s", rows, label, out);
        fflush(stdout);
    }
}

static void find_text(void) {
    char q[256];
    prompt("Find: ", q, sizeof(q));
    size_t n = strlen(q);
    if (!n) return;
    size_t len = file_len();
    for (size_t p = cur + 1; p + n <= len; p++) {
        size_t i;
        for (i = 0; i < n && at(p + i) == q[i]; i++);
        if (i == n) { cur = p; return; }
    }
    for (size_t p = 0; p + n <= len && p <= cur; p++) {
        size_t i;
        for (i = 0; i < n && at(p + i) == q[i]; i++);
        if (i == n) { cur = p; return; }
    }
}

static void goto_line(void) {
    char s[32];
    prompt("Line: ", s, sizeof(s));
    char *e;
    unsigned long long n = strtoull(s, &e, 10);
    if (e == s || n == 0) return;
    cur = line_start((size_t)(n - 1));
}

static int save(void) {
    int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -1;
    size_t n = file_len();
    char out[8192];
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > sizeof(out)) chunk = sizeof(out);
        for (size_t i = 0; i < chunk; i++) out[i] = at(off + i);
        size_t wrote = 0;
        while (wrote < chunk) {
            ssize_t w = write(fd, out + wrote, chunk - wrote);
            if (w <= 0) { close(fd); return -1; }
            wrote += (size_t)w;
        }
        off += chunk;
    }
    if (close(fd) < 0) return -1;
    saved_undo = undo.len;
    dirty = 0;
    return 0;
}

static int load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno != ENOENT) return -1;
        text.cap = 1024;
        text.buf = malloc(text.cap);
        if (!text.buf) return -1;
        text.lo = 0;
        text.hi = text.cap;
        return 0;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 0) { close(fd); return -1; }
    size_t n = (size_t)st.st_size;
    text.cap = n + 1024 > n ? n + 1024 : n;
    if (text.cap < 1024) text.cap = 1024;
    text.buf = malloc(text.cap);
    if (!text.buf) { close(fd); return -1; }
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, text.buf + off, n - off);
        if (r <= 0) { free(text.buf); close(fd); return -1; }
        off += (size_t)r;
    }
    close(fd);
    text.lo = n;
    text.hi = text.cap;
    return 0;
}

static void help(void) {
    puts("AVTTY - A Very Tiny Text Yard");
    puts("Usage: avtty [file]");
    puts("\nCtrl-S  save\nCtrl-Q  quit\nCtrl-F  find\nCtrl-G  goto line\nCtrl-Z  undo\nCtrl-Y  redo");
}

int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) { help(); return 0; }
    if (argc > 2) { fprintf(stderr, "usage: avtty [file]\n"); return 1; }
    name = argc == 2 ? argv[1] : NULL;
    if (!load(name ? name : "")) {}
    else die("cannot open file");

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGHUP, on_sig);
    raw_on_now();
    draw();
    for (;;) {
        int k = read_key();
        if (k < 0) break;
        size_t len = file_len();
        if (k == 17) {
            if (dirty) {
                int rows, cols; term_size(&rows, &cols);
                printf("\033[%d;1H\033[KUnsaved changes. Ctrl-Q again to quit.", rows);
                fflush(stdout);
                int n = read_key();
                if (n != 17) { draw(); continue; }
            }
            break;
        } else if (k == 19) {
            if (!name) { draw(); continue; }
            if (save() < 0) {
                int rows, cols; term_size(&rows, &cols);
                printf("\033[%d;1H\033[KSave failed: %s", rows, strerror(errno)); fflush(stdout);
            }
        } else if (k == 6) find_text();
        else if (k == 7) goto_line();
        else if (k == 26) undo_one();
        else if (k == 25) redo_one();
        else if (k == 1001) {
            move_vertical(-1);
        } else if (k == 1002) {
            move_vertical(1);
        } else if (k == 1003) {
            if (cur < len) cur++;
        } else if (k == 1004) {
            if (cur) cur--;
        } else if (k == 1005) {
            size_t p = cur; while (p && at(p - 1) != '\n') p--; cur = p;
        } else if (k == 1006) {
            cur = line_end(cur);
        } else if (k == 1007) {
            if (cur < len) edit_delete(cur, 1);
        } else if (k == 127 || k == 8) {
            if (cur) { edit_delete(cur - 1, 1); cur--; }
        } else if (k == '\r' || k == '\n') {
            char c = '\n'; edit_insert(cur, &c, 1); cur++;
        } else if (k >= 32 && k < 127) {
            char c = (char)k; edit_insert(cur, &c, 1); cur++;
        }
        draw();
    }
    raw_off();
    printf("\033[2J\033[H");
    stack_clear(&undo); stack_clear(&redo); free(undo.v); free(redo.v); free(text.buf);
    return 0;
}
