#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"

#if !defined(_WIN32) && !defined(__CYGWIN__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__linux__) && !defined(__ANDROID__)
const char* sys_signame[NSIG] = {"zero", "HUP",   "INT",  "QUIT", "ILL",  "TRAP", "ABRT", "UNUSED", "FPE",
                                 "KILL", "USR1",  "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD",
                                 "CONT", "STOP",  "TSTP", "TTIN", "TTOU", "URG",  "XCPU", "XFSZ",   "VTALRM",
                                 "PROF", "WINCH", "IO",   "PWR",  "SYS",  NULL};
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#undef NSIG
#define NSIG 33
const char* sys_signame[NSIG] = {"zero", "HUP",   "INT",  "QUIT", "ILL",  "TRAP", "IOT",  "EMT",  "FPE",
                                 "KILL", "BUS",   "SEGV", "SYS",  "PIPE", "ALRM", "TERM", "URG",  "STOP",
                                 "TSTP", "CONT",  "CHLD", "TTIN", "TTOU", "IO",   "XCPU", "XFSZ", "VTALRM",
                                 "PROF", "WINCH", "PWR",  "USR1", "USR2", NULL};
#endif

void* xmalloc(size_t size) {
  if (size == 0) return NULL;
  void* p = malloc(size);
  if (!p) abort();
  return p;
}

void* xrealloc(void* p, size_t size) {
  if ((size == 0) && (p == NULL)) return NULL;
  p = realloc(p, size);
  if (!p) abort();
  return p;
}

char* uppercase(char* s) {
  while (*s) {
    *s = (char)toupper((unsigned char)*s);
    s++;
  }
  return s;
}

char* lowercase(char* s) {
  while (*s) {
    *s = (char)tolower((unsigned char)*s);
    s++;
  }
  return s;
}

bool endswith(const char* str, const char* suffix) {
  size_t str_len = strlen(str);
  size_t suffix_len = strlen(suffix);
  return str_len > suffix_len && !strcmp(str + (str_len - suffix_len), suffix);
}

int get_sig_name(int sig, char* buf, size_t len) {
  const char* name = sig >= 0 && sig < NSIG && sys_signame[sig] != NULL ? sys_signame[sig] : "unknown";
  int n = snprintf(buf, len, "SIG%s", name);
  uppercase(buf);
  return n;
}

int get_sig(const char* sig_name) {
  const char* short_name = strncasecmp(sig_name, "SIG", 3) == 0 ? sig_name + 3 : sig_name;
  for (int sig = 1; sig < NSIG; sig++) {
    const char* name = sys_signame[sig];
    if (name != NULL && (strcasecmp(name, sig_name) == 0 || strcasecmp(name, short_name) == 0)) return sig;
  }
  char* end = NULL;
  errno = 0;
  long value = strtol(sig_name, &end, 10);
  return errno == 0 && end != sig_name && *end == '\0' && value > 0 && value < NSIG ? (int)value : 0;
}

int open_uri(char* uri) {
#if defined(_WIN32) || defined(__CYGWIN__)
  return ShellExecute(0, 0, uri, 0, 0, SW_SHOW) > (HINSTANCE)32 ? 0 : 1;
#else
#ifndef __APPLE__
  // check if X server is running
  if (system("xset -q > /dev/null 2>&1")) return 1;
#endif

  pid_t pid = fork();
  if (pid < 0) return 1;

  if (pid == 0) {
    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      close(fd);
    }

#ifdef __APPLE__
    char* args[] = {"open", uri, NULL};
#else
    char* args[] = {"xdg-open", uri, NULL};
#endif

    execvp(args[0], args);
    _exit(1);
  }

  int status;
  if (waitpid(pid, &status, 0) < 0) return 1;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
#endif
}

#ifdef _WIN32
char* strsep(char** sp, char* sep) {
  char *p, *s;
  if (sp == NULL || *sp == NULL || **sp == '\0') return (NULL);
  s = *sp;
  p = s + strcspn(s, sep);
  if (*p != '\0') *p++ = '\0';
  *sp = p;
  return s;
}

#endif

static char* quote_arg(const char* arg) {
  int len = 0, n = 0;
  int force_quotes = 0;
  char *q, *d;
  const char* p = arg;
  if (!*p) force_quotes = 1;
  while (*p) {
    if (isspace((unsigned char)*p) || *p == '*' || *p == '?' || *p == '{' || *p == '\'')
      force_quotes = 1;
    else if (*p == '"')
      n++;
    else if (*p == '\\') {
      int count = 0;
      while (*p == '\\') {
        count++;
        p++;
        len++;
      }
      if (*p == '"' || !*p) n += count * 2 + 1;
      continue;
    }
    len++;
    p++;
  }
  if (!force_quotes && n == 0) {
    char* copy = xmalloc((size_t)len + 1);
    strcpy(copy, arg);
    return copy;
  }

  d = q = xmalloc((size_t)len + (size_t)n + 3);
  *d++ = '"';
  while (*arg) {
    if (*arg == '"')
      *d++ = '\\';
    else if (*arg == '\\') {
      int count = 0;
      while (*arg == '\\') {
        count++;
        *d++ = *arg++;
      }
      if (*arg == '"' || !*arg) {
        while (count-- > 0) *d++ = '\\';
        if (!*arg) break;
        *d++ = '\\';
      }
    }
    *d++ = *arg++;
  }
  *d++ = '"';
  *d++ = '\0';
  return q;
}

char* quote_command_line(char* argv[]) {
  size_t capacity = 1;
  for (char** arg = argv; *arg; arg++) {
    size_t len = strlen(*arg);
    if (len > INT_MAX / 2 - 4 || capacity > SIZE_MAX - 4 || len > (SIZE_MAX - capacity - 4) / 2) {
      errno = EOVERFLOW;
      return NULL;
    }
    capacity += len * 2 + 4;
  }
  char* result = xmalloc(capacity);
  size_t used = 0;
  for (char** arg = argv; *arg; arg++) {
    char* quoted = quote_arg(*arg);
    size_t len = strlen(quoted);
    if (used > 0) result[used++] = ' ';
    memcpy(result + used, quoted, len);
    used += len;
    free(quoted);
  }
  result[used] = '\0';
  return result;
}

#ifdef _WIN32
void print_error(char* func) {
  char* buffer = NULL;
  DWORD error = GetLastError();
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                 error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&buffer, 0, NULL);
  fprintf(stderr, "%s failed with error %lu: %s\n", func, (unsigned long)error, buffer ? buffer : "Unknown error");
  LocalFree(buffer);
}
#endif
