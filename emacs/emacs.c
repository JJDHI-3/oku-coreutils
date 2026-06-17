/* OKU Emacs -- a extensible text editor. */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define OKU_VERSION "0.0.1"
#define TAB_STOP 8
#define CTRL_KEY(k) ((k) & 0x1f)
#define META_BASE 2000

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

typedef struct erow {
    int size, rsize;
    char *chars;
    char *render;
} erow;

struct editorConfig {
    int cx, cy, rx;
    int rowoff, coloff;
    int screenrows, screencols;
    int numrows;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    int mark_active;
    int mark_x, mark_y;
    char *killbuf;
    struct termios orig_termios;
};

struct abuf {
    char *b;
    int len;
};
#define ABUF_INIT {NULL, 0}

static struct editorConfig E;

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen(void);
char *editorPrompt(char *prompt, void (*callback)(char *, int));
void editorFindFile(void);
void editorSave(void);
void editorSaveAs(void);
void editorOpen(char *filename);
void editorInsertChar(int c);
void editorInsertNewline(void);

/* ---------- terminal ---------- */

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
    perror(s);
    exit(1);
}

void disableRawMode(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");
}

void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);
    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey(void) {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (seq[0] == '[') {
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
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
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        } else {
            return META_BASE + (unsigned char)seq[0];
        }
        return '\x1b';
    }
    return (unsigned char)c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/* ---------- row ops ---------- */

void editorUpdateRow(erow *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++) if (row->chars[j] == '\t') tabs++;
    free(row->render);
    row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);
    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.row[at].render = NULL;
    E.row[at].rsize = 0;
    editorUpdateRow(&E.row[at]);
    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/* ---------- editing ---------- */

void editorInsertChar(int c) {
    if (E.cy == E.numrows) editorInsertRow(E.numrows, "", 0);
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline(void) {
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void editorDelChar(void) {
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;
    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

void editorDelCharForward(void) {
    if (E.cy == E.numrows) return;
    erow *row = &E.row[E.cy];
    if (E.cx < row->size) {
        editorRowDelChar(row, E.cx);
    } else if (E.cy < E.numrows - 1) {
        erow *next = &E.row[E.cy + 1];
        editorRowAppendString(row, next->chars, next->size);
        editorDelRow(E.cy + 1);
    }
}

/* ---------- movement ---------- */

void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) E.cx--;
            else if (E.cy > 0) { E.cy--; E.cx = E.row[E.cy].size; }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) E.cx++;
            else if (row && E.cx == row->size) { E.cy++; E.cx = 0; }
            break;
        case ARROW_UP:
            if (E.cy != 0) E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows - 1) E.cy++;
            break;
    }
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

void editorForwardWord(void) {
    while (E.cy < E.numrows) {
        erow *row = &E.row[E.cy];
        while (E.cx < row->size && !isalnum((unsigned char)row->chars[E.cx])) E.cx++;
        if (E.cx == row->size) {
            if (E.cy == E.numrows - 1) return;
            E.cy++; E.cx = 0;
            continue;
        }
        while (E.cx < row->size && isalnum((unsigned char)row->chars[E.cx])) E.cx++;
        return;
    }
}

void editorBackwardWord(void) {
    while (1) {
        if (E.cx == 0) {
            if (E.cy == 0) return;
            E.cy--; E.cx = E.row[E.cy].size;
            if (E.cx == 0) continue;
        }
        erow *row = &E.row[E.cy];
        while (E.cx > 0 && !isalnum((unsigned char)row->chars[E.cx - 1])) E.cx--;
        while (E.cx > 0 && isalnum((unsigned char)row->chars[E.cx - 1])) E.cx--;
        return;
    }
}

void editorScrollPage(int dir) {
    if (dir > 0) {
        E.cy += E.screenrows - 2;
        if (E.cy >= E.numrows) E.cy = E.numrows ? E.numrows - 1 : 0;
    } else {
        E.cy -= E.screenrows - 2;
        if (E.cy < 0) E.cy = 0;
    }
    if (E.cy < E.numrows && E.cx > E.row[E.cy].size) E.cx = E.row[E.cy].size;
}

/* ---------- kill ring / region ---------- */

void editorNormalizeRegion(int *sy, int *sx, int *ey, int *ex) {
    *sy = E.mark_y; *sx = E.mark_x; *ey = E.cy; *ex = E.cx;
    if (*sy > *ey || (*sy == *ey && *sx > *ex)) {
        int t;
        t = *sy; *sy = *ey; *ey = t;
        t = *sx; *sx = *ex; *ex = t;
    }
}

char *editorRegionToString(int sy, int sx, int ey, int ex) {
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    for (int y = sy; y <= ey; y++) {
        erow *row = &E.row[y];
        int start = (y == sy) ? sx : 0;
        int end = (y == ey) ? ex : row->size;
        int n = end - start;
        if (n < 0) n = 0;
        while (len + n + 2 > cap) { cap *= 2; buf = realloc(buf, cap); }
        memcpy(buf + len, row->chars + start, n);
        len += n;
        if (y != ey) buf[len++] = '\n';
    }
    buf[len] = '\0';
    return buf;
}

void editorDeleteRegion(int sy, int sx, int ey, int ex) {
    if (sy == ey) {
        erow *row = &E.row[sy];
        memmove(&row->chars[sx], &row->chars[ex], row->size - ex + 1);
        row->size -= (ex - sx);
        editorUpdateRow(row);
    } else {
        erow *srow = &E.row[sy];
        erow *lrow = &E.row[ey];
        int taillen = lrow->size - ex;
        srow->chars = realloc(srow->chars, sx + taillen + 1);
        memcpy(&srow->chars[sx], &lrow->chars[ex], taillen);
        srow->size = sx + taillen;
        srow->chars[srow->size] = '\0';
        editorUpdateRow(srow);
        for (int y = ey; y > sy; y--) editorDelRow(y);
    }
    E.cy = sy; E.cx = sx;
    E.dirty++;
}

void editorKillLine(void) {
    if (E.cy >= E.numrows) return;
    erow *row = &E.row[E.cy];
    free(E.killbuf);
    if (E.cx < row->size) {
        int len = row->size - E.cx;
        E.killbuf = malloc(len + 1);
        memcpy(E.killbuf, &row->chars[E.cx], len);
        E.killbuf[len] = '\0';
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
        E.dirty++;
    } else if (E.cy < E.numrows - 1) {
        erow *next = &E.row[E.cy + 1];
        E.killbuf = strdup("\n");
        row->chars = realloc(row->chars, row->size + next->size + 1);
        memcpy(&row->chars[row->size], next->chars, next->size);
        row->size += next->size;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
        editorDelRow(E.cy + 1);
    } else {
        E.killbuf = strdup("");
    }
}

void editorYank(void) {
    if (!E.killbuf) return;
    for (char *p = E.killbuf; *p; p++) {
        if (*p == '\n') editorInsertNewline();
        else editorInsertChar((unsigned char)*p);
    }
}

void editorKillRegion(void) {
    if (!E.mark_active) { editorSetStatusMessage("No mark set"); return; }
    int sy, sx, ey, ex;
    editorNormalizeRegion(&sy, &sx, &ey, &ex);
    free(E.killbuf);
    E.killbuf = editorRegionToString(sy, sx, ey, ex);
    editorDeleteRegion(sy, sx, ey, ex);
    E.mark_active = 0;
}

void editorCopyRegion(void) {
    if (!E.mark_active) { editorSetStatusMessage("No mark set"); return; }
    int sy, sx, ey, ex;
    editorNormalizeRegion(&sy, &sx, &ey, &ex);
    free(E.killbuf);
    E.killbuf = editorRegionToString(sy, sx, ey, ex);
    E.mark_active = 0;
    editorSetStatusMessage("Region copied");
}

/* ---------- search ---------- */

void editorFindCallback(char *query, int key) {
    static int last_match = -1;
    static int direction = 1;
    if (key == '\r' || key == '\x1b' || key == CTRL_KEY('g')) {
        last_match = -1; direction = 1; return;
    } else if (key == CTRL_KEY('s')) {
        direction = 1;
    } else if (key == CTRL_KEY('r')) {
        direction = -1;
    } else {
        last_match = -1; direction = 1;
    }
    if (last_match == -1) direction = 1;
    int current = last_match;
    for (int i = 0; i < E.numrows; i++) {
        current += direction;
        if (current == -1) current = E.numrows - 1;
        else if (current == E.numrows) current = 0;
        erow *row = &E.row[current];
        char *match = strstr(row->chars, query);
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = match - row->chars;
            E.rowoff = E.numrows;
            break;
        }
    }
}

void editorIsearch(int dir) {
    int sx = E.cx, sy = E.cy, sro = E.rowoff, sco = E.coloff;
    char *query = editorPrompt(dir > 0 ? "I-search: %s" : "I-search backward: %s", editorFindCallback);
    if (query) {
        free(query);
    } else {
        E.cx = sx; E.cy = sy; E.rowoff = sro; E.coloff = sco;
    }
}

/* ---------- file i/o ---------- */

char *editorRowsToString(int *buflen) {
    int totlen = 0;
    for (int j = 0; j < E.numrows; j++) totlen += E.row[j].size + 1;
    *buflen = totlen;
    char *buf = malloc(totlen ? totlen : 1);
    char *p = buf;
    for (int j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p++ = '\n';
    }
    return buf;
}

void editorOpen(char *filename) {
    for (int i = 0; i < E.numrows; i++) editorFreeRow(&E.row[i]);
    free(E.row);
    E.row = NULL;
    E.numrows = 0;
    free(E.filename);
    E.filename = strdup(filename);
    E.cx = 0; E.cy = 0; E.rowoff = 0; E.coloff = 0; E.dirty = 0;
    E.mark_active = 0;

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        editorSetStatusMessage("New file");
        return;
    }
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) linelen--;
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorFindFile(void) {
    char *fname = editorPrompt("Find file: %s", NULL);
    if (!fname) return;
    editorOpen(fname);
    free(fname);
}

void editorSave(void) {
    if (E.filename == NULL) {
        char *fname = editorPrompt("File to save in: %s", NULL);
        if (fname == NULL) { editorSetStatusMessage("Save aborted"); return; }
        E.filename = fname;
    }
    int len;
    char *buf = editorRowsToString(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1 && write(fd, buf, len) == len) {
            close(fd);
            free(buf);
            E.dirty = 0;
            editorSetStatusMessage("Wrote %s", E.filename);
            return;
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

void editorSaveAs(void) {
    char *fname = editorPrompt("Write file: %s", NULL);
    if (!fname) return;
    free(E.filename);
    E.filename = fname;
    editorSave();
}

/* ---------- M-x ---------- */

void editorExecuteExtendedCommand(void) {
    char *cmd = editorPrompt("M-x %s", NULL);
    if (!cmd) return;
    if (strcmp(cmd, "save-buffer") == 0) {
        editorSave();
    } else if (strcmp(cmd, "find-file") == 0) {
        editorFindFile();
    } else if (strcmp(cmd, "write-file") == 0) {
        editorSaveAs();
    } else if (strcmp(cmd, "goto-line") == 0) {
        char *line = editorPrompt("Goto line: %s", NULL);
        if (line) {
            int n = atoi(line);
            E.cy = n - 1;
            if (E.cy < 0) E.cy = 0;
            if (E.cy >= E.numrows) E.cy = E.numrows ? E.numrows - 1 : 0;
            E.cx = 0;
            free(line);
        }
    } else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "save-buffers-kill-terminal") == 0) {
        write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
        exit(0);
    } else {
        editorSetStatusMessage("[%s is not a valid command]", cmd);
    }
    free(cmd);
}

/* ---------- minibuffer prompt ---------- */

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';
    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();
        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b' || c == CTRL_KEY('g')) {
            editorSetStatusMessage("Quit");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) { bufsize *= 2; buf = realloc(buf, bufsize); }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if (callback) callback(buf, c);
    }
}

/* ---------- screen ---------- */

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) { free(ab->b); }

int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        rx++;
    }
    return rx;
}

void editorScroll(void) {
    E.rx = 0;
    if (E.cy < E.numrows) E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    if (E.rx < E.coloff) E.coloff = E.rx;
    if (E.rx >= E.coloff + E.screencols) E.coloff = E.rx - E.screencols + 1;
}

void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int wlen = snprintf(welcome, sizeof(welcome), "OKU Emacs %s -- a extensible text editor", OKU_VERSION);
                if (wlen > E.screencols) wlen = E.screencols;
                int padding = (E.screencols - wlen) / 2;
                if (padding) { abAppend(ab, "~", 1); padding--; }
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, wlen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            erow *row = &E.row[filerow];
            int len = row->rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, &row->render[E.coloff], len);
        }
        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "--- %.30s%s   *OKU Emacs*",
                        E.filename ? E.filename : "*scratch*",
                        E.dirty ? " [Modified]" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "L%d C%d ---", E.cy + 1, E.rx + 1);
    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen) { abAppend(ab, rstatus, rlen); break; }
        abAppend(ab, " ", 1);
        len++;
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 8) abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen(void) {
    editorScroll();
    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);
    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));
    abAppend(&ab, "\x1b[?25h", 6);
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/* ---------- input dispatch ---------- */

void editorProcessKeypress(void) {
    static int waiting_for_x = 0;
    int c = editorReadKey();

    if (waiting_for_x) {
        waiting_for_x = 0;
        switch (c) {
            case CTRL_KEY('c'):
                if (E.dirty) {
                    char *resp = editorPrompt("Active buffer modified; kill anyway? (yes or no) %s", NULL);
                    if (!resp) { editorSetStatusMessage(""); return; }
                    int ok = (strcmp(resp, "yes") == 0);
                    free(resp);
                    if (!ok) { editorSetStatusMessage("Quit"); return; }
                }
                write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
                exit(0);
            case CTRL_KEY('s'): editorSave(); break;
            case CTRL_KEY('w'): editorSaveAs(); break;
            case CTRL_KEY('f'): editorFindFile(); break;
            case 'b': editorSetStatusMessage("No other buffers"); break;
            case CTRL_KEY('g'): editorSetStatusMessage("Quit"); break;
            default: editorSetStatusMessage("C-x %c is undefined", c < 128 ? c : '?');
        }
        return;
    }

    switch (c) {
        case '\r':
            editorInsertNewline();
            break;
        case CTRL_KEY('x'):
            waiting_for_x = 1;
            break;
        case CTRL_KEY('g'):
            E.mark_active = 0;
            editorSetStatusMessage("Quit");
            break;
        case CTRL_KEY('f'): case ARROW_RIGHT:
            editorMoveCursor(ARROW_RIGHT);
            break;
        case CTRL_KEY('b'): case ARROW_LEFT:
            editorMoveCursor(ARROW_LEFT);
            break;
        case CTRL_KEY('n'): case ARROW_DOWN:
            editorMoveCursor(ARROW_DOWN);
            break;
        case CTRL_KEY('p'): case ARROW_UP:
            editorMoveCursor(ARROW_UP);
            break;
        case CTRL_KEY('a'): case HOME_KEY:
            E.cx = 0;
            break;
        case CTRL_KEY('e'): case END_KEY:
            if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
            break;
        case META_BASE + 'f':
            editorForwardWord();
            break;
        case META_BASE + 'b':
            editorBackwardWord();
            break;
        case CTRL_KEY('v'): case PAGE_DOWN:
            editorScrollPage(1);
            break;
        case META_BASE + 'v': case PAGE_UP:
            editorScrollPage(-1);
            break;
        case CTRL_KEY('l'):
            E.rowoff = E.cy - E.screenrows / 2;
            if (E.rowoff < 0) E.rowoff = 0;
            break;
        case CTRL_KEY('d'): case DEL_KEY:
            editorDelCharForward();
            break;
        case BACKSPACE: case CTRL_KEY('h'):
            editorDelChar();
            break;
        case 0:
            E.mark_active = 1;
            E.mark_x = E.cx;
            E.mark_y = E.cy;
            editorSetStatusMessage("Mark set");
            break;
        case CTRL_KEY('k'):
            editorKillLine();
            break;
        case CTRL_KEY('y'):
            editorYank();
            break;
        case CTRL_KEY('w'):
            editorKillRegion();
            break;
        case META_BASE + 'w':
            editorCopyRegion();
            break;
        case META_BASE + 'x':
            editorExecuteExtendedCommand();
            break;
        case CTRL_KEY('s'):
            editorIsearch(1);
            break;
        case CTRL_KEY('r'):
            editorIsearch(-1);
            break;
        case CTRL_KEY('q'): {
            int c2 = editorReadKey();
            if (!iscntrl(c2) || c2 == '\t') editorInsertChar(c2);
            break;
        }
        case '\x1b':
            break;
        default:
            if (!iscntrl(c) && c < 128) editorInsertChar(c);
            break;
    }
}

/* ---------- init ---------- */

void initEditor(void) {
    E.cx = 0; E.cy = 0; E.rx = 0;
    E.rowoff = 0; E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.mark_active = 0;
    E.killbuf = NULL;
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) editorOpen(argv[1]);
    editorSetStatusMessage("OKU Emacs %s -- C-x C-c to exit, C-x C-s to save, M-x for commands", OKU_VERSION);
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
