#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define BUF_SIZE 512

// Helper function to handle `echo` commands
void handle_echo_cmd(const char *args) {
  if (args != NULL) {
    printf("%s\n", args);
  } else {
    printf("\n");
  }
}

// Helper function to handle `exit` commands
int handle_exit_cmd(const char *args) {
  return (args != NULL) ? atoi(args) : 0;
}

// Helper function handle `type` commands
void handle_type_cmd(const char *args) {
  if (args == NULL || *args == '\0') {
    printf("type: usage: type name [...]\n");
    return;
  }

  if (
    (strncmp(args, "echo", 5) == 0) ||
    (strncmp(args, "exit", 5) == 0) ||
    (strncmp(args, "type", 5) == 0)
  ) {
    printf("%s is a shell builtin\n", args);
  } else {
    printf("%s: not found\n", args);
  }
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
      break;
    }

    // Remove trailing newline only if it exists
    size_t inputLen = strlen(input);
    if (inputLen > 0 && input[inputLen - 1] == '\n') {
      input[inputLen - 1] = '\0';
    }

    // Trim leading spaces
    char *trimmedInput = trim_leading_spaces(input);
    if (*trimmedInput == '\0') {
      continue;
    }

    // Preserve input for error reporting
    char inputCopy[BUF_SIZE];
    strcpy(inputCopy, trimmedInput);

    // Extract command and args
    char* args = strchr(trimmedInput, ' ');
    if (args != NULL) {
      *args = '\0';
      args++;

      while (*args == ' ') {
        args++;
      }
    }

    char* command = trimmedInput;
    
    if (strncmp(command, "exit", 5) == 0) {
      status = handle_exit_cmd(args);
      break;
    } else if (strncmp(command, "echo", 5) == 0) {
      handle_echo_cmd(args);
    } else if (strncmp(command, "type", 5) == 0) {
      handle_type_cmd(args);
    } else {
      printf("%s: command not found\n", inputCopy);
    }
  }

  return status;
}
