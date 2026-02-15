#define NAPI_VERSION 8
#include <node_api.h>
#include <uv.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#define NAPI_CALL(call) do {              \
  if ((call) != napi_ok) return NULL;     \
} while (0)

#define THROW_ERRNO(env) do {             \
  napi_throw_error(env, NULL, strerror(errno)); \
  return NULL;                            \
} while (0)

// --- Handle struct ---

struct sctp_handle {
  uv_poll_t uv_poll;
  napi_env env;
  napi_ref js_callback_ref;
  int fd;
  int maxlen;
  char *recv_buf;
  int closed;
  int close_pending;
  int finalizer_called;
};

// --- Helpers ---

static int parse_sockaddr(napi_env env, napi_value host_val, napi_value port_val,
                          struct sockaddr_storage *out, socklen_t *out_len) {
  char host[NI_MAXHOST];
  size_t host_len;
  if (napi_get_value_string_utf8(env, host_val, host, sizeof(host), &host_len) != napi_ok) {
    napi_throw_error(env, NULL, "host must be a string");
    return -1;
  }

  int32_t port;
  if (napi_get_value_int32(env, port_val, &port) != napi_ok) {
    napi_throw_error(env, NULL, "port must be an integer");
    return -1;
  }

  char port_str[6];
  snprintf(port_str, sizeof(port_str), "%d", port);

  struct addrinfo hints = {0};
  hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
  hints.ai_socktype = SOCK_SEQPACKET;
  hints.ai_protocol = IPPROTO_SCTP;

  struct addrinfo *res = NULL;
  int rc = getaddrinfo(host, port_str, &hints, &res);
  if (rc != 0 || !res) {
    if (res) freeaddrinfo(res);
    napi_throw_error(env, NULL, rc ? gai_strerror(rc) : "getaddrinfo returned no results");
    return -1;
  }

  memset(out, 0, sizeof(*out));
  memcpy(out, res->ai_addr, res->ai_addrlen);
  *out_len = res->ai_addrlen;
  freeaddrinfo(res);
  return 0;
}

// --- Lifecycle ---

static void handle_maybe_free(struct sctp_handle *h) {
  if (h->closed && h->finalizer_called) {
    if (h->recv_buf) {
      free(h->recv_buf);
      h->recv_buf = NULL;
    }
    free(h);
  }
}

static void handle_uv_close_cb(uv_handle_t *handle) {
  struct sctp_handle *h = (struct sctp_handle *)handle->data;
  h->closed = 1;
  handle_maybe_free(h);
}

static void handle_finalizer(napi_env env, void *finalize_data, void *finalize_hint) {
  struct sctp_handle *h = (struct sctp_handle *)finalize_data;
  h->finalizer_called = 1;

  if (!h->closed && !h->close_pending) {
    h->close_pending = 1;
    if (h->js_callback_ref) {
      napi_delete_reference(h->env, h->js_callback_ref);
      h->js_callback_ref = NULL;
    }
    uv_poll_stop(&h->uv_poll);
    if (h->fd >= 0) { close(h->fd); h->fd = -1; }
    uv_close((uv_handle_t *)&h->uv_poll, handle_uv_close_cb);
    return;
  }

  handle_maybe_free(h);
}

// --- Poll callback ---

#define MAX_RECV_PER_TICK 64

static void poll_cb(uv_poll_t *handle, int status, int events) {
  struct sctp_handle *h = (struct sctp_handle *)handle->data;
  if (h->close_pending || h->closed) return;
  if (status < 0) return; // libuv error on the fd

  napi_handle_scope scope;
  napi_open_handle_scope(h->env, &scope);

  for (int i = 0; i < MAX_RECV_PER_TICK; i++) {
    struct sockaddr_storage from_addr = {0};
    socklen_t from_len = sizeof(from_addr);
    struct sctp_rcvinfo rinfo = {0};
    socklen_t infolen = sizeof(rinfo);
    unsigned int infotype = 0;
    int msg_flags = 0;

    struct iovec iov = { .iov_base = h->recv_buf, .iov_len = h->maxlen };
    ssize_t n = sctp_recvv(h->fd, &iov, 1,
      (struct sockaddr *)&from_addr, &from_len,
      &rinfo, &infolen, &infotype, &msg_flags);

    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      napi_value cb, global, err_str;
      if (napi_get_reference_value(h->env, h->js_callback_ref, &cb) != napi_ok) break;
      if (napi_get_global(h->env, &global) != napi_ok) break;
      napi_create_string_utf8(h->env, strerror(errno), NAPI_AUTO_LENGTH, &err_str);
      napi_call_function(h->env, global, cb, 1, &err_str, NULL);
      break;
    }

    if (msg_flags & MSG_NOTIFICATION) continue;

    // Build result: [data, host, port]
    napi_value result;
    napi_create_array_with_length(h->env, 3, &result);

    // Safe copy: each JS Buffer owns its own memory
    napi_value data_buf;
    napi_create_buffer_copy(h->env, n, h->recv_buf, NULL, &data_buf);
    napi_set_element(h->env, result, 0, data_buf);

    char host_str[NI_MAXHOST];
    char port_str[NI_MAXSERV];
    getnameinfo((struct sockaddr *)&from_addr, from_len,
                host_str, sizeof(host_str), port_str, sizeof(port_str),
                NI_NUMERICHOST | NI_NUMERICSERV);
    napi_value host_val;
    napi_create_string_utf8(h->env, host_str, NAPI_AUTO_LENGTH, &host_val);
    napi_set_element(h->env, result, 1, host_val);

    napi_value port_val;
    napi_create_int32(h->env, atoi(port_str), &port_val);
    napi_set_element(h->env, result, 2, port_val);

    napi_value cb, global;
    if (napi_get_reference_value(h->env, h->js_callback_ref, &cb) != napi_ok) break;
    if (napi_get_global(h->env, &global) != napi_ok) break;
    napi_call_function(h->env, global, cb, 1, &result, NULL);
  }

  napi_close_handle_scope(h->env, scope);
}

// --- Handle methods (bound via data pointer) ---

// handle.send(buffer, host, port) -> bytes_sent
static napi_value HandleSend(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3];
  void *fn_data;
  NAPI_CALL(napi_get_cb_info(env, info, &argc, args, NULL, &fn_data));

  if (argc < 3) {
    napi_throw_error(env, NULL, "send() requires 3 arguments: buffer, host, port");
    return NULL;
  }

  struct sctp_handle *h = (struct sctp_handle *)fn_data;

  if (h->fd < 0) {
    napi_throw_error(env, NULL, "handle is closed");
    return NULL;
  }

  void *data;
  size_t len;
  NAPI_CALL(napi_get_buffer_info(env, args[0], &data, &len));

  struct sockaddr_storage addr;
  socklen_t addr_len;
  if (parse_sockaddr(env, args[1], args[2], &addr, &addr_len) < 0) return NULL;

  // SCTP_UNORDERED: no head-of-line blocking — messages are independent datagrams.
  // PR-SCTP TTL 30s: drop unsent messages older than 30s rather than retransmitting
  // stale data. Appropriate for real-time messaging where late data is useless.
  struct sctp_sendv_spa spa = {
    .sendv_flags = SCTP_SEND_SNDINFO_VALID | SCTP_SEND_PRINFO_VALID,
    .sendv_sndinfo.snd_flags = SCTP_UNORDERED,
    .sendv_prinfo = { .pr_policy = SCTP_PR_SCTP_TTL, .pr_value = 30000 },
  };

  struct iovec iov = { .iov_base = data, .iov_len = len };
  ssize_t sent = sctp_sendv(h->fd, &iov, 1,
    (struct sockaddr *)&addr, addr_len,
    &spa, sizeof(spa), SCTP_SENDV_SPA, 0);

  if (sent < 0) THROW_ERRNO(env);

  napi_value result;
  NAPI_CALL(napi_create_int32(env, (int32_t)sent, &result));
  return result;
}

// handle.close()
static napi_value HandleClose(napi_env env, napi_callback_info info) {
  void *fn_data;
  NAPI_CALL(napi_get_cb_info(env, info, NULL, NULL, NULL, &fn_data));
  struct sctp_handle *h = (struct sctp_handle *)fn_data;

  if (h->closed || h->close_pending) return NULL;

  h->close_pending = 1;
  uv_poll_stop(&h->uv_poll);

  if (h->fd >= 0) { close(h->fd); h->fd = -1; }

  if (h->js_callback_ref) {
    napi_delete_reference(h->env, h->js_callback_ref);
    h->js_callback_ref = NULL;
  }

  uv_close((uv_handle_t *)&h->uv_poll, handle_uv_close_cb);
  return NULL;
}

// --- open(bindHost, bindPort, recvMaxlen, onRecv) -> handle ---

static napi_value SctpOpen(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value args[4];
  NAPI_CALL(napi_get_cb_info(env, info, &argc, args, NULL, NULL));

  if (argc < 4) {
    napi_throw_error(env, NULL, "open() requires 4 arguments: host, port, maxlen, callback");
    return NULL;
  }

  // Parse bind address
  struct sockaddr_storage addr;
  socklen_t addr_len;
  if (parse_sockaddr(env, args[0], args[1], &addr, &addr_len) < 0) return NULL;

  int32_t maxlen;
  NAPI_CALL(napi_get_value_int32(env, args[2], &maxlen));

  // Create socket using the family from the parsed address
  int fd = socket(addr.ss_family, SOCK_SEQPACKET | SOCK_NONBLOCK, IPPROTO_SCTP);
  if (fd < 0) THROW_ERRNO(env);

  // For IPv6 dual-stack, allow IPv4-mapped addresses
  if (addr.ss_family == AF_INET6) {
    int off = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
  }

  // --- Socket options (shared by server and future client sockets) ---

  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  // Disable Nagle — send immediately rather than coalescing small chunks.
  // Messages are already framed at the app layer.
  setsockopt(fd, IPPROTO_SCTP, SCTP_NODELAY, &on, sizeof(on));

  // Aggressive retransmit timers for LAN use.
  // Default RTO (1s initial, 1s min, 60s max) is designed for WAN;
  // 250/200/5000 ms keeps failover fast on a local subnet.
  struct sctp_rtoinfo rto = {
    .srto_initial = 250, .srto_min = 200, .srto_max = 5000
  };
  setsockopt(fd, IPPROTO_SCTP, SCTP_RTOINFO, &rto, sizeof(rto));

  // Enable rcvinfo so sctp_recvv can populate sctp_rcvinfo
  setsockopt(fd, IPPROTO_SCTP, SCTP_RECVRCVINFO, &on, sizeof(on));

  // --- Server: bind + listen (one-to-many) ---
  // For a client-only socket, skip this and connect() to a remote peer instead.

  if (bind(fd, (struct sockaddr *)&addr, addr_len) < 0) {
    close(fd);
    THROW_ERRNO(env);
  }
  if (listen(fd, 10) < 0) {
    close(fd);
    THROW_ERRNO(env);
  }

  // Allocate handle
  struct sctp_handle *h = calloc(1, sizeof(struct sctp_handle));
  if (!h) {
    close(fd);
    napi_throw_error(env, NULL, "calloc failed");
    return NULL;
  }

  h->recv_buf = malloc(maxlen);
  if (!h->recv_buf) {
    close(fd);
    free(h);
    napi_throw_error(env, NULL, "malloc failed");
    return NULL;
  }

  h->env = env;
  h->fd = fd;
  h->maxlen = maxlen;

  // Store JS callback
  NAPI_CALL(napi_create_reference(env, args[3], 1, &h->js_callback_ref));

  // Start uv_poll
  uv_loop_t *loop;
  napi_get_uv_event_loop(env, &loop);
  uv_poll_init(loop, &h->uv_poll, fd);
  h->uv_poll.data = h;
  uv_poll_start(&h->uv_poll, UV_READABLE, poll_cb);

  // Build JS handle object with send() and close() methods
  napi_value js_handle;
  napi_create_object(env, &js_handle);
  napi_wrap(env, js_handle, h, handle_finalizer, NULL, NULL);

  napi_value send_fn;
  napi_create_function(env, "send", NAPI_AUTO_LENGTH, HandleSend, h, &send_fn);
  napi_set_named_property(env, js_handle, "send", send_fn);

  napi_value close_fn;
  napi_create_function(env, "close", NAPI_AUTO_LENGTH, HandleClose, h, &close_fn);
  napi_set_named_property(env, js_handle, "close", close_fn);

  return js_handle;
}

// --- Module init ---

static napi_value Init(napi_env env, napi_value exports) {
  napi_value open_fn;
  napi_create_function(env, "open", NAPI_AUTO_LENGTH, SctpOpen, NULL, &open_fn);
  napi_set_named_property(env, exports, "open", open_fn);
  return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
