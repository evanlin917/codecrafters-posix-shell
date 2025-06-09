#include <stdio.h>
#include <string.h>

int main() {
  // Flush after every printf
  setbuf(stdout, NULL);

  while (1) {
    printf("$ ");

    // Wait for user input
    const size_t bufSize = 512;
    char input[bufSize];
    char* userInput = fgets(input, bufSize, stdin);

    if (userInput == NULL) {
      break;
    }

    // Remove trailing newline only if it exists
    size_t inputLen = strlen(input);
    if (inputLen > 0 && input[inputLen - 1] == '\n') {
      input[strlen(input) - 1] = '\0';
    }

    if (strcmp(input, "exit 0") == 0) {
      break;
    }
    
    printf("%s: command not found\n", input);
  }

  return 0;
}
