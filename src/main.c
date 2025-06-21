#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <limits.h>
#include <fcntl.h>

#define BUF_SIZE 512
#define MAX_ARGS 64
#define ARG_SIZE 64

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

// Structure to keep information regarding redirection.
typedef struct {
    int has_stdout_redirect;
    char* stdout_file;
    int is_stdout_append;    // Flag to indicate append mode for stdout
    int has_stderr_redirect;
    char* stderr_file;
    int is_stderr_append;    // Flag to indicate append mode for stderr
} RedirectionInfo;

// Structure to modify parsed arguments to handle redirection
typedef struct {
    char** argv;
    RedirectionInfo* redir_info;
} ParseResult;

// Helper function to initialize argument buffer
ArgBuffer* init_arg_buffer() {
    ArgBuffer* buf = malloc(sizeof(ArgBuffer));
    if (!buf) {
        perror("init_arg_buffer: malloc failed for ArgBuffer");
        return NULL;
    }

    buf->capacity = ARG_SIZE;
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

// Helper function to parse entire input line into individual arguments.
char** parse_arguments(const char* input_line) {
    char** argv = malloc(MAX_ARGS * sizeof(char*));
    if (!argv) {
        perror("parse_arguments: malloc failed for argv");
        return NULL;
    }
    int argc = 0;

    ParseState state = {0, 0}; // Initialize state: not in single or double quotes
    ArgBuffer* current_arg_buffer = init_arg_buffer();
    if (!current_arg_buffer) {
        free(argv); // Free argv allocated before init_arg_buffer failed
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
                i++; // Consume the quote, do NOT add to buffer
            } else {
                // In single quotes, ALL characters are literal, added to buffer
                if (add_char_to_buffer(current_arg_buffer, current_char) < 0) {
                    free_arg_buffer(current_arg_buffer);
                    free_argv(argv);
                    return NULL;
                }
                i++;
            }
        } else if (state.in_double_quote) {
            if (current_char == '"') {
                state.in_double_quote = 0; // End double quote
                i++; // Consume the quote, do NOT add to buffer
            } else if (current_char == '\\') {
                i++; // Advance past the backslash to the character being (potentially) escaped

                if (input_line[i] == '\0') {
                    // Trailing backslash in double quotes -> literal backslash
                    if (add_char_to_buffer(current_arg_buffer, '\\') < 0) {
                        free_arg_buffer(current_arg_buffer);
                        free_argv(argv);
                        return NULL;
                    }
                } else if (input_line[i] == '"' || input_line[i] == '\\' ||
                           input_line[i] == '$' || input_line[i] == '`') {
                    // Specific characters: \ escapes these, the backslash is removed, char is literal.
                    if (add_char_to_buffer(current_arg_buffer, input_line[i]) < 0) {
                        free_arg_buffer(current_arg_buffer);
                        free_argv(argv);
                        return NULL;
                    }
                } else {
                    // For all other characters (like `\n`, `\5`, `\t`, `\X` etc.):
                    // The backslash itself is preserved as a literal character,
                    // followed by the next character, also as a literal.
                    if (add_char_to_buffer(current_arg_buffer, '\\') < 0 ||
                        add_char_to_buffer(current_arg_buffer, input_line[i]) < 0) {
                        free_arg_buffer(current_arg_buffer);
                        free_argv(argv);
                        return NULL;
                    }
                }
                i++; // Advance past the character that was (or wasn't) escaped
            } else {
                // Regular character in double quotes, add to buffer
                if (add_char_to_buffer(current_arg_buffer, current_char) < 0) {
                    free_arg_buffer(current_arg_buffer);
                    free_argv(argv);
                    return NULL;
                }
                i++;
            }
        } else { // Not in any quotes
            if (current_char == '\\') {
                i++; // Advance past the backslash to the character it's escaping
                if (input_line[i] == '\0') {
                    // Trailing backslash unquoted is literal (e.g., `cmd arg\`)
                    if (add_char_to_buffer(current_arg_buffer, '\\') < 0) {
                        free_arg_buffer(current_arg_buffer);
                        free_argv(argv);
                        return NULL;
                    }
                } else {
                    // Non-quoted backslash escapes the next character.
                    if (add_char_to_buffer(current_arg_buffer, input_line[i]) < 0) {
                        free_arg_buffer(current_arg_buffer);
                        free_argv(argv);
                        return NULL;
                    }
                }
                i++; // Advance past the escaped character (or the original position if trailing \)
            } else if (current_char == '\'') {
                state.in_single_quote = 1; // Enter single quote (don't add quote to buffer)
                i++;
            } else if (current_char == '"') {
                state.in_double_quote = 1; // Enter double quote (don't add quote to buffer)
                i++;
            }
            // --- Unified and Prioritized Tokenization Logic ---
            else {
                // If we have content in the buffer, finalize it as an argument
                if (current_arg_buffer->length > 0) {
                    if (argc >= MAX_ARGS - 1) {
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
                    current_arg_buffer->length = 0;
                    current_arg_buffer->buffer[0] = '\0';
                }

                // Now, handle the current character(s)
                // Check for compound operators first (longest match principle)
                if ((i + 1 < strlen(input_line)) && input_line[i+1] == '>') { // Potential multi-char redir
                    if (current_char == '>') { // Found a ">>"
                        if (argc >= MAX_ARGS - 1) {
                            fprintf(stderr, "parse_arguments: too many arguments (max %d)\n", MAX_ARGS - 1);
                            free_arg_buffer(current_arg_buffer);
                            free_argv(argv);
                            return NULL;
                        }
                        argv[argc] = strdup(">>");
                        if (!argv[argc]) {
                            perror("parse_arguments: strdup failed for '>>'");
                            free_arg_buffer(current_arg_buffer);
                            free_argv(argv);
                            return NULL;
                        }
                        argc++;
                        i += 2; // Consume both '>'
                    } else if (current_char == '1' || current_char == '2') { // Potential "1>>" or "2>>"
                        if ((i + 2 < strlen(input_line)) && input_line[i+2] == '>') { // Confirmed "1>>" or "2>>"
                            if (argc >= MAX_ARGS - 1) {
                                fprintf(stderr, "parse_arguments: too many arguments (max %d)\n", MAX_ARGS - 1);
                                free_arg_buffer(current_arg_buffer);
                                free_argv(argv);
                                return NULL;
                            }
                            char compound_redir[4]; // "1>>\0" or "2>>\0"
                            compound_redir[0] = current_char;
                            compound_redir[1] = '>';
                            compound_redir[2] = '>';
                            compound_redir[3] = '\0';
                            argv[argc] = strdup(compound_redir);
                            if (!argv[argc]) {
                                perror("parse_arguments: strdup failed for '1>>'/'2>>'");
                                free_arg_buffer(current_arg_buffer);
                                free_argv(argv);
                                return NULL;
                            }
                            argc++;
                            i += 3; // Consume digit and two '>'
                        } else { // It's "1>" or "2>" (not "1>>" or "2>>")
                             // This case will be handled by the next `else if` for single `>` after digit
                            if (add_char_to_buffer(current_arg_buffer, current_char) < 0) {
                                free_arg_buffer(current_arg_buffer);
                                free_argv(argv);
                                return NULL;
                            }
                            i++;
                        }
                    } else { // Next char is '>', but current is not '>', '1', or '2'. E.g., 'A>' in `echo A>b`
                        // Fall through to general character or single operator
                        if (add_char_to_buffer(current_arg_buffer, current_char) < 0) {
                            free_arg_buffer(current_arg_buffer);
                            free_argv(argv);
                            return NULL;
                        }
                        i++;
                    }
                }
                // Check for single operators like >, <, |
                else if (current_char == '>' || current_char == '<' || current_char == '|') {
                    if (argc >= MAX_ARGS - 1) {
                        fprintf(stderr, "parse_arguments: too many arguments (max %d)\n", MAX_ARGS - 1);
                        free_arg_buffer(current_arg_buffer);
                        free_argv(argv);
                        return NULL;
                    }
                    char temp_char_str[2];
                    temp_char_str[0] = current_char;
                    temp_char_str[1] = '\0';
                    argv[argc] = strdup(temp_char_str);
                    if (!argv[argc]) {
                        perror("parse_arguments: strdup failed for single operator");
                        free_arg_buffer(current_arg_buffer);
                        free_argv(argv);
                        return NULL;
                    }
                    argc++;
                    i++; // Consume the single character
                }
                // Check for whitespace (always separates arguments)
                else if (isspace((unsigned char)current_char)) {
                    i++; // Consume the space
                    // Skip subsequent whitespace
                    while (isspace((unsigned char)input_line[i])) {
                        i++;
                    }
                }
                // If not a special character, add to current argument buffer (regular character)
                else {
                    if (add_char_to_buffer(current_arg_buffer, current_char) < 0) {
                        free_arg_buffer(current_arg_buffer);
                        free_argv(argv);
                        return NULL;
                    }
                    i++;
                }
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
        if (!argv[argc]) { // Changed from !argv to !argv[argc] - this was a potential bug
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

// Helper function to initialize redirection information
RedirectionInfo* init_redirection_info() {
    RedirectionInfo* redir = malloc(sizeof(RedirectionInfo));
    if (!redir) {
        perror("init_redirection_info: malloc failed");
        return NULL;
    }
    redir->has_stdout_redirect = 0;
    redir->stdout_file = NULL;
    redir->is_stdout_append = 0;
    redir->has_stderr_redirect = 0;
    redir->stderr_file = NULL;
    redir->is_stderr_append = 0;

    return redir;
}

// Helper function to free redirection info
void free_redirection_info(RedirectionInfo* redir) {
    if (redir != NULL) {
        free(redir->stdout_file);
        free(redir->stderr_file);
        free(redir);
    }
}

// Helper function to deal with arguments involving redirection
ParseResult* parse_args_with_redirection(const char* input_line) {
    ParseResult* result = malloc(sizeof(ParseResult));
    if (!result) {
        perror("parse_args_with_redirection: malloc failed for ParseResult");
        return NULL;
    }

    result->redir_info = init_redirection_info();
    if (!result->redir_info) {
        free(result);
        return NULL;
    }

    char** all_args = parse_arguments(input_line);
    if (!all_args) {
        free_redirection_info(result->redir_info); // redir_info exists even if parse_arguments fails
        free(result);
        return NULL;
    }

    int total_args = 0;
    while (all_args[total_args] != NULL) {
        total_args++;
    }

    int redirect_stdout_idx = -1;       // For `>` or `1>`
    int redirect_append_stdout_idx = -1; // For `>>` or `1>>`
    int redirect_stderr_idx = -1;       // For `2>`
    int redirect_append_stderr_idx = -1; // For `2>>` (if implemented later)


    for (int i = 0; i < total_args; i++) {
        if (strcmp(all_args[i], ">") == 0 || strcmp(all_args[i], "1>") == 0) {
            // Check for conflicts with other stdout redirections
            if (redirect_stdout_idx != -1 || redirect_append_stdout_idx != -1) {
                fprintf(stderr, "shell: syntax error: multiple stdout redirections\n");
                goto error_cleanup_parse_redir;
            }
            redirect_stdout_idx = i;
        } else if (strcmp(all_args[i], ">>") == 0 || strcmp(all_args[i], "1>>") == 0) { // NEW: Handle append
            // Check for conflicts with other stdout redirections
            if (redirect_stdout_idx != -1 || redirect_append_stdout_idx != -1) {
                fprintf(stderr, "shell: syntax error: multiple stdout redirections\n");
                goto error_cleanup_parse_redir;
            }
            redirect_append_stdout_idx = i;
        } else if (strcmp(all_args[i], "2>") == 0) {
            // Check for conflicts with other stderr redirections
            if (redirect_stderr_idx != -1 || redirect_append_stderr_idx != -1) { // Check for future 2>>
                fprintf(stderr, "shell: syntax error: multiple stderr redirections\n");
                goto error_cleanup_parse_redir;
            }
            redirect_stderr_idx = i;
        }
        // If you add 2>>, you'd add a similar check for its index.
        // else if (strcmp(all_args[i], "2>>") == 0) { ... }
    }

    // Determine the effective end of arguments for the command itself
    // Initialize with total_args, then find the minimum index of any redirection operator
    int argv_end_idx = total_args;

    if (redirect_stdout_idx != -1 && redirect_stdout_idx < argv_end_idx) {
        argv_end_idx = redirect_stdout_idx;
    }
    if (redirect_append_stdout_idx != -1 && redirect_append_stdout_idx < argv_end_idx) {
        argv_end_idx = redirect_append_stdout_idx;
    }
    if (redirect_stderr_idx != -1 && redirect_stderr_idx < argv_end_idx) {
        argv_end_idx = redirect_stderr_idx;
    }
    // Add similar logic here for 2>> if implemented

    // Process stdout redirection (truncating or appending)
    if (redirect_stdout_idx != -1 || redirect_append_stdout_idx != -1) {
        int actual_redirect_operator_idx = (redirect_stdout_idx != -1) ? redirect_stdout_idx : redirect_append_stdout_idx;

        if (actual_redirect_operator_idx + 1 >= total_args || all_args[actual_redirect_operator_idx + 1] == NULL) {
            fprintf(stderr, "shell: syntax error: expected filename after stdout redirection\n");
            goto error_cleanup_parse_redir;
        }
        result->redir_info->has_stdout_redirect = 1;
        result->redir_info->stdout_file = strdup(all_args[actual_redirect_operator_idx + 1]); // Corrected this line in previous turn
        if (!result->redir_info->stdout_file) {
            perror("parse_args_with_redirection: strdup failed for stdout filename");
            goto error_cleanup_parse_redir;
        }
        // Set append flag if the operator was an append one
        if (redirect_append_stdout_idx != -1) {
            result->redir_info->is_stdout_append = 1;
        }
    }

    // Process stderr redirection
    if (redirect_stderr_idx != -1) {
        if (redirect_stderr_idx + 1 >= total_args || all_args[redirect_stderr_idx + 1] == NULL) {
            fprintf(stderr, "shell: syntax error: expected filename after stderr redirection\n");
            goto error_cleanup_parse_redir;
        }
        result->redir_info->has_stderr_redirect = 1;
        result->redir_info->stderr_file = strdup(all_args[redirect_stderr_idx + 1]);
        if (!result->redir_info->stderr_file) {
            perror("parse_args_with_redirection: strdup failed for stderr filename");
            goto error_cleanup_parse_redir;
        }
        // Assuming 2> is always truncating for now; set is_stderr_append if 2>> is added
        // result->redir_info->is_stderr_append = 0;
    }

    // Construct the command's argv, stopping at the first redirection symbol
    result->argv = malloc((argv_end_idx + 1) * sizeof(char*));
    if (!result->argv) {
        perror("parse_args_with_redirection: malloc failed for result argv");
        goto error_cleanup_parse_redir;
    }

    for (int i = 0; i < argv_end_idx; i++) {
        result->argv[i] = strdup(all_args[i]);
        if (!result->argv[i]) {
            perror("parse_args_with_redirection: strdup failed for command arg");
            for (int j = 0; j < i; j++) {
                free(result->argv[j]);
            }
            free(result->argv); // Free partial argv
            goto error_cleanup_parse_redir;
        }
    }
    result->argv[argv_end_idx] = NULL; // Null-terminate the command's argv

    free_argv(all_args); // all_args is copied, so free the original array and its contents
    
    return result;

error_cleanup_parse_redir: // Centralized error handling for parse_args_with_redirection
    free_argv(all_args); // all_args might be NULL if parse_arguments failed, free_argv handles this
    free_redirection_info(result->redir_info); // redir_info might be NULL if init_redirection_info failed
    free(result); // result might be NULL if malloc failed at the start
    return NULL;
}

// Helper function to free parse result
void free_parse_result(ParseResult* result) {
    if (result != NULL) {
        free_argv(result->argv);
        free_redirection_info(result->redir_info);
        free(result);
    }
}

// Helper function to handle `echo` commands
void handle_echo_cmd(char** argv) {
    // argv[0] is "echo", subsequent elements are the arguments to echo
    for (int i = 1; argv[i] != NULL; i++) {
        // The quotes are now stripped during parsing in parse_arguments
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
    if (argv[1] == NULL || *argv[1] == '\0') {
        fprintf(stderr, "type: usage: type name [...]\n"); // usage goes to stderr
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
        if (!pathEnv) {
            fprintf(stderr, "%s: not found\n", cmd_to_type); // Not found goes to stderr
            continue;
        }
        
        char* pathCopy = strdup(pathEnv);
        if (!pathCopy) {
            perror("type: strdup failed for PATH"); // Errors go to stderr
            fprintf(stderr, "%s: not found\n", cmd_to_type); // Not found goes to stderr
            continue;
        }

        char* dir = strtok(pathCopy, ":");
        int found = 0;
        while (dir != NULL) {
            char fullPath[PATH_MAX];
            int ret = snprintf(fullPath, sizeof(fullPath), "%s/%s", dir, cmd_to_type);

            if (ret >= sizeof(fullPath)) {
                dir = strtok(NULL, ":");
                continue;
            }

            if (access(fullPath, F_OK) == 0 && access(fullPath, X_OK) == 0) {
                printf("%s is %s\n", cmd_to_type, fullPath);
                found = 1;
                break;
            }
            dir = strtok(NULL, ":");
        }

        if (!found) {
            fprintf(stderr, "%s: not found\n", cmd_to_type); // Not found goes to stderr
        }
        free(pathCopy);
    }
}


// Helper function to handle `pwd` commands
char* handle_pwd_cmd() {
    char* buffer = (char*)malloc(PATH_MAX);
    if (!buffer) {
        perror("pwd: malloc failed for path buffer"); // Errors go to stderr
        return NULL;
    }

    if (getcwd(buffer, PATH_MAX) == NULL) {
        perror("pwd: getcwd failed"); // Errors go to stderr
        free(buffer);
        return NULL;
    }
    return buffer;
}

// Helper function to handle `cd` commands
void handle_cd_cmd(char** argv) {
    const char* path = argv[1];

    const char* target_path;
    char expanded_path[PATH_MAX];

    if (path == NULL || *path == '\0' || strcmp(path, "~") == 0) {
        target_path = getenv("HOME");
        if (target_path == NULL) {
            fprintf(stderr, "cd: HOME environment variable not set\n"); // Errors go to stderr
            return;
        }
    } else if (*path == '~') {
        const char* home = getenv("HOME");
        if (home == NULL) {
            fprintf(stderr, "cd: HOME environment variable not set\n"); // Errors go to stderr
            return;
        }
        snprintf(expanded_path, sizeof(expanded_path), "%s%s", home, path + 1);
        target_path = expanded_path;
    } else {
        target_path = path;
    }

    if (chdir(target_path) != 0) {
        fprintf(stderr, "cd: %s: No such file or directory\n", target_path); // Errors go to stderr
    }
}

// Helper function to find if executable exists in PATH
char* find_exe_in_path(const char* exe) {
    // If it's an absolute or relative path (contains '/'), don't search PATH
    if (strchr(exe, '/') != NULL) {
        if (access(exe, F_OK) == 0 && access(exe, X_OK) == 0) {
            return strdup(exe);
        }
        return NULL;
    }

    char* pathEnv = getenv("PATH");
    if (!pathEnv) {
        return NULL;
    }

    char* pathCopy = strdup(pathEnv);
    if (!pathCopy) {
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
void execute_external_exe_with_redirection(const char* exePath, char* argv[], RedirectionInfo* redir_info) {
    pid_t pid = fork();

    if (pid == 0) { // Child process
        // Redirect STDOUT if specified
        if (redir_info->has_stdout_redirect) {
            int open_flags = O_WRONLY | O_CREAT;
            if (redir_info->is_stdout_append) { // NEW: Check for append mode
                open_flags |= O_APPEND; // Add O_APPEND
            } else {
                open_flags |= O_TRUNC; // Otherwise, use O_TRUNC (default for '>')
            }

            int fd = open(redir_info->stdout_file, open_flags, 0644);
            if (fd == -1) {
                perror("open stdout redirection file");
                _exit(1); // Use _exit in child after fork for safety
            }

            if (dup2(fd, STDOUT_FILENO) == -1) {
                perror("dup2 stdout");
                close(fd);
                _exit(1); // Use _exit
            }
            close(fd); // Child closes its copy of the original fd
        }

        // Redirect STDERR if specified
        if (redir_info->has_stderr_redirect) {
            // Assuming 2> is always truncating for now; if 2>> is implemented, add check here
            int open_flags_stderr = O_WRONLY | O_CREAT | O_TRUNC; 
            if (redir_info->is_stderr_append) { // Check for stderr append
                open_flags_stderr |= O_APPEND;
            } else {
                open_flags_stderr |= O_TRUNC;
            }

            int fd = open(redir_info->stderr_file, open_flags_stderr, 0644);
            if (fd == -1) {
                perror("open stderr redirection file");
                _exit(1); // Use _exit
            }

            if (dup2(fd, STDERR_FILENO) == -1) { // Redirect STDERR_FILENO (fd 2)
                perror("dup2 stderr");
                close(fd);
                _exit(1); // Use _exit
            }
            close(fd); // Child closes its copy of the original fd
        }

        execv(exePath, argv);
        perror("execv failed"); // Only reached if execv fails
        _exit(1); // Child exits if execv fails (use _exit)
    } else if (pid > 0) { // Parent process
        int status;
        waitpid(pid, &status, 0); // Parent waits for child
    } else { // Fork failed
        perror("fork failed");
    }
}

// Helper function to setup stdout redirection (for built-ins)
int setup_stdout_redirection(const char* filename, int is_append) { // Added is_append parameter
    int open_flags = O_WRONLY | O_CREAT;
    if (is_append) {
        open_flags |= O_APPEND;
    } else {
        open_flags |= O_TRUNC;
    }

    int fd = open(filename, open_flags, 0644);
    if (fd == -1) {
        perror("open");
        return -1;
    }

    int saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout == -1) {
        perror("dup");
        close(fd);
        return -1;
    }

    if (dup2(fd, STDOUT_FILENO) == -1) {
        perror("dup2");
        close(fd);
        close(saved_stdout);
        return -1;
    }

    close(fd);
    return saved_stdout;
}

// Helper function to restore stdout (for built-ins)
void restore_stdout(int saved_stdout) {
    if (saved_stdout != -1) {
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
    }
}

// Helper function to setup stderr redirection (for built-ins)
int setup_stderr_redirection(const char* filename, int is_append) { // Added is_append parameter
    int open_flags = O_WRONLY | O_CREAT;
    if (is_append) {
        open_flags |= O_APPEND;
    } else {
        open_flags |= O_TRUNC;
    }

    int fd = open(filename, open_flags, 0644);
    if (fd == -1) {
        perror("open");
        return -1;
    }

    int saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr == -1) {
        perror("dup");
        close(fd);
        return -1;
    }

    if (dup2(fd, STDERR_FILENO) == -1) {
        perror("dup2");
        close(fd);
        close(saved_stderr);
        return -1;
    }

    close(fd);
    return saved_stderr;
}

// Helper function to restore stderr (for built-ins)
void restore_stderr(int saved_stderr) {
    if (saved_stderr != -1) {
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
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

        if (shellInput == NULL) {
            printf("\n");
            break;
        }

        // Remove trailing newline
        size_t inputLen = strlen(input);
        if (inputLen > 0 && input[inputLen - 1] == '\n') {
            input[inputLen - 1] = '\0';
        }

        char* processedInput = input;

        // Parse the entire line into arguments
        ParseResult* parsed_result = parse_args_with_redirection(processedInput);

        // Handle parsing errors (e.g., unterminated quotes)
        if (parsed_result == NULL) {
            continue; 
        }

        // If no command was parsed (e.g., input was just whitespace or empty after parsing)
        if (parsed_result->argv[0] == NULL) {
            free_parse_result(parsed_result);
            continue;
        }

        const char* command = parsed_result->argv[0];
        int saved_stdout = -1;
        int saved_stderr = -1;

        if (strcmp(command, "exit") == 0) {
            status = handle_exit_cmd(parsed_result->argv);
            free_parse_result(parsed_result);
            break;
        } else if (
            strcmp(command, "echo") == 0 ||
            strcmp(command, "type") == 0 ||
            strcmp(command, "pwd") == 0 ||
            strcmp(command, "cd") == 0
        ) {
            // Built-in commands handle redirection in the parent process
            if (parsed_result->redir_info->has_stdout_redirect) {
                saved_stdout = setup_stdout_redirection(parsed_result->redir_info->stdout_file, parsed_result->redir_info->is_stdout_append); // Pass is_stdout_append
                if (saved_stdout == -1) {
                    fprintf(stderr, "ERROR: Failed to setup stdout redirection to %s\n", parsed_result->redir_info->stdout_file);
                    free_parse_result(parsed_result);
                    continue;
                }
            }

            if (parsed_result->redir_info->has_stderr_redirect) {
                saved_stderr = setup_stderr_redirection(parsed_result->redir_info->stderr_file, parsed_result->redir_info->is_stderr_append); // Pass is_stderr_append
                if (saved_stderr == -1) {
                    fprintf(stderr, "ERROR: Failed to setup stderr redirection to %s\n", parsed_result->redir_info->stderr_file);
                    // Crucial: If stderr redirect fails, restore stdout if it was successfully redirected
                    if (saved_stdout != -1) {
                        restore_stdout(saved_stdout);
                    }
                    free_parse_result(parsed_result);
                    continue;
                }
            }

            if (strcmp(command, "echo") == 0) {
                handle_echo_cmd(parsed_result->argv);
            } else if (strcmp(command, "type") == 0) {
                handle_type_cmd(parsed_result->argv);
            } else if (strcmp(command, "pwd") == 0) {
                char* pwd = handle_pwd_cmd();
                if (pwd != NULL) {
                    printf("%s\n", pwd);
                    free(pwd);
                } else {
                    fprintf(stderr, "pwd: could not retrieve current working directory\n"); // pwd errors go to stderr
                }
            } else if (strcmp(command, "cd") == 0) {
                handle_cd_cmd(parsed_result->argv);
            }
            
            // Restore stdout and stderr after built-in execution if they were redirected
            if (saved_stdout != -1) {
                restore_stdout(saved_stdout);
            }
            if (saved_stderr != -1) {
                restore_stderr(saved_stderr);
            }
        } else {
            // External commands handle redirection in the child process
            char* exePath = find_exe_in_path(command);
            if (exePath != NULL) {
                execute_external_exe_with_redirection(exePath, parsed_result->argv, parsed_result->redir_info);
                free(exePath);
            } else {
                printf("%s: command not found\n", command); // Command not found goes to stderr
            }
        }

        free_parse_result(parsed_result);
    }

    return status;
}