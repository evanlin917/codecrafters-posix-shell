#include <stdio.h>
#include <string.h>

int main() {
  // Flush after every printf
  setbuf(stdout, NULL);

  while (1) {
    printf("$ ");

    // Wait for user input
    char input[100];
    char* userInput = fgets(input, 100, stdin);

    if (userInput == NULL) {
      break;
    }

    // Remove trailing newline only if it exists
    if (input[strlen(input) - 1] == '\n') {
      input[strlen(input) - 1] = '\0';
    }
    
    printf("%s: command not found\n", input);
  }

  return 0;
}
