#define _GNU_SOURCE
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define CTRL(x) ((x) & 0x1f)

typedef struct {
    char *data;
    size_t len, cap;
} Line;

typedef struct {
    Line *lines;
    size_t len, cap;
} Buffer;

typedef struct {
    size_t row, col;
    char *text;
    size_t len;
} Snapshot;

static struct termios original;
static Buffer buffer;
static Snapshot *undo_stack, *redo_stack;
static size_t undo_len, redo_len;
static size_t cursor_row, cursor_col;
static size_t row_offset, col_offset;
static char *filename;
static int modified;

static void die(const char *message)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    perror(message);
    exit(1);
}

static void resize_array(void **array, size_t *capacity,
                         size_t needed, size_t element_size)
{
    if (needed <= *capacity)
        return;

    size_t capacity_new = *capacity ? *capacity * 2 : 16;

    while (capacity_new < needed)
        capacity_new *= 2;

    *array = realloc(*array, capacity_new * element_size);

    if (!*array)
        die("realloc");

    *capacity = capacity_new;
}

static void terminal_mode(int enable)
{
    if (!enable) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        return;
    }

    struct termios raw = original;

    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

static void handle_signal(int signal_number)
{
    (void)signal_number;
    terminal_mode(0);
    exit(1);
}

static void line_insert(size_t row, const char *text, size_t length)
{
    resize_array((void **)&buffer.lines, &buffer.cap,
                 buffer.len + 1, sizeof(*buffer.lines));

    memmove(buffer.lines + row + 1,
            buffer.lines + row,
            (buffer.len - row) * sizeof(*buffer.lines));

    Line *line = &buffer.lines[row];

    line->data = malloc(length + 1);
    if (!line->data)
        die("malloc");

    memcpy(line->data, text, length);
    line->data[length] = 0;
    line->len = length;
    line->cap = length + 1;

    buffer.len++;
}

static void line_delete(size_t row)
{
    free(buffer.lines[row].data);

    memmove(buffer.lines + row,
            buffer.lines + row + 1,
            (buffer.len - row - 1) * sizeof(*buffer.lines));

    buffer.len--;
}

static void line_reserve(Line *line, size_t needed)
{
    if (needed <= line->cap)
        return;

    size_t capacity = line->cap ? line->cap * 2 : 16;

    while (capacity < needed)
        capacity *= 2;

    line->data = realloc(line->data, capacity);

    if (!line->data)
        die("realloc");

    line->cap = capacity;
}

static void snapshot_free(Snapshot *snapshot)
{
    free(snapshot->text);
    snapshot->text = NULL;
}

static void clear_redo(void)
{
    for (size_t i = 0; i < redo_len; i++)
        snapshot_free(&redo_stack[i]);

    free(redo_stack);
    redo_stack = NULL;
    redo_len = 0;
}

static void snapshot_push(Snapshot **stack, size_t *length)
{
    size_t row = cursor_row;

    resize_array((void **)stack, length, *length + 1, sizeof(**stack));

    Snapshot *snapshot = &(*stack)[*length - 1];

    snapshot->row = row;
    snapshot->col = cursor_col;
    snapshot->len = buffer.lines[row].len;
    snapshot->text = malloc(snapshot->len + 1);

    if (!snapshot->text)
        die("malloc");

    memcpy(snapshot->text, buffer.lines[row].data, snapshot->len + 1);
}

static void save_undo(void)
{
    snapshot_push(&undo_stack, &undo_len);
    clear_redo();
}

static void restore_snapshot(Snapshot *snapshot)
{
    Line *line = &buffer.lines[snapshot->row];

    line_reserve(line, snapshot->len + 1);
    memcpy(line->data, snapshot->text, snapshot->len + 1);
    line->len = snapshot->len;

    cursor_row = snapshot->row;
    cursor_col = snapshot->col;

    if (cursor_col > line->len)
        cursor_col = line->len;
}

static void undo(void)
{
    if (!undo_len)
        return;

    Snapshot current = {
        cursor_row,
        cursor_col,
        NULL,
        buffer.lines[cursor_row].len
    };

    current.text = malloc(current.len + 1);
    if (!current.text)
        die("malloc");

    memcpy(current.text, buffer.lines[cursor_row].data, current.len + 1);

    Snapshot snapshot = undo_stack[--undo_len];
    resize_array((void **)&redo_stack, &redo_len,
                 redo_len + 1, sizeof(*redo_stack));
    redo_stack[redo_len++] = current;

    restore_snapshot(&snapshot);
    snapshot_free(&snapshot);
    modified = 1;
}

static void redo(void)
{
    if (!redo_len)
        return;

    Snapshot current = {
        cursor_row,
        cursor_col,
        NULL,
        buffer.lines[cursor_row].len
    };

    current.text = malloc(current.len + 1);
    if (!current.text)
        die("malloc");

    memcpy(current.text, buffer.lines[cursor_row].data, current.len + 1);

    Snapshot snapshot = redo_stack[--redo_len];
    resize_array((void **)&undo_stack, &undo_len,
                 undo_len + 1, sizeof(*undo_stack));
    undo_stack[undo_len++] = current;

    restore_snapshot(&snapshot);
    snapshot_free(&snapshot);
    modified = 1;
}

static void insert_character(char c)
{
    Line *line = &buffer.lines[cursor_row];

    save_undo();
    line_reserve(line, line->len + 2);

    memmove(line->data + cursor_col + 1,
            line->data + cursor_col,
            line->len - cursor_col + 1);

    line->data[cursor_col++] = c;
    line->len++;
    modified = 1;
}

static void backspace(void)
{
    Line *line = &buffer.lines[cursor_row];

    if (cursor_col) {
        save_undo();

        memmove(line->data + cursor_col - 1,
                line->data + cursor_col,
                line->len - cursor_col + 1);

        line->len--;
        cursor_col--;
        modified = 1;
        return;
    }

    if (!cursor_row)
        return;

    save_undo();

    size_t previous_length = buffer.lines[cursor_row - 1].len;
    Line *previous = &buffer.lines[cursor_row - 1];

    line_reserve(previous, previous_length + line->len + 1);
    memcpy(previous->data + previous_length,
           line->data,
           line->len + 1);

    previous->len += line->len;
    line_delete(cursor_row);

    cursor_row--;
    cursor_col = previous_length;
    modified = 1;
}

static void delete_character(void)
{
    Line *line = &buffer.lines[cursor_row];

    if (cursor_col < line->len) {
        save_undo();

        memmove(line->data + cursor_col,
                line->data + cursor_col + 1,
                line->len - cursor_col);

        line->len--;
        modified = 1;
        return;
    }

    if (cursor_row + 1 >= buffer.len)
        return;

    save_undo();

    Line *next = &buffer.lines[cursor_row + 1];
    line_reserve(line, line->len + next->len + 1);

    memcpy(line->data + line->len,
           next->data,
           next->len + 1);

    line->len += next->len;
    line_delete(cursor_row + 1);
    modified = 1;
}

static void newline(void)
{
    Line *line = &buffer.lines[cursor_row];
    size_t right_length = line->len - cursor_col;

    save_undo();

    line_insert(cursor_row + 1,
                line->data + cursor_col,
                right_length);

    line->len = cursor_col;
    line->data[cursor_col] = 0;

    cursor_row++;
    cursor_col = 0;
    modified = 1;
}

static void load_file(const char *path)
{
    FILE *file = fopen(path, "r");

    if (!file) {
        if (errno != ENOENT)
            die("fopen");

        line_insert(0, "", 0);
        return;
    }

    char *line = NULL;
    size_t capacity = 0;

    while (getline(&line, &capacity, file) != -1) {
        size_t length = strlen(line);

        while (length && (line[length - 1] == '\n' ||
                          line[length - 1] == '\r'))
            line[--length] = 0;

        line_insert(buffer.len, line, length);
    }

    free(line);
    fclose(file);

    if (!buffer.len)
        line_insert(0, "", 0);
}

static int save_file(void)
{
    char *temporary = malloc(strlen(filename) + 16);

    if (!temporary)
        die("malloc");

    sprintf(temporary, "%s.avtty.tmp", filename);

    int output = open(temporary,
                      O_WRONLY | O_CREAT | O_TRUNC,
                      0600);

    if (output == -1) {
        free(temporary);
        return 0;
    }

    struct stat information;

    if (stat(filename, &information) == 0)
        fchmod(output, information.st_mode & 0777);

    for (size_t i = 0; i < buffer.len; i++) {
        Line *line = &buffer.lines[i];

        if (write(output, line->data, line->len) != (ssize_t)line->len ||
            write(output, "\n", 1) != 1) {
            close(output);
            unlink(temporary);
            free(temporary);
            return 0;
        }
    }

    if (fsync(output) == -1) {
        close(output);
        unlink(temporary);
        free(temporary);
        return 0;
    }

    close(output);

    if (rename(temporary, filename) == -1) {
        unlink(temporary);
        free(temporary);
        return 0;
    }

    free(temporary);
    modified = 0;
    return 1;
}

static int read_key(void)
{
    unsigned char c;

    if (read(STDIN_FILENO, &c, 1) != 1)
        return -1;

    if (c != 27)
        return c;

    unsigned char sequence[4];
    int length = 0;

    while (length < 4 &&
           read(STDIN_FILENO, sequence + length, 1) == 1)
        length++;

    if (length >= 2 && sequence[0] == '[') {
        if (sequence[1] == 'A') return 1000;
        if (sequence[1] == 'B') return 1001;
        if (sequence[1] == 'C') return 1002;
        if (sequence[1] == 'D') return 1003;
        if (sequence[1] == 'H') return 1004;
        if (sequence[1] == 'F') return 1005;
        if (sequence[1] == '3' && length >= 3 &&
            sequence[2] == '~') return 1006;
    }

    return 27;
}

static void update_scroll(size_t height, size_t width)
{
    if (cursor_row < row_offset)
        row_offset = cursor_row;

    if (cursor_row >= row_offset + height)
        row_offset = cursor_row - height + 1;

    if (cursor_col < col_offset)
        col_offset = cursor_col;

    if (cursor_col >= col_offset + width)
        col_offset = cursor_col - width + 1;
}

static void status(const char *message, size_t width)
{
    char text[256];

    snprintf(text, sizeof(text),
             " %s%s  %zu:%zu  %s",
             filename,
             modified ? " *" : "",
             cursor_row + 1,
             cursor_col + 1,
             message);

    printf("\033[7m%-*.*s\033[0m",
           (int)width, (int)width, text);
}

static void draw(void)
{
    struct winsize size;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == -1)
        return;

    size_t height = size.ws_row > 1 ? size.ws_row - 1 : 1;
    size_t width = size.ws_col ? size.ws_col : 80;

    update_scroll(height, width);

    printf("\033[H\033[2J");

    for (size_t screen_row = 0; screen_row < height; screen_row++) {
        size_t row = row_offset + screen_row;

        if (row < buffer.len) {
            Line *line = &buffer.lines[row];

            if (col_offset < line->len) {
                size_t length = line->len - col_offset;

                if (length > width)
                    length = width;

                write(STDOUT_FILENO,
                      line->data + col_offset,
                      length);
            }
        }

        if (screen_row + 1 < height)
            putchar('\n');
    }

    printf("\033[%hu;1H", size.ws_row);
    status("Ctrl-S save  Ctrl-Q quit  Ctrl-F find  Ctrl-G goto  Ctrl-Z undo  Ctrl-Y redo",
           width);

    size_t screen_y = cursor_row - row_offset + 1;
    size_t screen_x = cursor_col - col_offset + 1;

    printf("\033[%zu;%zuH", screen_y, screen_x);
    fflush(stdout);
}

static void message(const char *text)
{
    struct winsize size;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == -1)
        return;

    printf("\033[%d;1H\033[2K", size.ws_row);
    status(text, size.ws_col);
    printf("\033[%zu;%zuH",
           cursor_row - row_offset + 1,
           cursor_col - col_offset + 1);
    fflush(stdout);
}

static void search(void)
{
    char query[256];
    size_t length = 0;

    message("Search: ");

    while (length + 1 < sizeof(query)) {
        int c = read_key();

        if (c == 27)
            return;

        if (c == '\r' || c == '\n')
            break;

        if (c == 127 || c == 8) {
            if (length)
                query[--length] = 0;
            continue;
        }

        if (c >= 32 && c < 127)
            query[length++] = c;

        query[length] = 0;
    }

    if (!length)
        return;

    for (size_t pass = 0; pass < buffer.len; pass++) {
        size_t row = (cursor_row + pass) % buffer.len;
        char *match = strstr(buffer.lines[row].data, query);

        if (match) {
            cursor_row = row;
            cursor_col = (size_t)(match - buffer.lines[row].data);
            return;
        }
    }

    message("Not found");
}

static void goto_line(void)
{
    char input[32];
    size_t length = 0;

    message("Goto line: ");

    while (length + 1 < sizeof(input)) {
        int c = read_key();

        if (c == 27)
            return;

        if (c == '\r' || c == '\n')
            break;

        if (c == 127 || c == 8) {
            if (length)
                input[--length] = 0;
            continue;
        }

        if (c >= '0' && c <= '9')
            input[length++] = c;

        input[length] = 0;
    }

    if (!length)
        return;

    size_t row = strtoul(input, NULL, 10);

    if (!row)
        return;

    if (row > buffer.len)
        row = buffer.len;

    cursor_row = row - 1;

    if (cursor_col > buffer.lines[cursor_row].len)
        cursor_col = buffer.lines[cursor_row].len;
}

static void help(void)
{
    puts("AVTTY - A Very Tiny Text Yard");
    puts("");
    puts("Usage:");
    puts("  avtty <file>");
    puts("");
    puts("Keys:");
    puts("  Ctrl-S       Save");
    puts("  Ctrl-Q       Quit");
    puts("  Ctrl-F       Search");
    puts("  Ctrl-G       Go to line");
    puts("  Ctrl-Z       Undo");
    puts("  Ctrl-Y       Redo");
    puts("  Arrow keys   Move");
    puts("  Home/End     Start/end of line");
    puts("  Backspace    Delete backwards");
    puts("  Delete       Delete forwards");
    puts("  Enter        New line");
}

static void edit(void)
{
    for (;;) {
        draw();

        int key = read_key();

        if (key == -1)
            break;

        if (key == CTRL('Q')) {
            if (!modified)
                break;

            message("Unsaved changes. Ctrl-Q again to quit.");
            int second = read_key();

            if (second == CTRL('Q'))
                break;

            continue;
        }

        if (key == CTRL('S')) {
            if (!save_file())
                message("Save failed");
            continue;
        }

        if (key == CTRL('F')) {
            search();
            continue;
        }

        if (key == CTRL('G')) {
            goto_line();
            continue;
        }

        if (key == CTRL('Z')) {
            undo();
            continue;
        }

        if (key == CTRL('Y')) {
            redo();
            continue;
        }

        if (key == 1000) {
            if (cursor_row)
                cursor_row--;

            if (cursor_col > buffer.lines[cursor_row].len)
                cursor_col = buffer.lines[cursor_row].len;

            continue;
        }

        if (key == 1001) {
            if (cursor_row + 1 < buffer.len)
                cursor_row++;

            if (cursor_col > buffer.lines[cursor_row].len)
                cursor_col = buffer.lines[cursor_row].len;

            continue;
        }

        if (key == 1002) {
            if (cursor_col < buffer.lines[cursor_row].len)
                cursor_col++;

            continue;
        }

        if (key == 1003) {
            if (cursor_col)
                cursor_col--;

            continue;
        }

        if (key == 1004) {
            cursor_col = 0;
            continue;
        }

        if (key == 1005) {
            cursor_col = buffer.lines[cursor_row].len;
            continue;
        }

        if (key == 1006) {
            delete_character();
            continue;
        }

        if (key == 127 || key == 8) {
            backspace();
            continue;
        }

        if (key == '\r' || key == '\n') {
            newline();
            continue;
        }

        if (key >= 32 && key <= 126)
            insert_character((char)key);
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        help();
        return 0;
    }

    if (argc != 2) {
        fprintf(stderr, "usage: avtty <file>\n");
        return 1;
    }

    filename = argv[1];

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "avtty: stdin and stdout must be terminals\n");
        return 1;
    }

    if (tcgetattr(STDIN_FILENO, &original) == -1)
        die("tcgetattr");

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);

    load_file(filename);
    terminal_mode(1);
    edit();
    terminal_mode(0);

    for (size_t i = 0; i < buffer.len; i++)
        free(buffer.lines[i].data);

    for (size_t i = 0; i < undo_len; i++)
        snapshot_free(&undo_stack[i]);

    for (size_t i = 0; i < redo_len; i++)
        snapshot_free(&redo_stack[i]);

    free(buffer.lines);
    free(undo_stack);
    free(redo_stack);

    return 0;
}
