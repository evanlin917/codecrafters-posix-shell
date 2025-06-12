#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <limits.h>

#define BUF_SIZE 512
#define MAX_ARGS 64

// Helper function to handle `echo` commands
void handle_echo_cmd(const char* args) {
  if (args != NULL) {
    printf("%s\n", args);
  } else {
    printf("\n");
  }
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
    (strcmp(args, "pwd") == 0)
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
  char* buffer;
  char* curr_dir;

  buffer = (char*)malloc(PATH_MAX);
  if (buffer == NULL) {
    fprintf(stderr, "pwd: memory allocation failed for path buffer\n");
    return NULL;
  }

  curr_dir = getcwd(buffer, PATH_MAX);
  if (curr_dir == NULL) {
    perror("pwd: getcwd failed");
    free(buffer);
    return NULL;
  }

  free(buffer);
  return curr_dir;
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

  int i = 0;
  argv[i] = command;
  if (argv[i] == NULL) {
    fprintf(stderr, "shell: given command is NULL\n");
    free(argv);
    return NULL;
  }
  i++;

  if (args != NULL && *args != '\0') {
    char* argsCopy = malloc(strlen(args) + 1);
    if (argsCopy == NULL) {
      fprintf(stderr, "shell: memory allocation failed for arguments copy\n");
      free(argv[0]);
      free(argv);
      return NULL;
    }
    strcpy(argsCopy, args);

    char* token = strtok(argsCopy, " ");
    while (token != NULL && i < MAX_ARGS - 1) {
      if (*token != '\0') {
        argv[i] = strdup(token);
        if (argv[i] == NULL) {
          fprintf(stderr, "shell: parsed argument is NULL\n");
          for (int j = 1; j < i; j++) {
            free(argv[j]);
          }

          free(argv);
          free(argsCopy);
          return NULL;
        }
        i++;
      }
      token = strtok(NULL, " ");
    }
    free(argsCopy);
  }

  argv[i] = NULL;

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
        // free(pwd);
      } else {
        printf("pwd could not retrieve current working directory\n");
      }
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
