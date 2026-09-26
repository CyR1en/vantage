#include "vantage.h"
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
    struct termios original;
    struct sigaction handlers[6];
    size_t handler_count;
    bool raw, screen, cleanup_error;
} Terminal;

static Terminal terminal;
static bool cleanup_registered;
static volatile sig_atomic_t exit_signal, resized;
static const int handled_signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE, SIGWINCH};

static void handle_signal(int signal_number) {
    if (signal_number == SIGWINCH) {
        resized = 1;
    } else if (!exit_signal) {
        exit_signal = signal_number;
    }
}

static bool write_all(int fd, const char *text, size_t length) {
    while (length) {
        ssize_t count = write(fd, text, length);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        text += count;
        length -= (size_t)count;
    }
    return true;
}

static void restore_terminal(void) {
    if (terminal.raw) {
        int result;
        do {
            result = tcsetattr(STDIN_FILENO, TCSANOW, &terminal.original);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            terminal.cleanup_error = true;
        }
        terminal.raw = false;
    }
    if (terminal.screen) {
        static const char restore[] = "\033[0m\033[?25h\033[?1049l";
        if (fflush(stdout) == EOF) {
            terminal.cleanup_error = true;
        }
        if (!write_all(STDOUT_FILENO, restore, sizeof(restore) - 1)) {
            terminal.cleanup_error = true;
        }
        terminal.screen = false;
    }
    while (terminal.handler_count) {
        size_t index = --terminal.handler_count;
        if (sigaction(handled_signals[index], &terminal.handlers[index], NULL) < 0) {
            terminal.cleanup_error = true;
        }
    }
}

static bool prepare_terminal(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        errno = ENOTTY;
        return false;
    }
    if (tcgetattr(STDIN_FILENO, &terminal.original) < 0) {
        return false;
    }
    if (!cleanup_registered) {
        if (atexit(restore_terminal)) {
            errno = ENOMEM;
            return false;
        }
        cleanup_registered = true;
    }
    struct sigaction action = {0};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(handled_signals) / sizeof(*handled_signals); ++i) {
        if (sigaction(handled_signals[i], &action, &terminal.handlers[i]) < 0) {
            return false;
        }
        ++terminal.handler_count;
    }
    struct termios raw = terminal.original;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL | INLCR | IGNCR | ISTRIP);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    terminal.raw = true;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        return false;
    }
    terminal.screen = true;
    if (fputs("\033[?1049h\033[?25l", stdout) == EOF || fflush(stdout) == EOF) {
        return false;
    }
    return true;
}

typedef struct {
    size_t directory, tree_selection, file_selection, tree_scroll, file_scroll;
    bool files;
} Browser;

typedef enum {
    KEY_NONE,
    KEY_UP,
    KEY_DOWN,
    KEY_OPEN,
    KEY_PARENT,
    KEY_FILES,
    KEY_ROOT,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_QUIT,
    KEY_EOF,
    KEY_ERROR
} Key;

static int read_byte(unsigned char *byte, int milliseconds) {
    if (exit_signal) {
        return 0;
    }
    fd_set readers;
    FD_ZERO(&readers);
    FD_SET(STDIN_FILENO, &readers);
    struct timeval timeout = {.tv_sec = milliseconds / 1000,
                              .tv_usec = (milliseconds % 1000) * 1000};
    int ready = select(STDIN_FILENO + 1, &readers, NULL, NULL, &timeout);
    if (ready < 0) {
        return errno == EINTR ? 0 : -1;
    }
    if (!ready) {
        return 0;
    }
    ssize_t count = read(STDIN_FILENO, byte, 1);
    if (count < 0) {
        return errno == EINTR || errno == EAGAIN ? 0 : -1;
    }
    return count ? 1 : -2;
}

static Key escape_key(void) {
    unsigned char byte;
    int result = read_byte(&byte, 40);
    if (result == -1) {
        return KEY_ERROR;
    }
    if (result == -2) {
        return KEY_EOF;
    }
    if (!result) {
        return exit_signal || resized ? KEY_NONE : KEY_QUIT;
    }
    if (byte != '[' && byte != 'O') {
        return KEY_NONE;
    }
    unsigned parameter = 0;
    bool first_parameter = true;
    for (unsigned count = 0; count < 24; ++count) {
        result = read_byte(&byte, 40);
        if (result == -1) {
            return KEY_ERROR;
        }
        if (result == -2) {
            return KEY_EOF;
        }
        if (!result) {
            return KEY_NONE;
        }
        if (byte >= '0' && byte <= '9' && first_parameter) {
            if (parameter < 1000) {
                parameter = parameter * 10 + (unsigned)(byte - '0');
            }
        } else if (byte == ';') {
            first_parameter = false;
        } else if (byte >= '@' && byte <= '~') {
            switch (byte) {
            case 'A':
                return KEY_UP;
            case 'B':
                return KEY_DOWN;
            case 'C':
                return KEY_OPEN;
            case 'D':
                return KEY_PARENT;
            case 'H':
                return KEY_HOME;
            case 'F':
                return KEY_END;
            case '~':
                if (parameter == 1 || parameter == 7) {
                    return KEY_HOME;
                }
                if (parameter == 4 || parameter == 8) {
                    return KEY_END;
                }
                if (parameter == 5) {
                    return KEY_PAGE_UP;
                }
                if (parameter == 6) {
                    return KEY_PAGE_DOWN;
                }
                return KEY_NONE;
            default:
                return KEY_NONE;
            }
        }
    }
    return KEY_NONE;
}

static Key next_key(void) {
    unsigned char byte;
    int result = read_byte(&byte, 150);
    if (result == -1) {
        return KEY_ERROR;
    }
    if (result == -2) {
        return KEY_EOF;
    }
    if (!result) {
        return KEY_NONE;
    }
    switch (byte) {
    case 3:
        exit_signal = SIGINT;
        return KEY_NONE;
    case 26:
        return KEY_QUIT; /* Leave a usable shell instead of suspending a raw tty. */
    case 27:
        return escape_key();
    case 'q':
    case 'Q':
        return KEY_QUIT;
    case 'j':
        return KEY_DOWN;
    case 'k':
        return KEY_UP;
    case '\r':
    case '\n':
    case 'l':
        return KEY_OPEN;
    case 8:
    case 127:
    case 'h':
        return KEY_PARENT;
    case 'f':
        return KEY_FILES;
    case 'g':
        return KEY_ROOT;
    default:
        return KEY_NONE;
    }
}

static void window_size(int *rows, int *columns) {
    struct winsize size = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0) {
        (void)ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
    }
    *rows = size.ws_row ? size.ws_row : 24;
    /* Leave the final column free so an autowrap cannot scroll the footer. */
    *columns = (size.ws_col ? size.ws_col : 80) - 1;
}

static int ascii(const char *text, int remaining) {
    if (remaining <= 0) {
        return 0;
    }
    size_t length = strlen(text);
    if (length > (size_t)remaining) {
        length = (size_t)remaining;
    }
    (void)fwrite(text, 1, length, stdout);
    return (int)length;
}

static void line(int row) {
    (void)fprintf(stdout, "\033[%d;1H\033[2K", row);
}

static void print_path(const VantageView *view, size_t entry, const char *label, int columns) {
    int remaining = columns - ascii(label, columns);
    size_t root_length = strlen(view->config->root);
    remaining -= vantage_print_text(stdout, (const unsigned char *)view->config->root, root_length,
                                    remaining);
    if (entry == view->root || remaining <= 0) {
        return;
    }
    if (!root_length || view->config->root[root_length - 1] != '/') {
        remaining -= ascii("/", remaining);
    }
    char *path = vantage_path(view, entry);
    if (path) {
        (void)vantage_print_text(stdout, (const unsigned char *)path, strlen(path), remaining);
        vs_free(path);
    } else {
        (void)ascii("(unresolved)", remaining);
    }
}

static size_t list_count(const VantageView *view, const Browser *browser) {
    return browser->files
               ? view->file_count
               : view->offsets[browser->directory + 1] - view->offsets[browser->directory];
}

static size_t list_entry(const VantageView *view, const Browser *browser, size_t index) {
    return browser->files ? view->files[index]
                          : view->children[view->offsets[browser->directory] + index];
}

static size_t *selection(Browser *browser) {
    return browser->files ? &browser->file_selection : &browser->tree_selection;
}

static size_t *scroll_position(Browser *browser) {
    return browser->files ? &browser->file_scroll : &browser->tree_scroll;
}

static int body_rows(int rows, int *headers, int *footers) {
    *headers = rows >= 10 ? 5 : rows >= 6 ? 3 : rows >= 3 ? 1 : 0;
    *footers = rows >= 7 ? 2 : rows >= 2 ? 1 : 0;
    return rows - *headers - *footers;
}

static void keep_visible(Browser *browser, size_t count, int visible) {
    size_t *selected = selection(browser), *scroll = scroll_position(browser);
    if (!count) {
        *selected = *scroll = 0;
        return;
    }
    if (*selected >= count) {
        *selected = count - 1;
    }
    size_t page = visible > 0 ? (size_t)visible : 1;
    if (*scroll > *selected) {
        *scroll = *selected;
    }
    if (*selected - *scroll >= page) {
        *scroll = *selected - page + 1;
    }
    size_t maximum = count > page ? count - page : 0;
    if (*scroll > maximum) {
        *scroll = maximum;
    }
}

static void print_entry(const VantageView *view, const Browser *browser, size_t node, bool selected,
                        uint64_t total, int columns) {
    const VSEntry *entry = view->inventory->entries + node;
    uint64_t bytes = vantage_bytes(view, node), unknown = vantage_unknown(view, node);
    bool missing_size = entry->kind == 1 && !(entry->valid & VS_VALID_SIZE);
    char size[32], field[64];
    if (missing_size) {
        snprintf(size, sizeof(size), "unknown");
    } else {
        vantage_size(size, bytes);
        size_t used = strlen(size);
        if (unknown && used + 1 < sizeof(size)) {
            size[used] = '+';
            size[used + 1] = 0;
        }
    }
    if (selected) {
        (void)fputs("\033[7m", stdout);
    }
    snprintf(field, sizeof(field), "%s%10s ", selected ? "> " : "  ", size);
    int remaining = columns - ascii(field, columns);
    long double share = total ? (long double)bytes / (long double)total : 0;
    if (share > 1) {
        share = 1;
    }
    if (columns >= 38) {
        if (missing_size) {
            snprintf(field, sizeof(field), "%5s%% ", "?");
        } else {
            snprintf(field, sizeof(field), "%5.1Lf%% ", share * 100);
        }
        remaining -= ascii(field, remaining);
    }
    if (columns >= 54) {
        int filled = (int)(share * 10 + 0.5L);
        field[0] = '[';
        for (int i = 0; i < 10; ++i) {
            field[i + 1] = missing_size ? '?' : i < filled ? '#' : '-';
        }
        memcpy(field + 11, "] ", 3);
        remaining -= ascii(field, remaining);
    }
    remaining -= ascii(entry->kind == 2 ? "/ " : entry->kind == 5 ? "@ " : "  ", remaining);
    if (browser->files) {
        char *path = vantage_path(view, node);
        if (path) {
            remaining -=
                vantage_print_text(stdout, (const unsigned char *)path, strlen(path), remaining);
            vs_free(path);
        } else {
            remaining -= ascii("(unresolved)", remaining);
        }
    } else {
        remaining -= vantage_print_text(stdout, view->inventory->names + entry->name_offset,
                                        entry->name_length, remaining);
    }
    if (selected) {
        while (remaining-- > 0) {
            (void)fputc(' ', stdout);
        }
        (void)fputs("\033[0m", stdout);
    }
}

static bool redraw(const VantageView *view, Browser *browser, int rows, int columns) {
    int headers, footers;
    int visible = body_rows(rows, &headers, &footers);
    size_t count = list_count(view, browser);
    keep_visible(browser, count, visible);
    size_t current = browser->files ? view->root : browser->directory;
    uint64_t total = vantage_bytes(view, current);
    bool partial = view->result->completion != VS_COMPLETE || !view->result->graph_valid ||
                   vantage_unknown(view, view->root) != 0;
    (void)fputs("\033[H\033[2J", stdout);
    char text[256], root_size[32], here_size[32], unique_size[32];
    vantage_size(root_size, vantage_bytes(view, view->root));
    vantage_size(here_size, total);
    vantage_size(unique_size, view->result->unique_bytes);
    if (headers >= 1) {
        line(1);
        (void)fputs("\033[1m", stdout);
        snprintf(text, sizeof(text), "vantage | %s%s", browser->files ? "Biggest files" : "Tree",
                 partial ? " | PARTIAL: lower bounds" : "");
        (void)ascii(text, columns);
        (void)fputs("\033[0m", stdout);
    }
    if (headers >= 2) {
        line(2);
        snprintf(text, sizeof(text), "Total %s | Here %s | %s | scan %.2fs", root_size, here_size,
                 view->config->size == VS_ALLOCATED ? "allocated" : "logical",
                 (double)view->result->total_ns / 1000000000.0);
        (void)ascii(text, columns);
    }
    if (headers >= 3) {
        line(3);
        print_path(view, current, "At: ", columns);
    }
    if (headers >= 4) {
        line(4);
        snprintf(text, sizeof(text), "Hard links count per name; unique file bytes: %s",
                 unique_size);
        (void)ascii(text, columns);
    }
    if (headers >= 5) {
        line(5);
        if (partial) {
            snprintf(text, sizeof(text),
                     "PARTIAL: known sizes are lower bounds; unknown files: %" PRIu64,
                     vantage_unknown(view, view->root));
        } else {
            snprintf(text, sizeof(text), "Size / share of %s / name (%zu entries)",
                     browser->files ? "total" : "directory", count);
        }
        (void)ascii(text, columns);
    }
    size_t start = *scroll_position(browser), selected = *selection(browser);
    for (int row = 0; row < visible; ++row) {
        line(headers + row + 1);
        if ((size_t)row < count - start) {
            size_t index = start + (size_t)row;
            print_entry(view, browser, list_entry(view, browser, index), index == selected, total,
                        columns);
        } else if (!count && !row) {
            (void)ascii(browser->files ? "(No regular files)" : "(Empty directory)", columns);
        }
    }
    if (footers == 2) {
        line(rows - 1);
        if (count) {
            print_path(view, list_entry(view, browser, selected), "Selected: ", columns);
        } else {
            print_path(view, current, "Selected: ", columns);
        }
    }
    if (footers) {
        line(rows);
        (void)ascii("j/k Move  Enter Open  h Back  f Files  g Root  q/Esc Quit", columns);
    }
    return !ferror(stdout) && fflush(stdout) != EOF;
}

static void go_parent(const VantageView *view, Browser *browser) {
    if (browser->files) {
        browser->files = false;
        return;
    }
    if (browser->directory == view->root) {
        return;
    }
    size_t child = browser->directory;
    uint64_t parent = view->inventory->entries[child].parent_index;
    browser->directory = parent < view->root ? (size_t)parent : view->root;
    browser->tree_selection = browser->tree_scroll = 0;
    size_t begin = view->offsets[browser->directory], end = view->offsets[browser->directory + 1];
    for (size_t i = begin; i < end; ++i) {
        if (view->children[i] == child) {
            browser->tree_selection = i - begin;
            break;
        }
    }
}

static void navigate(const VantageView *view, Browser *browser, Key key, int page_rows) {
    size_t count = list_count(view, browser), *selected = selection(browser);
    size_t page = page_rows > 0 ? (size_t)page_rows : 1;
    switch (key) {
    case KEY_UP:
        if (*selected) {
            --*selected;
        }
        break;
    case KEY_DOWN:
        if (count && *selected < count - 1) {
            ++*selected;
        }
        break;
    case KEY_HOME:
        *selected = 0;
        break;
    case KEY_END:
        if (count) {
            *selected = count - 1;
        }
        break;
    case KEY_PAGE_UP:
        *selected = *selected > page ? *selected - page : 0;
        break;
    case KEY_PAGE_DOWN:
        if (count) {
            *selected += page < count - 1 - *selected ? page : count - 1 - *selected;
        }
        break;
    case KEY_PARENT:
        go_parent(view, browser);
        break;
    case KEY_OPEN:
        if (!browser->files && count) {
            size_t node = list_entry(view, browser, *selected);
            if (view->inventory->entries[node].kind == 2) {
                browser->directory = node;
                browser->tree_selection = browser->tree_scroll = 0;
            }
        }
        break;
    case KEY_FILES:
        browser->files = !browser->files;
        break;
    case KEY_ROOT:
        browser->files = false;
        browser->directory = view->root;
        browser->tree_selection = browser->tree_scroll = 0;
        break;
    default:
        break;
    }
}

int vantage_tui(const VantageView *view, bool files_only) {
    terminal.cleanup_error = false;
    exit_signal = resized = 0;
    if (!prepare_terminal()) {
        int error = errno;
        restore_terminal();
        if (exit_signal) {
            return exit_signal == SIGPIPE ? 1 : 128 + exit_signal;
        }
        fprintf(stderr, "vantage: terminal setup failed: %s\n", strerror(error));
        return 1;
    }
    Browser browser = {.directory = view->root, .files = files_only};
    int rows, columns;
    window_size(&rows, &columns);
    bool dirty = true;
    int status = 0;
    for (;;) {
        if (exit_signal) {
            status = exit_signal == SIGPIPE ? 1 : 128 + exit_signal;
            break;
        }
        int next_rows, next_columns;
        window_size(&next_rows, &next_columns);
        if (resized || rows != next_rows || columns != next_columns) {
            resized = 0;
            rows = next_rows;
            columns = next_columns;
            dirty = true;
        }
        if (dirty) {
            if (!redraw(view, &browser, rows, columns)) {
                status = 1;
                break;
            }
            dirty = false;
        }
        Key key = next_key();
        if (key == KEY_ERROR) {
            status = 1;
            break;
        }
        if (key == KEY_QUIT || key == KEY_EOF) {
            break;
        }
        if (key != KEY_NONE) {
            int headers, footers;
            navigate(view, &browser, key, body_rows(rows, &headers, &footers));
            dirty = true;
        }
    }
    restore_terminal();
    if (exit_signal) {
        status = exit_signal == SIGPIPE ? 1 : 128 + exit_signal;
    }
    if (!status && terminal.cleanup_error) {
        status = 1;
    }
    return status;
}
