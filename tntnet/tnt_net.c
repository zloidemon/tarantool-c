
/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>

#include <sys/uio.h>

#include <uri.h>

#include <tarantool/tarantool.h>
#include <tarantool/tnt_net.h>
#include <tarantool/tnt_io.h>

static void tnt_net_free(struct tnt_stream *s) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	tnt_io_close(sn);
	tnt_mem_free(sn->greeting);
	tnt_iob_free(&sn->sbuf);
	tnt_iob_free(&sn->rbuf);
	tnt_opt_free(&sn->opt);
	tnt_schema_delete(sn->schema);
	tnt_mem_free(sn->schema);
	tnt_mem_free(s->data);
	s->data = NULL;
}

static ssize_t
tnt_net_read(struct tnt_stream *s, char *buf, size_t size) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	/* read doesn't touches wrcnt */
	return tnt_io_recv(sn, buf, size);
}

static ssize_t
tnt_net_write(struct tnt_stream *s, const char *buf, size_t size) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	ssize_t rc = tnt_io_send(sn, buf, size);
	if (rc != -1)
		s->wrcnt++;
	return rc;
}

static ssize_t
tnt_net_writev(struct tnt_stream *s, struct iovec *iov, int count) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	ssize_t rc = tnt_io_sendv(sn, iov, count);
	if (rc != -1)
		s->wrcnt++;
	return rc;
}

static ssize_t
tnt_net_recv_cb(struct tnt_stream *s, char *buf, ssize_t size) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	return tnt_io_recv(sn, buf, size);
}

static int
tnt_net_reply(struct tnt_stream *s, struct tnt_reply *r) {
	if (s->wrcnt == 0)
		return 1;
	s->wrcnt--;
	return tnt_reply_from(r, (tnt_reply_t)tnt_net_recv_cb, s);
}

struct tnt_stream *tnt_net(struct tnt_stream *s) {
	int allocated = s == NULL;
	s = tnt_stream_init(s);
	if (s == NULL)
		return NULL;
	/* allocating stream data */
	s->data = tnt_mem_alloc(sizeof(struct tnt_stream_net));
	if (s->data == NULL) {
		if (allocated)
			tnt_stream_free(s);
		return NULL;
	}
	memset(s->data, 0, sizeof(struct tnt_stream_net));
	/* initializing interfaces */
	s->read = tnt_net_read;
	s->read_reply = tnt_net_reply;
	s->write = tnt_net_write;
	s->writev = tnt_net_writev;
	s->free = tnt_net_free;
	/* initializing internal data */
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	sn->fd = -1;
	sn->greeting = tnt_mem_alloc(TNT_GREETING_SIZE);
	if (sn->greeting == NULL) {
		s->free(s);
		if (allocated)
			tnt_stream_free(s);
		return NULL;
	}
	if (tnt_opt_init(&sn->opt) == -1) {
		s->free(s);
		if (allocated)
			tnt_stream_free(s);
	}
	return s;
}

int tnt_set(struct tnt_stream *s, int opt, ...) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	va_list args;
	va_start(args, opt);
	sn->error = tnt_opt_set(&sn->opt, opt, args);
	va_end(args);
	return (sn->error == TNT_EOK) ? 0 : -1;
}

int tnt_init(struct tnt_stream *s) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	if ((sn->schema = tnt_schema_new(NULL)) == NULL) {
		sn->error = TNT_EMEMORY;
		return -1;
	}
	if (tnt_iob_init(&sn->sbuf, sn->opt.send_buf, sn->opt.send_cb,
		sn->opt.send_cbv, sn->opt.send_cb_arg) == -1) {
		sn->error = TNT_EMEMORY;
		return -1;
	}
	if (tnt_iob_init(&sn->rbuf, sn->opt.recv_buf, sn->opt.recv_cb, NULL,
		sn->opt.recv_cb_arg) == -1) {
		sn->error = TNT_EMEMORY;
		return -1;
	}
	sn->inited = 1;
	return 0;
}

int tnt_reload_schema(struct tnt_stream *s) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	if (!sn->connected)
		return -1;
	struct tnt_stream *obj = tnt_object(NULL);
	tnt_object_add_array(obj, 0);
	uint32_t oldsync = tnt_stream_reqid(s, 127);
	tnt_select(s, tnt_vsp_space, tnt_vin_name,
		   UINT32_MAX, 0, TNT_ITER_ALL, obj);
	tnt_select(s, tnt_vsp_index, tnt_vin_name,
		   UINT32_MAX, 0, TNT_ITER_ALL, obj);
	tnt_stream_reqid(s, oldsync);
	tnt_flush(s);
	struct tnt_iter it; tnt_iter_reply(&it, s);
	int sloaded = 0;
	struct tnt_reply bkp; tnt_reply_init(&bkp);
	while (tnt_next(&it)) {
		struct tnt_reply *r = TNT_IREPLY_PTR(&it);
		switch (r->sync) {
		case(127):
			if (r->error) goto error;
			tnt_schema_add_spaces(sn->schema, r->data,
					      r->data_end - r->data);
			sloaded += 1;
			break;
		case(128):
			if (r->error) goto error;
			if (!(sloaded & 1)) {
				memcpy(&bkp, r, sizeof(struct tnt_reply));
				r->buf = NULL;
				break;
			}
			sloaded += 2;
			tnt_schema_add_indexes(sn->schema, r->data,
					       r->data_end - r->data);
			break;
		default:
			goto error;
		}
	}
	if (bkp.buf) {
		tnt_schema_add_indexes(sn->schema, bkp.data,
				       bkp.data_end - bkp.data);
		sloaded += 2;
		tnt_reply_free(&bkp);
	}
	if (sloaded != 3) goto error;

	tnt_iter_free(&it);
	tnt_stream_free(obj);
	return 0;
error:
	tnt_iter_free(&it);
	tnt_stream_free(obj);
	return -1;
}

int tnt_authenticate(struct tnt_stream *s) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	if (!sn->connected)
		return -1;
	if (sn->schema) tnt_schema_flush(sn->schema);
	struct uri *uri = sn->opt.uri;
	struct tnt_stream *obj = tnt_object(NULL);
	tnt_object_add_array(obj, 0);
	uint32_t oldsync = tnt_stream_reqid(s, 127);
	tnt_auth(s, uri->login, uri->login_len,
		 uri->password, uri->password_len);
	tnt_select(s, tnt_vsp_space, tnt_vin_name,
		   UINT32_MAX, 0, TNT_ITER_ALL, obj);
	tnt_select(s, tnt_vsp_index, tnt_vin_name,
		   UINT32_MAX, 0, TNT_ITER_ALL, obj);
	tnt_stream_reqid(s, oldsync);
	tnt_flush(s);
	struct tnt_iter it; tnt_iter_reply(&it, s);
	int sloaded = 0;
	struct tnt_reply bkp; tnt_reply_init(&bkp);
	while (tnt_next(&it)) {
		struct tnt_reply *r = TNT_IREPLY_PTR(&it);
		switch (r->sync) {
		case(127):
			if (r->error) goto error;
			sloaded += 1;
			break;
		case(128):
			if (r->error) goto error;
			tnt_schema_add_spaces(sn->schema, r->data,
					      r->data_end - r->data);
			sloaded += 2;
			break;
		case(129):
			if (r->error) goto error;
			if (!(sloaded & 2)) {
				memcpy(&bkp, r, sizeof(struct tnt_reply));
				r->buf = NULL;
				break;
			}
			sloaded += 4;
			tnt_schema_add_indexes(sn->schema, r->data,
					       r->data_end - r->data);
			break;
		default:
			goto error;
		}
	}
	if (bkp.buf) {
		tnt_schema_add_indexes(sn->schema, bkp.data,
				       bkp.data_end - bkp.data);
		sloaded += 4;
		tnt_reply_free(&bkp);
	}
	if (sloaded != 7) goto error;

	tnt_iter_free(&it);
	tnt_stream_free(obj);
	return 0;
error:
	tnt_iter_free(&it);
	tnt_stream_free(obj);
	return -1;
}

int tnt_connect(struct tnt_stream *s) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	if (!sn->inited) tnt_init(s);
	if (sn->connected)
		tnt_close(s);
	sn->error = tnt_io_connect(sn);
	if (sn->error != TNT_EOK)
		return -1;
	if (s->read(s, sn->greeting, TNT_GREETING_SIZE) == -1 ||
	    sn->error != TNT_EOK)
		return -1;
	if (sn->opt.uri->login && sn->opt.uri->password)
		if (tnt_authenticate(s) == -1)
			return -1;
	return 0;
}

void tnt_close(struct tnt_stream *s) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	tnt_io_close(sn);
}

ssize_t tnt_flush(struct tnt_stream *s) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	return tnt_io_flush(sn);
}

int tnt_fd(struct tnt_stream *s) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	return sn->fd;
}

enum tnt_error tnt_error(struct tnt_stream *s) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	return sn->error;
}

/* must be in sync with enum tnt_error */

struct tnt_error_desc {
	enum tnt_error type;
	char *desc;
};

static struct tnt_error_desc tnt_error_list[] =
{
	{ TNT_EOK,      "ok"                       },
	{ TNT_EFAIL,    "fail"                     },
	{ TNT_EMEMORY,  "memory allocation failed" },
	{ TNT_ESYSTEM,  "system error"             },
	{ TNT_EBIG,     "buffer is too big"        },
	{ TNT_ESIZE,    "bad buffer size"          },
	{ TNT_ERESOLVE, "gethostbyname(2) failed"  },
	{ TNT_ETMOUT,   "operation timeout"        },
	{ TNT_EBADVAL,  "bad argument"             },
	{ TNT_ELOGIN,   "failed to login"          },
	{ TNT_LAST,      NULL                      }
};

char *tnt_strerror(struct tnt_stream *s) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	if (sn->error == TNT_ESYSTEM) {
		static char msg[256];
		snprintf(msg, sizeof(msg), "%s (errno: %d)",
			 strerror(sn->errno_), sn->errno_);
		return msg;
	}
	return tnt_error_list[(int)sn->error].desc;
}

int tnt_errno(struct tnt_stream *s) {
	struct tnt_stream_net *sn = TNT_SNET_CAST(s);
	return sn->errno_;
}
