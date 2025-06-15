#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <limits.h>

#define BUF_SIZE 512
#define MAX_ARGS 64

typedef struct {
  int in_single_quote;
  int in_double_quote;
  int escape_next;
  int pos;
} ParseState;

typedef struct {
  char processed_char;
  int should_add_char;
  int should_break_arg;
  int advance_pos;
} CharResult;

typedef struct {
  char* buffer;
  size_t capacity;
  size_t length;
} ArgBuffer;

// Helper function to initialize parsing state
ParseState init_parse_state() {
  ParseState state = {0};
  return state;
}

// Helper function to initialize argument buffer
ArgBuffer* init_arg_buffer() {
  ArgBuffer* buf = malloc(sizeof(ArgBuffer));
  if (!buf) {
    return NULL;
  }

  buf->capacity = 64;
  buf->length = 0;
  buf->buffer = malloc(buf->capacity);
  if (!buf->buffer) {
    free(buf);
    return NULL;
  }
  buf->buffer[0] = '\0';

  return buf;
}

// Helper function to process escape sequences
char process_escape_sequence(char escaped_char, ParseState* state) {
  switch (escaped_char) {
    case 'n':
      return '\n';
    case 't':
      return "\t";
    case '\\':
      return '\\';
    case '"':
      return '"';
    case '\'':
      return '\'';
    case ' ':
      return ' ';
    default:
      return escaped_char;           
  }
}

// Helper function to handle quote state transitions
void handle_quote_transition(char current_char, ParseState* state) {
  if (state->escape_next) {
    return;
  }

  if (current_char =='\'' && !state->in_double_quote) {
    state->in_single_quote = !state->in_single_quote;
  } else if (current_char == '"' && !state->in_single_quote) {
    state->in_double_quote = !state->in_double_quote;
  }
}

// Helper argument to determine if current character should break current argument
int should_break_argument(char current_char, ParseState* state) {
  if (state->in_single_quote || state->in_double_quote || state->escape_next) {
    return 0;
  }

  return (current_char == ' ');
}

// Helper function to process single character and return result
CharResult process_character(const char* input, ParseState* state) {
  CharResult result = {0};
  char current_char = input[state->pos];

  if (state->escape_next == 1) {
    result.processed_char = process_escape_sequence(current_char, state);
    result.should_add_char = 1;
    result.advance_pos = 1;
    state->escape_next = 0;
    return result;
  }

  if (current_char == '\\' && !state->in_single_quote) {
    state->escape_next = 1;
    result.advance_pos = 1;
    return result;
  }

  handle_quote_transition(current_char, state);
  result.should_break_arg = should_break_argument(current_char, state);

  if (!result.should_break_arg && 
      !((current_char == '\'' || current_char =='"') && !state->escape_next)) {
      result.processed_char = current_char;
      result.should_add_char = 1;
  }

  result.advance_pos = 1;
  return result;
}

// Helper function to add character to argument buffer with dynamic resizing
int add_char_to_buffer(ArgBuffer* buf, char c) {
  if (buf->length + 1 >= buf->capacity) {
    size_t new_capacity = buf->capacity * 2;
    char* new_buffer = realloc(buf->buffer, new_capacity);
    if (!new_buffer) {
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

// Helper function to cleanup argv on error
void cleanup_argv_on_error(char** argv, int argc) {
  for (int i = 0; i < argc; i++) {
    if (argv[i] != NULL) {
      free(argv[i]);
    }
  }

  free(argv);
}

// Helper function to handle `echo` commands
void handle_echo_cmd(const char* args) {
  if (args == NULL) {
    printf("\n");
    return;
  }

  ParseState state = init_parse_state();
  int input_len = strlen(args);
  int first_arg = 1;
  ArgBuffer* current_arg = init_arg_buffer();

  if (current_arg == NULL) {
    printf("\n");
    return;
  }

  while (state.pos < input_len && args[state.pos] == ' ') {
    state.pos++;
  }

  while (state.pos < input_len) {
    CharResult result = process_character(args, &state);

    if (result.should_add_char == 1) {
      if (add_char_to_buffer(current_arg, result.processed_char) < 0) {
        free_arg_buffer(current_arg);
        printf("\n");
        return;
      }
    }

    if (result.should_break_arg == 1 && current_arg->length > 0) {
      if (!first_arg) {
        printf(" ");
      }
      printf("%s", current_arg->buffer);
      first_arg = 0;

      current_arg->length = 0;
      current_arg->buffer[0] = '\0';

      while (state.pos + result.advance_pos < input_len && 
             args[state.pos + result.advance_pos] == ' ') {
        result.advance_pos++;
      }
    }
    
    state.pos += result.advance_pos;
  }

  if (current_arg->length > 0) {
    if (!first_arg) {
      printf(" ");
    }
    printf("%s", current_arg->buffer);
  }

  printf("\n");
  free_arg_buffer(current_arg);
}

// Helper function to handle `exit` commands
int handle_exit_cmd(const char* args) {
  return (args != NULL) ? atoi(args) : 0;
}

// Helper function to handle `type` commands
void handle_type_cmd(const char* args) {
  if (args == NULL || *args == '\0') {
    printf("type: usage: type name [...]\n");
    return;
  }

  if (
    (strcmp(args, "echo") == 0) ||
    (strcmp(args, "exit") == 0) ||
    (strcmp(args, "type") == 0) ||
    (strcmp(args, "pwd") == 0) ||
    (strcmp(args, "cd") == 0)
  ) {
    printf("%s is a shell builtin\n", args);
    return;
  }

  char* pathEnv = getenv("PATH");
  if (pathEnv == NULL) {
    printf("%s: not found\n", args);
    return;
  }
  
  char* pathCopy = malloc(strlen(pathEnv) + 1);
  if (pathCopy == NULL) {
    fprintf(stderr, "type: memory allocation failed for PATH processing\n");
    printf("%s: not found\n", args);
    return;
  }
  strcpy(pathCopy, pathEnv);

  char* dir = strtok(pathCopy, ":");
  while (dir != NULL) {
    char fullPath[BUF_SIZE];
    int ret = snprintf(fullPath, sizeof(fullPath), "%s/%s", dir, args);

    if (ret >= sizeof(fullPath)) {
      dir = strtok(NULL, ":");
      continue;
    }

    if (access(fullPath, F_OK) == 0 && access(fullPath, X_OK) == 0) {
      printf("%s is %s\n", args, fullPath);
      free(pathCopy);
      return; 
    }

    dir = strtok(NULL, ":");
  }

  printf("%s: not found\n", args);
  free(pathCopy);
}

// Helper function to handle `pwd` commands
char* handle_pwd_cmd() {
  char* buffer = (char*)malloc(PATH_MAX);
  if (buffer == NULL) {
    fprintf(stderr, "pwd: memory allocation failed for path buffer\n");
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
void handle_cd_cmd(const char* path) {
  const char* target_path;

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

    char expanded_path[PATH_MAX];
    snprintf(expanded_path, sizeof(expanded_path), "%s%s", home, path + 1);
    target_path = expanded_path;
  } else {
    target_path = path;
  }

  if (chdir(target_path) != 0) {
    printf("cd: %s: No such file or directory\n", target_path);
  }
}

// Helper function to trim leading spaces
char* trim_leading_spaces(char* str) {
  while (isspace((unsigned char) *str)) {
    str++;
  }

  return str;
}

// Helper function to find if executable exists in PATH
char* find_exe_in_path(const char* exe) {
  char* pathEnv = getenv("PATH");
  if (pathEnv == NULL) {
    return NULL;
  }

  char* pathCopy = malloc(strlen(pathEnv) + 1);
  if (pathCopy == NULL) {
    fprintf(stderr, "shell: memory allocation failed for PATH processing\n");
    return NULL;
  }
  strcpy(pathCopy, pathEnv);

  char* dir = strtok(pathCopy, ":");
  while (dir != NULL) {
    char fullPath[BUF_SIZE];
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

  if (pid == 0) {
    execv(exePath, argv);
    perror("execv failed");
    exit(1);
  } else if (pid > 0) {
    int status;
    wait(&status);
  } else {
    perror("fork failed");
  }
}

// Helper function to extract list of executable inputs
char** split_args(const char* command, const char* args) {
  char** argv = malloc(MAX_ARGS * sizeof(char*));
  if (argv == NULL) {
    fprintf(stderr, "shell: memory allocation failed for argument parsing\n");
    return NULL;
  }

  int argc = 0;
  argv[argc] = strdup(command);
  if (argv[argc] == NULL) {
    fprintf(stderr, "shell: given command is NULL\n");
    free(argv);
    return NULL;
  }
  argc++;

  if (args != NULL && *args != '\0') {
    ParseState state = init_parse_state();
    ArgBuffer* current_arg = init_arg_buffer();

    if (current_arg == NULL) {
      cleanup_argv_on_error(argv, argc);
      return NULL;
    }

    int input_len = strlen(args);

    while (state.pos < input_len && args[state.pos] == ' ') {
      state.pos++;
    }

    while (state.pos < input_len && argc < MAX_ARGS - 1) {
      CharResult result = process_character(args, &state);

      if (result.should_add_char == 1) {
        if (add_char_to_buffer(current_arg, result.processed_char) < 0) {
          free_arg_buffer(current_arg);
          cleanup_argv_on_error(argv, argc);
          return NULL;
        }
      }

      if (result.should_break_arg == 1 && current_arg->length > 0) {
        argv[argc] = strdup(current_arg->buffer);
        if (argv[argc] == NULL) {
          free_arg_buffer(current_arg);
          cleanup_argv_on_error(argv, argc);
          return NULL;
        }

        argc++;
        
        current_arg->length = 0;
        current_arg->buffer[0] = '\0';

        while (state.pos + result.advance_pos < input_len && 
               args[state.pos + result.advance_pos] == ' ') {
          result.advance_pos++;
        }
      }

      state.pos += result.advance_pos;
    }

    if (current_arg ->length > 0 && argc < MAX_ARGS - 1) {
      argv[argc] = strdup(current_arg->buffer);
      if (argv[argc] != NULL) {
        argc++;
      }
    }

    free_arg_buffer(current_arg);

    if (state.in_single_quote == 1 || state.in_double_quote == 1) {
      fprintf(stderr, "shell: unterminated quote in command line\n");
    }
  }

  argv[argc] = NULL;

  return argv;
}

// Helper function to free arguments given as executable input
void free_argv(char** argv) {
  if (argv == NULL) {
    return;
  }

  for (int i = 1; argv[i] != NULL; i++) {
    free(argv[i]);
  }

  free(argv);
}

int main() {
  // Flush after every printf
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

    // Remove trailing newline only if it exists
    size_t inputLen = strlen(input);
    if (inputLen > 0 && input[inputLen - 1] == '\n') {
      input[inputLen - 1] = '\0';
    }

    // Trim leading spaces
    char* trimmedInput = trim_leading_spaces(input);
    if (*trimmedInput == '\0') {
      continue;
    }

    // Preserve input for error reporting
    char inputCopy[BUF_SIZE];
    strncpy(inputCopy, trimmedInput, BUF_SIZE - 1);
    inputCopy[BUF_SIZE - 1] = '\0';

    // Extract command and args
    char* args = strchr(inputCopy, ' ');
    char* command;

    if (args != NULL) {
      *args = '\0';
      args++;

      while (*args == ' ') {
        args++;
      }

      if (*args == '\0') {
        args = NULL;
      }
      command = inputCopy;
    } else {
      command = inputCopy;
      args = NULL;
    }
    
    char* exePath = find_exe_in_path(command);

    if (strcmp(command, "exit") == 0) {
      status = handle_exit_cmd(args);
      break;
    } else if (strcmp(command, "echo") == 0) {
      handle_echo_cmd(args);
    } else if (strcmp(command, "type") == 0) {
      handle_type_cmd(args);
    } else if (strcmp(command, "pwd") == 0) {
      char* pwd = handle_pwd_cmd();
      if (pwd != NULL) {
        printf("%s\n", pwd);
        free(pwd);
      } else {
        printf("pwd could not retrieve current working directory\n");
      }
    } else if (strcmp(command, "cd") == 0) {
      handle_cd_cmd(args);
    } else {
      char* exePath = find_exe_in_path(command);
      if (exePath != NULL) {
        char** argv = split_args(command, args);
        if (argv != NULL) {
          execute_external_exe(exePath, argv);
          free_argv(argv);
        } else {
          printf("Memory allocation failed\n");
        }
        free(exePath);
      } else {
        printf("%s: command not found\n", inputCopy);
      }
    }
  }

  return status;
}
