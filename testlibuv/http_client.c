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
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* A connection is modeled as an abstraction on top of two simple state
 * machines, one for reading and one for writing.  Either state machine
 * is, when active, in one of three states: busy, done or stop; the fourth
 * and final state, dead, is an end state and only relevant when shutting
 * down the connection.  A short overview:
 *
 *                          busy                  done           stop
 *  ----------|---------------------------|--------------------|------|
 *  readable  | waiting for incoming data | have incoming data | idle |
 *  writable  | busy writing out data     | completed write    | idle |
 *
 * We could remove the done state from the writable state machine. For our
 * purposes, it's functionally equivalent to the stop state.
 *
 * When the connection with upstream has been established, the client_ctx
 * moves into a state where incoming data from the client is sent upstream
 * and vice versa, incoming data from upstream is sent to the client.  In
 * other words, we're just piping data back and forth.  See conn_cycle()
 * for details.
 *
 * An interesting deviation from libuv's I/O model is that reads are discrete
 * rather than continuous events.  In layman's terms, when a read operation
 * completes, the connection stops reading until further notice.
 *
 * The rationale for this approach is that we have to wait until the data
 * has been sent out again before we can reuse the read buffer.
 *
 * It also pleasingly unifies with the request model that libuv uses for
 * writes and everything else; libuv may switch to a request model for
 * reads in the future.
 */
enum conn_state {
  c_busy,  /* Busy; waiting for incoming data or for a write to complete. */
  c_done,  /* Done; read incoming data or write finished. */
  c_stop,  /* Stopped. */
  c_dead
};

/* Session states. */
enum sess_state {
  s_req_start,        /* Start waiting for request data. */
  s_req_parse,        /* Wait for request data. */
  s_kill,             /* Tear down session. */
  s_almost_dead_0,    /* Waiting for finalizers to complete. */
  s_almost_dead_1,    /* Waiting for finalizers to complete. */
  s_almost_dead_2,    /* Waiting for finalizers to complete. */
  s_almost_dead_3,    /* Waiting for finalizers to complete. */
  s_almost_dead_4,    /* Waiting for finalizers to complete. */
  s_dead              /* Dead. Safe to free now. */
};

static void do_next(client_ctx *cx);
static int do_req_start(client_ctx *cx);
static int do_req_parse(client_ctx *cx);
static int do_kill(client_ctx *cx);
static int do_almost_dead(client_ctx *cx);
static void conn_timer_reset(conn *c);
static void conn_timer_expire(uv_timer_t *handle, int status);
static void conn_read(conn *c);
static void conn_read_done(uv_stream_t *handle,
                           ssize_t nread,
                           const uv_buf_t *buf);
static void conn_alloc(uv_handle_t *handle, size_t size, uv_buf_t *buf);
static void conn_write(conn *c, const void *data, unsigned int len);
static void conn_write_done(uv_write_t *req, int status);
static void conn_close(conn *c);
static void conn_close_done(uv_handle_t *handle);

/* |incoming| has been initialized by server.c when this is called. */
void http_client_finish_init(server_ctx *sx, client_ctx *cx) {
  conn *incoming;
  http_ctx *parser;

  cx->sx = sx;
  cx->state = s_req_start;

  incoming = &cx->clientconn;
  incoming->client = cx;
  incoming->result = 0;
  incoming->rdstate = c_stop;
  incoming->wrstate = c_stop;
  incoming->idle_timeout = sx->idle_timeout;
  CHECK(0 == uv_timer_init(sx->loop, &incoming->timer_handle));
  
  parser = &cx->parser;
  parser->status = ps_attr;
  parser->curattr = cx->clientconn.t.buf;
  parser->curattrlen = 0;
  parser->next = cx->clientconn.t.buf;
  parser->curval = 0;
  parser->curvallen = 0;
  parser->uri = 0;
  parser->urilen = 0;

  /* Wait for the initial packet. */
  conn_read(incoming);
}

/* This is the core state machine that drives the client <-> upstream proxy.
 * We move through the initial handshake and authentication steps first and
 * end up (if all goes well) in the proxy state where we're just proxying
 * data between the client and upstream.
 */
static void do_next(client_ctx *cx) {
  int new_state;

  ASSERT(cx->state != s_dead);
  switch (cx->state) {
    case s_req_start:
    case s_req_parse:
      new_state = do_req_parse(cx);
      break;
    case s_kill:
      new_state = do_kill(cx);
      break;
    case s_almost_dead_0:
    case s_almost_dead_1:
    case s_almost_dead_2:
    case s_almost_dead_3:
    case s_almost_dead_4:
      new_state = do_almost_dead(cx);
      break;
    default:
      UNREACHABLE();
  }
  cx->state = new_state;

  if (cx->state == s_dead) {
    if (DEBUG_CHECKS) {
      memset(cx, -1, sizeof(*cx));
    }
    free(cx);
  }
}

static int do_req_parse(client_ctx *cx) {
	conn *incoming;
	conn *outgoing;
	http_ctx *parser;
	uint8_t *data;
	size_t size;
	int err;

	parser = &cx->parser;
	incoming = &cx->clientconn;
	ASSERT(incoming->rdstate == c_done);
	ASSERT(incoming->wrstate == c_stop);
	incoming->rdstate = c_stop;

	if (incoming->result < 0) {
		pr_err("read error: %s", uv_strerror(incoming->result));
		return do_kill(cx);
	}

	data = (uint8_t *)incoming->t.buf;
	size = (size_t)incoming->result;
	err = http_parse(parser, &data, &size);
	if (err == http_ok) {
		conn_read(incoming);
		return s_req_parse;  /* Need more data. */
	}

	if (size != 0) {
		pr_err("junk in request %u", (unsigned)size);
		return do_kill(cx);
	}

}

static int do_kill(client_ctx *cx) {
  int new_state;

  if (cx->state >= s_almost_dead_0) {
    return cx->state;
  }

  /* Try to cancel the request. The callback still runs but if the
   * cancellation succeeded, it gets called with status=UV_ECANCELED.
   */
  new_state = s_almost_dead_1;

  conn_close(&cx->clientconn);
  return new_state;
}

static int do_almost_dead(client_ctx *cx) {
  ASSERT(cx->state >= s_almost_dead_0);
  return cx->state + 1;  /* Another finalizer completed. */
}

static int conn_cycle(const char *who, conn *a, conn *b) {
  if (a->result < 0) {
    if (a->result != UV_EOF) {
      pr_err("%s error: %s", who, uv_strerror(a->result));
    }
    return -1;
  }

  if (b->result < 0) {
    return -1;
  }

  if (a->wrstate == c_done) {
    a->wrstate = c_stop;
  }

  /* The logic is as follows: read when we don't write and write when we don't
   * read.  That gives us back-pressure handling for free because if the peer
   * sends data faster than we consume it, TCP congestion control kicks in.
   */
  if (a->wrstate == c_stop) {
    if (b->rdstate == c_stop) {
      conn_read(b);
    } else if (b->rdstate == c_done) {
      conn_write(a, b->t.buf, b->result);
      b->rdstate = c_stop;  /* Triggers the call to conn_read() above. */
    }
  }

  return 0;
}

static void conn_timer_reset(conn *c) {
  CHECK(0 == uv_timer_start(&c->timer_handle,
                            conn_timer_expire,
                            c->idle_timeout,
                            0));
}

static void conn_timer_expire(uv_timer_t *handle, int status) {
  conn *c;

  CHECK(0 == status);
  c = CONTAINER_OF(handle, conn, timer_handle);
  c->result = UV_ETIMEDOUT;
  do_next(c->client);
}

static void conn_read(conn *c) {
  ASSERT(c->rdstate == c_stop);
  CHECK(0 == uv_read_start(&c->handle.stream, conn_alloc, conn_read_done));
  c->rdstate = c_busy;
  conn_timer_reset(c);
}

static void conn_read_done(uv_stream_t *handle,
                           ssize_t nread,
                           const uv_buf_t *buf) {
  conn *c;

  c = CONTAINER_OF(handle, conn, handle);
  ASSERT(c->t.buf == buf->base);
  ASSERT(c->rdstate == c_busy);
  c->rdstate = c_done;
  c->result = nread;

  uv_read_stop(&c->handle.stream);
  do_next(c->client);
}

static void conn_alloc(uv_handle_t *handle, size_t size, uv_buf_t *buf) {
  conn *c;

  c = CONTAINER_OF(handle, conn, handle);
  ASSERT(c->rdstate == c_busy);
  buf->base = c->t.buf;
  buf->len = sizeof(c->t.buf);
}

static void conn_write(conn *c, const void *data, unsigned int len) {
  uv_buf_t buf;

  ASSERT(c->wrstate == c_stop || c->wrstate == c_done);
  c->wrstate = c_busy;

  /* It's okay to cast away constness here, uv_write() won't modify the
   * memory.
   */
  buf.base = (char *) data;
  buf.len = len;

  CHECK(0 == uv_write(&c->write_req,
                      &c->handle.stream,
                      &buf,
                      1,
                      conn_write_done));
  conn_timer_reset(c);
}

static void conn_write_done(uv_write_t *req, int status) {
  conn *c;

  if (status == UV_ECANCELED) {
    return;  /* Handle has been closed. */
  }

  c = CONTAINER_OF(req, conn, write_req);
  ASSERT(c->wrstate == c_busy);
  c->wrstate = c_done;
  c->result = status;
  do_next(c->client);
}

static void conn_close(conn *c) {
  ASSERT(c->rdstate != c_dead);
  ASSERT(c->wrstate != c_dead);
  c->rdstate = c_dead;
  c->wrstate = c_dead;
  c->timer_handle.data = c;
  c->handle.handle.data = c;
  uv_close(&c->handle.handle, conn_close_done);
  uv_close((uv_handle_t *) &c->timer_handle, conn_close_done);
}

static void conn_close_done(uv_handle_t *handle) {
  conn *c;

  c = handle->data;
  do_next(c->client);
}
