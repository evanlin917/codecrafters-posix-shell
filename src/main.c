#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <limits.h> // For PATH_MAX

#define BUF_SIZE 512
#define MAX_ARGS 64

// Structure to hold parsing state (quoting)
typedef struct {
    int in_single_quote;
    int in_double_quote;
} ParseState;

// Structure for dynamic argument buffer
typedef struct {
    char* buffer;
    size_t capacity;
    size_t length;
} ArgBuffer;

// Helper function to initialize argument buffer
ArgBuffer* init_arg_buffer() {
    ArgBuffer* buf = malloc(sizeof(ArgBuffer));
    if (!buf) {
        perror("init_arg_buffer: malloc failed for ArgBuffer");
        return NULL;
    }

    buf->capacity = 64; // Initial capacity
    buf->length = 0;
    buf->buffer = malloc(buf->capacity);
    if (!buf->buffer) {
        perror("init_arg_buffer: malloc failed for buffer");
        free(buf);
        return NULL;
    }
    buf->buffer[0] = '\0'; // Null-terminate an empty string

    return buf;
}

// Helper function to add character to argument buffer with dynamic resizing
int add_char_to_buffer(ArgBuffer* buf, char c) {
    if (buf->length + 1 >= buf->capacity) {
        size_t new_capacity = buf->capacity * 2;
        char* new_buffer = realloc(buf->buffer, new_capacity);
        if (!new_buffer) {
            perror("add_char_to_buffer: realloc failed");
            return -1;
        }
        buf->buffer = new_buffer;
        buf->capacity = new_capacity;
    }

    buf->buffer[buf->length++] = c;
    buf->buffer[buf->length] = '\0';
    return 0;
}

// Helper function to free argument buffer
void free_arg_buffer(ArgBuffer* buf) {
    if (buf != NULL) {
        free(buf->buffer);
        free(buf);
    }
}

// Helper function to cleanup argv on error or after use
void free_argv(char** argv) {
    if (argv == NULL) {
        return;
    }

    for (int i = 0; argv[i] != NULL; i++) {
        free(argv[i]);
    }
    free(argv);
}

// Helper to process C-style backslash escapes (like \n, \t)
// Returns 0 if not a C-style escape, 1 if processed.
// If processed, `*escaped_char` will hold the resulting character.
// `*chars_consumed` will hold how many chars from `input` were processed (e.g., 2 for \n).
int process_c_style_escape(const char* input, char* escaped_char, int* chars_consumed) {
    if (input[0] == '\0') { // Need at least one character after backslash
        return 0;
    }

    *chars_consumed = 2; // Default for \X where X is one char

    switch (input[0]) {
        case 'n': *escaped_char = '\n'; break;
        case 't': *escaped_char = '\t'; break;
        case 'b': *escaped_char = '\b'; break;
        case 'f': *escaped_char = '\f'; break;
        case 'v': *escaped_char = '\v'; break;
        case 'a': *escaped_char = '\a'; break;
        case 'r': *escaped_char = '\r'; break;
        case '\\': *escaped_char = '\\'; break; // Escaping backslash itself
        case '"': *escaped_char = '"'; break;   // Escaping double quote
        case '\'': *escaped_char = '\''; break; // Escaping single quote
        case '$': *escaped_char = '$'; break;   // Escaping dollar sign
        case '`': *escaped_char = '`'; break;   // Escaping backtick (this was also potentially problematic if it had similar artifacts)
        default: return 0; // Not a recognized C-style escape
    }
    return 1;
}

// Helper to process octal escape sequences (e.g., \0, \00, \000, \123)
// Returns number of characters consumed (including the '\'), or 0 if not a valid octal sequence.
// If successful, `*escaped_char` will hold the resulting character.
int process_octal_escape(const char* input, char* escaped_char) {
    int val = 0;
    int digits = 0;
    int i = 0;

    // Check for up to 3 octal digits
    while (i < 3 && isdigit((unsigned char)input[i]) && input[i] >= '0' && input[i] <= '7') {
        val = val * 8 + (input[i] - '0');
        digits++;
        i++;
    }

    if (digits > 0) {
        *escaped_char = (char)val;
        return digits + 1; // +1 for the leading backslash
    }
    return 0; // Not a valid octal sequence
}


// --- Core Parsing Function ---
char** parse_arguments(const char* input_line) {
    char** argv = malloc(MAX_ARGS * sizeof(char*));
    if (argv == NULL) {
        perror("parse_arguments: malloc failed for argv");
        return NULL;
    }
    int argc = 0;

    ParseState state = {0, 0}; // Initialize state: not in single or double quotes
    ArgBuffer* current_arg_buffer = init_arg_buffer();
    if (!current_arg_buffer) {
        free(argv);
        return NULL;
    }

    int i = 0; // Current position in input_line

    // Skip initial whitespace
    while (isspace((unsigned char)input_line[i])) {
        i++;
    }

    while (input_line[i] != '\0') {
        char current_char = input_line[i];

        if (state.in_single_quote) {
            if (current_char == '\'') {
                state.in_single_quote = 0; // End single quote
            } else {
                // In single quotes, ALL characters are literal, including backslashes.
                if (add_char_to_buffer(current_arg_buffer, current_char) < 0) {
                    free_arg_buffer(current_arg_buffer);
                    free_argv(argv);
                    return NULL;
                }
            }
            i++;
        } else if (state.in_double_quote) {
            if (current_char == '"') {
                state.in_double_quote = 0; // End double quote
                i++;
            } else if (current_char == '\\') {
                char escaped_char_val;
                int consumed_chars = 0;

                // Try C-style escapes first (like \n, \t)
                if (process_c_style_escape(&input_line[i+1], &escaped_char_val, &consumed_chars)) {
                    if (add_char_to_buffer(current_arg_buffer, escaped_char_val) < 0) {
                        free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
                    }
                    i += consumed_chars; // Advance past \ and the escaped char
                }
                // Then try octal escapes (like \033)
                else if ((consumed_chars = process_octal_escape(&input_line[i+1], &escaped_char_val)) > 0) {
                     if (add_char_to_buffer(current_arg_buffer, escaped_char_val) < 0) {
                        free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
                    }
                    i += consumed_chars; // Advance past \ and the octal digits
                }
                // If not a recognized escape, the backslash itself is literal
                else {
                    if (add_char_to_buffer(current_arg_buffer, current_char) < 0) { // Add '\\'
                        free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
                    }
                    i++; // Advance past the literal backslash
                    // If a character follows, it's also literal
                    if (input_line[i] != '\0') {
                        if (add_char_to_buffer(current_arg_buffer, input_line[i]) < 0) {
                            free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
                        }
                        i++; // Advance past the literal character after backslash
                    }
                }
            } else {
                // Regular character in double quotes, add to buffer
                if (add_char_to_buffer(current_arg_buffer, current_char) < 0) {
                    free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
                }
                i++;
            }
        } else { // Not in any quotes
            if (current_char == '\\') {
                char escaped_char_val;
                int consumed_chars = 0;

                // Try C-style escapes (often not processed unquoted by default shells,
                // but if your "read more" implies it, here's where it goes)
                // For typical shell, \n unquoted is 'n'
                // Revert to simpler interpretation for unquoted \
                
                i++; // Advance past the backslash
                if (input_line[i] == '\0') {
                    // Trailing backslash unquoted is literal
                    if (add_char_to_buffer(current_arg_buffer, '\\') < 0) {
                        free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
                    }
                } else {
                    // Unquoted backslash escapes the next character.
                    // This means the next character is added literally, dropping the '\'.
                    if (add_char_to_buffer(current_arg_buffer, input_line[i]) < 0) {
                        free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
                    }
                    i++; // Advance past the escaped character
                }
            } else if (current_char == '\'') {
                state.in_single_quote = 1; // Enter single quote
                i++;
            } else if (current_char == '"') {
                state.in_double_quote = 1; // Enter double quote
                i++;
            } else if (isspace((unsigned char)current_char)) {
                // Argument boundary: if buffer has content, add it as an argument
                if (current_arg_buffer->length > 0) {
                    if (argc >= MAX_ARGS - 1) { // Check for max arguments
                        fprintf(stderr, "parse_arguments: too many arguments (max %d)\n", MAX_ARGS - 1);
                        free_arg_buffer(current_arg_buffer);
                        free_argv(argv);
                        return NULL;
                    }
                    argv[argc] = strdup(current_arg_buffer->buffer);
                    if (!argv[argc]) {
                        perror("parse_arguments: strdup failed");
                        free_arg_buffer(current_arg_buffer);
                        free_argv(argv);
                        return NULL;
                    }
                    argc++;
                    current_arg_buffer->length = 0; // Reset buffer for next argument
                    current_arg_buffer->buffer[0] = '\0';
                }
                // Skip subsequent whitespace
                while (isspace((unsigned char)input_line[i])) {
                    i++;
                }
            } else {
                // Regular character, add to buffer
                if (add_char_to_buffer(current_arg_buffer, current_char) < 0) {
                    free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
                }
                i++;
            }
        }
    }

    // After loop, add any remaining content in the buffer as the last argument
    if (current_arg_buffer->length > 0) {
        if (argc >= MAX_ARGS - 1) {
            fprintf(stderr, "parse_arguments: too many arguments (max %d)\n", MAX_ARGS - 1);
            free_arg_buffer(current_arg_buffer);
            free_argv(argv);
            return NULL;
        }
        argv[argc] = strdup(current_arg_buffer->buffer);
        if (!argv[argc]) {
            perror("parse_arguments: strdup failed for final arg");
            free_arg_buffer(current_arg_buffer);
            free_argv(argv);
            return NULL;
        }
        argc++;
    }
    
    // Check for unterminated quotes at the end of the line
    if (state.in_single_quote || state.in_double_quote) {
        fprintf(stderr, "shell: unterminated quote\n");
        free_arg_buffer(current_arg_buffer);
        free_argv(argv);
        return NULL; // Return NULL to indicate a parsing error
    }

    argv[argc] = NULL; // Null-terminate the argv array as required by execv
    free_arg_buffer(current_arg_buffer);
    return argv;
}

// Helper function to handle `echo` commands
void handle_echo_cmd(char** argv) {
    // argv[0] is "echo", subsequent elements are the arguments to echo
    for (int i = 1; argv[i] != NULL; i++) {
        printf("%s%s", argv[i], (argv[i+1] != NULL) ? " " : "");
    }
    printf("\n");
}

// Helper function to handle `exit` commands
int handle_exit_cmd(char** argv) {
    if (argv[1] != NULL) {
        return atoi(argv[1]);
    }
    return 0;
}

// Helper function to handle `type` commands
void handle_type_cmd(char** argv) {
    // argv[0] is "type", argv[1] onwards are commands to type
    if (argv[1] == NULL || *argv[1] == '\0') {
        printf("type: usage: type name [...]\n");
        return;
    }

    for (int i = 1; argv[i] != NULL; i++) {
        const char* cmd_to_type = argv[i];

        if (
            (strcmp(cmd_to_type, "echo") == 0) ||
            (strcmp(cmd_to_type, "exit") == 0) ||
            (strcmp(cmd_to_type, "type") == 0) ||
            (strcmp(cmd_to_type, "pwd") == 0) ||
            (strcmp(cmd_to_type, "cd") == 0)
        ) {
            printf("%s is a shell builtin\n", cmd_to_type);
            continue; // Check next argument
        }

        char* pathEnv = getenv("PATH");
        if (pathEnv == NULL) {
            printf("%s: not found\n", cmd_to_type);
            continue; // Check next argument
        }
        
        char* pathCopy = strdup(pathEnv); // Use strdup for convenience and safety
        if (pathCopy == NULL) {
            perror("type: strdup failed for PATH");
            printf("%s: not found\n", cmd_to_type); // Still print not found for this arg
            continue; // Check next argument
        }

        char* dir = strtok(pathCopy, ":");
        int found = 0;
        while (dir != NULL) {
            char fullPath[PATH_MAX]; // Use PATH_MAX for full path
            int ret = snprintf(fullPath, sizeof(fullPath), "%s/%s", dir, cmd_to_type);

            if (ret >= sizeof(fullPath)) {
                // Path too long, skip this dir
                dir = strtok(NULL, ":");
                continue;
            }

            if (access(fullPath, F_OK) == 0 && access(fullPath, X_OK) == 0) {
                printf("%s is %s\n", cmd_to_type, fullPath);
                found = 1;
                break; // Found it, move to next cmd_to_type
            }
            dir = strtok(NULL, ":");
        }

        if (!found) {
            printf("%s: not found\n", cmd_to_type);
        }
        free(pathCopy); // Free the duplicated PATH string
    }
}


// Helper function to handle `pwd` commands
char* handle_pwd_cmd() {
    char* buffer = (char*)malloc(PATH_MAX);
    if (buffer == NULL) {
        perror("pwd: malloc failed for path buffer");
        return NULL;
    }

    if (getcwd(buffer, PATH_MAX) == NULL) {
        perror("pwd: getcwd failed");
        free(buffer);
        return NULL;
    }
    return buffer;
}

// Helper function to handle `cd` commands
void handle_cd_cmd(char** argv) {
    const char* path = argv[1]; // The first argument to cd

    const char* target_path;
    char expanded_path[PATH_MAX]; // Buffer for expanded path

    if (path == NULL || *path == '\0' || strcmp(path, "~") == 0) {
        target_path = getenv("HOME");
        if (target_path == NULL) {
            fprintf(stderr, "cd: HOME environment variable not set\n");
            return;
        }
    } else if (*path == '~') {
        const char* home = getenv("HOME");
        if (home == NULL) {
            fprintf(stderr, "cd: HOME environment variable not set\n");
            return;
        }
        // Construct path: HOME + rest of path
        snprintf(expanded_path, sizeof(expanded_path), "%s%s", home, path + 1);
        target_path = expanded_path;
    } else {
        target_path = path;
    }

    if (chdir(target_path) != 0) {
        printf("cd: %s: No such file or directory\n", target_path);
    }
}

// Helper function to find if executable exists in PATH
char* find_exe_in_path(const char* exe) {
    // If it's an absolute or relative path (contains '/'), don't search PATH
    if (strchr(exe, '/') != NULL) {
        if (access(exe, F_OK) == 0 && access(exe, X_OK) == 0) {
            return strdup(exe); // Return a duplicated string for consistency
        }
        return NULL;
    }

    char* pathEnv = getenv("PATH");
    if (pathEnv == NULL) {
        return NULL;
    }

    char* pathCopy = strdup(pathEnv);
    if (pathCopy == NULL) {
        perror("find_exe_in_path: strdup failed for PATH");
        return NULL;
    }

    char* dir = strtok(pathCopy, ":");
    while (dir != NULL) {
        char fullPath[PATH_MAX];
        int ret = snprintf(fullPath, sizeof(fullPath), "%s/%s", dir, exe);

        if (ret >= sizeof(fullPath)) {
            dir = strtok(NULL, ":");
            continue;
        }

        if (access(fullPath, F_OK) == 0 && access(fullPath, X_OK) == 0) {
            char* result = strdup(fullPath);
            free(pathCopy);
            return result;
        }
        dir = strtok(NULL, ":");
    }

    free(pathCopy);
    return NULL;
}

// Helper function to fork process and execute external executables
void execute_external_exe(const char* exePath, char* argv[]) {
    pid_t pid = fork();

    if (pid == 0) { // Child process
        execv(exePath, argv); // argv is directly passed
        perror("execv failed"); // execv only returns on error
        exit(1); // Exit child process on execv failure
    } else if (pid > 0) { // Parent process
        int status;
        waitpid(pid, &status, 0); // Wait for the specific child
    } else { // Fork failed
        perror("fork failed");
    }
}

int main() {
    // Flush after every printf for immediate output in interactive mode
    setbuf(stdout, NULL);
    int status = 0;

    while (1) {
        printf("$ ");

        // Wait for user input
        char input[BUF_SIZE];
        char* shellInput = fgets(input, BUF_SIZE, stdin);

        if (shellInput == NULL) { // EOF (Ctrl+D)
            printf("\n");
            break;
        }

        // Remove trailing newline
        size_t inputLen = strlen(input);
        if (inputLen > 0 && input[inputLen - 1] == '\n') {
            input[inputLen - 1] = '\0';
        }

        // Skip command if input is empty after trimming, or only whitespace
        if (strlen(input) == 0) {
            continue;
        }

        // Parse the entire line into arguments
        char** parsed_argv = parse_arguments(input); 

        // Handle parsing errors (e.g., unterminated quotes)
        if (parsed_argv == NULL) {
            // Error message already printed by parse_arguments
            continue; 
        }

        // If no command was parsed (e.g., input was just quotes that cancelled out, or empty after parsing)
        if (parsed_argv[0] == NULL) {
            free_argv(parsed_argv);
            continue;
        }

        const char* command = parsed_argv[0];

        if (strcmp(command, "exit") == 0) {
            status = handle_exit_cmd(parsed_argv);
            free_argv(parsed_argv);
            break;
        } else if (strcmp(command, "echo") == 0) {
            handle_echo_cmd(parsed_argv);
        } else if (strcmp(command, "type") == 0) {
            handle_type_cmd(parsed_argv);
        } else if (strcmp(command, "pwd") == 0) {
            char* pwd = handle_pwd_cmd();
            if (pwd != NULL) {
                printf("%s\n", pwd);
                free(pwd);
            } else {
                // Error message from handle_pwd_cmd, just print generic
                printf("pwd: could not retrieve current working directory\n");
            }
        } else if (strcmp(command, "cd") == 0) {
            handle_cd_cmd(parsed_argv);
        } else {
            char* exePath = find_exe_in_path(command);
            if (exePath != NULL) {
                execute_external_exe(exePath, parsed_argv); // Pass the full parsed_argv
                free(exePath);
            } else {
                printf("%s: command not found\n", command);
            }
        }
        
        // Free the parsed arguments for the current command
        free_argv(parsed_argv);
    }

    return status;
}