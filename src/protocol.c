#include <ctype.h>
#include <errno.h>
#include <json.h>
#include <libwebsockets.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "pty.h"
#include "server.h"
#include "utils.h"

// initial message list
static char initial_cmds[] = {SET_WINDOW_TITLE, SET_PREFERENCES};

static int send_initial_message(struct lws* wsi, int index) {
  char hostname[128] = "unknown";
  char cmd = initial_cmds[index];
  if (cmd == SET_WINDOW_TITLE) {
    gethostname(hostname, sizeof(hostname) - 1);
    hostname[sizeof(hostname) - 1] = '\0';
  }
  bool title = cmd == SET_WINDOW_TITLE;
  const char* value = title ? server->command : server->prefs_json;
  int n = snprintf(NULL, 0, "%c%s%s%s%s", cmd, value, title ? " (" : "", title ? hostname : "", title ? ")" : "");
  if (n < 0) return -1;
  unsigned char* message = xmalloc(LWS_PRE + (size_t)n + 1);
  unsigned char* payload = message + LWS_PRE;
  snprintf((char*)payload, (size_t)n + 1, "%c%s%s%s%s", cmd, value, title ? " (" : "", title ? hostname : "",
           title ? ")" : "");
  int written = lws_write(wsi, payload, (size_t)n, LWS_WRITE_BINARY);
  free(message);
  return written == n ? written : -1;
}

static json_object* parse_window_size(const char* buf, size_t len, uint16_t* cols, uint16_t* rows) {
  if (len > INT_MAX) return NULL;
  json_tokener* tok = json_tokener_new();
  if (tok == NULL) return NULL;
  json_object* obj = json_tokener_parse_ex(tok, buf, (int)len);
  bool valid = json_tokener_get_error(tok) == json_tokener_success && json_object_is_type(obj, json_type_object);
  size_t end = json_tokener_get_parse_end(tok);
  while (end < len && isspace((unsigned char)buf[end])) end++;
  valid = valid && end == len;
  json_tokener_free(tok);
  uint16_t values[] = {*cols, *rows};
  const char* keys[] = {"columns", "rows"};
#ifdef _WIN32
  const int64_t maximum = INT16_MAX;
#else
  const int64_t maximum = UINT16_MAX;
#endif
  for (int i = 0; valid && i < 2; i++) {
    json_object* value = NULL;
    if (!json_object_object_get_ex(obj, keys[i], &value)) continue;
    int64_t number = json_object_get_int64(value);
    valid = json_object_is_type(value, json_type_int) && number > 0 && number <= maximum;
    if (valid) values[i] = (uint16_t)number;
  }
  if (!valid) {
    json_object_put(obj);
    return NULL;
  }
  *cols = values[0];
  *rows = values[1];
  return obj;
}

static bool check_host_origin(struct lws* wsi) {
  char buf[256];
  memset(buf, 0, sizeof(buf));
  int len = lws_hdr_copy(wsi, buf, (int)sizeof(buf), WSI_TOKEN_ORIGIN);
  if (len <= 0) return false;

  char origin_host[256];
  const char *prot, *address, *path;
  int port;
  if (lws_parse_uri(buf, &prot, &address, &port, &path)) return false;
  if (port == 80 || port == 443) {
    snprintf(origin_host, sizeof(origin_host), "%s", address);
  } else {
    snprintf(origin_host, sizeof(origin_host), "%s:%d", address, port);
  }

  char host_buf[256];
  memset(host_buf, 0, sizeof(host_buf));
  len = lws_hdr_copy(wsi, host_buf, (int)sizeof(host_buf), WSI_TOKEN_HOST);

  return len > 0 && strcasecmp(origin_host, host_buf) == 0;
}

static pty_ctx_t* pty_ctx_init(struct pss_tty* pss) {
  pty_ctx_t* ctx = xmalloc(sizeof(pty_ctx_t));
  ctx->pss = pss;
  ctx->ws_closed = false;
  return ctx;
}

static void pty_ctx_free(pty_ctx_t* ctx) { free(ctx); }

static void process_read_cb(pty_process* process, pty_buf_t* buf, bool eof) {
  pty_ctx_t* ctx = (pty_ctx_t*)process->ctx;
  if (ctx->ws_closed) {
    pty_buf_free(buf);
    return;
  }

  if (eof && !process_running(process))
    ctx->pss->lws_close_status = process->exit_code == 0 ? 1000 : 1006;
  else
    ctx->pss->pty_buf = buf;
  lws_callback_on_writable(ctx->pss->wsi);
}

static void process_exit_cb(pty_process* process) {
  pty_ctx_t* ctx = (pty_ctx_t*)process->ctx;
  if (ctx->ws_closed) {
    lwsl_notice("process killed with signal %d, pid: %d\n", process->exit_signal, process->pid);
    goto done;
  }

  lwsl_notice("process exited with code %d, pid: %d\n", process->exit_code, process->pid);
  ctx->pss->process = NULL;
  ctx->pss->lws_close_status = process->exit_code == 0 ? 1000 : 1006;
  lws_callback_on_writable(ctx->pss->wsi);

done:
  pty_ctx_free(ctx);

  // if we are going to exit, do it now.
  if (force_exit) exit(0);
}

static char** build_args(struct pss_tty* pss) {
  int i, n = 0;
  char** argv = xmalloc((server->argc + pss->argc + 1) * sizeof(char*));

  for (i = 0; i < server->argc; i++) {
    argv[n++] = server->argv[i];
  }

  for (i = 0; i < pss->argc; i++) {
    argv[n++] = pss->args[i];
  }

  argv[n] = NULL;

  return argv;
}

static char** build_env(struct pss_tty* pss) {
  int i = 0, n = 2;
  char** envp = xmalloc(n * sizeof(char*));

  // TERM
  envp[i] = xmalloc(36);
  snprintf(envp[i], 36, "TERM=%s", server->terminal_type);
  i++;

  // TTYD_USER
  if (strlen(pss->user) > 0) {
    envp = xrealloc(envp, (++n) * sizeof(char*));
    envp[i] = xmalloc(40);
    snprintf(envp[i], 40, "TTYD_USER=%s", pss->user);
    i++;
  }

  envp[i] = NULL;

  return envp;
}

static bool spawn_process(struct pss_tty* pss, uint16_t columns, uint16_t rows) {
  pty_process* process = process_init((void*)pty_ctx_init(pss), server->loop, build_args(pss), build_env(pss));
  if (server->cwd != NULL) process->cwd = strdup(server->cwd);
  if (columns > 0) process->columns = columns;
  if (rows > 0) process->rows = rows;
  int status = pty_spawn(process, process_read_cb, process_exit_cb);
  if (status != 0) {
    lwsl_err("pty_spawn: %s (%s)\n", uv_err_name(status), uv_strerror(status));
    pty_ctx_free(process->ctx);
    process_free(process);
    return false;
  }
  lwsl_notice("started process, pid: %d\n", process->pid);
  pss->process = process;
  lws_callback_on_writable(pss->wsi);

  return true;
}

static int wsi_output(struct lws* wsi, pty_buf_t* buf) {
  if (buf == NULL) return 0;
  char* message = xmalloc(LWS_PRE + 1 + buf->len);
  char* ptr = message + LWS_PRE;

  *ptr = OUTPUT;
  memcpy(ptr + 1, buf->base, buf->len);
  size_t n = buf->len + 1;

  int written = lws_write(wsi, (unsigned char*)ptr, n, LWS_WRITE_BINARY);
  free(message);
  if (written < 0 || (size_t)written != n) {
    lwsl_err("write OUTPUT to WS failed\n");
    return -1;
  }
  return 0;
}

static bool check_auth(struct lws* wsi, struct pss_tty* pss) {
  if (server->auth_header != NULL) {
    return lws_hdr_custom_copy(wsi, pss->user, sizeof(pss->user), server->auth_header, strlen(server->auth_header)) > 0;
  }

  if (server->credential != NULL) {
    char buf[256];
    int n = lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_HTTP_AUTHORIZATION);
    return n >= 7 && strncasecmp(buf, "Basic ", 6) == 0 && !strcmp(buf + 6, server->credential);
  }

  return true;
}

int callback_tty(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len) {
  struct pss_tty* pss = (struct pss_tty*)user;
  char buf[256];
  size_t n = 0;

  switch (reason) {
    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
      if (server->once && server->client_count > 0) {
        lwsl_warn("refuse to serve WS client due to the --once option.\n");
        return 1;
      }
      if (server->max_clients > 0 && server->client_count == server->max_clients) {
        lwsl_warn("refuse to serve WS client due to the --max-clients option.\n");
        return 1;
      }
      if (!check_auth(wsi, pss)) return 1;

      int path_len = lws_hdr_copy(wsi, pss->path, sizeof(pss->path), WSI_TOKEN_GET_URI);
#if defined(LWS_ROLE_H2)
      if (path_len <= 0) path_len = lws_hdr_copy(wsi, pss->path, sizeof(pss->path), WSI_TOKEN_HTTP_COLON_PATH);
#endif
      if (path_len <= 0 || strcmp(pss->path, endpoints.ws) != 0) {
        lwsl_warn("refuse to serve WS client for illegal ws path: %s\n", pss->path);
        return 1;
      }

      if (server->check_origin && !check_host_origin(wsi)) {
        lwsl_warn(
            "refuse to serve WS client from different origin due to the "
            "--check-origin option.\n");
        return 1;
      }
      break;

    case LWS_CALLBACK_ESTABLISHED:
      pss->initialized = false;
      pss->authenticated = false;
      pss->wsi = wsi;
      pss->lws_close_status = LWS_CLOSE_STATUS_NOSTATUS;

      if (server->url_arg) {
        while (lws_hdr_copy_fragment(wsi, buf, sizeof(buf), WSI_TOKEN_HTTP_URI_ARGS, n++) > 0) {
          if (strncmp(buf, "arg=", 4) == 0) {
            pss->args = xrealloc(pss->args, (pss->argc + 1) * sizeof(char*));
            pss->args[pss->argc] = strdup(&buf[4]);
            pss->argc++;
          }
        }
      }

      server->client_count++;

      lws_get_peer_simple(lws_get_network_wsi(wsi), pss->address, sizeof(pss->address));
      lwsl_notice("WS   %s - %s, clients: %d\n", pss->path, pss->address, server->client_count);
      break;

    case LWS_CALLBACK_SERVER_WRITEABLE:
      if (!pss->initialized) {
        if (pss->initial_cmd_index == sizeof(initial_cmds)) {
          pss->initialized = true;
          if (!pss->flow_paused) pty_resume(pss->process);
          lws_callback_on_writable(wsi);
          break;
        }
        if (send_initial_message(wsi, pss->initial_cmd_index) < 0) {
          lwsl_err("failed to send initial message, index: %d\n", pss->initial_cmd_index);
          lws_close_reason(wsi, LWS_CLOSE_STATUS_UNEXPECTED_CONDITION, NULL, 0);
          return -1;
        }
        pss->initial_cmd_index++;
        lws_callback_on_writable(wsi);
        break;
      }

      if (pss->lws_close_status > LWS_CLOSE_STATUS_NOSTATUS) {
        lws_close_reason(wsi, pss->lws_close_status, NULL, 0);
        return 1;
      }

      if (pss->pty_buf != NULL) {
        int status = wsi_output(wsi, pss->pty_buf);
        pty_buf_free(pss->pty_buf);
        pss->pty_buf = NULL;
        if (status < 0) return -1;
        if (!pss->flow_paused) pty_resume(pss->process);
      }
      break;

    case LWS_CALLBACK_RECEIVE:
      if (len > MAX_WS_MESSAGE_SIZE - pss->len) {
        lws_close_reason(wsi, LWS_CLOSE_STATUS_MESSAGE_TOO_LARGE, NULL, 0);
        return -1;
      }
      size_t needed = pss->len + len;
      if (needed > pss->capacity) {
        size_t capacity = pss->capacity ? pss->capacity : 1024;
        while (capacity < needed) capacity *= 2;
        pss->buffer = xrealloc(pss->buffer, capacity);
        pss->capacity = capacity;
      }
      if (len > 0) memcpy(pss->buffer + pss->len, in, len);
      pss->len = needed;
      if (lws_remaining_packet_payload(wsi) > 0 || !lws_is_final_fragment(wsi)) return 0;
      if (pss->len == 0) {
        lws_close_reason(wsi, LWS_CLOSE_STATUS_PROTOCOL_ERR, NULL, 0);
        return -1;
      }
      const char command = pss->buffer[0];
      if ((server->credential != NULL && !pss->authenticated && command != JSON_DATA) ||
          (command != JSON_DATA && pss->process == NULL)) {
        lws_close_reason(wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION, NULL, 0);
        return -1;
      }
      switch (command) {
        case INPUT:
          if (!server->writable || pss->len == 1) break;
          int err = pty_write(pss->process, pty_buf_init(pss->buffer + 1, pss->len - 1));
          if (err) {
            lwsl_err("uv_write: %s (%s)\n", uv_err_name(err), uv_strerror(err));
            return -1;
          }
          break;
        case RESIZE_TERMINAL:
          if (pss->process == NULL) break;
          json_object* size =
              parse_window_size(pss->buffer + 1, pss->len - 1, &pss->process->columns, &pss->process->rows);
          if (size == NULL) return -1;
          json_object_put(size);
          pty_resize(pss->process);
          break;
        case PAUSE:
          pss->flow_paused = true;
          pty_pause(pss->process);
          break;
        case RESUME:
          pss->flow_paused = false;
          if (pss->initialized && pss->pty_buf == NULL) pty_resume(pss->process);
          break;
        case JSON_DATA:
          if (pss->process != NULL) break;
          uint16_t columns = 0;
          uint16_t rows = 0;
          json_object* obj = parse_window_size(pss->buffer, pss->len, &columns, &rows);
          if (obj == NULL) return -1;
          if (server->credential != NULL) {
            struct json_object* o = NULL;
            if (json_object_object_get_ex(obj, "AuthToken", &o)) {
              const char* token = json_object_get_string(o);
              if (token != NULL && !strcmp(token, server->credential))
                pss->authenticated = true;
              else
                lwsl_warn("WS authentication failed\n");
            }
            if (!pss->authenticated) {
              json_object_put(obj);
              lws_close_reason(wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION, NULL, 0);
              return -1;
            }
          }
          json_object_put(obj);
          if (!spawn_process(pss, columns, rows)) return 1;
          break;
        default:
          lwsl_warn("ignored unknown message type: %c\n", command);
          break;
      }

      if (pss->buffer != NULL) {
        free(pss->buffer);
        pss->buffer = NULL;
        pss->len = pss->capacity = 0;
      }
      break;

    case LWS_CALLBACK_CLOSED:
      if (pss->wsi == NULL) break;

      server->client_count--;
      lwsl_notice("WS closed from %s, clients: %d\n", pss->address, server->client_count);
      if (pss->buffer != NULL) free(pss->buffer);
      if (pss->pty_buf != NULL) pty_buf_free(pss->pty_buf);
      for (int i = 0; i < pss->argc; i++) {
        free(pss->args[i]);
      }
      free(pss->args);

      if (pss->process != NULL) {
        ((pty_ctx_t*)pss->process->ctx)->ws_closed = true;
        if (process_running(pss->process)) {
          pty_pause(pss->process);
          lwsl_notice("killing process, pid: %d\n", pss->process->pid);
        }
        pty_kill(pss->process, server->sig_code);
      }

      if ((server->once || server->exit_no_conn) && server->client_count == 0) {
        lwsl_notice("exiting due to the --once/--exit-no-conn option.\n");

        // stop accepting new ws connections
        lws_cancel_service(context);

        if (process_running(pss->process)) {
          force_exit = true;
          lwsl_notice("send ^C to force exit.\n");
        } else {
          exit(0);
        }
      }
      break;

    default:
      break;
  }

  return 0;
}
