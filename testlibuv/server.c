/* Copyright StrongLoop, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "defs.h"
//#include <netinet/in.h>  /* INET6_ADDRSTRLEN */
#include <stdlib.h>
#include <string.h>

#ifndef INET6_ADDRSTRLEN
# define INET6_ADDRSTRLEN 63
#endif

typedef struct {
  uv_getaddrinfo_t getaddrinfo_req;
  server_config config;
  server_ctx *servers;
  uv_loop_t *loop;
} server_state;

static void do_bind(uv_getaddrinfo_t *req, int status, struct addrinfo *ai);
static void on_connection(uv_stream_t *server, int status);

int server_run(const server_config *cf, uv_loop_t *loop) {
  struct addrinfo hints;
  server_state state;
  int err;

  memset(&state, 0, sizeof(state));
  state.servers = NULL;
  state.config = *cf;
  state.loop = loop;

  /* Resolve the address of the interface that we should bind to.
   * The getaddrinfo callback starts the server and everything else.
   */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  err = uv_getaddrinfo(loop,
                       &state.getaddrinfo_req,
                       do_bind,
                       cf->bind_host,
                       NULL,
                       &hints);
  if (err != 0) {
    pr_err("getaddrinfo: %s", uv_strerror(err));
    return err;
  }

  /* Start the event loop.  Control continues in do_bind(). */
  if (uv_run(loop, UV_RUN_DEFAULT)) {
    abort();
  }

  /* Please Valgrind. */
  uv_loop_delete(loop);
  free(state.servers);
  return 0;
}

/* Bind a server to each address that getaddrinfo() reported. */
static void do_bind(uv_getaddrinfo_t *req, int status, struct addrinfo *addrs) {
  char addrbuf[INET6_ADDRSTRLEN + 1];
  unsigned int ipv4_naddrs;
  unsigned int ipv6_naddrs;
  server_state *state;
  server_config *cf;
  struct addrinfo *ai;
  const void *addrv;
  const char *what;
  uv_loop_t *loop;
  server_ctx *sx;
  unsigned int n;
  int err;
  union {
    struct sockaddr addr;
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
  } s;

  state = CONTAINER_OF(req, server_state, getaddrinfo_req);
  loop = state->loop;
  cf = &state->config;

  if (status < 0) {
    pr_err("getaddrinfo(\"%s\"): %s", cf->bind_host, uv_strerror(status));
    uv_freeaddrinfo(addrs);
    return;
  }

  ipv4_naddrs = 0;
  ipv6_naddrs = 0;
  for (ai = addrs; ai != NULL; ai = ai->ai_next) {
    if (ai->ai_family == AF_INET) {
      ipv4_naddrs += 1;
    } else if (ai->ai_family == AF_INET6) {
      ipv6_naddrs += 1;
    }
  }

  if (ipv4_naddrs == 0 && ipv6_naddrs == 0) {
    pr_err("%s has no IPv4/6 addresses", cf->bind_host);
    uv_freeaddrinfo(addrs);
    return;
  }

  state->servers =
      xmalloc((ipv4_naddrs + ipv6_naddrs) * sizeof(state->servers[0]));

  n = 0;
  for (ai = addrs; ai != NULL; ai = ai->ai_next) {
    if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
      continue;
    }

    if (ai->ai_family == AF_INET) {
      s.addr4 = *(const struct sockaddr_in *) ai->ai_addr;
      s.addr4.sin_port = htons(cf->bind_port);
      addrv = &s.addr4.sin_addr;
    } else if (ai->ai_family == AF_INET6) {
      s.addr6 = *(const struct sockaddr_in6 *) ai->ai_addr;
      s.addr6.sin6_port = htons(cf->bind_port);
      addrv = &s.addr6.sin6_addr;
    } else {
      UNREACHABLE();
    }

    if (uv_inet_ntop(s.addr.sa_family, addrv, addrbuf, sizeof(addrbuf))) {
      UNREACHABLE();
    }

    sx = state->servers + n;
    sx->loop = loop;
    sx->idle_timeout = state->config.idle_timeout;
    CHECK(0 == uv_tcp_init(loop, &sx->tcp_handle));

    what = "uv_tcp_bind";
    err = uv_tcp_bind(&sx->tcp_handle, &s.addr, 0);
    if (err == 0) {
      what = "uv_listen";
      err = uv_listen((uv_stream_t *) &sx->tcp_handle, 128, on_connection);
    }

    if (err != 0) {
      pr_err("%s(\"%s:%hu\"): %s",
             what,
             addrbuf,
             cf->bind_port,
             uv_strerror(err));
      while (n > 0) {
        n -= 1;
        uv_close((uv_handle_t *) (state->servers + n), NULL);
      }
      break;
    }

    pr_info("listening on %s:%hu", addrbuf, cf->bind_port);
    n += 1;
  }

  uv_freeaddrinfo(addrs);
}

static void on_connection(uv_stream_t *server, int status) {
  server_ctx *sx;
  client_ctx *cx;

  CHECK(status == 0);
  sx = CONTAINER_OF(server, server_ctx, tcp_handle);
  cx = xmalloc(sizeof(*cx));
  CHECK(0 == uv_tcp_init(sx->loop, &cx->clientconn.handle.tcp));
  CHECK(0 == uv_accept(server, &cx->clientconn.handle.stream));
  http_client_finish_init(sx, cx);
}
