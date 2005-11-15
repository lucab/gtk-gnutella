/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
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

#ifndef _if_core_search_h_
#define _if_core_search_h_

#include "common.h"

#include "lib/misc.h"
#include "lib/vendors.h"
#include "if/core/nodes.h"

/***
 *** Searches
 ***/
typedef guint32 gnet_search_t;

/*
 * Flags for search_new()
 */
#define SEARCH_PASSIVE	 (1 << 0)	/**< Start a passive ssearch */
#define SEARCH_ENABLED	 (1 << 1)	/**< Start an enabled search */
#define SEARCH_BROWSE	 (1 << 2)	/**< Start a browse-host search */

/*
 * Host vectors held in query hits.
 */
typedef struct gnet_host_vec {
	gnet_host_t *hvec;		/**< Vector of alternate locations */
	gint hvcnt;				/**< Amount of hosts in vector */
} gnet_host_vec_t;

/*
 * Result sets `status' flags.
 */
#define ST_TLS				(1 << 10)	/**< Indicated support for TLS */
#define ST_BH				(1 << 9)	/**< Browse Host support */
#define ST_KNOWN_VENDOR		(1 << 8)	/**< Found known vendor code */
#define ST_PARSED_TRAILER	(1 << 7)	/**< Was able to parse trailer */
#define ST_UDP				(1 << 6)	/**< Got hit via UDP */
#define ST_BOGUS			(1 << 5)	/**< Bogus IP address */
#define ST_PUSH_PROXY		(1 << 4)	/**< Listed some push proxies */
#define ST_GGEP				(1 << 3)	/**< Trailer has a GGEP extension */
#define ST_UPLOADED			(1 << 2)	/**< Is "stable", people downloaded */
#define ST_BUSY				(1 << 1)	/**< Has currently no slots */
#define ST_FIREWALL			(1 << 0)	/**< Is behind a firewall */

/*
 * Processing of ignored files.
 */
#define SEARCH_IGN_DISPLAY_AS_IS	0	/**< Display normally */
#define SEARCH_IGN_DISPLAY_MARKED	1	/**< Display marked (lighter color) */
#define SEARCH_IGN_NO_DISPLAY		2	/**< Don't display */

/**
 * A results_set structure factorizes the common information from a Query Hit
 * packet, and then has a list of individual records, one for each hit.
 *
 * A single structure is created for each Query Hit packet we receive, but
 * then it can be dispatched for displaying some of its records to the
 * various searches in presence.
 */
typedef struct gnet_results_set {
	gchar *guid;				/**< Servent's GUID (atom) */
	host_addr_t addr;
	guint16 port;
	guint16 status;				/**< Parsed status bits from trailer */
	guint32 speed;
	time_t  stamp;				/**< Reception time of the hit */
	union vendor_code vcode;	/**< Vendor code */
	gchar *version;				/**< Version information (atom) */
	gint country;				/**< Country code -- encoded ISO3166 */
    flag_t  flags;
	gnet_host_vec_t *proxies;	/**< Optional: known push proxies */
	gchar *hostname;			/**< Optional: server's hostname */
	host_addr_t udp_addr;		/**< IP of delivering node, if hit from UDP */

	GSList *records;
	guint32 num_recs;
} gnet_results_set_t;

/*
 * Result record flags
 */
#define SR_DOWNLOADED	0x0001
#define SR_IGNORED		0x0002
#define SR_DONT_SHOW	0x0004

/**
 * An individual hit.  It referes to a file entry on the remote servent,
 * as identified by the parent results_set structure that contains this hit.
 */
typedef struct gnet_record {
	gchar  *name;				/**< File name */
	filesize_t size;			/**< Size of file, in bytes */
	guint32 index;				/**< Index for GET command */
	gchar  *sha1;				/**< SHA1 URN (binary form, atom) */
	gchar  *tag;				/**< Optional tag data string (atom) */
	gchar  *xml;				/**< Optional XML data string (atom) */
	gnet_host_vec_t *alt_locs;	/**< Optional: known alternate locations */
    flag_t  flags;
} gnet_record_t;


/**
 * Search callbacks
 */
typedef void (*search_got_results_listener_t)
    (GSList *, const gnet_results_set_t *);

/*
 * Search public interface, visible only from the bridge.
 */

#ifdef CORE_SOURCES

gnet_search_t search_new(const gchar *, time_t create_time, guint lifetime,
		guint32 timeout, flag_t flags);
void search_close(gnet_search_t sh);

void search_start(gnet_search_t sh);
void search_stop(gnet_search_t sh);

/*  search_is_stopped doesn't exist yet!
gboolean search_is_stopped(gnet_search_t sh);
*/

void search_reissue(gnet_search_t sh);
void search_add_kept(gnet_search_t sh, guint32 kept);

gboolean search_is_passive(gnet_search_t sh);
gboolean search_is_active(gnet_search_t sh);
gboolean search_is_frozen(gnet_search_t sh);
gboolean search_is_expired(gnet_search_t sh);

void search_set_reissue_timeout(gnet_search_t sh, guint32 timeout);
guint32 search_get_reissue_timeout(gnet_search_t sh);
guint search_get_lifetime(gnet_search_t sh);
time_t search_get_create_time(gnet_search_t sh);
void search_set_create_time(gnet_search_t sh, time_t t);

void search_free_alt_locs(gnet_record_t *rc);
void search_free_proxies(gnet_results_set_t *rs);

void search_update_items(gnet_search_t sh, guint32 items);

gboolean search_browse(gnet_search_t sh,
	const gchar *hostname, host_addr_t addr, guint16 port,
	const gchar *guid, gboolean push, const gnet_host_vec_t *proxies);

#endif /* CORE_SOURCES */
#endif /* _if_core_search_h_ */

/* vi: set ts=4 sw=4 cindent: */
