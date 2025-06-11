#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Helper function to handle `echo` commands
void handle_echo_cmd(const char *args) {
  if (args != NULL) {
    printf("%s\n", args);
  }
  else {
    printf("\n");
  }
}

// Helper function to handle `exit` commands
int handle_exit_cmd(const char *args) {
  return (args != NULL) ? atoi(args) : 0;
}

int main() {
  // Flush after every printf
  setbuf(stdout, NULL);
  int status = 0;

  while (1) {
    printf("$ ");

    // Wait for user input
    const size_t bufSize = 512;
    char input[bufSize];
    char* shellInput = fgets(input, bufSize, stdin);

    if (shellInput == NULL) {
      break;
    }

    // Remove trailing newline only if it exists
    size_t inputLen = strlen(input);
    if (inputLen > 0 && input[inputLen - 1] == '\n') {
      input[inputLen - 1] = '\0';
    }

    // Preserve input for error reporting
    char inputCopy[bufSize];
    strcpy(inputCopy, input);

    // Extract command and args from input given to shell
    char* args = strchr(input, ' ');
    if (args != NULL) {
      *args = '\0';
      args++;

      while (*args == ' ') {
        args++;
      }
    }

    char* command = input;
    if (command == NULL) {
      continue;
    }
    
    if (strcmp(command, "exit") == 0) {
      status = handle_exit_cmd(args);
      break;
    } else if (strcmp(command, "echo") == 0) {
      handle_echo_cmd(args);
    } else {
      printf("%s: command not found\n", inputCopy);
    }
  }

  return status;
}
