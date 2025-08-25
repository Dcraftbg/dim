#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>

#define STUI_NO_UNICODE
#define STUI_NO_COLORS
#define STUI_IMPLEMENTATION
#include "stui.h"

#if defined(__ANDROID__) || defined(_WIN32) || defined(_MINOS)
# define DISABLE_ALT_BUFFER 1
#endif

#if defined(_MINOS)
# define ENABLE_SWAP 0
# define NO_INSERT_CURSOR
#endif

#ifndef ENABLE_SWAP
# define ENABLE_SWAP 1
#endif

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
};
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
Editor editor={0};
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
static bool has_diagnostic = false;
#define MAX_DIAG_SIZE 128
static char diagnostic_buf[128];
void diagnosticv(const char* fmt, va_list args) {
    vsnprintf(diagnostic_buf, sizeof(diagnostic_buf), fmt, args);
    has_diagnostic = true;
}
void diagnostic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diagnosticv(fmt, args);
    va_end(args);
}
void redraw(void) {
    size_t width, height;
    stui_getsize(&width, &height);
    size_t lines_from_start = editor.lines.len - editor.view_line_start;
    size_t visible_lines = lines_from_start < (height-2) ? lines_from_start : (height-2);
    // Clear the entire window
    for(size_t y = 0; y < height; ++y) {
        for(size_t x = 0; x < width; ++x) {
            stui_putchar(x, y, ' ');
        }
    }
    for(size_t line_i = 0; line_i < visible_lines; ++line_i) {
        Line* line = &editor.lines.data[line_i + editor.view_line_start];
        size_t visible = line->size < width ? line->size : width; 
        for(size_t i = 0; i < visible; ++i) stui_putchar(i, line_i, editor.src.data[line->offset + i]);
    }
    size_t x = 0, y = height-2;
    const char* mode = mode_to_str(editor.mode);
    while(*mode && x < width) stui_putchar(x++, y, *mode++);
    stui_putchar(x++, y, ' ');
    const char* path = editor.path;
    while(*path && x < width) stui_putchar(x++, y, *path++);
    for(size_t i = x; i < width; ++i) {
        stui_putchar(i, y, '-');
    }
    y++;
    x = 0;
    size_t cursor_x = 0, cursor_y = 0;
    if(has_diagnostic && editor.mode != MODE_CMD) {
        const char* diag = diagnostic_buf;
        while(*diag && x < width) stui_putchar(x++, y, *diag++);
    }
    switch(editor.mode) {
    case MODE_NORMAL:
    case MODE_INSERT:
        cursor_x = editor.cursor_chr;
        cursor_y = editor.cursor_line - editor.view_line_start;
        break;
    case MODE_CMD:
        has_diagnostic = false;
        stui_putchar(0, y, ':');
        for(size_t i = 0; i < editor.cmdlen && x < width; ++i) {
            stui_putchar(1 + x++, height-1, editor.cmd[i]);
        }
        cursor_x = editor.cmdlen+1;
        cursor_y = height-1;
    }
    stui_refresh();
    stui_goto(cursor_x, cursor_y);

#ifndef NO_INSERT_CURSOR
    static bool is_cursor_insert = false;    
    if(editor.mode == MODE_INSERT) {
        if(!is_cursor_insert) {
            printf("\033[6 q");
            is_cursor_insert = true;
        }
    } else if(is_cursor_insert) {
        printf("\033[1 q");
        is_cursor_insert = false;
    }
    fflush(stdout);
#endif
}
#ifndef _MINOS
void _interrupt_handler_cleaner(int sig) {
    (void)sig;
    stui_clear();
    exit(1);
}
void _interrupt_handler_resize(int sig) {
    (void)sig;
    size_t w, h;
    stui_term_get_size(&w, &h);
    stui_setsize(w, h);
    stui_clear();
    redraw();
}
#endif
stui_term_flag_t old_flags;
void cleanup_flags(void) {
    stui_term_set_flags(old_flags);
}
#ifndef DISABLE_ALT_BUFFER
void restore_alt_buffer(void) {
    // Alternate buffer.
    // The escape sequence below shouldn't do anything on terminals that don't support it
    printf("\033[?1049l");
    fflush(stdout);
}
#endif
void register_signals(void) {
#ifndef _MINOS
    signal(SIGINT, _interrupt_handler_cleaner);
    signal(SIGWINCH, _interrupt_handler_resize);
#endif
}
// Must be called after changing the cursor position in any way
void update_view(void) {
    if(editor.cursor_line < editor.view_line_start) editor.view_line_start = editor.cursor_line;
    size_t width, height;
    stui_getsize(&width, &height);
    if((editor.cursor_line - editor.view_line_start) >= (height-2)) editor.view_line_start = editor.cursor_line - (height-3);
}
void handle_editor_movement(uint32_t key) {
    switch(key) {
    case STUI_KEY_UP:
        if(editor.cursor_line > 0) {
            editor.cursor_line--;
            if(editor.cursor_chr > editor.lines.data[editor.cursor_line].size) 
                editor.cursor_chr = editor.lines.data[editor.cursor_line].size;
        }
        break;
    case STUI_KEY_DOWN:
        if(editor.cursor_line+1 < editor.lines.len) {
            editor.cursor_line++;
            if(editor.cursor_chr > editor.lines.data[editor.cursor_line].size) 
                editor.cursor_chr = editor.lines.data[editor.cursor_line].size;
        }
        break;
    case STUI_KEY_RIGHT:
        if(editor.cursor_chr >= editor.lines.data[editor.cursor_line].size) {
            if(editor.cursor_line+1 < editor.lines.len) {
                editor.cursor_chr = 0;
                editor.cursor_line++;
            }
        } else {
            editor.cursor_chr++;
        }
        break;
    case STUI_KEY_LEFT: 
        if(editor.cursor_chr > 0) {
            editor.cursor_chr--;
        } else {
            if(editor.cursor_line > 0) {
                editor.cursor_line--;
                editor.cursor_chr = editor.lines.data[editor.cursor_line].size;
            }
        }
        break;
    }
    update_view();
}

#define cmd_func(name) void cmd_##name(void)

cmd_func(write) {
#if ENABLE_SWAP
    if(strlen(editor.path) >= 4095) {
        diagnostic("File path is too big!");
        return;
    }
    char tmpbuf[4096];
    snprintf(tmpbuf, sizeof(tmpbuf), "%s~", editor.path);
    int e = rename(editor.path, tmpbuf);
    if(e < 0 && errno != ENOENT) {
        diagnostic("Failed to created swap file %s -> %s: %s", editor.path, tmpbuf, strerror(errno));
        return;
    }
#endif
    ssize_t res = write_to_file(editor.path, editor.src.data, editor.src.len);
    if(res < 0) {
        diagnostic("Failed to write to `%s`: %s", editor.path, strerror(-res));
#if ENABLE_SWAP
        e = rename(tmpbuf, editor.path);
        if(e < 0) diagnostic("Failed to restore swap file %s -> %s: %s", tmpbuf, editor.path, strerror(errno));
#endif
    }
    else {
        diagnostic("Wrote %zu bytes", editor.src.len);
#if ENABLE_SWAP
        e = remove(tmpbuf);
        if(e < 0 && errno != ENOENT) diagnostic("WARN: Failed to remove swap file %s: %s", tmpbuf, strerror(errno));
#endif
    }
}
cmd_func(quit) {
    stui_clear();
    exit(0);
}
cmd_func(write_quit) {
    cmd_write();
    cmd_quit();
}

typedef struct {
    void (*func)(void);
    const char* name;
} Command;
#define cmd(_func, _name, ...) (Command){.func = cmd_##_func, .name=_name, __VA_ARGS__}

Command cmds[] = {
    cmd(write, "w"),
    cmd(write, "write"),
    cmd(quit , "q"),
    cmd(quit , "quit"),
    cmd(write_quit, "wq"),
};

int main(int argc, const char** argv) {
    const char* exe = shift_args(&argc, &argv);
    assert(exe);
    (void)exe;
    {
        size_t w, h;
        stui_term_get_size(&w, &h);
        stui_setsize(w, h);
    }
    old_flags = stui_term_get_flags();
    atexit(cleanup_flags);
#ifndef DISABLE_ALT_BUFFER
    printf("\033[?1049h");
    fflush(stdout);
    atexit(restore_alt_buffer);
#endif

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
    register_signals();
    stui_clear();
    stui_term_disable_echo();
    stui_term_enable_instant();
    for(;;) {
        redraw();
        switch(editor.mode) {
        case MODE_NORMAL: {
            int c = stui_get_key();
            switch(c) {
            case 'i':
                editor.mode = MODE_INSERT;
                break;
            case ':':
                editor.mode = MODE_CMD;
                break;
            case STUI_KEY_UP:
            case STUI_KEY_DOWN:
            case STUI_KEY_LEFT:
            case STUI_KEY_RIGHT:
                handle_editor_movement(c);
                break;
            default:
                // putcharat(c, putstrat("Unexpected chr ", 0, h-1), h-1);
                break;
            }
        } break;
        case MODE_INSERT: {
            int c = stui_get_key();
            switch(c) {
            case STUI_KEY_ESC:
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
            case STUI_KEY_UP:
            case STUI_KEY_DOWN:
            case STUI_KEY_LEFT:
            case STUI_KEY_RIGHT:
                handle_editor_movement(c);
                break;
            default:
                editor_add_char(&editor, c, editor.cursor_line, editor.cursor_chr);
                editor.cursor_chr++;
                break;
            }
        } break;
        case MODE_CMD: {
            int c = stui_get_key();
            if(c == STUI_KEY_ESC) {
                editor.mode = MODE_NORMAL;
                editor.cmdlen = 0;
                break;
            } else if(c == BACKSPACE && editor.cmdlen) editor.cmdlen--;
            else if(c == '\n') {
                if(editor.cmdlen == 0) {
                    editor.cmdlen = 0;
                    editor.mode = MODE_NORMAL;
                    break;
                }

                editor.cmd[editor.cmdlen] = '\0';
                assert(editor.cmdlen++ < sizeof(editor.cmd));
                bool found = false;
                // NOTE: we allow negative values for easy "goto end of file" :-1
                if(isdigit(editor.cmd[0]) || editor.cmd[0] == '-') {
                    found = true;
                    // TODO: use strtol instead and verify input
                    editor.cursor_line = atoi(editor.cmd);
                    if(editor.cursor_line > editor.lines.len) editor.cursor_line = editor.lines.len - 1;
                    editor.cursor_chr = 0;
                    update_view();
                } else {
                    for(size_t i = 0; i < sizeof(cmds)/sizeof(*cmds); ++i) {
                        if(strcmp(editor.cmd, cmds[i].name) == 0) {
                            cmds[i].func();
                            found = true;
                            break;
                        }
                    }
                }
                editor.cmdlen = 0;
                editor.mode = MODE_NORMAL;
                if(!found) {
                    diagnostic("Unknown command `%s`", editor.cmd);
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
