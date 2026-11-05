#include "utils.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test)                                              \
  do {                                                           \
    if (!(test)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); \
      exit(1);                                                   \
    }                                                            \
  } while (0)

int main(void) {
  CHECK(get_sig("TERM") == SIGTERM);
  CHECK(get_sig("SIGTERM") == SIGTERM);
  CHECK(get_sig("15") == 15);
  CHECK(get_sig("H") == 0);
  CHECK(get_sig("") == 0);
  CHECK(get_sig("15junk") == 0);
  CHECK(get_sig("999999999999999999999") == 0);
  char signal[32];
  get_sig_name(-1, signal, sizeof(signal));
  CHECK(strcmp(signal, "SIGUNKNOWN") == 0);
  get_sig_name(10000, signal, sizeof(signal));
  CHECK(strcmp(signal, "SIGUNKNOWN") == 0);
  char* arguments[] = {"program", "", "a b", "say\"hi", "C:\\some folder\\", NULL};
  char* quoted = quote_command_line(arguments);
  CHECK(strcmp(quoted, "program \"\" \"a b\" \"say\\\"hi\" \"C:\\some folder\\\\\"") == 0);
  free(quoted);
  char* long_argument = malloc(20001);
  CHECK(long_argument != NULL);
  memset(long_argument, 'x', 20000);
  long_argument[20000] = 0;
  char* long_arguments[] = {"program", long_argument, NULL};
  quoted = quote_command_line(long_arguments);
  CHECK(strlen(quoted) == 20008);
  CHECK(strcmp(quoted + 8, long_argument) == 0);
  free(quoted);
  free(long_argument);
  char* empty[] = {NULL};
  quoted = quote_command_line(empty);
  CHECK(strcmp(quoted, "") == 0);
  free(quoted);
  return 0;
}
