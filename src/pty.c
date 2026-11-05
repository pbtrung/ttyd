#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifndef _WIN32
#include <sys/ioctl.h>
#include <sys/wait.h>

#if defined(__OpenBSD__) || defined(__APPLE__)
#include <util.h>
#elif defined(__FreeBSD__)
#include <libutil.h>
#else
#include <pty.h>
#endif

#if defined(__APPLE__)
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char** environ;
#endif
#endif

#include "pty.h"
#include "utils.h"

#ifdef _WIN32
HRESULT(WINAPI* pCreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
HRESULT(WINAPI* pResizePseudoConsole)(HPCON, COORD);
void(WINAPI* pClosePseudoConsole)(HPCON);
#endif

static void alloc_cb(uv_handle_t* unused, size_t suggested_size, uv_buf_t* buf) {
  buf->base = xmalloc(suggested_size);
  buf->len = suggested_size;
}

static int init_pipes(pty_process* process) {
  uv_pipe_t* pipe = xmalloc(sizeof(*pipe));
  int status = uv_pipe_init(process->loop, pipe, 0);
  if (status != 0) {
    free(pipe);
    return status;
  }
  process->in = pipe;
  pipe = xmalloc(sizeof(*pipe));
  status = uv_pipe_init(process->loop, pipe, 0);
  if (status != 0) {
    free(pipe);
    return status;
  }
  process->out = pipe;
  return 0;
}

static void close_cb(uv_handle_t* handle) { free(handle); }

static void async_free_cb(uv_handle_t* handle) { free(handle->data); }

pty_buf_t* pty_buf_init(char* base, size_t len) {
  pty_buf_t* buf = xmalloc(sizeof(pty_buf_t));
  buf->base = xmalloc(len);
  if (len > 0) memcpy(buf->base, base, len);
  buf->len = len;
  return buf;
}

void pty_buf_free(pty_buf_t* buf) {
  if (buf == NULL) return;
  if (buf->base != NULL) free(buf->base);
  free(buf);
}

#ifndef _WIN32
static void finish_process(pty_process* process) {
  process->exit_cb(process);
  process_free(process);
}
#endif

static void read_cb(uv_stream_t* stream, ssize_t n, const uv_buf_t* buf) {
  uv_read_stop(stream);
  pty_process* process = (pty_process*)stream->data;
  process->paused = true;
  if (n == 0 || n == UV_ENOBUFS) {
    free(buf->base);
    pty_resume(process);
    return;
  }
  if (n < 0) {
    free(buf->base);
#ifdef _WIN32
    process->read_cb(process, NULL, true);
#else
    process->eof = true;
    if (process->exited) finish_process(process);
#endif
    return;
  }
  // Transfer the read allocation instead of copying each output chunk again.
  pty_buf_t* output = xmalloc(sizeof(*output));
  output->base = buf->base;
  output->len = (size_t)n;
  process->read_cb(process, output, false);
}

static void write_cb(uv_write_t* req, int unused) {
  pty_buf_t* buf = (pty_buf_t*)req->data;
  pty_buf_free(buf);
  free(req);
}

pty_process* process_init(void* ctx, uv_loop_t* loop, char* argv[], char* envp[]) {
  pty_process* process = xmalloc(sizeof(pty_process));
  memset(process, 0, sizeof(pty_process));
  process->ctx = ctx;
  process->loop = loop;
  process->argv = argv;
  process->envp = envp;
  process->columns = 80;
  process->rows = 24;
  process->exit_code = -1;
#ifndef _WIN32
  process->pty = -1;
#endif
  return process;
}

bool process_running(pty_process* process) {
  return process != NULL && process->pid > 0 && uv_kill(process->pid, 0) == 0;
}

void process_free(pty_process* process) {
  if (process == NULL) return;
#ifdef _WIN32
  if (process->si.lpAttributeList != NULL) {
    DeleteProcThreadAttributeList(process->si.lpAttributeList);
    free(process->si.lpAttributeList);
  }
  if (process->wait != NULL) UnregisterWaitEx(process->wait, INVALID_HANDLE_VALUE);
  if (process->pty != NULL) pClosePseudoConsole(process->pty);
  if (process->handle != NULL) CloseHandle(process->handle);
#else
  if (process->pty >= 0) close(process->pty);
  if (process->thread_started) uv_thread_join(&process->tid);
#endif
  if (process->in != NULL) uv_close((uv_handle_t*)process->in, close_cb);
  if (process->out != NULL) uv_close((uv_handle_t*)process->out, close_cb);
  if (process->argv != NULL) free(process->argv);
  if (process->cwd != NULL) free(process->cwd);
  if (process->envp != NULL) {
    for (char** p = process->envp; *p; p++) free(*p);
    free(process->envp);
  }
  if (process->async_initialized) {
    if (!uv_is_closing((uv_handle_t*)&process->async)) uv_close((uv_handle_t*)&process->async, async_free_cb);
  } else {
    free(process);
  }
}

void pty_pause(pty_process* process) {
  if (process == NULL || process->out == NULL) return;
  if (process->paused) return;
  uv_read_stop((uv_stream_t*)process->out);
  process->paused = true;
}

void pty_resume(pty_process* process) {
  if (process == NULL) return;
  if (!process->paused || process->out == NULL) return;
  process->out->data = process;
  if (uv_read_start((uv_stream_t*)process->out, alloc_cb, read_cb) == 0) process->paused = false;
}

int pty_write(pty_process* process, pty_buf_t* buf) {
  if (process == NULL) {
    pty_buf_free(buf);
    return UV_ESRCH;
  }
  if (buf->len == 0 || buf->len > UINT_MAX) {
    int status = buf->len == 0 ? 0 : UV_E2BIG;
    pty_buf_free(buf);
    return status;
  }
  uv_buf_t b = uv_buf_init(buf->base, (unsigned int)buf->len);
  uv_write_t* req = xmalloc(sizeof(uv_write_t));
  req->data = buf;
  int status = uv_write(req, (uv_stream_t*)process->in, &b, 1, write_cb);
  if (status != 0) {
    pty_buf_free(buf);
    free(req);
  }
  return status;
}

bool pty_resize(pty_process* process) {
  if (process == NULL) return false;
  if (process->columns <= 0 || process->rows <= 0) return false;
#ifdef _WIN32
  COORD size = {(int16_t)process->columns, (int16_t)process->rows};
  return pResizePseudoConsole(process->pty, size) == S_OK;
#else
  struct winsize size = {process->rows, process->columns, 0, 0};
  return ioctl(process->pty, TIOCSWINSZ, &size) == 0;
#endif
}

bool pty_kill(pty_process* process, int sig) {
  if (process == NULL) return false;
#ifdef _WIN32
  return TerminateProcess(process->handle, 1) != 0;
#else
  process->closing = true;
  if (process->exited) {
    finish_process(process);
    return true;
  }
  return uv_kill(-process->pid, sig) == 0;
#endif
}

#ifdef _WIN32
bool conpty_init() {
  uv_lib_t kernel;
  if (uv_dlopen("kernel32.dll", &kernel)) {
    uv_dlclose(&kernel);
    return false;
  }
  static struct {
    char* name;
    FARPROC* ptr;
  } conpty_entry[] = {{"CreatePseudoConsole", (FARPROC*)&pCreatePseudoConsole},
                      {"ResizePseudoConsole", (FARPROC*)&pResizePseudoConsole},
                      {"ClosePseudoConsole", (FARPROC*)&pClosePseudoConsole},
                      {NULL, NULL}};
  for (int i = 0; conpty_entry[i].name != NULL && conpty_entry[i].ptr != NULL; i++) {
    if (uv_dlsym(&kernel, conpty_entry[i].name, (void**)conpty_entry[i].ptr)) {
      uv_dlclose(&kernel);
      return false;
    }
  }
  return true;
}

static WCHAR* to_utf16(char* str) {
  int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
  if (len <= 0) return NULL;
  WCHAR* wstr = xmalloc((len + 1) * sizeof(WCHAR));
  if (len != MultiByteToWideChar(CP_UTF8, 0, str, -1, wstr, len)) {
    free(wstr);
    return NULL;
  }
  wstr[len] = L'\0';
  return wstr;
}

// convert argv to cmdline for CreateProcessW
static WCHAR* join_args(char** argv) {
  char* args = quote_command_line(argv);
  if (args == NULL) return NULL;
  WCHAR* result = to_utf16(args);
  free(args);
  return result;
}

static bool conpty_setup(HPCON* hnd, COORD size, STARTUPINFOEXW* si_ex, char** in_name, char** out_name) {
  static int count = 0;
  char buf[256];
  HPCON pty = INVALID_HANDLE_VALUE;
  SECURITY_ATTRIBUTES sa = {0};
  HANDLE in_pipe = INVALID_HANDLE_VALUE;
  HANDLE out_pipe = INVALID_HANDLE_VALUE;
  const DWORD open_mode = PIPE_ACCESS_INBOUND | PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE;
  const DWORD pipe_mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;
  DWORD pid = GetCurrentProcessId();
  bool ret = false;

  sa.nLength = sizeof(sa);

  snprintf(buf, sizeof(buf), "\\\\.\\pipe\\ttyd-term-in-%d-%d", pid, count);
  *in_name = strdup(buf);
  snprintf(buf, sizeof(buf), "\\\\.\\pipe\\ttyd-term-out-%d-%d", pid, count);
  *out_name = strdup(buf);
  in_pipe = CreateNamedPipeA(*in_name, open_mode, pipe_mode, 1, 0, 0, 30000, &sa);
  out_pipe = CreateNamedPipeA(*out_name, open_mode, pipe_mode, 1, 0, 0, 30000, &sa);
  if (in_pipe == INVALID_HANDLE_VALUE || out_pipe == INVALID_HANDLE_VALUE) {
    print_error("CreateNamedPipeA");
    goto failed;
  }

  HRESULT hr = pCreatePseudoConsole(size, in_pipe, out_pipe, 0, &pty);
  if (FAILED(hr)) {
    print_error("CreatePseudoConsole");
    goto failed;
  }

  si_ex->StartupInfo.cb = sizeof(STARTUPINFOEXW);
  si_ex->StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
  si_ex->StartupInfo.hStdError = NULL;
  si_ex->StartupInfo.hStdInput = NULL;
  si_ex->StartupInfo.hStdOutput = NULL;
  size_t bytes_required;
  InitializeProcThreadAttributeList(NULL, 1, 0, &bytes_required);
  PPROC_THREAD_ATTRIBUTE_LIST attributes = xmalloc(bytes_required);
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes_required)) {
    free(attributes);
    print_error("InitializeProcThreadAttributeList");
    goto failed;
  }
  si_ex->lpAttributeList = attributes;
  if (!UpdateProcThreadAttribute(si_ex->lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pty, sizeof(HPCON),
                                 NULL, NULL)) {
    print_error("UpdateProcThreadAttribute");
    goto failed;
  }
  count++;
  *hnd = pty;
  ret = true;
  goto done;

failed:
  if (pty != INVALID_HANDLE_VALUE) pClosePseudoConsole(pty);
  ret = false;
  free(*in_name);
  *in_name = NULL;
  free(*out_name);
  *out_name = NULL;
done:
  if (in_pipe != INVALID_HANDLE_VALUE) CloseHandle(in_pipe);
  if (out_pipe != INVALID_HANDLE_VALUE) CloseHandle(out_pipe);
  return ret;
}

static void connect_cb(uv_connect_t* req, int status) {
  if (status != 0) {
    fprintf(stderr, "connect_cb: pipe connect failed: %s\n", uv_strerror(status));
  }
  free(req);
}

static void CALLBACK conpty_exit(void* context, BOOLEAN unused) {
  pty_process* process = (pty_process*)context;
  uv_async_send(&process->async);
}

static void async_cb(uv_async_t* async) {
  pty_process* process = (pty_process*)async->data;
  UnregisterWait(process->wait);
  process->wait = NULL;

  DWORD exit_code = 1;
  GetExitCodeProcess(process->handle, &exit_code);
  process->exit_code = (int)exit_code;
  process->exit_signal = 1;
  process->exit_cb(process);

  process_free(process);
}

int pty_spawn(pty_process* process, pty_read_cb read_cb, pty_exit_cb exit_cb) {
  char* in_name = NULL;
  char* out_name = NULL;
  DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
  COORD size = {(int16_t)process->columns, (int16_t)process->rows};

  if (!conpty_setup(&process->pty, size, &process->si, &in_name, &out_name)) return UV_EIO;

  SetConsoleCtrlHandler(NULL, FALSE);

  PROCESS_INFORMATION pi = {0};
  WCHAR *cmdline = NULL, *cwd = NULL;
  int status = init_pipes(process);
  if (status != 0) goto cleanup;
  status = UV_EIO;

  uv_connect_t* in_req = xmalloc(sizeof(uv_connect_t));
  uv_connect_t* out_req = xmalloc(sizeof(uv_connect_t));
  uv_pipe_connect(in_req, process->in, in_name, connect_cb);
  uv_pipe_connect(out_req, process->out, out_name, connect_cb);

  cmdline = join_args(process->argv);
  if (cmdline == NULL) goto cleanup;
  if (process->envp != NULL) {
    char** p = process->envp;
    for (; *p; p++) {
      WCHAR* env = to_utf16(*p);
      if (env == NULL) goto cleanup;
      _wputenv(env);
      free(env);
    }
  }
  if (process->cwd != NULL) {
    cwd = to_utf16(process->cwd);
    if (cwd == NULL) goto cleanup;
  }

  if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, flags, NULL, cwd, &process->si.StartupInfo, &pi)) {
    print_error("CreateProcessW");
    DWORD exitCode = 0;
    if (GetExitCodeProcess(pi.hProcess, &exitCode)) printf("== exit code: %d\n", exitCode);
    goto cleanup;
  }

  CloseHandle(pi.hThread);
  process->pid = pi.dwProcessId;
  process->handle = pi.hProcess;
  process->paused = true;
  process->read_cb = read_cb;
  process->exit_cb = exit_cb;
  process->async.data = process;
  status = uv_async_init(process->loop, &process->async, async_cb);
  if (status != 0) goto cleanup;
  process->async_initialized = true;

  if (!RegisterWaitForSingleObject(&process->wait, pi.hProcess, conpty_exit, process, INFINITE, WT_EXECUTEONLYONCE)) {
    print_error("RegisterWaitForSingleObject");
    status = UV_EIO;
    goto cleanup;
  }

  status = 0;

cleanup:
  if (status != 0 && process->handle != NULL) {
    TerminateProcess(process->handle, 1);
    WaitForSingleObject(process->handle, INFINITE);
  }
  if (in_name != NULL) free(in_name);
  if (out_name != NULL) free(out_name);
  if (cmdline != NULL) free(cmdline);
  if (cwd != NULL) free(cwd);
  return status;
}
#else
static bool fd_set_cloexec(const int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags < 0) return false;
  return (flags & FD_CLOEXEC) != 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != -1;
}

static int fd_duplicate(int fd, uv_pipe_t* pipe) {
  int fd_dup = dup(fd);
  if (fd_dup < 0) return uv_translate_sys_error(errno);
  if (!fd_set_cloexec(fd_dup)) {
    int status = uv_translate_sys_error(errno);
    close(fd_dup);
    return status;
  }
  int status = uv_pipe_open(pipe, fd_dup);
  if (status) close(fd_dup);
  return status;
}

static void wait_cb(void* arg) {
  pty_process* process = (pty_process*)arg;

  pid_t pid;
  int stat = 0;
  do pid = waitpid(process->pid, &stat, 0);
  while (pid != process->pid && errno == EINTR);

  if (pid == process->pid && WIFEXITED(stat)) {
    process->exit_code = WEXITSTATUS(stat);
  }
  if (pid == process->pid && WIFSIGNALED(stat)) {
    int sig = WTERMSIG(stat);
    process->exit_code = 128 + sig;
    process->exit_signal = sig;
  }

  uv_async_send(&process->async);
}

static void async_cb(uv_async_t* async) {
  pty_process* process = (pty_process*)async->data;
  // A child can exit before its final output has been read. Keep the PTY and
  // context alive until EOF; explicit connection closure may discard output.
  process->exited = true;
  if (process->eof || process->closing) finish_process(process);
}

int pty_spawn(pty_process* process, pty_read_cb read_cb, pty_exit_cb exit_cb) {
  int status = 0;

  uv_disable_stdio_inheritance();

  int master, pid;
  struct winsize size = {process->rows, process->columns, 0, 0};
  pid = forkpty(&master, NULL, NULL, &size);
  if (pid < 0) {
    status = -errno;
    return status;
  } else if (pid == 0) {
    setsid();
    if (process->cwd != NULL && chdir(process->cwd) != 0) {
      perror("chdir failed");
      _exit(127);
    }
    if (process->envp != NULL) {
      char** p = process->envp;
      for (; *p; p++)
        if (putenv(*p) != 0) _exit(127);
    }
    int ret = execvp(process->argv[0], process->argv);
    if (ret < 0) {
      perror("execvp failed\n");
      _exit(-errno);
    }
  }

  int flags = fcntl(master, F_GETFL);
  if (flags == -1) {
    status = -errno;
    goto error;
  }
  if (fcntl(master, F_SETFL, flags | O_NONBLOCK) == -1) {
    status = -errno;
    goto error;
  }
  if (!fd_set_cloexec(master)) {
    status = -errno;
    goto error;
  }

  status = init_pipes(process);
  if (status != 0) goto error;

  status = fd_duplicate(master, process->in);
  if (status == 0) status = fd_duplicate(master, process->out);
  if (status != 0) goto error;

  process->pty = master;
  process->pid = pid;
  process->paused = true;
  process->read_cb = read_cb;
  process->exit_cb = exit_cb;
  process->async.data = process;
  status = uv_async_init(process->loop, &process->async, async_cb);
  if (status != 0) goto error;
  process->async_initialized = true;
  status = uv_thread_create(&process->tid, wait_cb, process);
  if (status != 0) goto error;
  process->thread_started = true;

  return 0;

error:
  close(master);
  process->pty = -1;
  uv_kill(pid, SIGKILL);
  while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
  }
  process->pid = 0;
  return status;
}
#endif
