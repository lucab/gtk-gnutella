/*
 * $Id$
 *
 * Copyright (c) 2006, Christian Biere
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup core
 * @file
 *
 * Common TLS functions.
 */

#include "common.h"

RCSID("$Id$");

#ifdef HAS_GNUTLS
#include <gnutls/gnutls.h>
#endif /* HAS_GNUTLS */

#include "tls_common.h"
#include "features.h"
#include "sockets.h"

#include "if/gnet_property_priv.h"
#include "if/core/settings.h"

#include "lib/header.h"
#include "lib/walloc.h"

#include "lib/override.h"		/* Must be the last header included */

#ifdef HAS_GNUTLS

struct tls_context {
	gnutls_session_t session;
	gnutls_anon_server_credentials server_cred;
	gnutls_anon_client_credentials client_cred;
};

static gnutls_certificate_credentials_t server_cert_cred;

static inline gnutls_session_t
tls_socket_get_session(struct gnutella_socket *s)
{
	g_return_val_if_fail(s, NULL);
	g_return_val_if_fail(s->tls.ctx, NULL);
	return s->tls.ctx->session;
}

#ifdef XXX_CUSTOM_PUSH_PULL
static inline void
tls_transport_debug(const char *op, int fd, size_t size, ssize_t ret)
{
	if (tls_debug > 1) {
		int saved_errno = errno;
		gboolean error = (ssize_t) -1 == ret;

		g_message("%s(): fd=%d size=%lu ret=%ld%s%s%s",
			op, fd, (gulong) size, (glong) ret,
			error ? " errno=\"" : "",
			error ? g_strerror(saved_errno) : "",
			error ? "\"" : "");

		errno = saved_errno;
	}
}

static ssize_t
tls_push(gnutls_transport_ptr ptr, const void *buf, size_t size) 
{
	struct gnutella_socket *s = ptr;
	ssize_t ret;

	g_assert(s);
	g_assert(s->file_desc >= 0);

	ret = write(s->file_desc, buf, size);
	tls_transport_debug("tls_push", s->file_desc, size, ret);
	return ret;
}

static ssize_t
tls_pull(gnutls_transport_ptr ptr, void *buf, size_t size) 
{
	struct gnutella_socket *s = ptr;
	ssize_t ret;

	g_assert(s);
	g_assert(s->file_desc >= 0);

	ret = read(s->file_desc, buf, size);
	tls_transport_debug("tls_pull", s->file_desc, size, ret);
	return ret;
}
#endif /* XXX_CUSTOM_PUSH_PULL */

/**
 * Change the monitoring condition on the socket.
 */
static void
tls_socket_evt_change(struct gnutella_socket *s, inputevt_cond_t cond)
{
	g_assert(s);
	g_assert(SOCKET_WITH_TLS(s));	/* No USES yet, may not have handshaked */
	g_assert(INPUT_EVENT_EXCEPTION != cond);
	g_assert(0 != s->gdk_tag);

	if (cond != s->tls.cb_cond) {
		int saved_errno = errno;

		if (tls_debug > 1) {
			int fd = socket_evt_fd(s);
			g_message("tls_socket_evt_change: fd=%d, cond=%s -> %s, handler=%p",
				fd, inputevt_cond_to_string(s->tls.cb_cond),
				inputevt_cond_to_string(cond), s->tls.cb_handler);
		}
		if (s->gdk_tag) {
			inputevt_remove(s->gdk_tag);
			s->gdk_tag = 0;
		}
		socket_evt_set(s, cond, s->tls.cb_handler, s->tls.cb_data);
		errno = saved_errno;
	}
}

static gnutls_dh_params_t
get_dh_params(void)
{
	static gnutls_dh_params_t dh_params;
	static gboolean initialized = FALSE;

	if (!initialized) {
 		if (gnutls_dh_params_init(&dh_params)) {
			g_warning("get_dh_params(): gnutls_dh_params_init() failed");
			return NULL;
		}
    	if (gnutls_dh_params_generate2(dh_params, TLS_DH_BITS)) {
			g_warning("get_dh_params(): gnutls_dh_params_generate2() failed");
			return NULL;
		}
		initialized = TRUE;
	}
	return dh_params;
}

/**
 * @return	TLS_HANDSHAKE_ERROR if the TLS handshake failed.
 *			TLS_HANDSHAKE_RETRY if the handshake is incomplete; thus
 *				tls_handshake() should called again on the next I/O event.
 *			TLS_HANDSHAKE_FINISHED if the TLS handshake succeeded. Note
 *				that this is also returned if TLS is disabled. Therefore
 *				this does not imply an encrypted connection.
 */
enum tls_handshake_result
tls_handshake(struct gnutella_socket *s)
{
	gnutls_session_t session;
	int ret;

	g_assert(s);

	session = tls_socket_get_session(s);
	g_assert(session);

#ifdef XXX_CUSTOM_PUSH_PULL
	{
		const void *ptr = gnutls_transport_get_ptr(session);
		if (!ptr) {
			gnutls_transport_set_ptr(session, s);
		}
	}
#else
	{
		int fd = GPOINTER_TO_INT(gnutls_transport_get_ptr(session));
		if (fd < 0) {
			fd = s->file_desc;
			gnutls_transport_set_ptr(session, GINT_TO_POINTER(fd));
		}
	}
#endif	/* XXX_CUSTOM_PUSH_PULL */


	ret = gnutls_handshake(session);
	switch (ret) {
	case 0:
		if (tls_debug) {
			g_message("TLS handshake succeeded");
		}
		tls_socket_evt_change(s, INPUT_EVENT_W);
		return TLS_HANDSHAKE_FINISHED;
	case GNUTLS_E_AGAIN:
	case GNUTLS_E_INTERRUPTED:
		tls_socket_evt_change(s, gnutls_record_get_direction(session)
				? INPUT_EVENT_WX : INPUT_EVENT_RX);
		return TLS_HANDSHAKE_RETRY;
	}
	if (tls_debug) {
		g_warning("gnutls_handshake() failed: %s",
				gnutls_strerror(ret));
	}
	return TLS_HANDSHAKE_ERROR;
}

/**
 * Initiates a new TLS session.
 *
 * @param is_incoming Whether this is an incoming connection.
 * @return The session pointer on success; NULL on failure.
 */
tls_context_t
tls_init(gboolean is_incoming)
{
	static const int cipher_list[] = {
		GNUTLS_CIPHER_AES_256_CBC, GNUTLS_CIPHER_AES_128_CBC,
		0
	};
	static const int kx_list[] = {
		GNUTLS_KX_ANON_DH,
		GNUTLS_KX_RSA,
		GNUTLS_KX_DHE_DSS,
		GNUTLS_KX_DHE_RSA,
		0
	};
	static const int mac_list[] = {
		GNUTLS_MAC_MD5, GNUTLS_MAC_SHA, GNUTLS_MAC_RMD160,
		0
	};
	static const int comp_list[] = {
		GNUTLS_COMP_DEFLATE, GNUTLS_COMP_NULL,
		0
	};
	static const int cert_list[] = {
		GNUTLS_CRT_X509, GNUTLS_CRT_OPENPGP,
		0
	};
	struct tls_context *ctx;

	ctx = walloc0(sizeof *ctx);

	if (is_incoming) {

		if (gnutls_anon_allocate_server_credentials(&ctx->server_cred)) {
			g_warning("gnutls_anon_allocate_server_credentials() failed");
			return NULL;
		}
		gnutls_anon_set_server_dh_params(ctx->server_cred, get_dh_params());

		if (gnutls_init(&ctx->session, GNUTLS_SERVER)) {
			g_warning("gnutls_init() failed");
			return NULL;
		}
		gnutls_dh_set_prime_bits(ctx->session, TLS_DH_BITS);

		if (gnutls_credentials_set(ctx->session,
				GNUTLS_CRD_ANON, ctx->server_cred)) {
			g_warning("gnutls_credentials_set() failed");
			return NULL;
		}

		if (server_cert_cred) {
			if (gnutls_credentials_set(ctx->session,
					GNUTLS_CRD_CERTIFICATE, server_cert_cred)) {
				g_warning("gnutls_credentials_set() failed");
				return NULL;
			}
		}
	} else {
		if (gnutls_anon_allocate_client_credentials(&ctx->client_cred)) {
			g_warning("gnutls_anon_allocate_client_credentials() failed");
			return NULL;
		}
		if (gnutls_init(&ctx->session, GNUTLS_CLIENT)) {
			g_warning("gnutls_init() failed");
			return NULL;
		}
		if (gnutls_credentials_set(ctx->session,
				GNUTLS_CRD_ANON, ctx->client_cred)) {
			g_warning("gnutls_credentials_set() failed");
			return NULL;
		}
	}

	gnutls_set_default_priority(ctx->session);
	if (gnutls_cipher_set_priority(ctx->session, cipher_list)) {
		g_warning("gnutls_cipher_set_priority() failed");
		return NULL;
	}
	if (gnutls_kx_set_priority(ctx->session, kx_list)) {
		g_warning("gnutls_kx_set_priority() failed");
		return NULL;
	}
	if (gnutls_mac_set_priority(ctx->session, mac_list)) {
		g_warning("gnutls_mac_set_priority() failed");
		return NULL;
	}
	if (gnutls_certificate_type_set_priority(ctx->session, cert_list)) {
		g_warning("gnutls_certificate_type_set_priority() failed");
		return NULL;
	}
	if (gnutls_compression_set_priority(ctx->session, comp_list)) {
		g_warning("gnutls_compression_set_priority() failed");
		return NULL;
	}
#ifdef XXX_CUSTOM_PUSH_PULL
	gnutls_transport_set_ptr(ctx->session, NULL);
	gnutls_transport_set_push_function(ctx->session, tls_push);
	gnutls_transport_set_pull_function(ctx->session, tls_pull);
#endif /* XXX_CUSTOM_PUSH_PULL */
	return ctx;
}

void
tls_bye(tls_context_t ctx, gboolean is_incoming)
{
	g_return_if_fail(ctx);
	gnutls_bye(ctx->session, is_incoming ? GNUTLS_SHUT_WR : GNUTLS_SHUT_RDWR);
}

void
tls_free(tls_context_t *ctx_ptr)
{
	tls_context_t ctx;

	g_assert(ctx_ptr);
	ctx = *ctx_ptr;
	if (ctx) {
		gnutls_deinit(ctx->session);
		if (ctx->server_cred) {
			gnutls_anon_free_server_credentials(ctx->server_cred);
			ctx->server_cred = NULL;
		}
		if (ctx->client_cred) {
			gnutls_anon_free_client_credentials(ctx->client_cred);
			ctx->client_cred = NULL;
		}
		*ctx_ptr = NULL;
	}
}

void
tls_global_init(void)
{
	static const struct {
		const char * const name;
		const int major;
		const int minor;
	} f = {
		"tls", 1, 0
	};
	char *cert_file, *key_file;
	int ret;

	if (gnutls_global_init()) {
		g_error("gnutls_global_init() failed");
	}
	get_dh_params();

	key_file = make_pathname(settings_config_dir(), "key.pem");
	cert_file = make_pathname(settings_config_dir(), "cert.pem");
	
	gnutls_certificate_allocate_credentials(&server_cert_cred);
	ret = gnutls_certificate_set_x509_key_file(server_cert_cred,
			cert_file, key_file, GNUTLS_X509_FMT_PEM);
	if (ret < 0) {
		g_warning("gnutls_certificate_set_x509_key_file() failed: %s",
			gnutls_strerror(ret));
		gnutls_certificate_free_credentials(server_cert_cred);
		server_cert_cred = NULL;
	} else {
		gnutls_certificate_set_dh_params(server_cert_cred, get_dh_params());
	}
	G_FREE_NULL(key_file);
	G_FREE_NULL(cert_file);

	header_features_add(&xfeatures.connections, f.name, f.major, f.minor);
	header_features_add(&xfeatures.downloads, f.name, f.major, f.minor);
	header_features_add(&xfeatures.uploads, f.name, f.major, f.minor);
}

static ssize_t
tls_write(struct wrap_io *wio, gconstpointer buf, size_t size)
{
	inputevt_cond_t cond = INPUT_EVENT_WX;
	struct gnutella_socket *s = wio->ctx;
	const char *p;
	size_t len;
	ssize_t ret;

	g_assert(size <= INT_MAX);
	g_assert(s != NULL);
	g_assert(buf != NULL);

	g_assert(SOCKET_USES_TLS(s));

	if (0 != s->tls.snarf) {
		p = NULL;
		len = 0;
	} else {
		p = buf;
		len = size;
		g_assert(NULL != p && 0 != len);
	}

	ret = gnutls_record_send(tls_socket_get_session(s), p, len);
	if (ret <= 0) {
		switch (ret) {
		case 0:
			break;
		case GNUTLS_E_INTERRUPTED:
		case GNUTLS_E_AGAIN:
			cond = gnutls_record_get_direction(tls_socket_get_session(s))
					? INPUT_EVENT_WX : INPUT_EVENT_RX;

			if (0 == s->tls.snarf) {
				s->tls.snarf = len;
				ret = len;
			} else {
				errno = VAL_EAGAIN;
				ret = -1;
			}
			break;
		case GNUTLS_E_PULL_ERROR:
		case GNUTLS_E_PUSH_ERROR:
			if (tls_debug)
				g_message("socket_tls_write(): errno=\"%s\"",
					g_strerror(errno));
			errno = EIO;
			ret = -1;
			break;
		default:
			gnutls_perror(ret);
			errno = EIO;
			ret = -1;
		}
	} else {
		if (0 != s->tls.snarf) {
			s->tls.snarf -= ret;
			errno = VAL_EAGAIN;
			ret = -1;
		}
	}

	if (s->gdk_tag)
		tls_socket_evt_change(s, cond);

	g_assert(ret == (ssize_t) -1 || (size_t) ret <= size);
	return ret;
}

static ssize_t
tls_read(struct wrap_io *wio, gpointer buf, size_t size)
{
	inputevt_cond_t cond = INPUT_EVENT_RX;
	struct gnutella_socket *s = wio->ctx;
	ssize_t ret;

	g_assert(size <= INT_MAX);
	g_assert(s != NULL);
	g_assert(buf != NULL);

	g_assert(SOCKET_USES_TLS(s));

	ret = gnutls_record_recv(tls_socket_get_session(s), buf, size);
	if (ret < 0) {
		switch (ret) {
		case GNUTLS_E_INTERRUPTED:
		case GNUTLS_E_AGAIN:
			cond = gnutls_record_get_direction(tls_socket_get_session(s))
					? INPUT_EVENT_WX : INPUT_EVENT_RX;
			errno = VAL_EAGAIN;
			break;
		case GNUTLS_E_PULL_ERROR:
		case GNUTLS_E_PUSH_ERROR:
			if (tls_debug)
				g_message("socket_tls_read(): errno=\"%s\"",
					g_strerror(errno));
			errno = EIO;
			break;
		default:
			gnutls_perror(ret);
			errno = EIO;
		}
		ret = -1;
	}

	if (s->gdk_tag)
		tls_socket_evt_change(s, cond);

	g_assert(ret == (ssize_t) -1 || (size_t) ret <= size);
	return ret;
}

static ssize_t
tls_writev(struct wrap_io *wio, const struct iovec *iov, int iovcnt)
{
	inputevt_cond_t cond = INPUT_EVENT_WX;
	struct gnutella_socket *s = wio->ctx;
	ssize_t ret, written;
	int i;

	g_assert(SOCKET_USES_TLS(s));
	g_assert(iovcnt > 0);

	if (0 != s->tls.snarf) {
		ret = gnutls_record_send(tls_socket_get_session(s), NULL, 0);
		if (ret > 0) {
			g_assert((ssize_t) s->tls.snarf >= ret);
			s->tls.snarf -= ret;
			if (0 != s->tls.snarf) {
				errno = VAL_EAGAIN;
				ret = -1;
				goto done;
			}
		} else {
			switch (ret) {
			case 0:
				ret = 0;
				goto done;
			case GNUTLS_E_INTERRUPTED:
			case GNUTLS_E_AGAIN:
				cond = gnutls_record_get_direction(tls_socket_get_session(s))
						? INPUT_EVENT_WX : INPUT_EVENT_RX;
				errno = VAL_EAGAIN;
				break;
			case GNUTLS_E_PULL_ERROR:
			case GNUTLS_E_PUSH_ERROR:
				if (tls_debug)
					g_message("socket_tls_writev(): errno=\"%s\"",
						g_strerror(errno));
				errno = EIO;
				break;
			default:
				gnutls_perror(ret);
				errno = EIO;
			}
			ret = -1;
			goto done;
		}
	}

	ret = -2;	/* Shut the compiler: iovcnt could still be 0 */
	written = 0;
	for (i = 0; i < iovcnt; ++i) {
		char *p;
		size_t len;

		p = iov[i].iov_base;
		len = iov[i].iov_len;
		g_assert(NULL != p && 0 != len);
		ret = gnutls_record_send(tls_socket_get_session(s), p, len);
		if (ret <= 0) {
			switch (ret) {
			case 0:
				ret = written;
				break;
			case GNUTLS_E_INTERRUPTED:
			case GNUTLS_E_AGAIN:
				cond = gnutls_record_get_direction(tls_socket_get_session(s))
						? INPUT_EVENT_WX : INPUT_EVENT_RX;
				s->tls.snarf = len;
				ret = written + len;
				break;
			case GNUTLS_E_PULL_ERROR:
			case GNUTLS_E_PUSH_ERROR:
				if (tls_debug)
					g_message("socket_tls_writev(): errno=\"%s\"",
						g_strerror(errno));
				ret = -1;
				break;
			default:
				gnutls_perror(ret);
				errno = EIO;
				ret = -1;
			}

			break;
		}

		written += ret;
		ret = written;
	}

done:
	if (s->gdk_tag)
		tls_socket_evt_change(s, cond);

	g_assert(ret == (ssize_t) -1 || ret >= 0);
	return ret;
}

static ssize_t
tls_readv(struct wrap_io *wio, struct iovec *iov, int iovcnt)
{
	inputevt_cond_t cond = INPUT_EVENT_RX;
	struct gnutella_socket *s = wio->ctx;
	int i;
	size_t rcvd = 0;
	ssize_t ret;

	g_assert(SOCKET_USES_TLS(s));
	g_assert(iovcnt > 0);

	ret = 0;	/* Shut the compiler: iovcnt could still be 0 */
	for (i = 0; i < iovcnt; ++i) {
		size_t len;
		char *p;

		p = iov[i].iov_base;
		len = iov[i].iov_len;
		g_assert(NULL != p && 0 != len);
		ret = gnutls_record_recv(tls_socket_get_session(s), p, len);
		if (ret > 0) {
			rcvd += ret;
		}
		if ((size_t) ret != len) {
			break;
		}
	}

	if (ret >= 0) {
		ret = rcvd;
	} else {
		switch (ret) {
		case GNUTLS_E_INTERRUPTED:
		case GNUTLS_E_AGAIN:
			cond = gnutls_record_get_direction(tls_socket_get_session(s))
					? INPUT_EVENT_WX : INPUT_EVENT_RX;
			if (0 != rcvd) {
				ret = rcvd;
			} else {
				errno = VAL_EAGAIN;
				ret = -1;
			}
			break;
		case GNUTLS_E_PULL_ERROR:
		case GNUTLS_E_PUSH_ERROR:
			if (tls_debug)
				g_message("socket_tls_readv(): errno=\"%s\"",
					g_strerror(errno));
			errno = EIO;
			ret = -1;
			break;
		default:
			gnutls_perror(ret);
			errno = EIO;
			ret = -1;
		}
	}

	if (s->gdk_tag)
		tls_socket_evt_change(s, cond);

	g_assert(ret == (ssize_t) -1 || ret >= 0);
	return ret;
}

static ssize_t
tls_no_sendto(struct wrap_io *unused_wio, const gnet_host_t *unused_to,
	gconstpointer unused_buf, size_t unused_size)
{
	(void) unused_wio;
	(void) unused_to;
	(void) unused_buf;
	(void) unused_size;
	g_error("no sendto() routine allowed");
	return -1;
}

void
tls_wio_link(struct wrap_io *wio)
{
	g_assert(wio);	
	wio->write = tls_write;
	wio->read = tls_read;
	wio->writev = tls_writev;
	wio->readv = tls_readv;
	wio->sendto = tls_no_sendto;
}

#else	/* !HAS_GNUTLS*/

enum tls_handshake_result
tls_handshake(tls_context_t ctx)
{
	(void) ctx;
	return TLS_HANDSHAKE_FINISHED;
}

tls_ctx_t
tls_init(gboolean is_incoming)
{
	(void) is_incoming;
	return NULL;
}

void
tls_global_init(void)
{
	/* Nothing to do */
}

#endif	/* HAS_GNUTLS */

/* vi: set ts=4 sw=4 cindent: */
