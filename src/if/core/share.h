/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi
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

#ifndef _if_core_share_h_
#define _if_core_share_h_

#include "lib/host_addr.h"

/**
 * Search query types.
 */
typedef enum {
    QUERY_STRING,
    QUERY_SHA1
} query_type_t;

/**
 * Sharing callbacks.
 */

typedef void (*search_request_listener_t) (
    query_type_t, const gchar *query, const host_addr_t addr, guint16);

/*
 * Public interface, visible from the bridge.
 */

#ifdef CORE_SOURCES

void shared_dir_add(const gchar *);
void share_scan(void);
guint64 shared_files_scanned(void);
guint64 shared_kbytes_scanned(void);
void share_add_search_request_listener(search_request_listener_t l);
void share_remove_search_request_listener(search_request_listener_t l);

#endif /* CORE_SOURCES */
#endif /* _if_core_share_h */

/* vi: set ts=4 sw=4 cindent: */
