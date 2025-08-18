/*
 * TLS module
 *
 * Copyright (C) 2005 iptelorg GmbH
 * Copyright (C) 2013 Motorola Solutions, Inc.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*!
 * \file
 * \brief Kamailio TLS support :: Common functions
 * \ingroup tls
 * Module: \ref tls
 */


#define _GNU_SOURCE 1 /* Needed for strndup */

#include <string.h>
#include <stdio.h>
#include <libgen.h>
#include "../../core/mem/shm_mem.h"
#include "../../core/globals.h"
#include "../../core/dprint.h"
#include "../../core/ip_addr.h"
#include "../../core/socket_info.h"
#include "../../core/udp_server.h"
#include "../../core/forward.h"
#include "../../core/resolve.h"

#include "tls_mod.h"
#include "tls_util.h"


extern int *ksr_tls_keylog_mode;
extern str ksr_tls_keylog_file;
extern str ksr_tls_keylog_peer;

static gen_lock_t *ksr_tls_keylog_file_lock = NULL;
static dest_info_t ksr_tls_keylog_peer_dst;

/*
 * Make a shared memory copy of ASCII zero terminated string
 * Return value: -1 on error
 *                0 on success
 */
int shm_asciiz_dup(char **dest, char *val)
{
	char *ret;
	int len;

	if(!val) {
		*dest = NULL;
		return 0;
	}

	len = strlen(val);
	ret = shm_malloc(len + 1);
	if(!ret) {
		ERR("No memory left\n");
		return -1;
	}
	memcpy(ret, val, len + 1);
	*dest = ret;
	return 0;
}


/*
 * Delete old TLS configuration that is not needed anymore
 */
void collect_garbage(void)
{
	tls_domains_cfg_t *prev, *cur, *next;

	/* Make sure we do not run two garbage collectors
	      * at the same time
	      */
	lock_get(tls_domains_cfg_lock);

	/* Skip the current configuration, garbage starts
	      * with the 2nd element on the list
	      */
	prev = *tls_domains_cfg;
	cur = (*tls_domains_cfg)->next;

	while(cur) {
		next = cur->next;
		if(atomic_get(&cur->ref_count) == 0) {
			/* Not referenced by any existing connection */
			prev->next = cur->next;
			tls_free_cfg(cur);
		} else {
			/* Only update prev if we didn't remove cur */
			prev = cur;
		}
		cur = next;
	}

	lock_release(tls_domains_cfg_lock);
}

/*
 * Get any leftover errors from OpenSSL and print them.
 * ERR_get_error() also removes the error from the OpenSSL error stack.
 * This is useful to call before any SSL_* IO calls to make sure
 * we don't have any leftover errors from previous calls (OpenSSL docs).
 */
void tls_openssl_clear_errors(void)
{
	int i;
	char err[256];
	while((i = ERR_get_error())) {
		ERR_error_string(i, err);
		INFO("clearing leftover error before SSL_* calls: %s\n", err);
	}
}

/**
 *
 */
int ksr_tls_keylog_file_init(void)
{
	if(ksr_tls_keylog_mode == NULL) {
		return 0;
	}
	if(!((*ksr_tls_keylog_mode & KSR_TLS_KEYLOG_MODE_INIT)
			   && (*ksr_tls_keylog_mode & KSR_TLS_KEYLOG_MODE_FILE))) {
		return 0;
	}
	if(ksr_tls_keylog_file.s == NULL || ksr_tls_keylog_file.len <= 0) {
		return -1;
	}
	if(ksr_tls_keylog_file_lock != NULL) {
		return 0;
	}
	ksr_tls_keylog_file_lock = lock_alloc();
	if(ksr_tls_keylog_file_lock == NULL) {
		return -2;
	}
	if(lock_init(ksr_tls_keylog_file_lock) == NULL) {
		return -3;
	}
	return 0;
}

/**
 *
 */
/* clang-format off */
static const char *ksr_tls_keylog_vfilters[] = {
	"CLIENT_RANDOM ",
	"CLIENT_HANDSHAKE_TRAFFIC_SECRET ",
	"SERVER_HANDSHAKE_TRAFFIC_SECRET ",
	"EXPORTER_SECRET ",
	"CLIENT_TRAFFIC_SECRET_0 ",
	"SERVER_TRAFFIC_SECRET_0 ",
	NULL
};
/* clang-format on */

/**
 *
 */
int ksr_tls_keylog_vfilter_match(const char *line)
{
	int i;

	for(i = 0; ksr_tls_keylog_vfilters[i] != NULL; i++) {
		if(strcasecmp(ksr_tls_keylog_vfilters[i], line) == 0) {
			return 1;
		}
	}
	return 0;
}

/**
 *
 */
int ksr_tls_keylog_file_write(const SSL *ssl, const char *line)
{
	FILE *lf = NULL;
	int ret = 0;

	if(ksr_tls_keylog_file_lock == NULL) {
		return 0;
	}

	lock_get(ksr_tls_keylog_file_lock);
	lf = fopen(ksr_tls_keylog_file.s, "a");
	if(lf) {
		fprintf(lf, "%s\n", line);
		fclose(lf);
	} else {
		LM_ERR("failed to open keylog file: %s\n", ksr_tls_keylog_file.s);
		ret = -1;
	}
	lock_release(ksr_tls_keylog_file_lock);
	return ret;
}


/**
 *
 */
int ksr_tls_keylog_peer_init(void)
{
	int proto;
	str host;
	int port;

	if(ksr_tls_keylog_mode == NULL) {
		return 0;
	}
	if(!((*ksr_tls_keylog_mode & KSR_TLS_KEYLOG_MODE_INIT)
			   && (*ksr_tls_keylog_mode & KSR_TLS_KEYLOG_MODE_PEER))) {
		return 0;
	}
	if(ksr_tls_keylog_peer.s == NULL || ksr_tls_keylog_peer.len <= 0) {
		return -1;
	}
	init_dest_info(&ksr_tls_keylog_peer_dst);
	if(parse_phostport(ksr_tls_keylog_peer.s, &host.s, &host.len, &port, &proto)
			!= 0) {
		LM_CRIT("invalid peer addr parameter <%s>\n", ksr_tls_keylog_peer.s);
		return -2;
	}
	if(proto != PROTO_UDP) {
		LM_ERR("only udp supported in peer addr <%s>\n", ksr_tls_keylog_peer.s);
		return -3;
	}
	ksr_tls_keylog_peer_dst.proto = proto;
	if(sip_hostport2su(&ksr_tls_keylog_peer_dst.to, &host, port,
			   &ksr_tls_keylog_peer_dst.proto)
			!= 0) {
		LM_ERR("failed to resolve <%s>\n", ksr_tls_keylog_peer.s);
		return -4;
	}

	return 0;
}

/**
 *
 */
int ksr_tls_keylog_peer_send(const SSL *ssl, const char *line)
{
	if(ksr_tls_keylog_mode == NULL) {
		return 0;
	}
	if(!((*ksr_tls_keylog_mode & KSR_TLS_KEYLOG_MODE_INIT)
			   && (*ksr_tls_keylog_mode & KSR_TLS_KEYLOG_MODE_PEER))) {
		return 0;
	}

	if(ksr_tls_keylog_peer_dst.send_sock == NULL) {
		ksr_tls_keylog_peer_dst.send_sock =
				get_send_socket(NULL, &ksr_tls_keylog_peer_dst.to, PROTO_UDP);
		if(ksr_tls_keylog_peer_dst.send_sock == NULL) {
			LM_ERR("no send socket for <%s>\n", ksr_tls_keylog_peer.s);
			return -2;
		}
	}

	if(udp_send(&ksr_tls_keylog_peer_dst, (char *)line, strlen(line)) < 0) {
		LM_ERR("failed to send to <%s>\n", ksr_tls_keylog_peer.s);
		return -1;
	}
	return 0;
}
