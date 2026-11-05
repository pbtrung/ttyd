// Include the PTY implementation to exercise descriptor and error-path helpers.
#include "../src/pty.c"
#define CHECK(test)                                              \
  do {                                                           \
    if (!(test)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); \
      exit(1);                                                   \
    }                                                            \
  } while (0)
static int received;
static void on_read(pty_process* process, pty_buf_t* buf, bool eof) {
  CHECK(!eof);
  CHECK(process->paused);
  CHECK(buf->len == 2 && memcmp(buf->base, "ok", 2) == 0);
  received++;
  pty_buf_free(buf);
}
int main(void) {
  int fds[2];
  CHECK(pipe(fds) == 0);
  CHECK(fd_set_cloexec(fds[0]));
  CHECK((fcntl(fds[0], F_GETFD) & FD_CLOEXEC) != 0);
  CHECK(fd_set_cloexec(fds[0]));
  CHECK(!fd_set_cloexec(-1));
  uv_loop_t loop;
  CHECK(uv_loop_init(&loop) == 0);
  pty_process* process = process_init(NULL, &loop, NULL, NULL);
  CHECK(process->pty == -1);
  CHECK(init_pipes(process) == 0);
  CHECK(pty_write(process, pty_buf_init("x", 1)) < 0);  // No write callback on immediate failure.
  CHECK(pty_write(process, pty_buf_init(NULL, 0)) == 0);
  CHECK(uv_pipe_open(process->out, fds[0]) == 0);
  process->read_cb = on_read;
  process->paused = true;
  pty_resume(process);
  CHECK(!process->paused);
  uv_buf_t empty = uv_buf_init(malloc(16), 16);
  read_cb((uv_stream_t*)process->out, 0, &empty);  // Must free and restart, not stall/leak.
  CHECK(!process->paused);
  pty_pause(process);
  CHECK(process->paused);
  CHECK(write(fds[1], "ok", 2) == 2);
  uv_run(&loop, UV_RUN_NOWAIT);
  CHECK(received == 0);
  pty_resume(process);
  CHECK(!process->paused);
  uv_run(&loop, UV_RUN_ONCE);
  CHECK(received == 1);
  CHECK(process->paused);
  close(fds[1]);
  process_free(process);
  uv_run(&loop, UV_RUN_DEFAULT);
  CHECK(uv_loop_close(&loop) == 0);
  CHECK(pty_write(NULL, pty_buf_init("x", 1)) == UV_ESRCH);
  return 0;
}
