#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <limits.h>
#include <fcntl.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <dirent.h>
#include <strings.h>

#define BUF_SIZE 512
#define MAX_ARGS 64
#define ARG_SIZE 64
#define MAX_JOBS 100
#define MAX_COMPLETIONS 64

// Define built-in commands for completion
const char* builtins[] = {
    "echo", "exit", "type", "pwd", "cd", "history", "jobs", "complete", NULL
};

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
    int stdout_mode; // 0=overwrite, 1=append
    char* stdout_file;
    int has_stderr_redirect;
    int stderr_mode; // 0=overwrite, 1=append
    char* stderr_file;
} RedirectionInfo;

// Structure to modify parsed arguments to handle redirection
typedef struct {
    char** argv;
    RedirectionInfo* redir_info;
    int is_background_process;
} ParseResult;

// Structure to maintain record of running background processes
typedef struct {
    int job_id;
    pid_t pid;
    char command[BUF_SIZE];
    int is_active;
    int is_done;
} Job;

// Structure to keep mappings of command to completer script
typedef struct {
    char* command;
    char* completer;
} CompletionRegister;

// Structure grouping registrations with count tracking variable
typedef struct {
    CompletionRegister list[MAX_COMPLETIONS];
    int count;
} CompletionSystem;

// Helper function to initialize job lists
void init_jobs_system(Job* list) {
    for (int i = 0; i < MAX_JOBS; i++) {
        list[i].job_id = 0;
        list[i].pid = 0;
        list[i].is_active = 0;
        memset(list[i].command, 0, BUF_SIZE);
    }
}

// Helper function to initialize completion specifications
void init_completion_system(CompletionSystem* sys) {
    sys->count = 0;
    for (int i = 0; i < MAX_COMPLETIONS; i++) {
        sys->list[i].command = NULL;
        sys->list[i].completer = NULL;
    }
}

// Helper function to find smallest available job ID to allow for job number recycling
int get_smallest_available_job(Job* list) {
    int candidate = 1;
    while (1) {
        int id_is_taken = 0;

        // Scan the job list to see if current candidate ID is in use
        for (int i = 0; i < MAX_JOBS; i++) {
            if (list[i].is_active && list[i].job_id == candidate) {
                id_is_taken = 1;
                break;
            }
        }

        if (!id_is_taken) {
            return candidate;
        }

        candidate++;
    }
}

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
                i++; // Consume the quote, do NOT add to buffer
            } else {
                // In single quotes, ALL characters are literal, added to buffer
                if (add_char_to_buffer(current_arg_buffer, current_char) < 0) {
                    free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
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
                        free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
                    }
                } else if (input_line[i] == '"' || input_line[i] == '\\' ||
                           input_line[i] == '$' || input_line[i] == '`') {
                    // Specific characters: \ escapes these, the backslash is removed, char is literal.
                    if (add_char_to_buffer(current_arg_buffer, input_line[i]) < 0) {
                        free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
                    }
                } else {
                    // For all other characters (like `\n`, `\5`, `\t`, `\X` etc.):
                    // The backslash itself is preserved as a literal character,
                    // followed by the next character, also as a literal.
                    if (add_char_to_buffer(current_arg_buffer, '\\') < 0 ||
                        add_char_to_buffer(current_arg_buffer, input_line[i]) < 0) {
                        free_arg_buffer(current_arg_buffer); free_argv(argv); return NULL;
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
            // Order of checks is crucial: Compound Append > Single append > Compound redirects > Single redirects > Whitespace > Regular chars

            // 1. Compound Append (e.g., "1>>", "2>>")
            //    Check if current character is a digit '1' or '2' AND if it's followed by '>>'
            else if ((i + 2 < strlen(input_line)) && (current_char == '1' || current_char == '2') && input_line[i+1] == '>' && input_line[i+2] == '>') {
                // Finalize any existing argument if one is being built
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
                // Add the compound append token itself
                if (argc >= MAX_ARGS - 1) {
                    fprintf(stderr, "parse_arguments: too many arguments (max %d)\n", MAX_ARGS - 1);
                    free_arg_buffer(current_arg_buffer);
                    free_argv(argv);
                    return NULL;
                }
                char compound_append[4];
                compound_append[0] = current_char; // '1' or '2'
                compound_append[1] = '>';
                compound_append[2] = '>';
                compound_append[3] = '\0';
                argv[argc] = strdup(compound_append);
                if (!argv[argc]) {
                    perror("parse_arguments: strdup failed for compound redir");
                    free_arg_buffer(current_arg_buffer);
                    free_argv(argv);
                    return NULL;
                }
                argc++;
                i += 3; // Consume the digit, the first '>', and the second '>'
            }
            // 2. Append Operator (e.g. >>)
            //    Check if current character is a '>' AND if it's followed by '>'
            else if ((i+1 < strlen(input_line)) && current_char == '>' && input_line[i+1] == '>') {
                // Finalize any existing argument if one is being built
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
                // Add the append token itself
                if (argc >= MAX_ARGS - 1) {
                    fprintf(stderr, "parse_arguments: too many arguments (max %d)\n", MAX_ARGS - 1);
                    free_arg_buffer(current_arg_buffer);
                    free_argv(argv);
                    return NULL;
                }
                char append_token[3];
                append_token[0] = '>';
                append_token[1] = '>';
                append_token[2] = '\0';
                argv[argc] = strdup(append_token);
                if (!argv[argc]) {
                    perror("parse_arguments: strdup failed for compound redir");
                    free_arg_buffer(current_arg_buffer);
                    free_argv(argv);
                    return NULL;
                }
                argc++;
                i += 2; // Consume both '>' characters
            }
            // 3. Compound Redirection (e.g., "1>", "2>")
            //    Check if the current character is a digit '1' or '2' AND if it's followed by '>'
            else if ((i + 1 < strlen(input_line)) && (current_char == '1' || current_char == '2') && input_line[i+1] == '>') {
                // Finalize any existing argument if one is being built
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
                
                // Add the compound redirection token itself
                if (argc >= MAX_ARGS - 1) {
                    fprintf(stderr, "parse_arguments: too many arguments (max %d)\n", MAX_ARGS - 1);
                    free_arg_buffer(current_arg_buffer);
                    free_argv(argv);
                    return NULL;
                }
                char compound_redir[3];
                compound_redir[0] = current_char; // '1' or '2'
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
                i += 2; // Consume both the digit and the '>'
            }
            // 4. Single Redirection/Pipe Operators (e.g., ">", "<", "|")
            else if (current_char == '>' || current_char == '<' || current_char == '|') {
                // Finalize any existing argument if one is being built
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
                
                // Add the single operator as its own argument
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
                    perror("parse_arguments: strdup failed for single redir");
                    free_arg_buffer(current_arg_buffer);
                    free_argv(argv);
                    return NULL;
                }
                argc++;
                i++; // Consume the current character
            }
            // 5. Whitespace (always separates arguments)
            else if (isspace((unsigned char)current_char)) {
                // If there's content in the buffer, finalize it as an argument
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
                i++; // Consume the space
                // Skip subsequent whitespace
                while (isspace((unsigned char)input_line[i])) {
                    i++;
                }
            }
            // 6. Regular characters (builds an argument)
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

// Helper function to initialize redirection information
RedirectionInfo* init_redirection_info() {
    RedirectionInfo* redir = malloc(sizeof(RedirectionInfo));
    if (redir == NULL) {
        perror("init_redirection_info: malloc failed");
        return NULL;
    }
    redir->has_stdout_redirect = 0;
    redir->stdout_file = NULL;
    redir->stdout_mode = 0; // Default to overwrite
    redir->has_stderr_redirect = 0;
    redir->stderr_file = NULL;
    redir->stderr_mode = 0; // Default to overwrite

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

    int redirect_stdout_idx = -1;
    int redirect_stderr_idx = -1;

    for (int i = 0; i < total_args; i++) {
        // Updated to explicitly check for "1>" as produced by parse_arguments
        if (strcmp(all_args[i], ">") == 0 || strcmp(all_args[i], "1>") == 0) {
            if (redirect_stdout_idx != -1) {
                fprintf(stderr, "shell: syntax error: multiple stdout redirections\n");
                free_argv(all_args);
                free_redirection_info(result->redir_info);
                free(result);
                return NULL;
            }
            redirect_stdout_idx = i;
            result->redir_info->stdout_mode = 0;
        } else if (strcmp(all_args[i], ">>") == 0 || strcmp(all_args[i], "1>>") == 0) {
            if (redirect_stdout_idx != -1) {
                fprintf(stderr, "shell: syntax error: multiple stdout redirections\n");
                free_argv(all_args);
                free_redirection_info(result->redir_info);
                free(result);
                return NULL;
            }
            redirect_stdout_idx = i;
            result->redir_info->stdout_mode = 1; // Append
        } else if (strcmp(all_args[i], "2>") == 0) {
            if (redirect_stderr_idx != -1) {
                fprintf(stderr, "shell: syntax error: multiple stderr redirections\n");
                free_argv(all_args);
                free_redirection_info(result->redir_info);
                free(result);
                return NULL;
            }
            redirect_stderr_idx = i;
            result->redir_info->stderr_mode = 0;
        } else if (strcmp(all_args[i], "2>>") == 0) {
            if (redirect_stderr_idx != -1) {
                fprintf(stderr, "shell: syntax error: multiple stderr redirections\n");
                free_argv(all_args);
                free_redirection_info(result->redir_info);
                free(result);
                return NULL;
            }
            redirect_stderr_idx = i;
            result->redir_info->stderr_mode = 1;
        }
    }

    if (redirect_stdout_idx != -1) {
        if (redirect_stdout_idx + 1 >= total_args || all_args[redirect_stdout_idx + 1] == NULL) {
            fprintf(stderr, "shell: syntax error: expected filename after stdout redirection\n");
            free_argv(all_args);
            free_redirection_info(result->redir_info);
            free(result);
            return NULL;
        }
        result->redir_info->has_stdout_redirect = 1;
        result->redir_info->stdout_file = strdup(all_args[redirect_stdout_idx + 1]);
        if (result->redir_info->stdout_file == NULL) {
            perror("parse_args_with_redirection: strdup failed for stdout filename");
            free_argv(all_args);
            free_redirection_info(result->redir_info);
            free(result);
            return NULL;
        }
    }

    if (redirect_stderr_idx != -1) {
        if (redirect_stderr_idx + 1 >= total_args || all_args[redirect_stderr_idx + 1] == NULL) {
            fprintf(stderr, "shell: syntax error: expected filename after stderr redirection\n");
            free_argv(all_args);
            free_redirection_info(result->redir_info);
            free(result);
            return NULL;
        }
        result->redir_info->has_stderr_redirect = 1;
        result->redir_info->stderr_file = strdup(all_args[redirect_stderr_idx + 1]);
        if (result->redir_info->stderr_file == NULL) {
            perror("parse_args_with_redirection: strdup failed for stderr filename");
            free_argv(all_args);
            free_redirection_info(result->redir_info);
            free(result);
            return NULL;
        }
    }

    result->argv = malloc((total_args + 1) * sizeof(char*));
    if (result->argv == NULL) {
        perror("parse_args_with_redirection: malloc failed for result argv");
        free_argv(all_args);
        free_redirection_info(result->redir_info);
        free(result);
        return NULL;
    }

    int argc = 0;
    for (int i = 0; i < total_args; i++) {
        if (i == redirect_stdout_idx || i == redirect_stderr_idx) {
            i++;
            continue;
        }
        result->argv[argc] = strdup(all_args[i]);
        if (result->argv[argc] == NULL) {
            perror("parse_args_with_redirection: strdup failed for command arg");
            for (int j = 0; j < argc; j++) {
                free(result->argv[j]);
            }
            free(result->argv);
            free_argv(all_args);
            free_redirection_info(result->redir_info);
            free(result);
            return NULL;
        }
        argc++;
    }
    result->argv[argc] = NULL;

    if (argc > 0 && strcmp(result->argv[argc - 1], "&") == 0) {
        result->is_background_process = 1;
        free(result->argv[argc - 1]);
        result->argv[argc - 1] = NULL;
    } else {
        result->is_background_process = 0;
    }

    free_argv(all_args);
    
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
        // The quotes are now stripped during parsing in parse_arguments
        printf("%s%s", argv[i], (argv[i+1] != NULL) ? " " : "");
    }
    printf("\n");
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
            (strcmp(cmd_to_type, "cd") == 0) ||
            (strcmp(cmd_to_type, "history") == 0) ||
            (strcmp(cmd_to_type, "jobs") == 0) ||
            (strcmp(cmd_to_type, "complete") == 0)
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

// Global variable keeping track of last written history index.
static int last_history_written_idx = 0;

// Helper function to load history from file
static void load_history_from_file(const char* filename) {
    FILE* fp = fopen(filename, "r");
    if (fp == NULL) {
        perror("history");
        return;
    }

    char* line = NULL;
    size_t len = 0;
    ssize_t read;

    while ((read = getline(&line, &len, fp)) != -1) {
        if (read > 0 && line[read-1] == '\n') {
            line[read-1] = '\0';
            read--;
        }

        if (read <= 0 || strlen(line) == 0) {
            continue;
        }

        add_history(line);
    }

    free(line);
    fclose(fp);
}

// Helper function to save history to a file
static void save_history_to_file(const char* filename) {
    FILE* fp = fopen(filename, "w");
    if (fp == NULL) {
        perror("history");
        return;
    }

    HIST_ENTRY** history_entries = history_list();
    if (!history_entries) {
        fclose(fp);
        return;
    }

    for (int i = 0; history_entries[i] != NULL; i++) {
        fprintf(fp, "%s\n", history_entries[i]->line);
    }

    fclose(fp);
}

// Helper function to handle `history` commands
void handle_history_cmd(char** argv) {
    if (argv[1] != NULL && strcmp(argv[1], "-r") == 0) {
        if (argv[2] == NULL) {
            fprintf(stderr, "history: -r: option requires an argument\n");
            return;
        }

        load_history_from_file(argv[2]);
        return;
    }
    // Handle history -w <file> option: write history to file (overwrite)
    else if (argv[1] != NULL && strcmp(argv[1], "-w") == 0) {
        if (argv[2] == NULL) {
            fprintf(stderr, "history: -w: option requires an argument\n");
            return;
        }

        save_history_to_file(argv[2]);
        last_history_written_idx = history_length;
        return;
    }
    // Handle history -a <file> option: append history to file
    else if (argv[1] != NULL && strcmp(argv[1], "-a") == 0) {
        if (argv[2] == NULL) {
            fprintf(stderr, "history: -a option requires an argument\n");
            return;
        }

        const char* filename = argv[2];
        FILE* fp = fopen(filename, "a");
        if (fp == NULL) {
            perror("history");
            return;
        }

        HIST_ENTRY** history_entries = history_list();
        if (!history_entries) {
            fclose(fp);
            return;
        }

        for (int i = last_history_written_idx; history_entries[i] != NULL; i++) {
            fprintf(fp, "%s\n", history_entries[i]->line);
        }

        fclose(fp);
        last_history_written_idx = history_length;
        return;
    }
    
    // Exiting history display logic below
    HIST_ENTRY** history_entries = history_list();
    if (!history_entries) {
        return;
    }

    int total_entries = 0;
    while (history_entries[total_entries] != NULL) {
        total_entries++;
    }

    int n = total_entries;
    if (argv[1] != NULL) {
        char* endptr;
        long temp = strtol(argv[1], &endptr, 10);

        // Validate the argument
        if (*endptr != '\0' || temp <= 0) {
            fprintf(stderr, "history: %s: numeric argument required\n", argv[1]);
            return;
        }

        n = (int) temp;
        if (n > total_entries) {
            n = total_entries;
        }
    }

    int start_idx = total_entries - n;
    for (int i = start_idx; i < total_entries; i++) {
        printf("%5d %s\n", i + history_base, history_entries[i]->line);
    }
}

// Helper function to handle `jobs` commands
void handle_jobs_cmd(char** argv, Job* list) {
    (void)argv;
    int max_id = -1;
    int second_max_id = -1;
    
    // First Pass: Find highest and second-highest active job IDs
    for (int i = 0; i < MAX_JOBS; i++) {
        if (list[i].is_active) {
            if (list[i].job_id > max_id) {
                second_max_id = max_id;
                max_id = list[i].job_id;
            } else if (list[i].job_id > second_max_id) {
                second_max_id = list[i].job_id;
            }
        }
    }

    // Second Pass: Print jobs and their calculated markers
    for (int i = 0; i < MAX_JOBS; i++) {
        if (list[i].is_active) {
            // Default space for all other older jobs
            char marker = ' ';

            if  (list[i].job_id == max_id) {
                marker = '+';
            } else if (list[i].job_id == second_max_id) {
                marker = '-';
            }

            if (list[i].is_done) {
                // Print job as done with proper 24-character padding
                printf("[%d]%c %-24s%s\n", list[i].job_id, marker, "Done", list[i].command);

                // Remove job entry from list records immediately
                list[i].is_active = 0;
                list[i].is_done = 0;
            } else {
                printf("[%d]%c %-24s%s &\n", list[i].job_id, marker, "Running", list[i].command);
            }
        }
    }
}

// Helper function to look pu an existing command in registry
int find_completion_index(CompletionSystem* sys, const char* command) {
    for (int i = 0; i < sys->count; i++) {
        if (sys->list[i].command != NULL && strcmp(sys->list[i].command, command) == 0) {
            return i;
        }
    }

    return -1;
}

// Helper function to register context and retrieve it when needed
CompletionSystem* get_set_completion_context(CompletionSystem* new_sys) {
    static CompletionSystem* saved_sys = NULL;
    if (new_sys != NULL) {
        saved_sys = new_sys;
    }
    return saved_sys;
}

// Helper function which generates custom completion scripts registered
char* script_completion_generator(const char* text, int state) {
    CompletionSystem* sys = get_set_completion_context(NULL);

    if (state != 0 || sys == NULL) {
        return NULL;
    }

    char line_copy[BUF_SIZE];
    strncpy(line_copy, rl_line_buffer, sizeof(line_copy) - 1);
    line_copy[sizeof(line_copy) - 1] = '\0';

    char* cmd_name = strtok(line_copy, " \t");
    if (!cmd_name) {
        return NULL;
    }

    int idx = find_completion_index(sys, cmd_name);
    if (idx == -1) {
        return NULL;
    }
    
    const char* script_path = sys->list[idx].completer;

    FILE* fp = popen(script_path, "r");
    if (!fp) {
        perror("popen failed running completer");
        return NULL;
    }

    char output_line[BUF_SIZE];
    if (fgets(output_line, sizeof(output_line), fp) != NULL) {
        size_t len = strlen(output_line);
        if (len > 0 && output_line[len - 1] == '\n') {
            output_line[len - 1] = '\0';
        }
        pclose(fp);
        return strdup(output_line);
    }

    pclose(fp);
    return NULL;
}

// Helper function to handle `complete` commands
void handle_complete_cmd(char** argv, CompletionSystem* sys) {
    if (argv == NULL || argv[0] == NULL || sys == NULL) {
        return;
    }

    
    if (argv[1] != NULL && strcmp(argv[1], "-p") == 0) {
        // The -p Flag: Prints out matching specifications
        if (argv[2] == NULL) {
            fprintf(stderr, "complete: usage: complete [-p] [command]\n");
            return;
        }

        int idx = find_completion_index(sys, argv[2]);
        if (idx != -1) {
            printf("complete -C '%s' %s\n", sys->list[idx].completer, sys->list[idx].command);
        } else {
            fprintf(stderr, "complete: %s: no completion specification\n", argv[2]);
        }
    } else if (argv[1] != NULL ) {
        // The -C Flag: Registers new completion script
        if (argv[2] == NULL || argv[3] == NULL) {
            fprintf(stderr, "complete: usage: complete -C [completer] [command]\n");
            return;
        }

        const char* script_path = argv[2];
        const char* cmd_name = argv[3];

        int idx = find_completion_index(sys, cmd_name);
        if (idx != -1) {
            free(sys->list[idx].completer);
            sys->list[idx].completer = strdup(script_path);
        } else {
            if (sys->count < MAX_COMPLETIONS) {
                sys->list[sys->count].command = strdup(cmd_name);
                sys->list[sys->count].completer = strdup(script_path);
                sys->count++;
            } else {
                fprintf(stderr, "complete: completion registry is full\n");
            }
        }
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
void execute_external_exe_with_redirection(const char* exePath, char* argv[], RedirectionInfo* redir_info, int is_background_process, Job* list) {
    pid_t pid = fork();

    if (pid == 0) { // Child process
        if (redir_info->has_stdout_redirect) {
            int flags = O_WRONLY | O_CREAT;
            flags |= (redir_info->stdout_mode == 1) ? O_APPEND : O_TRUNC;
            int fd = open(redir_info->stdout_file, flags, 0644);
            if (fd == -1) {
                perror("open stdout redirection file");
                exit(1); // Child exits on error
            }

            if (dup2(fd, STDOUT_FILENO) == -1) {
                perror("dup2 stdout");
                close(fd);
                exit(1); // Child exits on error
            }
            close(fd); // Child closes its copy of the original fd
        }

        if (redir_info->has_stderr_redirect) {
            int flags = O_WRONLY | O_CREAT;
            flags |= (redir_info->stderr_mode == 1) ? O_APPEND : O_TRUNC;
            int fd = open(redir_info->stderr_file, flags, 0644);
            if (fd == -1) {
                perror("open stderr redirection file");
                exit(1);
            }

            if (dup2(fd, STDERR_FILENO) == -1) {
                perror("dup2 stderr");
                close(fd);
                exit(1);
            }
            close(fd);
        }

        execv(exePath, argv);
        perror("execv failed"); // Only reached if execv fails
        exit(1); // Child exits if execv fails
    } else if (pid > 0) { // Parent process
        if (is_background_process) {
            int assigned_job_id = get_smallest_available_job(list);

            printf("[%d] %d\n", assigned_job_id, pid);

            for (int i = 0; i < MAX_JOBS; i++) {
                if (!list[i].is_active) {
                    list[i].job_id = assigned_job_id;
                    list[i].pid = pid;
                    list[i].command[0] = '\0';
                    for (int j = 0; argv[j] != NULL; j++) {
                        strncat(list[i].command, argv[j], BUF_SIZE - strlen(list[i].command) - 1);
                        if (argv[j + 1] != NULL) {
                            strncat(list[i].command, " ", BUF_SIZE - strlen(list[i].command) - 1);
                        }
                    }
                    list[i].is_active = 1;
                    list[i].is_done = 0;
                    break;
                }
            }
        } else {
            // Traditional blocking behavior for foreground jobs
            int status;
            waitpid(pid, &status, 0); // Parent waits for child
        }
    } else { // Fork failed
        perror("fork failed");
    }
}

// Helper function to setup output redirection (for built-ins)
int setup_stdout_redirection(const char* filename, int mode) {
    int flags = O_WRONLY | O_CREAT;
    flags |= (mode == 1) ? O_APPEND : O_TRUNC;
    int fd = open(filename, flags, 0644);
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
int setup_stderr_redirection(const char* filename, int mode) {
    int flags = O_WRONLY | O_CREAT;
    flags |= (mode == 1) ? O_APPEND : O_TRUNC;
    int fd = open(filename, flags, 0644);
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

void restore_stderr(int saved_stderr) {
    if (saved_stderr != -1) {
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }
}

// Generator function for command completion in PATH
char* command_generator(const char* text, int state) {
    static int builtin_idx;
    static int path_idx;
    static DIR* dirp = NULL;
    static char** path_dirs = NULL;
    static int path_count = 0;

    if (!state) {
        builtin_idx = 0;
        path_idx = 0;
        dirp = NULL;

        if (path_dirs) {
            for (int i = 0; i < path_count; i++) {
                free(path_dirs[i]);
            }
            free(path_dirs);
            path_dirs = NULL;
            path_count = 0;
        }

        char* path = getenv("PATH");
        if (path) {
            char* copy = strdup(path);
            if (copy) {
                char* token;
                int count = 0;
                char* saveptr = NULL;

                token = strtok_r(copy, ":", &saveptr);
                while(token) {
                    count++;
                    token = strtok_r(NULL, ":", &saveptr);
                }

                path_dirs = malloc(count * sizeof(char*));
                if (path_dirs) {
                    // We need to tokenize again to copy each directory
                    char* copy2 = strdup(path);
                    saveptr = NULL;
                    token = strtok_r(copy2, ":", &saveptr);
                    int i = 0;
                    while(token && i < count) {
                        path_dirs[i] = strdup(token);
                        token = strtok_r(NULL, ":", &saveptr);
                        i++;
                    }
                    free(copy2);
                    path_count = count;
                }
                free(copy);
            }
        }
    }

    // First, check built-in commands
    while (builtin_idx < (int)(sizeof(builtins)/sizeof(char*)) - 1) {
        const char* name = builtins[builtin_idx++];
        if (strncasecmp(text, name, strlen(text)) == 0) {
            return strdup(name);
        }
    }

    while (path_idx < path_count) {
        if (!dirp) {
            dirp = opendir(path_dirs[path_idx]);
            if (!dirp) {
                path_idx++;
                continue;
            }
        }

        struct dirent* entry;
        while ((entry = readdir(dirp)) != NULL) {
            if (strncmp(text, entry->d_name, strlen(text)) == 0) {
                char full_path[PATH_MAX];
                snprintf(full_path, sizeof(full_path), "%s/%s", path_dirs[path_idx], entry->d_name);

                if (access(full_path, X_OK) == 0) {
                    char* name_copy = strdup(entry->d_name);
                    return name_copy;
                }
            }
        }

        closedir(dirp);
        dirp = NULL;
        path_idx++;
    }

    return NULL;
}

// Generator function for filename completion
char* filename_generator(const char* text, int state) {
    static DIR* dirp = NULL;
    static char static_prefix[BUF_SIZE]; // Persists across calls when state != 0
    static char dir_path[PATH_MAX]; // Persists to reconstruct full path
    struct dirent* entry;

    // State 0 means this is the first call for this completion request
    if (!state) {
        rl_filename_completion_desired = 1;

        if (dirp) {
            closedir(dirp);
            dirp = NULL;
        }
        
        // Find the last occurrenc of '/'
        char* last_slash = strrchr(text, '/');

        if (last_slash != NULL) {
            // Calculate lengths
            size_t dir_len = (last_slash - text) + 1; // Includes the '/'

            // Copy directory portion and null-terminate it
            strncpy(dir_path, text, dir_len);
            dir_path[dir_len] = '\0';

            // Copy whatever follows slash as the search prefix
            strcpy(static_prefix, last_slash + 1);

            // Open specified directory
            dirp = opendir(dir_path);
        }
        else {
            // Fallback: No slash means current directory
            strcpy(dir_path, ""); // Keep empty so that full path stitching works later
            strcpy(static_prefix, text);
            dirp = opendir(".");
        }

        if (!dirp) {
            perror("filename_generator: opendir failed");
            return NULL;
        }
    }

    // Read through the directory entries
    while ((entry = readdir(dirp)) != NULL) {
        // Skip the current and parent directory shortcuts
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Compare against extracted static_prefix
        if (strncmp(static_prefix, entry->d_name, strlen(static_prefix)) == 0) {

            // Completion for directories
            if (entry->d_type == DT_DIR) {
                rl_completion_append_character = '/';
            }
            else {
                rl_completion_append_character = ' ';
            }

            char full_match[PATH_MAX];
            snprintf(full_match, sizeof(full_match), "%s%s", dir_path, entry->d_name);

            // Return match. Readline will call again with state != 0
            return strdup(full_match);
        }
    }

    // No more matches left, so clean up and return NULL
    closedir(dirp);
    dirp = NULL;
    return NULL;
}

// Generator function for builtin command completion
char** builtin_completion(const char* text, int start, int end) {
    // Always reset to the default space at the beginning of a new TAB press
    rl_completion_append_character = ' ';

    if (start == 0) {
        // User is completing command name
        rl_attempted_completion_over = 1;
        return rl_completion_matches(text, command_generator);
    }
    else {
        // User is completing an argument
        CompletionSystem* sys = get_set_completion_context(NULL);

        char line_copy[BUF_SIZE];
        strncpy(line_copy, rl_line_buffer, sizeof(line_copy) - 1);
        line_copy[sizeof(line_copy) - 1] = '\0';

        char* cmd_name = strtok(line_copy, " \t");
        if (cmd_name && sys && find_completion_index(sys, cmd_name) != -1) {
            rl_attempted_completion_over = 1;
            rl_completion_append_character = ' ';

            // Override entry function to route directly to local-state generator
            return rl_completion_matches(text, script_completion_generator);
        }

        // Fallback to default file/directory behavior if no custom script is registered
        rl_attempted_completion_over = 0;
        return rl_completion_matches(text, filename_generator);
    }
}

// Helper function to split tokens by pipe operators
char*** split_tokens_by_pipe(char** tokens, int* n_segments) {
    int count = 1;
    for (int i = 0; tokens[i] != NULL; i++) {
        if (strcmp(tokens[i], "|") == 0) {
            count++;
        }
    }
    *n_segments = count;

    char*** segments = malloc(count * sizeof(char**));
    int seg_index = 0;
    int start = 0;
    int i = 0;
    
    while (1) {
        if (tokens[i] == NULL || strcmp(tokens[i], "|") == 0) {
            int seg_length = i - start;
            segments[seg_index] = malloc((seg_length + 1) * sizeof(char*));
            
            for (int j = 0; j < seg_length; j++) {
                segments[seg_index][j] = tokens[start + j];
            }
            segments[seg_index][seg_length] = NULL;
            seg_index++;
            start = i + 1;
            
            if (tokens[i] == NULL) break;
        }
        i++;
    }
    
    return segments;
}

// Execute a pipeline of commands
void execute_pipeline(ParseResult** segments, int n_segments, Job* jobs_list, int is_background_process, CompletionSystem* comp_sys) {
    int prev_pipe[2] = {-1, -1};
    int next_pipe[2] = {-1, -1};
    pid_t* pids = malloc(n_segments * sizeof(pid_t));
    
    for (int i = 0; i < n_segments; i++) {
        // Create new pipe if not last command
        if (i < n_segments - 1) {
            if (pipe(next_pipe)) {
                perror("pipe");
                return;
            }
        }
        
        pid_t pid = fork();
        if (pid == 0) { // Child process
            // Connect to previous pipe (if exists)
            if (i > 0) {
                dup2(prev_pipe[0], STDIN_FILENO);
                close(prev_pipe[0]);
                close(prev_pipe[1]);
            }
            
            // Connect to next pipe (if exists)
            if (i < n_segments - 1) {
                close(next_pipe[0]);
                dup2(next_pipe[1], STDOUT_FILENO);
                close(next_pipe[1]);
            }
            
            // Handle redirection for this command
            RedirectionInfo* redir = segments[i]->redir_info;
            if (redir->has_stdout_redirect) {
                int flags = O_WRONLY | O_CREAT;
                flags |= (redir->stdout_mode == 1) ? O_APPEND : O_TRUNC;
                int fd = open(redir->stdout_file, flags, 0644);
                if (fd == -1) {
                    perror("open stdout redirection file");
                    exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            
            if (redir->has_stderr_redirect) {
                int flags = O_WRONLY | O_CREAT;
                flags |= (redir->stderr_mode == 1) ? O_APPEND : O_TRUNC;
                int fd = open(redir->stderr_file, flags, 0644);
                if (fd == -1) {
                    perror("open stderr redirection file");
                    exit(1);
                }
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
            
            // Execute built-in or external command
            const char* command = segments[i]->argv[0];
            if (strcmp(command, "echo") == 0) {
                handle_echo_cmd(segments[i]->argv);
                exit(0);
            } else if (strcmp(command, "exit") == 0) {
                exit(handle_exit_cmd(segments[i]->argv));
            } else if (strcmp(command, "type") == 0) {
                handle_type_cmd(segments[i]->argv);
                exit(0);
            } else if (strcmp(command, "pwd") == 0) {
                char* pwd = handle_pwd_cmd();
                if (pwd != NULL) {
                    printf("%s\n", pwd);
                    free(pwd);
                }
                exit(0);
            } else if (strcmp(command, "cd") == 0) {
                handle_cd_cmd(segments[i]->argv);
                exit(0);
            } else if (strcmp(command, "history") == 0) {
                handle_history_cmd(segments[i]->argv);
                exit(0);
            } else if (strcmp(command, "jobs") == 0) {
                handle_jobs_cmd(segments[i]->argv, jobs_list);
                exit(0);
            }
            else if (strcmp(command, "complete") == 0) {
                handle_complete_cmd(segments[i]->argv, comp_sys);
                exit(0);
            }
            else {
                char* exePath = find_exe_in_path(command);
                if (exePath != NULL) {
                    execv(exePath, segments[i]->argv);
                    perror("execv failed");
                    exit(1);
                } else {
                    fprintf(stderr, "%s: command not found\n", command);
                    exit(1);
                }
            }
        } else if (pid < 0) {
            perror("fork");
            return;
        } else {
            pids[i] = pid;
            // Close previous pipe ends
            if (i > 0) {
                close(prev_pipe[0]);
                close(prev_pipe[1]);
            }
            // Set up for next iteration
            if (i < n_segments - 1) {
                prev_pipe[0] = next_pipe[0];
                prev_pipe[1] = next_pipe[1];
            } else {
                // Close next_pipe if it exists for the last command
                if (next_pipe[0] != -1) close(next_pipe[0]);
                if (next_pipe[1] != -1) close(next_pipe[1]);
            }
        }
    }
    
    // Wait for all child processes
    if (!is_background_process) {
        for (int i = 0; i < n_segments; i++) {
            waitpid(pids[i], NULL, 0);
        }
    } else {
        int assigned_job_id = get_smallest_available_job(jobs_list);
        
        printf("[%d] %d\n", assigned_job_id, pids[0]);

        for (int i = 0; i < MAX_JOBS; i++) {
            if (!jobs_list[i].is_active) {
                jobs_list[i].job_id = assigned_job_id;
                jobs_list[i].pid = pids[0]; // Track the group leader
                
                // Reconstruct the full pipeline text representation for display
                jobs_list[i].command[0] = '\0';
                for (int s = 0; s < n_segments; s++) {
                    for (int j = 0; segments[s]->argv[j] != NULL; j++) {
                        strncat(jobs_list[i].command, segments[s]->argv[j], BUF_SIZE - strlen(jobs_list[i].command) - 1);
                        if (segments[s]->argv[j + 1] != NULL) {
                            strncat(jobs_list[i].command, " ", BUF_SIZE - strlen(jobs_list[i].command) - 1);
                        }
                    }
                    if (s < n_segments - 1) {
                        strncat(jobs_list[i].command, " | ", BUF_SIZE - strlen(jobs_list[i].command) - 1);
                    }
                }
                
                jobs_list[i].is_active = 1;
                jobs_list[i].is_done = 0;
                break;
            }
        }
    }
    
    free(pids);
}

// Helper function to automatically display and clear completed background jobs
void flush_done_jobs(Job* list) {
    int max_id = -1;
    int second_max_id = -1;

    // Pass 1: Find the highest and second-highest active job IDs
    for (int i = 0; i < MAX_JOBS; i++) {
        if (list[i].is_active) {
            if (list[i].job_id > max_id) {
                second_max_id = max_id;
                max_id = list[i].job_id;
            } else if (list[i].job_id > second_max_id) {
                second_max_id = list[i].job_id;
            }
        }
    }

    // Pass 2: Only print and remove jobs which are DONE
    for (int i = 0; i < MAX_JOBS; i++) {
        if (list[i].is_active && list[i].is_done) {
            char marker = ' ';
            if (list[i].job_id == max_id) {
                marker = '+';
            } else if (list[i].job_id == second_max_id) {
                marker = '-';
            }

            // Print the "Done" line matching your formatting requirements
            printf("[%d]%c %-24s%s\n", list[i].job_id, marker, "Done", list[i].command);

            // Remove it from records immediately
            list[i].is_active = 0;
            list[i].is_done = 0;
        }
    }
}

// Helper function reaps finished child processes to prevent zombies
void reap_background_jobs(Job* list) {
    int status;
    pid_t reaped_pid;

    while ((reaped_pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (list[i].is_active && list[i].pid == reaped_pid && !list[i].is_done) {
                list[i].is_done = 1;
                break;
            }
        }
    }
}

int main() {
    // Initialize jobs tracking array
    Job jobs_list[MAX_JOBS];
    int next_job_id = 1;
    init_jobs_system(jobs_list);

    // Initialize completion tracking system
    CompletionSystem comp_sys;
    init_completion_system(&comp_sys);

    // Initialize readline
    rl_readline_name = "myshell";
    rl_basic_word_break_characters = " \t\n\"\'`@$><=;|&{(";

    // Set up completion function
    rl_attempted_completion_function = builtin_completion;
    rl_completion_append_character = ' ';

    // Securely pass local comp_sys stack address to Readline's engine context
    get_set_completion_context(&comp_sys);

    // Load history from HISTFILE if set
    char* histfile = getenv("HISTFILE");
    char* saved_histfile = histfile ? strdup(histfile) : NULL;
    if (saved_histfile != NULL) {
        load_history_from_file(saved_histfile);
        last_history_written_idx = history_length;
    }

    // Flush after every printf for immediate output in interactive mode
    setbuf(stdout, NULL);
    int status = 0;

    while (1) {
        reap_background_jobs(jobs_list);
        flush_done_jobs(jobs_list);

        char* input = readline("$ ");
        if (input == NULL) {
            printf("\n");
            break;
        }

        char* processedInput = input;
        while (isspace((unsigned char)*processedInput)) {
            processedInput++;
        }

        // Skip empty commands
        if (*processedInput == '\0') {
            free(input);
            continue;
        }

        // Add non-empty commands to history
        add_history(input);

        // Parse the entire line into tokens
        char** tokens = parse_arguments(processedInput);
        if (tokens == NULL) {
            free(input);
            continue;
        }

        // Check for pipeline operator
        int has_pipe = 0;
        for (int i = 0; tokens[i] != NULL; i++) {
            if (strcmp(tokens[i], "|") == 0) {
                has_pipe = 1;
                break;
            }
        }

        if (has_pipe) {
            // Split tokens into segments separated by pipes
            int n_segments;
            char*** segments = split_tokens_by_pipe(tokens, &n_segments);
            
            // Parse each segment for redirection
            ParseResult** segment_results = malloc(n_segments * sizeof(ParseResult*));
            for (int i = 0; i < n_segments; i++) {
                segment_results[i] = malloc(sizeof(ParseResult));
                segment_results[i]->redir_info = init_redirection_info();
                segment_results[i]->argv = segments[i]; // Use the segment tokens directly
                
                // Parse redirection within this segment
                int redirect_stdout_idx = -1;
                int redirect_stderr_idx = -1;
                int token_count = 0;
                while (segments[i][token_count] != NULL) token_count++;
                
                for (int j = 0; j < token_count; j++) {
                    if (strcmp(segments[i][j], ">") == 0 || strcmp(segments[i][j], "1>") == 0) {
                        redirect_stdout_idx = j;
                        segment_results[i]->redir_info->stdout_mode = 0;
                    } else if (strcmp(segments[i][j], ">>") == 0 || strcmp(segments[i][j], "1>>") == 0) {
                        redirect_stdout_idx = j;
                        segment_results[i]->redir_info->stdout_mode = 1;
                    } else if (strcmp(segments[i][j], "2>") == 0) {
                        redirect_stderr_idx = j;
                        segment_results[i]->redir_info->stderr_mode = 0;
                    } else if (strcmp(segments[i][j], "2>>") == 0) {
                        redirect_stderr_idx = j;
                        segment_results[i]->redir_info->stderr_mode = 1;
                    }
                }
                
                // Handle stdout redirection
                if (redirect_stdout_idx != -1) {
                    if (redirect_stdout_idx + 1 >= token_count) {
                        fprintf(stderr, "shell: syntax error: expected filename after stdout redirection\n");
                        continue;
                    }
                    segment_results[i]->redir_info->has_stdout_redirect = 1;
                    segment_results[i]->redir_info->stdout_file = strdup(segments[i][redirect_stdout_idx + 1]);
                    
                    // Remove redirection tokens from argv
                    segments[i][redirect_stdout_idx] = NULL;
                }
                
                // Handle stderr redirection
                if (redirect_stderr_idx != -1) {
                    if (redirect_stderr_idx + 1 >= token_count) {
                        fprintf(stderr, "shell: syntax error: expected filename after stderr redirection\n");
                        continue;
                    }
                    segment_results[i]->redir_info->has_stderr_redirect = 1;
                    segment_results[i]->redir_info->stderr_file = strdup(segments[i][redirect_stderr_idx + 1]);
                    
                    // Remove redirection tokens from argv
                    segments[i][redirect_stderr_idx] = NULL;
                }
            }

            int pipeline_is_background = 0;
            if (n_segments > 0) {
                int last_seg_argc = 0;
                while (segment_results[n_segments - 1]->argv[last_seg_argc] != NULL) {
                    last_seg_argc++;
                }

                if (last_seg_argc > 0 && strcmp(segment_results[n_segments - 1]->argv[last_seg_argc - 1], "&") == 0) {
                    pipeline_is_background = 1;
                    free(segment_results[n_segments - 1]->argv[last_seg_argc - 1]);
                    segment_results[n_segments - 1]->argv[last_seg_argc - 1] = NULL;
                }
            }
            
            // Execute the pipeline
            execute_pipeline(segment_results, n_segments, jobs_list, pipeline_is_background, &comp_sys);
            
            // Clean up
            for (int i = 0; i < n_segments; i++) {
                free_redirection_info(segment_results[i]->redir_info);
                free(segment_results[i]);
                free(segments[i]);
            }
            free(segment_results);
            free(segments);
            free_argv(tokens);
        } else {
            // Non-pipeline command
            ParseResult* parsed_result = parse_args_with_redirection(processedInput);
            if (parsed_result == NULL) {
                free(input);
                free_argv(tokens);
                continue;
            }

            if (parsed_result->argv[0] == NULL) {
                free_parse_result(parsed_result);
                free(input);
                free_argv(tokens);
                continue;
            }

            const char* command = parsed_result->argv[0];
            int saved_stdout = -1;
            int saved_stderr = -1;

            if (strcmp(command, "exit") == 0) {
                status = handle_exit_cmd(parsed_result->argv);
                free_parse_result(parsed_result);
                free_argv(tokens);
                free(input);
                break;
            } else if (
                strcmp(command, "echo") == 0 ||
                strcmp(command, "type") == 0 ||
                strcmp(command, "pwd") == 0 ||
                strcmp(command, "cd") == 0 ||
                strcmp(command, "history") == 0 ||
                strcmp(command, "jobs") == 0 ||
                strcmp(command, "complete") == 0
            ) {
                if (parsed_result->redir_info->has_stdout_redirect) {
                    saved_stdout = setup_stdout_redirection(parsed_result->redir_info->stdout_file, parsed_result->redir_info->stdout_mode);
                }
                if (parsed_result->redir_info->has_stderr_redirect) {
                    saved_stderr = setup_stderr_redirection(parsed_result->redir_info->stderr_file, parsed_result->redir_info->stderr_mode);
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
                    }
                } else if (strcmp(command, "cd") == 0) {
                    handle_cd_cmd(parsed_result->argv);
                } else if (strcmp(command, "history") == 0) {
                    handle_history_cmd(parsed_result->argv);
                } else if (strcmp(command, "jobs") == 0) {
                    reap_background_jobs(jobs_list);
                    handle_jobs_cmd(parsed_result->argv, jobs_list);
                }
                else if (strcmp(command, "complete") == 0) {
                    handle_complete_cmd(parsed_result->argv, &comp_sys);
                }

                if (saved_stdout != -1) restore_stdout(saved_stdout);
                if (saved_stderr != -1) restore_stderr(saved_stderr);
            } else {
                char* exePath = find_exe_in_path(command);
                if (exePath != NULL) {
                    execute_external_exe_with_redirection(exePath, parsed_result->argv, parsed_result->redir_info, parsed_result->is_background_process, jobs_list);
                    free(exePath);
                } else {
                    printf("%s: command not found\n", command);
                }
            }
            free_parse_result(parsed_result);
            free_argv(tokens);
        }

        free(input);
    }

    // Save HISTFILE on exit
    if (saved_histfile != NULL) {
        // Re-check if current HISTFILE value
        char* current_histfile = getenv("HISTFILE");

        // Use saved_histfile if HISTFILE is unset or changed
        if (current_histfile == NULL || strcmp(saved_histfile, current_histfile) != 0) {
            save_history_to_file(saved_histfile);
        } else {
            save_history_to_file(current_histfile);
        }

        free(saved_histfile);
    }

    return status;
}