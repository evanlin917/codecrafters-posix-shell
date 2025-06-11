#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>

#define BUF_SIZE 512

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

// Helper function handle `type` commands
void handle_type_cmd(const char* args) {
  if (args == NULL || *args == '\0') {
    printf("type: usage: type name [...]\n");
    return;
  }

  if (
    (strcmp(args, "echo") == 0) ||
    (strcmp(args, "exit") == 0) ||
    (strcmp(args, "type") == 0)
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

    if (access(fullPath, F_OK) == 0) {
      if (access(fullPath, X_OK) == 0) {
        printf("%s is %s\n", args, fullPath);
        free(pathCopy);
        return;
      }
    }

    dir = strtok(NULL, ":");
  }

  printf("%s: not found\n", args);
  free(pathCopy);
}

// Helper function to trim leading spaces
char* trim_leading_spaces(char* str) {
  while (isspace((unsigned char) *str)) {
    str++;
  }

  return str;
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
    char* args = strchr(trimmedInput, ' ');
    char* command;

    if (args != NULL) {
      *args = '\0';
      args++;

      while (*args == ' ') {
        args++;
      }
      command = trimmedInput;
    } else {
      command = trimmedInput;
    }
    
    if (strcmp(command, "exit") == 0) {
      status = handle_exit_cmd(args);
      break;
    } else if (strcmp(command, "echo") == 0) {
      handle_echo_cmd(args);
    } else if (strcmp(command, "type") == 0) {
      handle_type_cmd(args);
    } else {
      printf("%s: command not found\n", inputCopy);
    }
  }

  return status;
}
