#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

#ifdef _MINOS
#define WIDTH 80
#define HEIGHT 24
#else
#define WIDTH 80
#define HEIGHT 24
#endif
static char _screen[WIDTH*HEIGHT] = {0};
size_t cx, cy;
void get_winsize(size_t* w, size_t* h) {
    *w = WIDTH;
    *h = HEIGHT;
}
void clear() {
    memset(_screen, 0, sizeof(_screen));
    printf("\033[2J");
    printf("\033[H");
    fflush(stdout);
    cx = 0;
    cy = 0;
}
void gotopos(size_t x, size_t y) {
    if(cx != x || cy != y) {
        cx = x;
        cy = y;
        printf("\033[%zu;%zuH", y+1, x+1);
        fflush(stdout);
    }
}
void putcharat(char c, size_t x, size_t y) {
    assert(x < WIDTH && y < HEIGHT);
    size_t i = x+y*WIDTH;
    if(_screen[i] == c) {
        return;
    }
    _screen[i] = c;
    gotopos(x, y);
    fputc(c, stdout);
    fflush(stdout);
    cx++;
}
size_t putstrnat(const char* str, size_t n, size_t x, size_t y) {
    if(!str) return 0;
    for(size_t i = 0; i < n; ++i) {
        putcharat(str[i], x, y);
        x = (x+1) % WIDTH;
        if(x == 0) y = (y+1) % HEIGHT;
    }
    return n;
}
static size_t putstrat(const char* str, size_t x, size_t y) {
    return putstrnat(str, strlen(str), x, y);
}
void clearlineat(size_t y) {
    size_t w, h;
    get_winsize(&w, &h);
    (void)h;
    for(size_t i = 0; i < w; ++i) {
        putcharat(' ', i, y);
    }
}
#ifdef _MINOS
#include <minos/tty/tty.h>
#else
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#endif
void disable_echo() {
#ifdef _MINOS
    ttyflags_t flags;
    tty_get_flags(STDIN_FILENO, &flags);
    flags &= ~(TTY_ECHO);
    flags |= TTY_INSTANT;
    tty_set_flags(STDIN_FILENO, flags);
#else
    struct termios term;
    tcgetattr(fileno(stdin), &term);
    term.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(fileno(stdin), TCSANOW, &term);
#endif
}
void enable_echo() {
#ifdef _MINOS
    ttyflags_t flags;
    tty_get_flags(STDIN_FILENO, &flags);
    flags |= TTY_ECHO;
    flags &= ~TTY_INSTANT;
    tty_set_flags(STDIN_FILENO, flags);
#else
    struct termios term;
    tcgetattr(fileno(stdin), &term);
    term.c_lflag |= ECHO | ICANON;
    tcsetattr(fileno(stdin), 0, &term);
#endif
}

enum {
    MODE_NORMAL,
    MODE_INSERT,
    MODE_CMD,
    MODE_COUNT
};
const char* mode_str_map[MODE_COUNT] = {
    [MODE_NORMAL] = "Normal",
    [MODE_INSERT] = "Insert",
    [MODE_CMD]    = "Command",
};
const char* mode_to_str(uint32_t mode) {
    if(mode >= MODE_COUNT) return NULL;
    return mode_str_map[mode];
}
enum {
#ifdef _MINOS
    BACKSPACE='\b',
#else
    BACKSPACE=127,
#endif
    KEY_UP=256,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
};
int getch() {
    int c = getchar();
    if(c == 0x1B) {
        c=getchar();
        if(c != '[') {
            fprintf(stderr, "Escape sequence wasn't csi");
            return -1;
        }
        c=getchar();
        switch(c) {
        case 'A':
        case 'B':
        case 'C':
        case 'D':
            return c-'A'+KEY_UP;
        default:
            fprintf(stderr, "Unhandled escape sequence in getchar() (%c)\n", c);
            return -1;
        }
    }
    return c;
}
#ifdef _MINOS
ttyflags_t oldflags;
#else
struct termios oldtermios;
void _cleanup_termios() {
    tcsetattr(fileno(stdin), TCSANOW, &oldtermios);
}
void _interrupt_handler_cleaner(int sig) {
    (void)sig;
    _cleanup_termios();
    exit(1);
}
#endif
void register_cleaners() {
#ifdef _MINOS
    tty_get_flags(STDIN_FILENO, &oldflags);
    // TODO: Some sort of cleaners for resetting the tty mode back
#else
    tcgetattr(fileno(stdin), &oldtermios);
    signal(SIGINT, _interrupt_handler_cleaner);
    atexit(_cleanup_termios);
#endif
}
typedef struct {
    size_t offset;
    size_t size;
} Line;
#define da_reserve(da, extra) \
    do {\
        if((da)->len + (extra) > (da)->cap) {\
            size_t __da_ncap = (da)->cap * 2 + (extra);\
            (da)->data = realloc((da)->data, __da_ncap*sizeof(*(da)->data));\
            (da)->cap = __da_ncap;\
            assert((da)->data && "Ran out of memory LOL");\
        }\
    } while(0)
typedef struct {
    Line *data;
    size_t len, cap;
} Lines;
static void lines_reserve(Lines* da, size_t extra) {
    da_reserve(da, extra);
}
typedef struct {
    uint32_t mode;
    const char* path;
    char cmd[128];
    size_t cmdlen;
    struct {
        char* data;
        size_t len, cap;
    } src;
    Lines lines;
    size_t cursor_line;
    size_t cursor_chr;
    size_t view_line_start;
} Editor;

void editor_reserve_chars(Editor* editor, size_t extra) {
    da_reserve(&editor->src, extra);
}
void editor_add_strn(Editor* editor, const char* str, size_t len, size_t line, size_t chr) {
    editor_reserve_chars(editor, len);
    char* data = editor->src.data + editor->lines.data[line].offset + chr;
    memmove(
        data + len,
        data,
        editor->src.len - (data - editor->src.data)
    );
    memcpy(
        data,
        str,
        len
    );
    editor->src.len += len;
    editor->lines.data[line].size += len;
    for(size_t i = line+1; i < editor->lines.len; ++i) {
        editor->lines.data[i].offset += len;
    }
}
void editor_rem_chunk(Editor* editor, size_t len, size_t line, size_t chr) {
    assert(editor->lines.data[line].size >= len);
    char* data = editor->src.data + editor->lines.data[line].offset + chr;
    memmove(
        data,
        data+len,
        editor->src.len - ((data+len) - editor->src.data)
    );
    editor->src.len -= len;
    editor->lines.data[line].size -= len;
    for(size_t i = line+1; i < editor->lines.len; ++i) {
        editor->lines.data[i].offset -= len;
    }
}
static void editor_add_char(Editor* editor, char c, size_t line, size_t chr) {
    editor_add_strn(editor, &c, 1, line, chr);
}
void editor_add_line(Editor* editor, size_t prev, size_t chr) {
    editor_add_char(editor, '\n', prev, chr);
    lines_reserve(&editor->lines, 1);
    memmove(editor->lines.data+prev+1, editor->lines.data+prev, (editor->lines.len-prev) * sizeof(editor->lines.data[0]));
    size_t right = editor->lines.data[prev].size-1-chr;
    editor->lines.data[prev  ].size   = chr;
    editor->lines.data[prev+1].size   = right;
    editor->lines.data[prev+1].offset = editor->lines.data[prev].offset + chr + 1;
    editor->lines.len++;
}
void parse_lines(Editor* editor) {
    for(size_t i = 0; i < editor->src.len; ++i) {
        if(editor->src.data[i] == '\n') {
            lines_reserve(&editor->lines, 1);
            Line line = {
                .size = 0,
                .offset = i+1
            };
            editor->lines.data[editor->lines.len++] = line;
        } else {
            editor->lines.data[editor->lines.len-1].size++;
        }
    }
}
ssize_t write_to_file(const char* path, char* bytes, size_t len) {
    FILE* f = fopen(path, "wb");
    if(!f) return -errno;
    while(len) {
        size_t newly_written = fwrite(bytes, 1, len, f);
        if(newly_written == 0) {
            fclose(f);
            return -errno;
        }
        bytes += newly_written;
        len -= newly_written;
    }
    fclose(f);
    return len;
}
ssize_t read_from_file(const char* path, char** bytes, size_t* len) {
    FILE* f = fopen(path, "rb");
    if(!f) goto fopen_err; 
    if(fseek(f, 0, SEEK_END) < 0) goto fseek_err;
    long n = ftell(f);
    if(n < 0) goto ftell_err;
    if(fseek(f, 0, SEEK_SET) < 0) goto fseek_err;
    *len = n;
    char* buf;
    *bytes = buf = malloc(n);
    assert(buf && "Ran out of memory LOL");
    if(fread(buf, n, 1, f) != 1) goto fread_err;
    return 0;
fread_err:
    free(buf);
    *bytes = NULL;
    *len = 0;
ftell_err:
fseek_err:
    fclose(f);
fopen_err:
    return errno;
}
const char* shift_args(int *argc, const char ***argv) {
    if((*argc) <= 0) return NULL;
    return ((*argc)--, *((*argv)++));
}
int main(int argc, const char** argv) {
    static Editor editor={0};
    const char* exe = shift_args(&argc, &argv);
    assert(exe);
    (void)exe;
    editor.path = NULL;
    
    const char* arg;
    while((arg=shift_args(&argc, &argv))) {
        if(!editor.path) editor.path = arg;
        else {
            fprintf(stderr, "ERROR: Unexpected argument `%s`\n", arg);
            return 1;
        }
    }
    if(!editor.path) editor.path = "temp";
    // Add initial empty line
    lines_reserve(&editor.lines, 1);
    editor.lines.data[0].offset = 0;
    editor.lines.data[0].size   = 0;
    editor.lines.len++;
    ssize_t e = read_from_file(editor.path, &editor.src.data, &editor.src.cap);
    editor.src.len = editor.src.cap;
    if(e < 0) {
        if(e != -ENOENT) {
            fprintf(stderr, "ERROR: Failed to load `%s`: %s", editor.path, strerror(e));
            return 1;
        }
    } else {
        parse_lines(&editor);
    }
    register_cleaners();
    clear();
    disable_echo();
    size_t w, h;
    get_winsize(&w, &h);
    for(;;) {
        size_t visible_lines = editor.lines.len < (h-2) ? editor.lines.len : (h-2);
        for(size_t line_i = 0; line_i < visible_lines; ++line_i) {
            Line* line = &editor.lines.data[line_i];
            size_t visible = line->size < w ? line->size : w; 
            putstrnat(editor.src.data + line->offset, visible, 0, line_i);
            for(size_t x = visible; x < w; ++x) {
                putcharat(' ', x, line_i);
            }
        }
        size_t head = 0;
        head += putstrat(mode_to_str(editor.mode), head, h-2);
        putcharat(' ', head++, h-2);
        head += putstrat(editor.path, head, h-2);
        for(size_t i = head; i < w; ++i) {
            putcharat('-', i, h-2);
        }
        gotopos(0, h-1);
        switch(editor.mode) {
        case MODE_NORMAL: {
            int c = getch();
            switch(c) {
            case 'i':
                editor.mode = MODE_INSERT;
                break;
            case ':':
                editor.mode = MODE_CMD;
                gotopos(0, h-1);
                clearlineat(h-1);
                break;
            default:
                // putcharat(c, putstrat("Unexpected chr ", 0, h-1), h-1);
                break;
            }
        } break;
        case MODE_INSERT: {
            // TODO: Actually get cursor positon in window space
            gotopos(editor.cursor_chr, editor.cursor_line);
            int c = getch();
            switch(c) {
            case '`':
                editor.mode = MODE_NORMAL;
                break;
            case BACKSPACE:
                if(editor.cursor_chr == 0) {
                    if(editor.cursor_line) {
                        memmove(
                            editor.src.data + editor.lines.data[editor.cursor_line].offset-1,
                            editor.src.data + editor.lines.data[editor.cursor_line].offset,
                            (editor.src.len  - editor.lines.data[editor.cursor_line].offset) * sizeof(editor.src.data[0])
                        );
                        editor.src.len--;
                        memmove(
                            editor.lines.data + editor.cursor_line,
                            editor.lines.data + editor.cursor_line + 1,
                            (editor.lines.len  - (editor.cursor_line+1)) * sizeof(editor.lines.data[0])
                        );
                        editor.lines.len--;
                        for(size_t i = editor.cursor_line; i < editor.lines.len; ++i) {
                            editor.lines.data[i].offset--;
                        }
                        editor.cursor_line--;
                        editor.cursor_chr = editor.lines.data[editor.cursor_line].size;
                    }
                } else {
                    editor_rem_chunk(&editor, 1, editor.cursor_line, editor.cursor_chr-1);
                    editor.cursor_chr--;
                }
                // TODO: Handle removing line
                break;
            case '\n':
                editor_add_line(&editor, editor.cursor_line, editor.cursor_chr);
                editor.cursor_line++;
                editor.cursor_chr=0;
                break;
            case KEY_UP:
                if(editor.cursor_line > 0) {
                    editor.cursor_line--;
                    if(editor.cursor_chr > editor.lines.data[editor.cursor_line].size) 
                        editor.cursor_chr = editor.lines.data[editor.cursor_line].size;
                }
                break;
            case KEY_DOWN:
                if(editor.cursor_line+1 < editor.lines.len) {
                    editor.cursor_line++;
                    if(editor.cursor_chr > editor.lines.data[editor.cursor_line].size) 
                        editor.cursor_chr = editor.lines.data[editor.cursor_line].size;
                }
                break;
            case KEY_RIGHT:
                if(editor.cursor_chr >= editor.lines.data[editor.cursor_line].size) {
                    if(editor.cursor_line+1 < editor.lines.len) {
                        editor.cursor_chr = 0;
                        editor.cursor_line++;
                    }
                } else {
                    editor.cursor_chr++;
                }
                break;
            case KEY_LEFT: 
                if(editor.cursor_chr > 0) {
                    editor.cursor_chr--;
                } else {
                    if(editor.cursor_line > 0) {
                        editor.cursor_line--;
                        editor.cursor_chr = editor.lines.data[editor.cursor_line].size;
                    }
                }
                break;
            default:
                editor_add_char(&editor, c, editor.cursor_line, editor.cursor_chr);
                editor.cursor_chr++;
                break;
            }
        } break;
        case MODE_CMD: {
            putcharat(':', 0, h-1);
            putstrnat(editor.cmd, editor.cmdlen, 1, h-1);
            gotopos(editor.cmdlen+1, h-1);
            int c = getch();
            // TODO: We are temporarily using '`' as the quit key.
            // We don't do escape parsing very well
            if(c == '`') {
                editor.mode = MODE_NORMAL;
                editor.cmdlen = 0;
                clearlineat(h-1);
                break;
            } else if(c == BACKSPACE) {
                if(editor.cmdlen) editor.cmdlen--;
                putcharat(' ', editor.cmdlen+1, h-1);
            } else if(c == '\n') {
                if(editor.cmdlen == 0) {
                    editor.cmdlen = 0;
                    editor.mode = MODE_NORMAL;
                    clearlineat(h-1);
                    break;
                }

                editor.cmd[editor.cmdlen] = '\0';
                assert(editor.cmdlen++ < sizeof(editor.cmd));
                // NOTE: I don't need to check boundaries. \0 would make it fail anyway
                if(strcmp(editor.cmd, "q") == 0) {
                    clear();
                    #ifdef _MINOS
                    // NOTE: Temporary solution for now.
                    // At some point will just be atexit
                    tty_set_flags(STDIN_FILENO, oldflags);
                    #endif
                    return 0;
                } else if (strcmp(editor.cmd, "w") == 0) {
                    ssize_t res = write_to_file(editor.path, editor.src.data, editor.src.len);
                    if(res < 0) {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Failed to write to `%s`: %s", editor.path, strerror(-res));
                        putstrat(buf, 0, h-1);
                        editor.cmdlen = 0;
                        editor.mode = MODE_NORMAL;
                        break;
                    } else {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Wrote %zu bytes", editor.src.len);
                        putstrat(buf, 0, h-1);
                        editor.cmdlen = 0;
                        editor.mode = MODE_NORMAL;
                        break;
                    }
                } else {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "Unknown command `%s`", editor.cmd);
                    putstrat(buf, 0, h-1);
                    editor.cmdlen = 0;
                    editor.mode = MODE_NORMAL;
                    break;
                }
            } else {
                editor.cmd[editor.cmdlen] = c;
                assert(editor.cmdlen++ < sizeof(editor.cmd));
            }
        } break;
        default:
            fprintf(stderr, "Unhandled mode: %u", editor.mode);
            return 1;
        }
    }
}
