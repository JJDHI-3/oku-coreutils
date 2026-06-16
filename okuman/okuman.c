#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_LINE_LEN 16384
#define MAX_ARGS 128

// ANSI Formatting
#define BOLD      "\033[1m"
#define UNDERLINE "\033[4m"
#define RESET     "\033[0m"

// Global Rendering State
int current_indent = 0;
int margin_stack[32] = {4}; 
int margin_ptr = 0;
int in_tagged_paragraph = 0;

// Text Word-Filling Buffers
char text_buffer[MAX_LINE_LEN] = {0};
int word_started = 0;

void print_indent(int spaces) {
    for (int i = 0; i < spaces; i++) putchar(' ');
}

// Flush the compiled filled text string to the screen
void flush_text_buffer() {
    if (strlen(text_buffer) == 0) return;
    
    print_indent(current_indent);
    printf("%s\n", text_buffer);
    
    // Clear the buffer state
    text_buffer[0] = '\0';
    word_started = 0;
}

// Safe string appender to protect buffer limits
void append_to_buffer(const char *str) {
    if (strlen(text_buffer) + strlen(str) >= MAX_LINE_LEN - 1) return;
    strcat(text_buffer, str);
}

// Parses macro arguments respecting double quotes
int parse_args(char *line, char **argv) {
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGS) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;

        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') { *p = '\0'; p++; }
        } else {
            argv[argc++] = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) { *p = '\0'; p++; }
        }
    }
    return argc;
}

// Process inline text escape tokens without inserting immediate newlines
void process_inline_text(const char *src) {
    char temp[4] = {0};
    while (*src) {
        if (*src == '\\') {
            src++;
            if (*src == '\0') break;

            switch (*src) {
                case 'f': // Inline font shifts (\fB, \fI, \fP)
                    src++;
                    if (*src == 'B' || *src == '3') append_to_buffer(BOLD);
                    else if (*src == 'I' || *src == '2') append_to_buffer(UNDERLINE);
                    else if (*src == 'R' || *src == 'P' || *src == '1') append_to_buffer(RESET);
                    if (*src) src++;
                    break;
                case '-':
                    append_to_buffer("-");
                    src++;
                    break;
                case 'e':
                    append_to_buffer("\\");
                    src++;
                    break;
                case '&': // Zero-width space
                    src++;
                    break;
                case '0':
                case ' ':
                case 'p': // Unpaddable spaces
                    append_to_buffer(" ");
                    src++;
                    break;
                case '(': // 2-Character Symbols
                    src++;
                    if (*src && *(src + 1)) {
                        if (strncmp(src, "en", 2) == 0) append_to_buffer("-");
                        else if (strncmp(src, "em", 2) == 0) append_to_buffer("—");
                        else if (strncmp(src, "aq", 2) == 0) append_to_buffer("'");
                        else if (strncmp(src, "dq", 2) == 0) append_to_buffer("\"");
                        else if (strncmp(src, "oq", 2) == 0) append_to_buffer("`");
                        else if (strncmp(src, "cq", 2) == 0) append_to_buffer("'");
                        else if (strncmp(src, "bu", 2) == 0) append_to_buffer("•");
                        src += 2;
                    }
                    break;
                default:
                    temp[0] = '\\'; temp[1] = *src; temp[2] = '\0';
                    append_to_buffer(temp);
                    src++;
                    break;
            }
        } else {
            temp[0] = *src; temp[1] = '\0';
            append_to_buffer(temp);
            src++;
        }
    }
    append_to_buffer(RESET);
}

void process_alternating(char **argv, int argc, const char *f1, const char *f2) {
    for (int i = 0; i < argc; i++) {
        append_to_buffer((i % 2 == 0) ? f1 : f2);
        process_inline_text(argv[i]);
    }
}

void process_macro(char *macro, char *args_line) {
    char *argv[MAX_ARGS];
    int argc = parse_args(args_line, argv);

    // Structural Layout triggers a buffer flush
    if (strcmp(macro, ".TH") == 0 || strcmp(macro, ".SH") == 0 || strcmp(macro, ".SS") == 0 ||
        strcmp(macro, ".PP") == 0 || strcmp(macro, ".P") == 0 || strcmp(macro, ".LP") == 0 ||
        strcmp(macro, ".IP") == 0 || strcmp(macro, ".TP") == 0 || strcmp(macro, ".RS") == 0 || 
        strcmp(macro, ".RE") == 0 || strcmp(macro, ".br") == 0) {
        flush_text_buffer();
    }

    if (strcmp(macro, ".TH") == 0) {
        if (argc >= 2) {
            printf(BOLD "%s(%s)" RESET, argv[0], argv[1]);
            if (argc >= 5) printf("   --   %s", argv[4]);
            printf("\n");
        }
    } 
    else if (strcmp(macro, ".SH") == 0) {
        margin_stack[margin_ptr] = 4;
        current_indent = 0;
        printf("\n" BOLD);
        if (argc > 0) { process_inline_text(argv[0]); printf("%s", text_buffer); text_buffer[0] = '\0'; }
        printf(RESET "\n");
        current_indent = margin_stack[margin_ptr];
    } 
    else if (strcmp(macro, ".SS") == 0) {
        margin_stack[margin_ptr] = 4;
        current_indent = 4;
        printf("\n");
        print_indent(current_indent);
        printf(BOLD);
        if (argc > 0) { process_inline_text(argv[0]); printf("%s", text_buffer); text_buffer[0] = '\0'; }
        printf(RESET "\n");
        current_indent = 7;
    } 
    else if (strcmp(macro, ".RS") == 0) {
        int delta = 4;
        if (argc > 0) delta = atoi(argv[0]);
        if (margin_ptr < 31) {
            margin_stack[margin_ptr + 1] = margin_stack[margin_ptr] + delta;
            margin_ptr++;
        }
        current_indent = margin_stack[margin_ptr];
    } 
    else if (strcmp(macro, ".RE") == 0) {
        if (margin_ptr > 0) margin_ptr--;
        current_indent = margin_stack[margin_ptr];
    } 
    else if (strcmp(macro, ".PP") == 0 || strcmp(macro, ".P") == 0 || strcmp(macro, ".LP") == 0) {
        printf("\n");
        current_indent = margin_stack[margin_ptr];
    } 
    else if (strcmp(macro, ".IP") == 0) {
        printf("\n");
        current_indent = margin_stack[margin_ptr];
        if (argc > 0) {
            process_inline_text(argv[0]);
            // If it's a list item bullet point like "\(bu 3", indent the text following it
            if (argc > 1) {
                int custom_dist = atoi(argv[1]);
                flush_text_buffer();
                current_indent = margin_stack[margin_ptr] + custom_dist;
                word_started = 1; 
                return;
            }
        }
        flush_text_buffer();
    } 
    else if (strcmp(macro, ".TP") == 0) {
        printf("\n");
        in_tagged_paragraph = 1;
        current_indent = margin_stack[margin_ptr];
    } 
    else if (strcmp(macro, ".br") == 0) {
        printf("\n");
    }
    // Font Blocks
    else if (strcmp(macro, ".B") == 0) {
        if (word_started) append_to_buffer(" ");
        append_to_buffer(BOLD);
        for (int i = 0; i < argc; i++) { process_inline_text(argv[i]); if (i < argc - 1) append_to_buffer(" "); }
        append_to_buffer(RESET);
        word_started = 1;
    } 
    else if (strcmp(macro, ".I") == 0) {
        if (word_started) append_to_buffer(" ");
        append_to_buffer(UNDERLINE);
        for (int i = 0; i < argc; i++) { process_inline_text(argv[i]); if (i < argc - 1) append_to_buffer(" "); }
        append_to_buffer(RESET);
        word_started = 1;
    } 
    // Alternating Fonts
    else if (strcmp(macro, ".BR") == 0) { if (word_started) append_to_buffer(" "); process_alternating(argv, argc, BOLD, RESET); word_started = 1; }
    else if (strcmp(macro, ".BI") == 0) { if (word_started) append_to_buffer(" "); process_alternating(argv, argc, BOLD, UNDERLINE); word_started = 1; }
    else if (strcmp(macro, ".IR") == 0) { if (word_started) append_to_buffer(" "); process_alternating(argv, argc, UNDERLINE, RESET); word_started = 1; }
    else if (strcmp(macro, ".IB") == 0) { if (word_started) append_to_buffer(" "); process_alternating(argv, argc, UNDERLINE, BOLD); word_started = 1; }
    else if (strcmp(macro, ".RB") == 0) { if (word_started) append_to_buffer(" "); process_alternating(argv, argc, RESET, BOLD); word_started = 1; }
    else if (strcmp(macro, ".RI") == 0) { if (word_started) append_to_buffer(" "); process_alternating(argv, argc, RESET, UNDERLINE); word_started = 1; }
}

void parse_line(char *line) {
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }

    // Ignore comments
    if (strncmp(line, ".\\\"", 3) == 0 || strncmp(line, "\\\"", 2) == 0) return;

    if (line[0] == '.') {
        char macro[8] = {0};
        char *p = line;
        int i = 0;
        while (*p && !isspace((unsigned char)*p) && i < 7) {
            macro[i++] = *p++;
        }
        while (*p && isspace((unsigned char)*p)) p++;
        process_macro(macro, p);
    } else {
        if (strlen(line) == 0) {
            flush_text_buffer();
            printf("\n");
            return;
        }

        if (in_tagged_paragraph) {
            // This line is the tag header itself
            print_indent(current_indent);
            process_inline_text(line);
            printf("%s\n", text_buffer);
            text_buffer[0] = '\0';
            
            // Setup alignment for subsequent body lines
            current_indent = margin_stack[margin_ptr] + 8;
            in_tagged_paragraph = 0;
            word_started = 0;
        } else {
            if (word_started && text_buffer[0] != '\0') {
                append_to_buffer(" ");
            }
            process_inline_text(line);
            word_started = 1;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <path_to_man_page>\n", argv[0]);
        return 1;
    }

    FILE *file = fopen(argv[1], "r");
    if (!file) {
        perror("Error opening file");
        return 1;
    }

    char buffer[MAX_LINE_LEN];
    while (fgets(buffer, sizeof(buffer), file) != NULL) {
        parse_line(buffer);
    }
    flush_text_buffer(); // Catch any leftover strings

    fclose(file);
    return 0;
}
