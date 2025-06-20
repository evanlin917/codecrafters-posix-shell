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
    int has_output_redirect;
    char* output_file;
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

//  Helper function to parse entire input line into individual arguments.
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
                i++;
            } else {
                // In single quotes, ALL characters are literal, including backslashes.
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
                i++;
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
                    // This handles `f\n81` becoming `f` then literal `\` then literal `n` then `81`.
                    // This handles `f\52` becoming `f` then literal `\` then literal `5` then literal `2`.
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
                    free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
                }
                i++;
            }
        } else { // Not in any quotes
            if (current_char == '\\') {
                i++; // Advance past the backslash to the character it's escaping
                if (input_line[i] == '\0') {
                    // Trailing backslash unquoted is literal (e.g., `cmd arg\`)
                    if (add_char_to_buffer(current_arg_buffer, '\\') < 0) {
                        free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
                    }
                } else {
                    // Non-quoted backslash escapes the next character.
                    // It preserves its literal value, effectively dropping the backslash.
                    // E.g., `world\ \ \ \ \ \ script` -> `world      script`
                    // E.g., `"/tmp/file\ name"` -> `/tmp/file name`
                    if (add_char_to_buffer(current_arg_buffer, input_line[i]) < 0) {
                        free_arg_buffer(current_arg_buffer);
                        free_argv(argv);
                        return NULL;
                    }
                }
                i++; // Advance past the escaped character (or the original position if trailing \)
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
            } else if (current_char == '>' || current_char == '<' || current_char == '|') {
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

                if (argc >= MAX_ARGS - 1) {
                    fprintf(stderr, "parse_arguments: too many arguments (max %d)\n", MAX_ARGS - 1);
                    free_arg_buffer(current_arg_buffer);
                    free_argv(argv);
                    return NULL;
                }

                if ((current_char == '1' || current_char == '2') && input_line[i+1] == '>') {
                    char compound_redir[3];
                    compound_redir[0] = current_char;
                    compound_redir[1] = '>';
                    compound_redir[2] = '\0';
                    argv[argc] = strdup(compound_redir);
                    if (!argv[argc]) {
                        perror("parse_arguments: strdup failed for compound redir");
                        free_arg_buffer(current_arg_buffer);
                        free_argv(argv);
                        return NULL;
                    }
                    argc++;
                    i += 2;
                } else {
                    char temp_char_str[2];
                    temp_char_str[0] = current_char;
                    temp_char_str[1] = '\0';
                    argv[argc] = strdup(temp_char_str);
                    if (!argv[argc]) {
                        perror("parse_arguments: strdup failed for single redir");
                        free_arg_buffer(current_arg_buffer);
                        free_argv(argv);
                        return NULL;
                    }
                    argc++;
                    i++;
                }
            } else {
                // Regular character, add to buffer
                if (add_char_to_buffer(current_arg_buffer, current_char) < 0) {
                    free_arg_buffer(current_arg_buffer);
                    free_argv(argv);
                    return NULL;
                }
                i++;
            }
        }
    }

    // After loop, add any remaining content in the buffer as the last argument
    if (current_arg_buffer->length > 0) {
        if (argc >= MAX_ARGS - 1) {
            fprintf(stderr, "parse_arguments: too many arguments (max %d)\n", MAX_ARGS - 1);
            free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
        }
        argv[argc] = strdup(current_arg_buffer->buffer);
        if (!argv[argc]) {
            perror("parse_arguments: strdup failed for final arg");
            free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
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
    if (redir == NULL) {
        perror("init_redirection_info: malloc failed");
        return NULL;
    }
    redir->has_output_redirect = 0;
    redir->output_file = NULL;

    return redir;
}

// Helper function to free redirection info
void free_redirection_info(RedirectionInfo* redir) {
    if (redir != NULL) {
        free(redir->output_file);
        free(redir);
    }
}

// Helper function to deal with arguments involving redirection
ParseResult* parse_args_with_redirection(const char* input_line) {
    ParseResult* result = malloc(sizeof(ParseResult));
    if (result == NULL) {
        perror("parse_args_with_redirection: malloc failed");
        return NULL;
    }

    result->redir_info = init_redirection_info();
    if (result->redir_info == NULL) {
        free(result);
        return NULL;
    }

    char** all_args = parse_arguments(input_line);
    if (all_args == NULL) {
        free_redirection_info(result->redir_info);
        free(result);
        return NULL;
    }

    int total_args = 0;
    while (all_args[total_args] != NULL) {
        total_args++;
    }

    int redirect_idx = -1;
    for (int i = 0; i < total_args; i++) {
        if (strcmp(all_args[i], ">") == 0 || strcmp(all_args[i], ">1") == 0) {
            redirect_idx = i;
            break;
        }
    }

    if (redirect_idx != -1) {
        if (redirect_idx + 1 >= total_args || all_args[redirect_idx + 1] == NULL) {
            fprintf(stderr, "shell: syntax error: expected filename after redirection\n");
            free_argv(all_args);
            free_redirection_info(result->redir_info);
            free(result);
            return NULL;
        }

        result->redir_info->has_output_redirect = 1;
        result->redir_info->output_file = strdup(all_args[redirect_idx + 1]);
        if (result->redir_info->output_file == NULL) {
            perror("parse_args_with_redirection: strdup failed for filename");
            free_argv(all_args);
            free_redirection_info(result->redir_info);
            free(result);
            return NULL;
        }

        result->argv = malloc((redirect_idx + 1) * sizeof(char*));
        if (result->argv == NULL) {
            perror("parse_args_with_redirection: malloc failed for result argv");
            free_argv(all_args);
            free_redirection_info(result->redir_info);
            free(result);
            return NULL;
        }

        for (int i = 0; i < redirect_idx; i++) {
            result->argv[i] = strdup(all_args[i]);
            if (result->argv[i] == NULL) {
                perror("parse_args_with_redirection: strdup failed");
                for (int j = 0; j < i; j++) {
                    free(result->argv[j]);
                }

                free(result->argv);
                free_argv(all_args);
                free_redirection_info(result->redir_info);
                free(result);
                return NULL;
            }
        }

        result->argv[redirect_idx] = NULL;
    } else {
        result->argv = all_args;
        all_args = NULL;
    }

    if (all_args != NULL) {
        free_argv(all_args);
    }
    
    return result;
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
        fprintf(stderr, "DEBUG: handle_echo_cmd - argv[%d]: '%s'\n", i, argv[i]);
        printf("%s%s", argv[i], (argv[i+1] != NULL) ? " " : "");
    }
    printf("\n");
    fprintf(stderr, "DEBUG: handle_echo_cmd finished.\n");
}

// Helper function to handle `exit` commands
int handle_exit_cmd(char** argv) { // Now takes char** argv
    if (argv[1] != NULL) {
        return atoi(argv[1]);
    }
    return 0;
}

// Helper function to handle `type` commands
void handle_type_cmd(char** argv) { // Now takes char** argv
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
            continue;
        }
        
        char* pathCopy = strdup(pathEnv);
        if (pathCopy == NULL) {
            perror("type: strdup failed for PATH");
            printf("%s: not found\n", cmd_to_type);
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
            printf("%s: not found\n", cmd_to_type);
        }
        free(pathCopy);
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
    const char* path = argv[1];

    const char* target_path;
    char expanded_path[PATH_MAX];

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
            return strdup(exe);
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
void execute_external_exe_with_redirection(const char* exePath, char* argv[], RedirectionInfo* redir_info) {
    pid_t pid = fork();

    if (pid == 0) {
        if (redir_info->has_output_redirect) {
            int fd = open(redir_info->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd == -1) {
                perror("open");
                exit(1);
            }

            if (dup2(fd, STDOUT_FILENO) == -1) {
                perror("dup2");
                close(fd);
                exit(1);
            }
            close(fd);
        }

        execv(exePath, argv);
        perror("execv failed");
        exit(1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    } else {
        perror("fork failed");
    }
}

// Helper function to setup output redirection
int setup_output_redirection(const char* filename) {
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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

// Helper function to restore stdout
void restore_stdout(int saved_stdout) {
    if (saved_stdout != -1) {
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
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
            if (parsed_result->redir_info->has_output_redirect) {
                saved_stdout = setup_output_redirection(parsed_result->redir_info->output_file);
                if (saved_stdout == -1) {
                    fprintf(stderr, "ERROR: Failed to setup redirection to %s\n", parsed_result->redir_info->output_file);
                    free_parse_result(parsed_result);
                    continue;
                }
                fprintf(stderr, "DEBUG: Redirection to '%s' successfully set up. saved_stdout: %d\n", parsed_result->redir_info->output_file, saved_stdout);
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
                    printf("pwd: could not retrieve current working directory\n");
                }
            } else if (strcmp(command, "cd") == 0) {
                handle_cd_cmd(parsed_result->argv);
            }
            
            if (saved_stdout != -1) {
                restore_stdout(saved_stdout);
            }
        } else {
            char* exePath = find_exe_in_path(command);
            if (exePath != NULL) {
                execute_external_exe_with_redirection(exePath, parsed_result->argv, parsed_result->redir_info);
                free(exePath);
            } else {
                printf("%s: command not found\n", command);
            }
        }

        free_parse_result(parsed_result);
    }

    return status;
}