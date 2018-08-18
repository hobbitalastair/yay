/* yay.c
 *
 * A stupid text editor.
 *
 * Author:  Alastair Hughes
 * Contact: hobbitalastair at yandex dot com
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <ncurses.h>

#define BUFSIZE 4096

/* Generic buffer implementation.
 *
 * Currently this is implemented as a gap buffer - this is a simple buffer with
 * some "slack", of the form:
 * [first_section     <gap>    second_section]
 * where the first and second sections are *always* next to the start and end
 * of the buffer, respectively.
 */
typedef struct {
    size_t buflen;
    char* buf;
    size_t first_len;
    size_t second_len;
} Buf;

/* Error codes as strings.
 *
 * Allow passing strings as errors - this avoids needing a lookup table.
 * Note that NULL here is a success, so we provide a define here to avoid
 * confusion.
 */
typedef char* Error;
#define ERR_OK NULL

/* Current pointer position.
 *
 * The pointer position consists of the current position, the "old column",
 * and the current selection.
 * We use lines/columns for pointer coordinates, since they are easy to
 * translate to a position on the display, and also easy to move within the
 * data.
 * In addition to the current position, the old column is also stored. This is
 * to prevent the pointer being "pushed left" when scrolling up or down in a
 * buffer, and acts as the "last-commited" column.
 * The selection pointer position is the last "pinned" position, along with the
 * current state of the pin.
 */
typedef struct {
    size_t line;
    size_t column;
    size_t old_column;

    bool sel_on;
    size_t sel_line;
    size_t sel_column;
} Pointer;

/* Static global program name (either __FILE__ or argv[0]) */
char* name;


char line_cmp(size_t line1, size_t column1, size_t line2, size_t column2) {
    /* Compare the given positions.
     *
     * Return 1 if the first position is larger than the second, 0 if they are
     * equal, and -1 if the first position is smaller than the second.
     */

    if (line1 > line2) return 1;
    if (line1 < line2) return -1;
    if (column1 > column2) return 1;
    if (column1 < column2) return -1;
    return 0;
}

bool is_selected(Pointer p, size_t line, size_t column) {
    /* Return true if the given line/column is selected by the given pointer */
    return p.sel_on && (((line_cmp(p.line, p.column, line, column) >= 0 &&
             line_cmp(p.sel_line, p.sel_column, line, column) <= 0) ||
            (line_cmp(p.line, p.column, line, column) <= 0 &&
             line_cmp(p.sel_line, p.sel_column, line, column) >= 0)));
}

size_t buf_len(Buf* buf) {
    /* Return the length of the buffer */
    return buf->first_len + buf->second_len;
}

char buf_get(Buf* buf, size_t offset) {
    /* Return the character at the given offset */
    if (offset >= buf->first_len) {
        offset = (buf->buflen - buf->second_len) + (offset - buf->first_len);
    }
    return buf->buf[offset];
}

void buf_seek(Buf* buf, size_t offset) {
    /* Resize the first section in the gap buffer so that it is `offset` long */

    /* Move the excess first section to the end */
    while (buf->first_len > offset) {
        buf->buf[buf->buflen - buf->second_len - 1] =
            buf->buf[buf->first_len - 1];
        buf->first_len--;
        buf->second_len++;
    }

    /* Move the excess second section to the first */
    while (buf->first_len < offset) {
        buf->buf[buf->first_len] = buf->buf[buf->buflen - buf->second_len];
        buf->first_len++;
        buf->second_len--;
    }
}

Error buf_insert(Buf* buf, size_t offset, char c) {
    /* Insert the character at the given offset in the buffer */

    /* Expand the buffer */
    if (buf->buflen == buf_len(buf)) {
        char* new_buf = realloc(buf->buf, buf->buflen + BUFSIZE);
        if (new_buf == NULL) return strerror(errno);
        buf->buf = new_buf;
        buf->buflen += BUFSIZE;
        /* Move the second section of the buffer to the back */
        for (size_t i = 1; i <= buf->second_len; i++) {
            buf->buf[buf->buflen - i] = buf->buf[buf->buflen - i - BUFSIZE];
        }
    }

    buf_seek(buf, offset);
    buf->buf[buf->first_len] = c;
    buf->first_len++;

    return ERR_OK;
}

void buf_delete(Buf* buf, size_t offset) {
    /* Delete the character at the given offset in the buffer */
    buf_seek(buf, offset);
    buf->first_len--;
}

size_t line2offset(Buf* buf, size_t line, size_t column) {
    /* Return the offset of the given line and column */
    size_t current_line = 0;
    size_t offset = 0;

    while (current_line < line && offset < buf_len(buf)) {
        if (buf_get(buf, offset) == '\n') current_line++;
        offset++;
    }

    size_t current_column = 0;
    while (current_column < column && offset < buf_len(buf)) {
        if (buf_get(buf, offset) == '\n') break;
        current_column++;
        offset++;
    }

    return offset;
}

size_t buf_linelen(Buf* buf, size_t line) {
    /* Return the length of the given line */
    size_t line_offset = line2offset(buf, line, 0);
    size_t len = 0;
    
    while (line_offset + len < buf_len(buf) &&
            buf_get(buf, line_offset + len) != '\n') {
        len++;
    }

    return len;
}

size_t buf_linecount(Buf* buf) {
    /* Count the number of lines in the buffer */
    size_t offset = 0;
    size_t count = 0;

    while (offset < buf_len(buf)) {
        if (buf_get(buf, offset) == '\n') count++;
        offset++;
    }

    return count;
}

bool buf_load(Buf* buf, char* path) {
    /* Load the given file into the buffer, returning false on failure */

    int fd = open(path, O_RDONLY);
    if (fd == -1 && errno != ENOENT) {
        fprintf(stderr, "%s: open(%s): %s\n", name, path,
                strerror(errno));
        return false;
    } else if (fd != -1) {
        /* Read in the file contents */
        ssize_t count = 0;
        do {
            char* new_buf = realloc(buf->buf, buf->buflen + BUFSIZE);
            if (new_buf == NULL) {
                fprintf(stderr, "%s: realloc(): %s\n", name,
                        strerror(errno));
                return false;
            }
            buf->buf = new_buf;
            buf->buflen += BUFSIZE;
            buf->first_len += count;
        } while ((count = read(fd, &(buf->buf[buf->first_len]), BUFSIZE)) ==
                BUFSIZE);
        if (count == -1) {
            fprintf(stderr, "%s: read(): %s\n", name, strerror(errno));
            return false;
        }
        buf->first_len += count;
        close(fd);
    }

    return true;
}

Error buf_write(Buf* buf, char* path) {
    /* Write out the contents of the buffer to a file */

    FILE* file = fopen(path, "w");
    if (file == NULL) return strerror(errno);

    for (size_t i = 0; i < buf_len(buf); i++) {
        char c = buf_get(buf, i);
        if (fwrite(&c, 1, 1, file) != 1) return "bad write";
    }

    fclose(file);
    return ERR_OK;
}

void buf_sel_delete(Buf* buf, Pointer* p) {
    /* Delete the current selection from the buffer, updating the pointer as
     * required.
     */

    if (!p->sel_on) return;

    size_t v_line = 0;
    size_t v_char = 0;
    for (size_t offset = 0; offset < buf_len(buf); offset++) {
        /* Note that we need to store the deleted character for later
         * inspection as we delete it here!
         */
        char c = buf_get(buf, offset);
        if (is_selected(*p, v_line, v_char)) {
            buf_delete(buf, offset + 1);
            offset--;
        }

        if (c != '\n') {
            v_char++;
        } else {
            v_line++;
            v_char = 0;
        }
    }

    /* Update the pointer position to the start of the selection */
    if (line_cmp(p->line, p->column, p->sel_line, p->sel_column) > 0) {
        p->line = p->sel_line;
        p->column = p->sel_column;
    }
    p->sel_on = false;
}

size_t buf_sel_length(Buf* buf, Pointer p) {
    /* Return the length of the current selection */
    size_t len = 0;

    size_t v_line = 0;
    size_t v_char = 0;
    for (size_t offset = 0; offset < buf_len(buf); offset++) {
        if (is_selected(p, v_line, v_char)) len++;
        if (buf_get(buf, offset) != '\n') {
            v_char++;
        } else {
            v_line++;
            v_char = 0;
        }
    }

    return len;
}

char* buf_copy(Buf* buf, size_t offset, size_t len, Error* e) {
    /* Allocate and return a copy of the region starting at the given offset 
     * and of the given length.
     *
     * Returns NULL on failure, and may set the given Error*.
     */

    if (offset + len > buf_len(buf)) return NULL;

    char* clip_buf = malloc(len);
    if (clip_buf == NULL) {
        *e = strerror(errno);
        return NULL;
    }

    for (size_t i = offset; i < offset + len; i++) {
        clip_buf[i] = buf_get(buf, i);
    }

    return clip_buf;
}

bool set_copy_buf(char** copy_buf, Buf* buf, Pointer p, Error* e) {
    /* Set the current copy buffer */

    if (*copy_buf != NULL) free(*copy_buf);

    size_t len = buf_sel_length(buf, p);

    size_t offset = 0;
    if (line_cmp(p.line, p.column, p.sel_line, p.sel_column) <= 0) {
        offset = line2offset(buf, p.line, p.column);
    } else {
        offset = line2offset(buf, p.sel_line, p.sel_column);
    }

    *copy_buf = buf_copy(buf, offset, len, e);
    return (*copy_buf != NULL);
}

size_t render_buf(Buf* buf, size_t v_line, Pointer p, const char* status) {
    /* Render the current buffer from the given line, returning the number of
     * completed lines that could fit on the display.
     *
     * Also renders the current pointer position accounting for line wrapping,
     * and the display status line.
     */

    size_t v_height = 0; /* Current line, counted from v_line */
    size_t v_char = 0; /* Current character in the line */
    size_t offset = line2offset(buf, v_line, 0);

    /* Current x,y on the display and display dimensions */
    size_t disp_x = 0;
    size_t disp_y = 0;
    int height, width; getmaxyx(stdscr, height, width);
    height--; /* Leave a line for that status */

    /* Final pointer position */
    size_t p_x = 0;
    size_t p_y = 0;

    while (disp_y < height && offset < buf_len(buf)) {

        if (is_selected(p, v_line + v_height, v_char)) {
            attron(A_REVERSE);
        } else {
            attroff(A_REVERSE);
        }

        int c = buf_get(buf, offset);
        if (c >= 32 && c <= 126) {
            addch(c);
            disp_x++;
            v_char++;
        }
        if (c == '\t') {
            for (int i = 0; i < 4; i++) {
                addch(' ');
                disp_x++;
                if (disp_x >= width) {
                     disp_x = 0;
                     disp_y++;
                     move(disp_y, disp_x);
                }
            }
            v_char++;
        }
        if (c == '\n') {
            disp_x = 0;
            disp_y++;
            v_char = 0;
            v_height++;
            move(disp_y, disp_x);
        }
        if (disp_x >= width) {
            disp_x = 0;
            disp_y++;
            move(disp_y, disp_x);
        }

        if ((v_height + v_line) == p.line && v_char == p.column) {
            p_x = disp_x;
            p_y = disp_y;
        }

        offset++;
    }

    /* Render the status line */
    move(height, 0);
    attron(A_REVERSE | A_BOLD);
    if (status != NULL) addstr(status);
    char posbuf[80] = {0};
    snprintf(posbuf, sizeof(posbuf), "%zd,%zd", p.column, p.line);
    move(height, width - strlen(posbuf));
    addstr(posbuf);

    /* Calculate how many remaining lines would fit */
    v_height += height - disp_y;

    /* Move the pointer and return */
    move(p_y, p_x);
    return v_height;
}

bool view_buf(Buf* buf, char* path) {
    /* Handle displaying and viewing the given buffer using a ncurses
     * interface - ie the main editor functionality.
     */

    /* Pointer position */
    Pointer p = {0};

    /* Initial view position */
    size_t v_line = 0;
    size_t v_height = 0; /* Lines currently shown on the display */

    /* Current status line */
    char* status = NULL;

    /* Current copy buffer */
    char* copy_buf = NULL;

    while (1) {
        erase();
        standend();
        v_height = render_buf(buf, v_line, p, status);
        refresh();
        status = NULL;

        size_t offset = line2offset(buf, p.line, p.column);
        size_t line_len = buf_linelen(buf, p.line);
        int c = getch();
        if ((c >= 32 && c <= 126) || c == '\t') {
            buf_sel_delete(buf, &p);
            status = buf_insert(buf, offset, c);
            if (status == ERR_OK) {
                p.column++;
                p.old_column = p.column;
            }
        }
        if (c == '\n') {
            buf_sel_delete(buf, &p);
            status = buf_insert(buf, offset, c);
            if (status == ERR_OK) {
                p.line++;
                p.column = 0;
                p.old_column = p.column;
            }
        }
        if (c == KEY_DC || c == KEY_BACKSPACE) {
            buf_sel_delete(buf, &p);
            if (p.column != 0) {
                p.column--;
                p.old_column = p.column;
            } else if (p.line > 0) {
                p.line--;
                line_len = buf_linelen(buf, p.line);
                p.column = line_len;
                p.old_column = p.column;
            }
            /* Delete the character after moving to ensure linelength
             * calculation is correct.
             */
            buf_delete(buf, offset);
        }
        if (c == KEY_LEFT && p.column != 0) {
            p.column--;
            p.old_column = p.column;
        }
        if (c == KEY_RIGHT && p.column < line_len) {
            p.column++;
            p.old_column = p.column;
        }
        if (c == KEY_UP && p.line != 0) {
            p.line--;
            line_len = buf_linelen(buf, p.line);
            p.column = p.old_column;
            if (p.column > line_len) p.column = line_len;
        }
        if (c == KEY_DOWN && p.line < buf_linecount(buf)) {
            p.line++;
            line_len = buf_linelen(buf, p.line);
            p.column = p.old_column;
            if (p.column > line_len) p.column = line_len;
        }
        if (c == KEY_NPAGE) {
            size_t linecount = buf_linecount(buf);
            if (p.line + v_height > linecount) {
                p.line = linecount;
            } else {
                p.line += v_height;
            }
            line_len = buf_linelen(buf, p.line);
            p.column = p.old_column;
            if (p.column > line_len) p.column = line_len;
        }
        if (c == KEY_PPAGE) {
            if (v_height > p.line) p.line = 0; else p.line -= v_height;
            line_len = buf_linelen(buf, p.line);
            p.column = p.old_column;
            if (p.column > line_len) p.column = line_len;
        }
        if (c == KEY_HOME) {
            p.column = 0;
            p.old_column = p.column;
        }
        if (c == KEY_END) {
            p.column = line_len;
            p.old_column = p.column;
        }
        /* Control keys */
        if (c + 64 == 'S') {
            status = buf_write(buf, path);
        }
        if (c + 64 == 'Q') {
            return true;
        }
        if (c + 64 == 'T') {
            p.sel_on = !p.sel_on;
            p.sel_line = p.line;
            p.sel_column = p.column;
        }
        if (c + 64 == 'X' && set_copy_buf(&copy_buf, buf, p, &status)) {
            buf_sel_delete(buf, &p);
        }
        if (c + 64 == 'C') {
            set_copy_buf(&copy_buf, buf, p, &status);
        }
        if (c + 64 == 'P' && copy_buf != NULL) {
        }

        /* Shift the current display so that the cursor remains visible */
        size_t v_shift = v_height; /* Needs to be <= height */
        while (v_line + v_height <= p.line) {
            v_line += v_shift;
        }
        while (v_line > p.line) {
            if (v_line < v_shift) v_line = 0; else v_line -= v_shift;
        }
    }
}

int main(int count, char** argv) {
    name = __FILE__;
    if (count > 0) name = argv[0];
    if (count != 2) {
        fprintf(stderr, "usage: %s <filename>\n", name);
        exit(EXIT_FAILURE);
    }
    char* path = argv[1];

    Buf buf = {0};
    if (!buf_load(&buf, path)) exit(EXIT_FAILURE);

    initscr();
    bool ok = true;
    ok = ok && (raw() != ERR);
    ok = ok && (noecho() != ERR);
    ok = ok && (keypad(stdscr, true) != ERR);
    if (!ok) {
        fprintf(stderr, "%s: failed to initialise ncurses\n", name);
        exit(EXIT_FAILURE);
    }

    ok = view_buf(&buf, path);
    endwin();
    if (!ok) exit(EXIT_FAILURE);
}
