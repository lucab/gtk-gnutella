/*
 * $Id$
 *
 * Copyright (c) 2008, Raphael Manfredi
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
 * @ingroup dht
 * @file
 *
 * Local values management.
 *
 * This file is managing values stored under our keys.
 *
 * A key that we manage can hold several values.  We distinguish these values
 * by using a secondary key: the KUID of the creator of the value.  This means
 * a given node can only publish one value per key, which is OK considering
 * the 2^160 keyspace.
 *
 * Naturally, different things need to be published under different keys.
 * Defining how one maps something to a key is a global architecture decision,
 * otherwise nobody but the original publisher can find the information and
 * process it for what it is.
 *
 * To prevent abuse, we keep track of the amount of values (whatever the key)
 * published locally by a given IP address and by class C networks (/24) and
 * define a reasonable maximum for each.
 *
 * Values are bounded to a maximum size, which is node-dependent. For GTKG,
 * this is hardwired to 512 bytes and non-configurable.
 *
 * Each key tracks the amount of values stored under it and will not accept
 * more than a certain (small) amount, before denying storage for that key.
 * This is to achieve load balancing in the DHT and to let publishers know
 * that a given key is "popular".  A specific error code is returned when
 * the node is "full" for the key.
 *
 * Storage of values is not done in core but offloaded to disk. Only essential
 * information about the values is kept in RAM, mainly to handle limits and
 * data expiration.
 *
 * @author Raphael Manfredi
 * @date 2008
 */

#include "common.h"

RCSID("$Id$")

#include "values.h"
#include "kuid.h"
#include "knode.h"
#include "storage.h"
#include "acct.h"
#include "keys.h"

#include "if/gnet_property.h"
#include "if/gnet_property_priv.h"

#include "lib/atoms.h"
#include "lib/bstr.h"
#include "lib/dbmap.h"
#include "lib/dbmw.h"
#include "lib/host_addr.h"
#include "lib/misc.h"
#include "lib/pmsg.h"
#include "lib/tm.h"
#include "lib/vendors.h"
#include "lib/walloc.h"
#include "lib/override.h"		/* Must be the last header included */

#define MAX_VALUES		65536	/**< Max # of values we accept to manage */
#define MAX_VALUES_IP	16		/**< Max # of values allowed per IP address */
#define MAX_VALUES_NET	256		/**< Max # of values allowed per class C net */

/**
 * Information about a value that is stored to disk and not kept in memory.
 * The structure is serialized first, not written as-is.
 *
 * We do store the so-called secondary key here as well to allow traversal
 * of the values without going through the keys first.
 *
 * NB: the actual value is stored separately in a dedicated database, indexed
 * by the same 64-bit key as the valuedata.  Two reasons for this:
 * 1) SDBM puts constraints on the total size of key+value (<= 1008 bytes).
 *    Our local key is 8 bytes, that leaves 1000 bytes at most for values.
 * 2) The access pattern is going to be different.  We shall access the meta
 *    information more often than the value itself and we don't want to
 *    read and deserialize the actual value each time, or write it back when
 *    the meta information are updated.
 */
struct valuedata {
	kuid_t id;					/**< The primary key of the value */
	time_t publish;				/**< Initial publish time at our node */
	time_t replicated;			/**< Last replication time */
	time_t expire;				/**< Expiration time */
	/* Creator information */
	kuid_t cid;					/**< The "secondary key" of the value */
	vendor_code_t vcode;		/**< Vendor code who created the info */
	host_addr_t addr;			/**< IP address of creator */
	guint16 port;				/**< Port number of creator */
	guint8 major;				/**< Version major of creator */
	guint8 minor;				/**< Version minor of creator */
	/* Value information */
	dht_value_type_t type;		/**< Type of value stored */
	guint8 value_major;			/**< Major version of value */
	guint8 value_minor;			/**< Minor version of value */
	guint16 length;				/**< Value length */
	gboolean original;			/**< Whether we got data from creator */
};

/**
 * Internal counter used to assign DB keys to the values we're storing.
 * These access keys are retrievable from another DB indexed by the primary
 * key of the value (see keys.c).
 */
static guint64 valueid = 1;		/* 0 is not a valid key (used as marker) */

/**
 * Total amount of values currently managed.
 */
static int values_managed = 0;

/**
 * Counts number of values currently stored per IPv4 address and per class C
 * network.
 */
static GHashTable *values_per_ip;
static GHashTable *values_per_class_c;

/**
 * DBM wrapper to store valuedata.
 */
static dbmw_t *db_valuedata;
static char db_valbase[] = "dht_values";
static char db_valwhat[] = "DHT value data";

/**
 * DBM wrapper to store actual data.
 */
static dbmw_t *db_rawdata;
static char db_rawbase[] = "dht_raw";
static char db_rawwhat[] = "DHT raw data";

/**
 * @return amount of values managed.
 */
size_t
values_count(void)
{
	g_assert(values_managed >= 0);

	return (size_t) values_managed;
}

/**
 * Serialization routine for valuedata.
 */
static void
serialize_valuedata(pmsg_t *mb, gconstpointer data)
{
	const struct valuedata *vd = data;

	pmsg_write(mb, vd->id.v, KUID_RAW_SIZE);
	pmsg_write_time(mb, vd->publish);
	pmsg_write_time(mb, vd->replicated);
	pmsg_write_time(mb, vd->expire);
	/* Creator information */
	pmsg_write(mb, vd->cid.v, KUID_RAW_SIZE);
	pmsg_write_be32(mb, vd->vcode.u32);
	pmsg_write_ipv4_or_ipv6_addr(mb, vd->addr);
	pmsg_write_be16(mb, vd->port);
	pmsg_write_u8(mb, vd->major);
	pmsg_write_u8(mb, vd->minor);
	/* Value information */
	pmsg_write_be32(mb, vd->type);
	pmsg_write_u8(mb, vd->value_major);
	pmsg_write_u8(mb, vd->value_minor);
	pmsg_write_be16(mb, vd->length);
	pmsg_write_boolean(mb, vd->original);
}

/**
 * Deserialization routine for valuedata.
 */
static gboolean
deserialize_valuedata(bstr_t *bs, gpointer valptr, size_t len)
{
	struct valuedata *vd = valptr;

	g_assert(sizeof *vd == len);

	bstr_read(bs, vd->id.v, KUID_RAW_SIZE);
	bstr_read_time(bs, &vd->publish);
	bstr_read_time(bs, &vd->replicated);
	bstr_read_time(bs, &vd->expire);
	/* Creator information */
	bstr_read(bs, vd->cid.v, KUID_RAW_SIZE);
	bstr_read_be32(bs, &vd->vcode.u32);
	bstr_read_packed_ipv4_or_ipv6_addr(bs, &vd->addr);
	bstr_read_be16(bs, &vd->port);
	bstr_read_u8(bs, &vd->major);
	bstr_read_u8(bs, &vd->minor);
	/* Value information */
	bstr_read_be32(bs, &vd->type);
	bstr_read_u8(bs, &vd->value_major);
	bstr_read_u8(bs, &vd->value_minor);
	bstr_read_be16(bs, &vd->length);
	bstr_read_boolean(bs, &vd->original);

	if (bstr_has_error(bs))
		return FALSE;
	else if (bstr_unread_size(bs)) {
		/* Something is wrong, we're not deserializing the right data */
		g_warning("DHT deserialization of valuedata: has %lu unread bytes",
			(gulong) bstr_unread_size(bs));
		return FALSE;
	}

	return TRUE;
}

/**
 * Create a DHT value.
 *
 * @param creator		the creator of the value
 * @param id			the primary key of the value
 * @param type			the DHT value type code
 * @param major			the data format major version
 * @param minor			the data format minor version
 * @param data			walloc()'ed data, or NULL if length > VALUE_MAX_LEN
 * @param length		length of the data, as read from network
 */
dht_value_t *
dht_value_make(const knode_t *creator,
	kuid_t *primary_key, dht_value_type_t type,
	guint8 major, guint8 minor, gpointer data, guint16 length)
{
	dht_value_t *v;

	g_assert(length <= VALUE_MAX_LEN || NULL == data);
	g_assert(length || NULL == data);

	v = walloc(sizeof *v);
	v->creator = knode_refcnt_inc(creator);
	v->id = kuid_get_atom(primary_key);
	v->type = type;
	v->major = major;
	v->minor = minor;
	v->data = data;
	v->length = length;

	return v;
}

/**
 * Free DHT value, optionally freeing the data as well.
 */
void
dht_value_free(dht_value_t *v, gboolean free_data)
{
	g_assert(v);

	knode_free(deconstify_gpointer(v->creator));
	kuid_atom_free_null(&v->id);

	if (free_data && v->data) {
		g_assert(v->length && v->length <= VALUE_MAX_LEN);
		wfree(deconstify_gpointer(v->data), v->length);
	}

	wfree(v, sizeof *v);
}

/**
 * Make up a printable version of the DHT value type.
 *
 * @param type	a 4-letter DHT value type
 * @param buf	the destination buffer to hold the result
 * @param size	size of buf in bytes
 *
 * @return length of the resulting string before potential truncation.
 */
size_t
dht_value_type_to_string_buf(guint32 type, char *buf, size_t size)
{
	if (type == DHT_VT_BINARY) {
		return g_strlcpy(buf, "BIN.", size);
	} else {
		char tmp[5];
		size_t i;

		poke_be32(&tmp[0], type);

		for (i = 0; i < G_N_ELEMENTS(tmp) - 1; i++) {
			if (!is_ascii_print(tmp[i]))
				tmp[i] = '.';
		}
		tmp[4] = '\0';
		return g_strlcpy(buf, tmp, size);
	}
}

/**
 * Make up a printable version of the DHT value type.
 *
 * @return pointer to static data
 */
const char *
dht_value_type_to_string(guint32 type)
{
	static char buf[5];

	dht_value_type_to_string_buf(type, buf, sizeof buf);
	return buf;
}

/**
 * Make up a printable representation of a DHT value.
 *
 * @return pointer to static data
 */
const char *
dht_value_to_string(const dht_value_t *v)
{
	static char buf[200];
	char knode[128];
	char kuid[KUID_RAW_SIZE * 2 + 1];
	char type[5];

	bin_to_hex_buf(v->id, KUID_RAW_SIZE, kuid, sizeof kuid);
	knode_to_string_buf(v->creator, knode, sizeof knode);
	dht_value_type_to_string_buf(v->type, type, sizeof type);

	gm_snprintf(buf, sizeof buf,
		"value pk=%s as %s v%u.%u (%u byte%s) created by %s",
		kuid, type, v->major, v->minor, v->length, 1 == v->length ? "" : "s",
		knode);

	return buf;
}

/**
 * Get valuedata from database.
 */
static struct valuedata *
get_valuedata(guint64 dbkey)
{
	struct valuedata *vd;

	vd = dbmw_read(db_valuedata, &dbkey, NULL);

	if (vd == NULL) {
		/* XXX Must handle I/O errors correctly */
		if (dbmw_has_ioerr(db_valuedata)) {
			g_warning("DB I/O error, bad things will happen...");
			return NULL;
		}
		g_error("Value under key %s exists but was not found in DB",
			uint64_to_string(dbkey));
	}

	return vd;
}

/**
 * Validate that sender and creator agree on other things than just the
 * KUID: they must agree on everything.
 */
static gboolean
validate_creator(const knode_t *sender, const knode_t *creator)
{
	const char *what;

	if (sender->vcode.u32 != creator->vcode.u32) {
		what = "vendor code";
		goto mismatch;
	}
	if (
		sender->major != creator->major ||
		sender->minor != creator->major
	) {
		what = "version number";
		goto mismatch;
	}
	if (NET_TYPE_IPV4 != host_addr_net(creator->addr)) {
		what = "creator must use an IPv4 address";
		goto wrong;
	}
	if (!host_addr_equal(sender->addr, creator->addr)) {
		what = "IP address";
		goto mismatch;
	}
	if (sender->port != creator->port) {
		what = "port number";
		goto mismatch;
	}

	return TRUE;

mismatch:
	if (GNET_PROPERTY(dht_storage_debug) > 1)
		g_message("DHT STORE %s mismatch between sender %s and creator %s",
			what, knode_to_string(sender), knode_to_string2(creator));

	return FALSE;

wrong:
	if (GNET_PROPERTY(dht_storage_debug) > 1)
		g_message("DHT STORE %s: sender %s and creator %s",
			what, knode_to_string(sender), knode_to_string2(creator));

	return FALSE;
}

/**
 * Validate that we can accept a new value for the key with that creator.
 *
 * @return error code, STORE_SC_OK meaning we can accept the value, any
 * other code being an error condition that must be propagated back.
 */
static guint16
validate_new_acceptable(const dht_value_t *v)
{
	/*
	 * Check whether we have already reached the maximum amount of values
	 * that we accept to store within our node.
	 */

	if (values_managed >= MAX_VALUES)
		return STORE_SC_EXHAUSTED;

	/*
	 * Check creator quotas.
	 */

	{
		int count;
		const knode_t *c = v->creator;

		count = acct_net_get(values_per_class_c, c->addr, NET_CLASS_C_MASK);

		if (GNET_PROPERTY(dht_storage_debug) > 2) {
			guint32 net = host_addr_ipv4(c->addr) & NET_CLASS_C_MASK;

			g_message("DHT STORE has %d/%d value%s for class C network %s",
				count, MAX_VALUES_NET, 1 == count ? "" : "s",
				host_addr_to_string(host_addr_get_ipv4(net)));
		}

		if (count >= MAX_VALUES_NET)
			return STORE_SC_QUOTA;

		count = acct_net_get(values_per_ip, c->addr, NET_IPv4_MASK);

		if (GNET_PROPERTY(dht_storage_debug) > 2)
			g_message("DHT STORE has %d/%d value%s for IP %s",
				count, MAX_VALUES_IP, 1 == count ? "" : "s",
				host_addr_to_string(c->addr));

		if (count >= MAX_VALUES_IP)
			return STORE_SC_QUOTA;
	}

	/*
	 * Check key status: full and loaded attributes.
	 */

	{
		gboolean full;
		gboolean loaded;

		keys_get_status(v->id, &full, &loaded);
		if (full && loaded)
			return STORE_SC_FULL_LOADED;
		else if (full)
			return STORE_SC_FULL;
		else if (loaded)
			return STORE_SC_LOADED;
	}

	return STORE_SC_OK;
}

/**
 * Publish or replicate value in our local data store.
 *
 * @return store status code that will be relayed back to the remote node.
 */
static guint16
values_publish(const knode_t *kn, const dht_value_t *v)
{
	guint64 dbkey;
	const char *what;
	struct valuedata *vd = NULL;
	struct valuedata new_vd;
	gboolean check_data = FALSE;

	/*
	 * Look whether we already hold this value (in which case it would
	 * be a replication or a republishing from original creator).
	 */

	dbkey = keys_has(v->id, v->creator->id);

	if (0 == dbkey) {
		const knode_t *cn = v->creator;
		guint16 acceptable;

		acceptable = validate_new_acceptable(v);
		if (acceptable != STORE_SC_OK)
			return acceptable;

		vd = &new_vd;
		dbkey = valueid++;

		if (kuid_eq(kn->id, cn->id)) {
			if (!validate_creator(kn, cn))
				return STORE_SC_BAD_CREATOR;
			vd->original = TRUE;
		} else {
			if (NET_TYPE_IPV4 != host_addr_net(cn->addr))
				return STORE_SC_BAD_CREATOR;
			vd->original = FALSE;
		}

		keys_add_value(v->id, cn->id, dbkey);

		vd->id = *v->id;				/* struct copy */
		vd->publish = tm_time();
		vd->replicated = 0;
		vd->expire = 0;					/* XXX compute proper TTL */
		vd->cid = *cn->id;				/* struct copy */
		vd->vcode = cn->vcode;			/* struct copy */
		vd->addr = cn->addr;			/* struct copy */
		vd->port = cn->port;
		vd->major = cn->major;
		vd->minor = cn->minor;
		vd->type = v->type;
		vd->value_major = v->major;
		vd->value_minor = v->minor;
		vd->length = v->length;

		values_managed++;
		acct_net_update(values_per_class_c, cn->addr, NET_CLASS_C_MASK, +1);
		acct_net_update(values_per_ip, cn->addr, NET_IPv4_MASK, +1);
	} else {
		vd = get_valuedata(dbkey);

		/*
		 * If one the following assertions fails, then it means our data
		 * management is wrong and we messed up severely somewhere.
		 */

		g_assert(kuid_eq(&vd->id, v->id));				/* Primary key */
		g_assert(kuid_eq(&vd->cid, v->creator->id));	/* Secondary key */

		/*
		 * If it's not republished by the creator, then it's a replication
		 * from a k-neighbour (or a caching by a node which did not find the
		 * value here, but we got it from someone else in between).
		 *
		 * We make sure data is consistent with what we have.
		 */

		if (!kuid_eq(kn->id, v->creator->id)) {
			if (v->type != vd->type) {
				what = "DHT value type";
				goto mismatch;
			}
			if (
				v->major != vd->value_major ||
				v->minor != vd->value_minor
			) {
				what = "value format version";
				goto mismatch;
			}
			if (v->length != vd->length) {
				what = "value length";
				goto mismatch;
			}
			check_data = TRUE;
		} else {
			if (!validate_creator(kn, v->creator))
				return STORE_SC_BAD_CREATOR;

			/*
			 * They cannot change vendor codes without at least changing
			 * their KUID...
			 */

			if (vd->vcode.u32 != v->creator->vcode.u32) {
				what = "creator's vendor code";
				goto mismatch;
			}

			vd->publish = tm_time();
			vd->replicated = 0;
			vd->expire = 0;					/* XXX compute proper TTL */
			vd->original = TRUE;
		}
	}

	/*
	 * Check data if sent by someone other than the creator.
	 */

	if (check_data) {
		size_t length;
		gpointer data;

		data = dbmw_read(db_rawdata, &dbkey, &length);

		g_assert(length == vd->length);		/* Or our bookkeeping is faulty */
		g_assert(v->length == vd->length);	/* Ensured by preceding code */

		if (0 != memcmp(data, v->data, v->length)) {
			if (GNET_PROPERTY(dht_storage_debug) > 15)
				dump_hex(stderr, "Old value payload", data, length);

			what = "value data";
			goto mismatch;
		}

		/* 
		 * Here we checked everything the remote node sent us and it
		 * exactly matches what we have already.  Everything is thus fine
		 * and we're done.
		 */

		vd->replicated = tm_time();			/* XXX only when from k-ball */
	} else {
		/*
		 * We got either new data or something republished by the creator.
		 */

		dbmw_write(db_rawdata, &dbkey, deconstify_gpointer(v->data), v->length);
	}

	dbmw_write(db_valuedata, &dbkey, vd, sizeof *vd);

	return STORE_SC_OK;

mismatch:
	if (GNET_PROPERTY(dht_storage_debug) > 1) {
		g_message("DHT STORE spotted %s mismatch: got %s from %s {creator: %s}",
			what, dht_value_to_string(v), knode_to_string(kn),
			knode_to_string2(v->creator));
		g_message("DHT STORE had (pk=%s, sk=%s) %s v%u.%u %u byte%s (%s)",
			kuid_to_hex_string(&vd->id), kuid_to_hex_string2(&vd->cid),
			dht_value_type_to_string(vd->type),
			vd->value_major, vd->value_minor,
			vd->length, 1 == vd->length ? "" : "s",
			vd->original ? "original" : "copy");
	}

	return STORE_SC_DATA_MISMATCH;
}

/**
 * Store DHT value sent out by remote node.
 *
 * @param kn		the node who sent out the STORE request
 * @param v			the DHT value to store
 * @param token		whether a valid token was provided
 *
 * @return store status code that will be relayed back to the remote node.
 */
guint16
values_store(const knode_t *kn, const dht_value_t *v, gboolean token)
{
	guint16 status = STORE_SC_OK;

	knode_check(kn);
	g_assert(v);

	g_assert(dbmw_count(db_rawdata) == (size_t) values_managed);

	if (GNET_PROPERTY(dht_storage_debug)) {
		g_message("DHT STORE %s as %s v%u.%u (%u byte%s) created by %s (%s)",
			kuid_to_hex_string(v->id), dht_value_type_to_string(v->type),
			v->major, v->minor, v->length, 1 == v->length ? "" : "s",
			knode_to_string(v->creator),
			kuid_eq(v->creator->id, kn->id) ? "original" : "copy");

		/* v->data can be NULL if DHT value is larger than our maximum */
		if (v->data && GNET_PROPERTY(dht_storage_debug) > 15)
			dump_hex(stderr, "Value payload", v->data, v->length);
	}

	/*
	 * If we haven't got a valid token, report error.
	 *
	 * We come thus far with invalid tokens only to get consistent debugging
	 * traces.
	 */

	if (!token) {
		status = STORE_SC_BAD_TOKEN;
		goto done;
	}

	/*
	 * Reject too large a value.
	 */

	if (v->length >= VALUE_MAX_LEN) {
		status = STORE_SC_TOO_LARGE;
		goto done;
	}

	/*
	 * Reject improper value types (ANY).
	 */

	if (DHT_VT_ANY == v->type) {
		status = STORE_SC_BAD_TYPE;
		goto done;
	}

	/*
	 * Check for unusable addresses.
	 */

	if (!knode_is_usable(v->creator)) {
		status = STORE_SC_BAD_CREATOR;
		goto done;
	}

	/*
	 * We can attempt to store the value.
	 */

	status = values_publish(kn, v);

	g_assert(dbmw_count(db_rawdata) == (size_t) values_managed);

	/* FALL THROUGH */

done:
	if (GNET_PROPERTY(dht_storage_debug))
		g_message("DHT STORE status for %s is %u (%s)",
			kuid_to_hex_string(v->id), status, store_error_to_string(status));

	return status;
}

/**
 * Get DHT value from 64-bit DB key if of proper type.
 *
 * @param dbkey		the 64-bit DB key
 * @param type		either DHT_VT_ANY or the type we want
 *
 * @return the DHT value, or NULL if type is not matching.
 */
dht_value_t *
values_get(guint64 dbkey, dht_value_type_t type)
{
	struct valuedata *vd;
	gpointer vdata = NULL;
	knode_t *creator;
	dht_value_t *v;

	g_assert(dbkey != 0);		/* 0 is a special marker, not a valid key */

	vd = get_valuedata(dbkey);
	if (vd == NULL)
		return NULL;			/* DB failure */

	if (type != DHT_VT_ANY && type != vd->type)
		return NULL;

	/*
	 * OK, we have a value and its type matches.  Build the DHT value.
	 */

	if (vd->length) {
		size_t length;
		gpointer data;

		data = dbmw_read(db_rawdata, &dbkey, &length);

		g_assert(length == vd->length);		/* Or our bookkeeping is faulty */

		vdata = walloc(length);
		memcpy(vdata, data, length);
	}

	creator = knode_new((char *) &vd->cid, 0, vd->addr, vd->port, vd->vcode,
		vd->major, vd->minor);

	v = dht_value_make(creator, &vd->id, vd->type,
		vd->value_major, vd->value_minor, vdata, vd->length);

	knode_free(creator);
	return v;
}

/**
 * Initialize values management.
 */
void
values_init(void)
{
	db_valuedata = storage_create(db_valwhat, db_valbase,
		sizeof(guint64), sizeof(struct valuedata),
		serialize_valuedata, deserialize_valuedata,
		1, uint64_hash, uint64_eq);

	db_rawdata = storage_create(db_rawwhat, db_rawbase,
		sizeof(guint64), VALUE_MAX_LEN,
		NULL, NULL,
		1, uint64_hash, uint64_eq);

	values_per_ip = acct_net_create();
	values_per_class_c = acct_net_create();
}

/**
 * Close values management.
 */
void
values_close(void)
{
	storage_delete(db_valuedata, db_valbase);
	storage_delete(db_rawdata, db_rawbase);
	db_valuedata = db_rawdata = NULL;
}

/* vi: set ts=4 sw=4 cindent: */