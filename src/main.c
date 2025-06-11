#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>

#define BUF_SIZE 512
#define PATH_LENGTH 1024

typedef struct {
  int found;
  char path[PATH_LENGTH];
} CommandResult;

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

// Helper function to determine if command is found in PATH
CommandResult find_cmd_in_path(const char* cmd) {
  CommandResult result = {0, ""};

  char* path_env = getenv("PATH");
  if (!path_env) {
    return result;
  }

  char* paths = strdup(path_env);
  if (!paths) {
    perror("strdup failed");
    return result;
  }

  char* dir = strtok(paths, ":");
  while (dir != NULL) {
    snprintf(result.path, sizeof(result.path), "%s/%s", dir, cmd);

    if (access(result.path, X_OK) == 0) {
      result.found = 1;
      break;
    }
    dir = strtok(NULL, ":");
  }

  free(paths);
  return result;
}

// Helper function handle `type` commands
void handle_type_cmd(const char *args) {
  if (args == NULL || *args == '\0') {
    printf("type: usage: type name [...]\n");
    return;
  }

  char* args_copy = strdup(args);
  if (!args_copy) {
    perror("strdup failed");
    return;
  }

  char* cmd_name = strtok(args_copy, " ");

  while (cmd_name != NULL) {
    if (
      (strcmp(cmd_name, "echo") == 0) ||
      (strcmp(cmd_name, "exit") == 0) ||
      (strcmp(cmd_name, "type") == 0)
    ) {
      printf("%s is a shell builtin\n", args);
    } else {
      CommandResult result = find_cmd_in_path(cmd_name);
      if (result.found) {
        printf("%s is %s\n", cmd_name, result.path);
      } else {
        printf("%s: not found\n", cmd_name);
      }
    }
    cmd_name = strtok(NULL, " ");
  }
  free(args_copy);
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
