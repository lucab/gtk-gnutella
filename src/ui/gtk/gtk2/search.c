/* -*- mode: cc-mode; tab-width:4; -*-
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

/**
 * @file
 *
 * GUI filtering functions.
 *
 */

#include "gtk/gui.h"

#include "search_cb.h"

#include "gtk/bitzi.h"
#include "gtk/search.h"
#include "gtk/gtk-missing.h"
#include "gtk/settings.h"
#include "gtk/columns.h"
#include "gtk/notebooks.h"
#include "gtk/statusbar.h"

#include "if/gui_property.h"
#include "if/gui_property_priv.h"
#include "if/bridge/ui2c.h"
#include "if/core/bitzi.h"

#include "lib/atoms.h"
#include "lib/misc.h"
#include "lib/glib-missing.h"
#include "lib/iso3166.h"
#include "lib/utf8.h"
#include "lib/override.h"		/* Must be the last header included */

RCSID("$Id$");

#define MAX_TAG_SHOWN	60		/* Show only first chars of tag */

static gchar tmpstr[4096];

static GList *searches = NULL;		/* List of search structs */

static GtkTreeView *tree_view_search = NULL;
static GtkNotebook *notebook_search_results = NULL;
static GtkButton *button_search_clear = NULL;

static GList *list_search_history = NULL;
static gboolean search_gui_shutting_down = FALSE;

/*
 * Private function prototypes
 */
static GtkTreeViewColumn *add_column(GtkTreeView *treeview, const gchar *name,
	gint id, gint width, gfloat xalign, gint fg_column, gint bg_column);

/*
 * If no search are currently allocated
 */
static GtkWidget *default_search_tree_view = NULL;
GtkWidget *default_scrolled_window = NULL;


/* ----------------------------------------- */

static inline void
add_parent_with_sha1(GHashTable *ht, gpointer key, GtkTreeIter *iter)
{
	g_hash_table_insert(ht, key, w_tree_iter_copy(iter));
}

static inline void
remove_parent_with_sha1(GHashTable *ht, const gchar *sha1)
{
	gpointer key;

	key = atom_sha1_get(sha1);
	g_hash_table_remove(ht, key);
	atom_sha1_free(key);
}

GtkTreeIter *
find_parent_with_sha1(GHashTable *ht, gpointer key)
{
	GtkTreeIter *iter = NULL;
	gpointer *orig_key;

	if (g_hash_table_lookup_extended(ht, key,
			(gpointer) &orig_key, (gpointer) &iter))
		return iter;

	return NULL;
}

static gboolean
unref_record(
	GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	record_t *rc = NULL;
	GHashTable *dups = (GHashTable *) data;

	(void) path;

	gtk_tree_model_get(model, iter, c_sr_record, &rc, (-1));
	g_assert(g_hash_table_lookup(dups, rc) != NULL);
	g_assert(NULL != rc);
	g_assert(rc->refcount > 0);
	search_gui_unref_record(rc);
	g_assert(rc->refcount > 0);
	g_hash_table_remove(dups, rc);
	g_assert(g_hash_table_lookup(dups, rc) == NULL);
	return FALSE;
}

static void
search_gui_clear_store(search_t *sch)
{
	GtkTreeModel *model;

	model = GTK_TREE_MODEL(sch->model);
	gtk_tree_model_foreach(model, unref_record, sch->dups);
	gtk_tree_store_clear(GTK_TREE_STORE(model));
	g_assert(0 == g_hash_table_size(sch->dups));
}

/**
 * Reset internal search model.
 * Called when a search is restarted, for example.
 */
void
search_gui_reset_search(search_t *sch)
{
	search_gui_clear_store(sch);
	search_gui_clear_search(sch);
}

/**
 *	always_true
 */
gboolean
always_true(gpointer key, gpointer value, gpointer x)
{
	(void) key;
	(void) value;
	(void) x;
	return TRUE;
}

static gboolean
on_enter_notify(GtkWidget *widget, GdkEventCrossing *unused_event,
		gpointer unused_udata)
{
	GtkTreePath *path;

	(void) unused_event;
	(void) unused_udata;

	gtk_tree_view_get_cursor(GTK_TREE_VIEW(widget), &path, NULL);
	if (!path) {
		gtk_tooltips_set_tip(settings_gui_tooltips(), widget, NULL, NULL);
	}
	widget_force_tooltip(widget);
	return FALSE;
}

static gboolean
on_leave_notify(GtkWidget *widget, GdkEventCrossing *unused_event,
		gpointer unused_udata)
{
	(void) unused_event;
	(void) unused_udata;

	set_tooltips_keyboard_mode(widget, FALSE);
	return FALSE;
}


/**
 * Decrement refcount of hash table key entry.
 */
void
ht_unref_record(record_t *rc)
{
	g_assert(rc->refcount > 0);
	search_gui_unref_record(rc);
}

/**
 * Clear all results from search.
 */
void
search_gui_clear_search(search_t *sch)
{
	g_assert(sch);
	g_assert(sch->dups);

	/*
	 * Before invoking search_free_r_sets(), we must iterate on the
	 * hash table where we store records and decrement the refcount of
	 * each record, and remove them from the hash table.
	 *
	 * Otherwise, we will violate the pre-condition of search_free_record(),
	 * which is there precisely for that reason!
	 */
/* XXX */
#if 0
	g_hash_table_foreach_remove(sch->dups, dec_records_refcount, NULL);
#endif /* 0 */
	search_gui_free_r_sets(sch);
	g_hash_table_foreach_remove(sch->parents, always_true, NULL);

	sch->items = sch->unseen_items = 0;
	guc_search_update_items(sch->search_handle, sch->items);
}

/* *
 * Remove the search from the list of searches and free all
 * associated ressources (including filter and gui stuff).
 */
void
search_gui_close_search(search_t *sch)
{
    g_assert(sch != NULL);

    /*
     * We remove the search immeditaly from the list of searches,
     * because some of the following calls (may) depend on
     * "searches" holding only the remaining searches.
     * We may not free any ressources of "sch" yet, because
     * the same calls may still need them!.
     *      --BLUE 26/05/2002
     */

	search_gui_clear_store(sch);
 	searches = g_list_remove(searches, (gpointer) sch);

    search_gui_remove_search(sch);
	filter_close_search(sch);
	search_gui_clear_search(sch);
	g_hash_table_destroy(sch->dups);
	sch->dups = NULL;
	g_hash_table_destroy(sch->parents);
	sch->parents = NULL;

    guc_search_close(sch->search_handle);
	atom_str_free(sch->query);

	G_FREE_NULL(sch);
}

/**
 * Create a new search and start it. Use default reissue timeout.
 */
gboolean search_gui_new_search(
	const gchar *query, flag_t flags, search_t **search)
{
    guint32 timeout;

    gnet_prop_get_guint32_val
		(PROP_SEARCH_REISSUE_TIMEOUT, &timeout);
	return search_gui_new_search_full(query, timeout, SORT_NO_COL, SORT_NONE,
		flags | SEARCH_ENABLED, search);
}


void
do_atom_sha1_free(gpointer sha1)
{
	atom_sha1_free(sha1);
}


/**
 * @returns TRUE if search was sucessfully created and FALSE if an error
 * happened. If the "search" argument is not NULL a pointer to the new
 * search is stored there.
 */
gboolean
search_gui_new_search_full(const gchar *querystr, guint32 reissue_timeout,
	gint sort_col, gint sort_order, flag_t flags, search_t **search)
{
	const gchar *query, *error;
	search_t *sch;
	gnet_search_t sch_id;
	GList *glist;
	GtkTreeStore *model;
	GtkTreeIter iter;
    GtkWidget *button_search_close =
        lookup_widget(main_window, "button_search_close");
    GtkWidget *entry_search = lookup_widget(main_window, "entry_search");

	query = search_gui_parse_query(querystr, &error);
	if (!query) {
		statusbar_gui_warning(5, "%s", error);
		return FALSE;
	}
	sch_id = guc_search_new(query, reissue_timeout, flags);
	if (-1 == (gint) sch_id) {
		statusbar_gui_warning(5, "%s", _("Failed to create the search"));
		return FALSE;
	}

	sch = g_new0(search_t, 1);
	sch->sort_col = sort_col;		/* Unused in GTK2 currently */
	sch->sort_order = sort_order;	/* Unused in GTK2 currently */
	sch->query = atom_str_get(query);
	sch->enabled = (flags & SEARCH_ENABLED) ? TRUE : FALSE;
	sch->search_handle = sch_id;
	sch->passive = (flags & SEARCH_PASSIVE) ? TRUE : FALSE;
	sch->massive_update = FALSE;
	sch->dups = g_hash_table_new_full(
					(GHashFunc) search_gui_hash_func,
					(GEqualFunc) search_gui_hash_key_compare,
					(GDestroyNotify) ht_unref_record,
					NULL);
	if (!sch->dups)
		g_error("new_search: unable to allocate hash table.");
	sch->parents = g_hash_table_new_full(NULL, NULL,
		do_atom_sha1_free, (GDestroyNotify) w_tree_iter_free);

  	filter_new_for_search(sch);

	/* Create the list item */

	sch->list_item = gtk_list_item_new_with_label(sch->query);
	gtk_widget_show(sch->list_item);
	glist = g_list_prepend(NULL, (gpointer) sch->list_item);
	gtk_label_set_text(GTK_LABEL(
			lookup_widget(main_window, "label_search_current_search")), query);

	/* Create a new TreeView if needed, or use the default TreeView */

	if (searches) {
		/* We have to create a new TreeView for this search */
		gui_search_create_tree_view(&sch->scrolled_window, &sch->tree_view);

		gtk_object_set_user_data((GtkObject *) sch->scrolled_window,
								 (gpointer) sch);

		gtk_notebook_append_page(GTK_NOTEBOOK(notebook_search_results),
								 sch->scrolled_window, NULL);
	} else {
		/* There are no searches currently, we can use the default TreeView */

		if (default_scrolled_window && default_search_tree_view) {
			sch->scrolled_window = default_scrolled_window;
			sch->tree_view = default_search_tree_view;

			default_search_tree_view = default_scrolled_window = NULL;
		} else
			g_warning("new_search():"
				" No current search but no default tree_view !?");

		gtk_object_set_user_data((GtkObject *) sch->scrolled_window,
								 (gpointer) sch);
	}
	sch->model = gtk_tree_view_get_model(GTK_TREE_VIEW(sch->tree_view));

	/* Add the search to the TreeView in pane on the left */
	model = GTK_TREE_STORE(gtk_tree_view_get_model(tree_view_search));
	gtk_tree_store_append(model, &iter, NULL);
	gtk_tree_store_set(model, &iter,
		c_sl_name, sch->query,
		c_sl_hit, 0,
		c_sl_new, 0,
		c_sl_sch, sch,
		c_sl_fg, NULL,
		c_sl_bg, NULL,
		(-1));

	gui_search_update_tab_label(sch);
	sch->tab_updating = gtk_timeout_add(TAB_UPDATE_TIME * 1000,
        (GtkFunction) gui_search_update_tab_label, sch);

    if (!searches) {
		gtk_notebook_set_tab_label_text(
            GTK_NOTEBOOK(notebook_search_results),
            gtk_notebook_get_nth_page(
				GTK_NOTEBOOK(notebook_search_results), 0), _("(no search)"));
    }

	g_signal_connect(GTK_OBJECT(sch->list_item), "select",
		G_CALLBACK(on_search_selected), (gpointer) sch);
	search_gui_set_current_search(sch);
	gtk_widget_set_sensitive(button_search_close, TRUE);
    gtk_entry_set_text(GTK_ENTRY(entry_search), "");
	searches = g_list_append(searches, (gpointer) sch);

	if (sch->enabled)
		guc_search_start(sch->search_handle);

	if (NULL != search)
		*search = sch;
	return TRUE;
}

/* Searches results */
static gint
search_gui_compare_size_func(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data)
{
    record_t *rec_a = NULL;
	record_t *rec_b = NULL;

	(void) user_data;
    gtk_tree_model_get(model, a, c_sr_record, &rec_a, (-1));
    gtk_tree_model_get(model, b, c_sr_record, &rec_b, (-1));
	return CMP(rec_b->size, rec_a->size);
}

static gint
search_gui_compare_count_func(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data)
{
	guint m = (guint) gtk_tree_model_iter_n_children(model, a);
	guint n = (guint) gtk_tree_model_iter_n_children(model, b);
	return m == n ?
		search_gui_compare_size_func(model, a, b, user_data) : CMP(n, m);
}

#if 0
/* Who wants to see the IP addresses in the GtkTreeView, anyway? */
/* Me, allows to see all the hits from the same host groupped together --RAM */
static gint
search_gui_compare_host_func(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data)
{
    record_t *rec_a = NULL;
	record_t *rec_b = NULL;
	gint d;

    gtk_tree_model_get(model, a, c_sr_record, &rec_a, (-1));
    gtk_tree_model_get(model, b, c_sr_record, &rec_b, (-1));
	d = CMP(rec_a->results_set->ip, rec_b->results_set->ip);

	return d != 0
		? d : CMP(rec_b->results_set->port, rec_a->results_set->port);
}
#endif

void
search_gui_add_record(
	search_t *sch,
	record_t *rc,
	GString *vinfo,
	GdkColor *fg,
	GdkColor *bg)
{
	GtkTreeIter *parent;
	GtkTreeIter iter;
	GtkTreeStore *model = GTK_TREE_STORE(sch->model);
    const struct results_set *rs = rc->results_set;
	const gchar *name_utf8;
	size_t rw = 0;
  	gchar info[1024], ext_utf8[32];

	ext_utf8[0] = '\0';
	info[0] = '\0';

	/*
	 * When the search is displayed in multiple search results, the refcount
	 * can also be larger than 1.
	 * FIXME: Check that the refcount is less then the number of search that we
	 * have open
	 *		-- JA, 6/11/2003
	 */
	g_assert(rc->refcount >= 1);

	if (rc->tag) {
		size_t len = strlen(rc->tag);

		/*
		 * We want to limit the length of the tag shown, but we don't
		 * want to loose that information.	I imagine to have a popup
		 * "show file info" one day that will give out all the
		 * information.
		 *				--RAM, 09/09/2001
		 */

		len = MIN(len, MAX_TAG_SHOWN);
		rw = gm_snprintf(info, MIN(len, sizeof info), "%s", rc->tag);
	}
	if (vinfo->len) {
		g_assert(rw < sizeof info);
		rw += gm_snprintf(&info[rw], sizeof info - rw, "%s%s",
			info[0] != '\0' ? "; " : "", vinfo->str);
	}

	if (rc->alt_locs != NULL) {
		g_assert(rw < sizeof info);
		rw += gm_snprintf(&info[rw], sizeof info - rw, "%salt",
			info[0] != '\0' ? ", " : "");
	}

	/* Don't care if it's truncated. It's usually very short anyways. */
	if (info[0] != '\0') {
		const gchar *s;

		g_assert(rw < sizeof info);
		s = lazy_locale_to_utf8(info, rw);
		if (s != info) {
			g_strlcpy(info, s, sizeof info);
		}
	}

	if (NULL != rc->sha1) {
		gpointer key;

		key = atom_sha1_get(rc->sha1);
		parent = find_parent_with_sha1(sch->parents, key);
		gtk_tree_store_append(model, &iter, parent);
		if (NULL != parent) {
			guint n;

			n = (guint) gtk_tree_model_iter_n_children(
                                (GtkTreeModel *) model, parent) + 1;
			/* Use a string to suppress showing 0 in the # column */
			gm_snprintf(tmpstr, sizeof(tmpstr), "%u", n);
			gtk_tree_store_set(model, parent, c_sr_count, tmpstr, (-1));
 			/* we need only the reference for the parent */
			atom_sha1_free(key);
		} else
			add_parent_with_sha1(sch->parents, key, &iter);
	} else
		gtk_tree_store_append(model, &iter, (parent = NULL));

	g_assert(rc->refcount >= 1);

	search_gui_ref_record(rc);

	g_assert(rc->refcount >= 2);

	name_utf8 = lazy_locale_to_utf8(rc->name, 0);
	if (name_utf8) {
		const gchar *p = strrchr(name_utf8, '.');

		rw = g_strlcpy(ext_utf8, p ? ++p : "", sizeof ext_utf8);
		if (rw >= sizeof ext_utf8) {
			/* If the guessed extension is really this long, assume the
			 * part after the dot isn't an extension at all. */
			ext_utf8[0] = '\0';
		} else {
			/* Using g_utf8_strdown() would be cleaner but it allocates
			 * a new string which is ugly. Nobody uses such file extensions
			 * anyway. */
			ascii_strlower(ext_utf8, ext_utf8);
		}
	}

	gtk_tree_store_set(model, &iter,
		      c_sr_filename, name_utf8,
		      c_sr_ext, ext_utf8[0] != '\0' ? ext_utf8 : NULL,
		      c_sr_size, NULL != parent ? NULL : short_size(rc->size),
			  c_sr_loc, iso3166_country_cc(rs->country),
			  c_sr_meta, NULL,
		      c_sr_info, info[0] != '\0' ? info : NULL,
		      c_sr_fg, fg,
		      c_sr_bg, bg,
		      c_sr_record, rc,
		      (-1));
}

void
search_gui_set_clear_button_sensitive(gboolean flag)
{
	gtk_widget_set_sensitive(GTK_WIDGET(button_search_clear), flag);
}

/* ----------------------------------------- */


static void
download_selected_file(GtkTreeModel *model, GtkTreeIter *iter, GSList **sl)
{
	struct results_set *rs;
	struct record *rc = NULL;
	gboolean need_push;
	gchar *filename;

	g_assert(model != NULL);
	g_assert(iter != NULL);

	if (sl) {
		*sl = g_slist_prepend(*sl, w_tree_iter_copy(iter));
	}

	gtk_tree_model_get(model, iter, c_sr_record, &rc, (-1));
	g_assert(rc->refcount > 0);

	rs = rc->results_set;
	need_push = (rs->status & ST_FIREWALL) != 0;

	filename = gm_sanitize_filename(rc->name, FALSE, FALSE);
	guc_download_new(filename, rc->size, rc->index, rs->ip,
		rs->port, rs->guid, rs->hostname,
		rc->sha1, rs->stamp, need_push, NULL, rs->proxies);

	if (filename != rc->name)
		G_FREE_NULL(filename);

	if (rs->proxies != NULL)
		search_gui_free_proxies(rs);

	if (rc->alt_locs != NULL)
		search_gui_check_alt_locs(rs, rc);

	g_assert(rc->refcount > 0);
}

static void
remove_selected_file(gpointer data, gpointer user_data)
{
	GtkTreeModel *model = (GtkTreeModel *) user_data;
	GtkTreeIter *iter = data;
	GtkTreeIter child;
	record_t *rc = NULL;
	search_t *current_search = search_gui_get_current_search();

	current_search->items--;

	/* First get the record, it must be unreferenced at the end */
	gtk_tree_model_get(model, iter, c_sr_record, &rc, (-1));
	g_assert(rc->refcount > 1);

	if (gtk_tree_model_iter_nth_child(model, &child, iter, 0)) {
		gchar *filename, *ext, *info, *loc;
		gpointer fg;
		gpointer bg;
		record_t *child_rc = NULL;

		/* Copy the contents of the first child's row into
		 * the parent's row */
    	gtk_tree_model_get(model, &child,
              c_sr_filename, &filename,
              c_sr_ext, &ext,
              c_sr_loc, &loc,
              c_sr_info, &info,
              c_sr_fg, &fg,
              c_sr_bg, &bg,
              c_sr_record, &child_rc,
              (-1));

		g_assert(child_rc->refcount > 0);
		gtk_tree_store_set((GtkTreeStore *) model, iter,
              c_sr_filename, filename,
              c_sr_ext, ext,
              c_sr_loc, loc,
              c_sr_info, info,
              c_sr_fg, fg,
              c_sr_bg, bg,
              c_sr_record, child_rc,
              (-1));

		G_FREE_NULL(ext);
		G_FREE_NULL(filename);
		G_FREE_NULL(info);
		G_FREE_NULL(loc);

		/* And remove the child's row */
		gtk_tree_store_remove((GtkTreeStore *) model, &child);
		g_assert(child_rc->refcount > 0);
	} else {
		g_assert(rc->refcount > 0);
		/* The row has no children, remove it's sha1 and the row itself */
		if (NULL != rc->sha1)
			remove_parent_with_sha1(current_search->parents, rc->sha1);
		gtk_tree_store_remove((GtkTreeStore *) model, iter);
	}

	search_gui_unref_record(rc);
	g_assert(rc->refcount > 0);
	g_hash_table_remove(current_search->dups, rc);
	/* hash table with dups unrefs the record itself*/

	w_tree_iter_free(iter);
}

struct selection_ctx {
	GtkTreeView *tv;
	GSList **iters;
};

static void
download_selected_all_files(GtkTreeModel *model, GtkTreePath *path,
		GtkTreeIter *iter, gpointer data)
{
	struct selection_ctx *ctx = data;

	g_assert(ctx != NULL);

	download_selected_file(model, iter, ctx->iters);
    if (
            gtk_tree_model_iter_has_child(model, iter) &&
            !gtk_tree_view_row_expanded(ctx->tv, path)
    ) {
        GtkTreeIter child;
        gint i = 0;

        while (gtk_tree_model_iter_nth_child(model, &child, iter, i)) {
			download_selected_file(model, &child, ctx->iters);
            i++;
        }
	}
}

static void
collect_all_iters(GtkTreeModel *model, GtkTreePath *path,
		GtkTreeIter *iter, gpointer data)
{
	struct selection_ctx *ctx = data;

	g_assert(ctx != NULL);
	g_assert(ctx->iters != NULL);

	*ctx->iters = g_slist_prepend(*ctx->iters, w_tree_iter_copy(iter));
    if (
            gtk_tree_model_iter_has_child(model, iter) &&
            !gtk_tree_view_row_expanded(ctx->tv, path)
    ) {
        GtkTreeIter child;
        gint i = 0;

        while (gtk_tree_model_iter_nth_child(model, &child, iter, i)) {
			*ctx->iters = g_slist_prepend(*ctx->iters,
								w_tree_iter_copy(&child));
            i++;
        }
	}
}

struct menu_helper {
	gint page;
	GtkTreeIter iter;
};

static gboolean
search_gui_menu_select_helper(
	GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	gint page = -1;
	struct menu_helper *mh = data;

	(void) path;
	gtk_tree_model_get(model, iter, 1, &page, (-1));
	if (page == mh->page) {
		mh->iter = *iter;
		return TRUE;
	}
	return FALSE;
}

static void
search_gui_menu_select(gint page)
{
	GtkTreeView *treeview;
	GtkTreeModel *model;
	GtkTreeSelection *selection;
	struct menu_helper mh;

	mh.page = page;

	treeview = GTK_TREE_VIEW(
		lookup_widget(main_window, "treeview_menu"));
	model = GTK_TREE_MODEL(gtk_tree_view_get_model(treeview));
	gtk_tree_model_foreach(
		model, search_gui_menu_select_helper, &mh);
	selection = gtk_tree_view_get_selection(treeview);
	gtk_tree_selection_select_iter(selection, &mh.iter);
}

void
search_gui_download_files(void)
{
	search_t *search = search_gui_get_current_search();
	GSList *sl = NULL;
	struct selection_ctx ctx;
    gboolean clear;

	if (!search) {
		g_warning("search_download_files(): no possible search!");
		return;
	}

	/* XXX: This has to be GUI (not a core) property! */
    gnet_prop_get_boolean_val(PROP_SEARCH_REMOVE_DOWNLOADED, &clear);

	ctx.tv = GTK_TREE_VIEW(search->tree_view);
	ctx.iters = clear ? &sl : NULL;
	gtk_tree_selection_selected_foreach(gtk_tree_view_get_selection(ctx.tv),
		download_selected_all_files, &ctx);

	if (sl) {
		GtkTreeModel *model;

		model = gtk_tree_view_get_model(ctx.tv);
		g_slist_foreach(sl, remove_selected_file, model);
    	g_slist_free(sl);
	}

    gui_search_force_update_tab_label(search, time(NULL));
    search_gui_update_items(search);
    guc_search_update_items(search->search_handle, search->items);
}


void
search_gui_discard_files(void)
{
	search_t *search = search_gui_get_current_search();
	GSList *sl = NULL;
	struct selection_ctx ctx;

	if (!search) {
		g_warning("search_download_files(): no possible search!");
		return;
	}

	ctx.tv = GTK_TREE_VIEW(search->tree_view);
	ctx.iters = &sl;
	gtk_tree_selection_selected_foreach(gtk_tree_view_get_selection(ctx.tv),
		collect_all_iters, &ctx);

	if (sl) {
		GtkTreeModel *model;

		model = gtk_tree_view_get_model(ctx.tv);
		g_slist_foreach(sl, remove_selected_file, model);
    	g_slist_free(sl);
	}

    gui_search_force_update_tab_label(search, time(NULL));
    search_gui_update_items(search);
    guc_search_update_items(search->search_handle, search->items);
}

/***
 *** Private functions
 ***/

static void
add_list_columns(GtkTreeView *treeview)
{
	static const struct {
		const gchar * const title;
		const gint id;
		const gfloat align;
	} columns[] = {
		{ N_("Search"), c_sl_name, 0.0 },
		{ N_("Hits"),	c_sl_hit,  1.0 },
		{ N_("New"),	c_sl_new,  1.0 }
	};
	guint32 width[G_N_ELEMENTS(columns)];
	guint i;

	STATIC_ASSERT(SEARCH_LIST_VISIBLE_COLUMNS == G_N_ELEMENTS(columns));

    gui_prop_get_guint32(PROP_SEARCH_LIST_COL_WIDTHS, width, 0,
		G_N_ELEMENTS(width));
	for (i = 0; i < G_N_ELEMENTS(columns); i++) {
		add_column(treeview, _(columns[i].title), columns[i].id,
			width[i], columns[i].align, c_sl_fg, c_sl_bg);
	}
}

static void
add_results_column(
	GtkTreeView *treeview,
	const gchar *name,
	gint id,
	gint width,
	gfloat xalign,
	GtkTreeIterCompareFunc sortfunc)
{
    GtkTreeViewColumn *column;
	GtkTreeModel *model;

	model = gtk_tree_view_get_model(treeview);
	column = add_column(treeview, name, id, width, xalign, c_sr_fg, c_sr_bg);
	if (NULL != sortfunc)
		gtk_tree_sortable_set_sort_func(
			GTK_TREE_SORTABLE(model), id, sortfunc, NULL, NULL);
	g_signal_connect(G_OBJECT(column), "clicked",
		G_CALLBACK(on_tree_view_search_results_click_column),
		GINT_TO_POINTER(id));
}


/***
 *** Public functions
 ***/

void
search_gui_init(void)
{
	GType types[] = {
		G_TYPE_STRING,
		G_TYPE_INT,
		G_TYPE_INT,
		GDK_TYPE_COLOR,
		GDK_TYPE_COLOR,
		G_TYPE_POINTER
	};
    GtkTreeStore *store;
    GtkTreeView *tv;
	search_t *current_search;

    tree_view_search = GTK_TREE_VIEW(lookup_widget(main_window,
							"tree_view_search"));
    notebook_search_results = GTK_NOTEBOOK(lookup_widget(main_window,
								"notebook_search_results"));
    button_search_clear = GTK_BUTTON(lookup_widget(main_window,
							"button_search_clear"));

	search_gui_common_init();

	g_signal_connect(GTK_OBJECT(tree_view_search), "button_press_event",
		G_CALLBACK(on_tree_view_search_button_press_event), NULL);

	STATIC_ASSERT(c_sl_num == G_N_ELEMENTS(types));
	store = gtk_tree_store_newv(G_N_ELEMENTS(types), types);
	gtk_tree_view_set_model(tree_view_search, GTK_TREE_MODEL(store));
	add_list_columns(tree_view_search);
	g_signal_connect(G_OBJECT(tree_view_search),
		"cursor-changed",
		G_CALLBACK(on_tree_view_search_select_row),
		NULL);

	gui_search_create_tree_view(&default_scrolled_window,
		&default_search_tree_view);
    gtk_notebook_remove_page(notebook_search_results, 0);
	gtk_notebook_set_scrollable(notebook_search_results, TRUE);
	gtk_notebook_append_page
        (notebook_search_results, default_scrolled_window, NULL);
  	gtk_notebook_set_tab_label_text
        (notebook_search_results, default_scrolled_window, _("(no search)"));

	g_signal_connect(GTK_OBJECT(notebook_search_results), "switch_page",
		G_CALLBACK(on_search_notebook_switch), NULL);
	g_signal_connect(GTK_OBJECT(notebook_search_results), "focus_tab",
		G_CALLBACK(on_search_notebook_focus_tab), NULL);

   	current_search = search_gui_get_current_search();
    tv = current_search != NULL ?
			GTK_TREE_VIEW(current_search->tree_view) :
			GTK_TREE_VIEW(default_search_tree_view);
	tree_view_restore_visibility(tv, PROP_SEARCH_RESULTS_COL_VISIBLE);
	search_gui_retrieve_searches();
    search_add_got_results_listener(search_gui_got_results);
}

void
search_gui_shutdown(void)
{
	GtkTreeView *tv;
	search_t *current_search = search_gui_get_current_search();
	guint32 pos;

	search_gui_shutting_down = TRUE;
	search_callbacks_shutdown();
    search_remove_got_results_listener(search_gui_got_results);

	search_gui_store_searches();

	pos = gtk_paned_get_position(
			GTK_PANED(lookup_widget(main_window, "vpaned_results")));

	gui_prop_set_guint32(PROP_RESULTS_DIVIDER_POS, &pos, 0, 1);

	tv = current_search != NULL
		? GTK_TREE_VIEW(current_search->tree_view)
		: GTK_TREE_VIEW(default_search_tree_view);

	tree_view_save_widths(tv, PROP_SEARCH_RESULTS_COL_WIDTHS);
	tree_view_save_visibility(tv, PROP_SEARCH_RESULTS_COL_VISIBLE);

    while (searches != NULL)
        search_gui_close_search((search_t *) searches->data);

	tree_view_save_widths(tree_view_search, PROP_SEARCH_LIST_COL_WIDTHS);
	search_gui_common_shutdown();
}

const GList *
search_gui_get_searches(void)
{
	return (const GList *) searches;
}

static void
selection_counter_helper(
	GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	gint *counter = data;

	(void) model;
	(void) path;
	(void) iter;
	*counter += 1;
}

static gint
selection_counter(GtkTreeView *tv)
{
	gint rows = 0;

	if (tv) {
		gtk_tree_selection_selected_foreach(gtk_tree_view_get_selection(tv),
			selection_counter_helper, &rows);
	}

	return rows;
}

static gboolean
tree_view_search_remove(GtkTreeModel *model, GtkTreePath *unused_path,
		GtkTreeIter *iter, gpointer data)
{
	gpointer sch;

	(void) unused_path;
    gtk_tree_model_get(model, iter, c_sl_sch, &sch, (-1));
 	if (sch == data) {
    	gtk_tree_store_remove((GtkTreeStore *) model, iter);
		return TRUE;
	}

	return FALSE;
}


/**
 * Remove the search from the gui and update all widget accordingly.
 */
void
search_gui_remove_search(search_t *sch)
{
    GList *glist;
    gboolean sensitive;
	GtkTreeModel *model;
	search_t *current_search;

    g_assert(sch != NULL);

   	glist = g_list_prepend(NULL, (gpointer) sch->list_item);

	model = gtk_tree_view_get_model(tree_view_search);
    gtk_tree_model_foreach(model, tree_view_search_remove, sch);

    gtk_timeout_remove(sch->tab_updating);

    if (searches) {				/* Some other searches remain. */
		gtk_notebook_remove_page(notebook_search_results,
			gtk_notebook_page_num(notebook_search_results,
				sch->scrolled_window));
	} else {
		/*
		 * Keep the GtkTreeView of this search, clear it and make it the
		 * default GtkTreeView
		 */

		default_search_tree_view = sch->tree_view;
		default_scrolled_window = sch->scrolled_window;

		search_gui_forget_current_search();
		search_gui_update_items(NULL);

		gtk_label_set_text(GTK_LABEL(
			lookup_widget(main_window, "label_search_current_search")), "");

        gtk_notebook_set_tab_label_text(notebook_search_results,
			default_scrolled_window, _("(no search)"));

		gtk_widget_set_sensitive(GTK_WIDGET(button_search_clear), FALSE);
	}

    sensitive = searches != NULL;
	gtk_widget_set_sensitive(
        lookup_widget(main_window, "button_search_close"), sensitive);

	current_search = search_gui_get_current_search();

	if (current_search != NULL)
		sensitive = sensitive &&
			selection_counter(GTK_TREE_VIEW(current_search->tree_view)) > 0;

    gtk_widget_set_sensitive(
		lookup_widget(popup_search, "popup_search_download"), sensitive);
}

void
search_gui_set_current_search(search_t *sch)
{
	search_t *old_sch = search_gui_get_current_search();
    GtkWidget *spinbutton_reissue_timeout;
    static gboolean locked = FALSE;
    gboolean passive;
    gboolean frozen;
    guint32 reissue_timeout;
	search_t *current_search = old_sch;

	g_assert(sch != NULL);

    if (locked)
		return;
    locked = TRUE;

	if (old_sch)
		gui_search_force_update_tab_label(old_sch, time(NULL));

    passive = guc_search_is_passive(sch->search_handle);
    frozen = guc_search_is_frozen(sch->search_handle);
    reissue_timeout = guc_search_get_reissue_timeout
		(sch->search_handle);

    /*
     * We now propagate the column visibility from the current_search
     * to the new current_search.
     */

    if (current_search != NULL) {
        GtkTreeView *tv_new = GTK_TREE_VIEW(sch->tree_view);
        GtkTreeView *tv_old = GTK_TREE_VIEW(current_search->tree_view);

		tree_view_save_widths(tv_old, PROP_SEARCH_RESULTS_COL_WIDTHS);
		tree_view_save_visibility(tv_old, PROP_SEARCH_RESULTS_COL_VISIBLE);
		tree_view_restore_visibility(tv_new, PROP_SEARCH_RESULTS_COL_VISIBLE);
    } else if (default_search_tree_view) {
		tree_view_save_widths(GTK_TREE_VIEW(default_search_tree_view),
			PROP_SEARCH_RESULTS_COL_WIDTHS);
	}


	search_gui_current_search(sch);
	sch->unseen_items = 0;

    spinbutton_reissue_timeout = lookup_widget
        (main_window, "spinbutton_search_reissue_timeout");

    if (sch != NULL) {
        gui_search_force_update_tab_label(sch, time(NULL));
        search_gui_update_items(sch);

        gtk_spin_button_set_value
            (GTK_SPIN_BUTTON(spinbutton_reissue_timeout), reissue_timeout);
        gtk_widget_set_sensitive(spinbutton_reissue_timeout, !passive);
        gtk_widget_set_sensitive(
            lookup_widget(popup_search, "popup_search_download"),
			selection_counter(GTK_TREE_VIEW(sch->tree_view)) > 0);
        gtk_widget_set_sensitive(GTK_WIDGET(button_search_clear),
            sch->items != 0);
        gtk_widget_set_sensitive(
            lookup_widget(popup_search_list, "popup_search_restart"), !passive);
        gtk_widget_set_sensitive(
            lookup_widget(popup_search_list, "popup_search_duplicate"), !passive);
        gtk_widget_set_sensitive(
            lookup_widget(popup_search_list, "popup_search_stop"), !frozen);
        gtk_widget_set_sensitive(
            lookup_widget(popup_search_list, "popup_search_resume"), frozen);

        /*
         * Combo "Active searches"
         */
        gtk_list_item_select(GTK_LIST_ITEM(sch->list_item));
    } else {
		static const gchar * const popup_items[] = {
			"popup_search_restart",
			"popup_search_duplicate",
			"popup_search_stop",
			"popup_search_resume",
		};
		guint i;

        gtk_tree_selection_unselect_all(
			gtk_tree_view_get_selection(tree_view_search));
        gtk_widget_set_sensitive(spinbutton_reissue_timeout, FALSE);
       	gtk_widget_set_sensitive(
            lookup_widget(popup_search, "popup_search_download"), FALSE);

		for (i = 0; i < G_N_ELEMENTS(popup_items); i++) {
       		gtk_widget_set_sensitive(
            	lookup_widget(popup_search_list, popup_items[i]), FALSE);
		}
    }

	tree_view_restore_widths(GTK_TREE_VIEW(sch->tree_view),
			PROP_SEARCH_RESULTS_COL_WIDTHS);

    /*
     * Search results notebook
     */
	gtk_notebook_set_current_page(notebook_search_results,
		gtk_notebook_page_num(notebook_search_results, sch->scrolled_window));

	/* Update the label showing the query string for this search */
	gtk_label_set_text(GTK_LABEL(
			lookup_widget(main_window, "label_search_current_search")),
			sch->query);

	search_gui_menu_select(nb_main_page_search);
    locked = FALSE;
}

static GtkTreeModel *
create_model(void)
{
	GType columns[] = {
		G_TYPE_STRING,		/* File */
		G_TYPE_STRING,		/* Extension */
		G_TYPE_STRING,		/* Size */
		G_TYPE_STRING,		/* Source counter */
		G_TYPE_STRING,		/* Location */
		G_TYPE_STRING,		/* Metadata */
		G_TYPE_STRING,		/* Info */
		/* The following columns are invisible */
		GDK_TYPE_COLOR,		/* Foreground */
		GDK_TYPE_COLOR,		/* Background */
		G_TYPE_POINTER		/* (record_t *) */
	};
	GtkTreeStore *model;

	STATIC_ASSERT(c_sr_num == G_N_ELEMENTS(columns));

	/* create tree store */
	model = gtk_tree_store_newv(G_N_ELEMENTS(columns), columns);

	return (GtkTreeModel *) model;
}


static GtkTreeViewColumn *
add_column(
	GtkTreeView *treeview,
	const gchar *name,
	gint id,
	gint width,
	gfloat xalign,
	gint fg_column,
	gint bg_column)
{
    GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;

	renderer = gtk_cell_renderer_text_new();
	gtk_cell_renderer_text_set_fixed_height_from_font(
		GTK_CELL_RENDERER_TEXT(renderer), 1);
	column = gtk_tree_view_column_new_with_attributes(name, renderer,
		"background-gdk", bg_column,
		"foreground-gdk", fg_column,
		"text", id,
		NULL);
	g_object_set(G_OBJECT(renderer),
		"background-set", TRUE,
		"foreground-set", TRUE,
		"mode", GTK_CELL_RENDERER_MODE_INERT,
		"xalign", xalign,
		"ypad", (guint) GUI_CELL_RENDERER_YPAD,
		NULL);
	g_object_set(G_OBJECT(column),
		"fixed-width", MAX(1, width),
		"min-width", 1,
		"reorderable", TRUE,
		"resizable", TRUE,
		"sizing", GTK_TREE_VIEW_COLUMN_FIXED,
		NULL);
    gtk_tree_view_column_set_sort_column_id(column, id);
    gtk_tree_view_append_column(treeview, column);

	return column;
}

static void
add_results_columns(GtkTreeView *treeview)
{
	static const struct {
		const gchar * const title;
		const gint id;
		const gfloat align;
		const GtkTreeIterCompareFunc func;
	} columns[] = {
		{ N_("File"),	   c_sr_filename, 0.0, NULL },
		{ N_("Extension"), c_sr_ext,	  0.0, NULL },
		{ N_("Size"),	   c_sr_size,	  1.0, search_gui_compare_size_func },
		{ N_("#"),		   c_sr_count,	  1.0, search_gui_compare_count_func },
		{ N_("Loc"),	   c_sr_loc,	  0.0, NULL },
		{ N_("Metadata"),  c_sr_meta,	  0.0, NULL },
		{ N_("Info"),	   c_sr_info,	  0.0, NULL }
	};
	guint32 width[G_N_ELEMENTS(columns)];
	guint i;

	STATIC_ASSERT(SEARCH_RESULTS_VISIBLE_COLUMNS == G_N_ELEMENTS(columns));

    gui_prop_get_guint32(PROP_SEARCH_RESULTS_COL_WIDTHS, width, 0,
		G_N_ELEMENTS(width));
	for (i = 0; i < G_N_ELEMENTS(columns); i++) {
		add_results_column(treeview, _(columns[i].title), columns[i].id,
			width[columns[i].id], columns[i].align, columns[i].func);
	}
}

/* Create a new GtkTreeView for search results */

void
gui_search_create_tree_view(GtkWidget ** sw, GtkWidget ** tv)
{
	GtkTreeModel *model = create_model();
	GtkTreeSelection *selection;
	GtkTreeView	*treeview;

	*sw = gtk_scrolled_window_new(NULL, NULL);

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(*sw),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);

	treeview = GTK_TREE_VIEW(gtk_tree_view_new_with_model(model));
	*tv = GTK_WIDGET(treeview);
	g_object_unref(model);

	selection = gtk_tree_view_get_selection(treeview);
	gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
	gtk_tree_view_set_headers_visible(treeview, TRUE);
	gtk_tree_view_set_headers_clickable(treeview, TRUE);
	gtk_tree_view_set_enable_search(treeview, TRUE);
	gtk_tree_view_set_rules_hint(treeview, TRUE);

      /* add columns to the tree view */
	add_results_columns(treeview);

	gtk_container_add(GTK_CONTAINER(*sw), *tv);

	if (!GTK_WIDGET_VISIBLE (*sw))
		gtk_widget_show_all(*sw);

	g_signal_connect(GTK_OBJECT(treeview), "cursor-changed",
		G_CALLBACK(on_tree_view_search_results_select_row), treeview);
	g_signal_connect(GTK_OBJECT(treeview), "button_press_event",
		G_CALLBACK(on_tree_view_search_results_button_press_event), NULL);
    g_signal_connect(GTK_OBJECT(treeview), "key_press_event",
		G_CALLBACK(on_tree_view_search_results_key_press_event), NULL);
    g_signal_connect(GTK_OBJECT(treeview), "enter-notify-event",
		G_CALLBACK(on_enter_notify), NULL);
    g_signal_connect(GTK_OBJECT(treeview), "leave-notify-event",
		G_CALLBACK(on_leave_notify), NULL);
}

static gboolean
tree_view_search_update(
	GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	search_t *sch;

	(void) path;
    gtk_tree_model_get(model, iter, c_sl_sch, &sch, (-1));
 	if (sch == data) {
   		GtkWidget *widget;
		GdkColor *fg;
		GdkColor *bg;

		widget = GTK_WIDGET(tree_view_search);
		if (sch->unseen_items > 0) {
    		fg = &(gtk_widget_get_style(widget)->fg[GTK_STATE_PRELIGHT]);
    		bg = &(gtk_widget_get_style(widget)->bg[GTK_STATE_PRELIGHT]);
		} else if (!sch->enabled) {
    		fg = &(gtk_widget_get_style(widget)->fg[GTK_STATE_INSENSITIVE]);
    		bg = &(gtk_widget_get_style(widget)->bg[GTK_STATE_INSENSITIVE]);
		} else {
			fg = NULL;
			bg = NULL;
		}

		gtk_tree_store_set((GtkTreeStore *) model, iter,
			c_sl_hit, sch->items,
			c_sl_new, sch->unseen_items,
			c_sl_fg, fg,
			c_sl_bg, bg,
			(-1));
		return TRUE;
	}

	return FALSE;
}

/* Like search_update_tab_label but always update the label */
void
gui_search_force_update_tab_label(search_t *sch, time_t now)
{
    search_t *search;
	GtkTreeModel *model;

    search = search_gui_get_current_search();

	if (sch == search || sch->unseen_items == 0)
		gm_snprintf(tmpstr, sizeof(tmpstr), "%s\n(%d)", sch->query,
				   sch->items);
	else
		gm_snprintf(tmpstr, sizeof(tmpstr), "%s\n(%d, %d)", sch->query,
				   sch->items, sch->unseen_items);
	sch->last_update_items = sch->items;
	gtk_notebook_set_tab_label_text
        (notebook_search_results, sch->scrolled_window, tmpstr);
	model = gtk_tree_view_get_model(tree_view_search);
	gtk_tree_model_foreach(model, tree_view_search_update, sch);
	sch->last_update_time = now;
}

/**
 * Doesn't update the label if nothing's changed or if the last update was
 * recent.
 */
gboolean
gui_search_update_tab_label(struct search *sch)
{
	static time_t now;

	if (sch->items != sch->last_update_items) {
		now = time(NULL);

		if (delta_time(now, sch->last_update_time) >= TAB_UPDATE_TIME)
			gui_search_force_update_tab_label(sch, now);
	}

	return TRUE;
}

void
gui_search_clear_results(void)
{
	search_t *search;

	search = search_gui_get_current_search();
	search_gui_reset_search(search);
	gui_search_force_update_tab_label(search, time(NULL));
	search_gui_update_items(search);
}

/*
 * gui_search_get_colors
 *
 * Extract the mark/ignore/download color.
 */
void
gui_search_get_colors(
	search_t *sch,
	GdkColor **mark_color, GdkColor **ignore_color, GdkColor **download_color)
{
    *mark_color = &(gtk_widget_get_style(GTK_WIDGET(sch->tree_view))
        ->bg[GTK_STATE_INSENSITIVE]);

    *ignore_color = &(gtk_widget_get_style(GTK_WIDGET(sch->tree_view))
        ->fg[GTK_STATE_INSENSITIVE]);

    *download_color =  &(gtk_widget_get_style(GTK_WIDGET(sch->tree_view))
        ->fg[GTK_STATE_ACTIVE]);
}

/**
 * Adds a search string to the search history combo. Makes
 * sure we do not get more than 10 entries in the history.
 * Also makes sure we don't get duplicate history entries.
 * If a string is already in history and it's added again,
 * it's moved to the beginning of the history list.
 */
void
gui_search_history_add(gchar *s)
{
    GList *new_hist = NULL;
    GList *cur_hist = list_search_history;
    guint n = 0;

    g_return_if_fail(s);

    while (cur_hist != NULL) {
        if (n < 9 && 0 != g_ascii_strcasecmp(s, cur_hist->data)) {
            /* copy up to the first 9 items */
            new_hist = g_list_append(new_hist, cur_hist->data);
            n ++;
        } else {
            /* and free the rest */
            G_FREE_NULL(cur_hist->data);
        }
        cur_hist = cur_hist->next;
    }
    /* put the new item on top */
    new_hist = g_list_prepend(new_hist, g_strdup(s));

    /* set new history */
    gtk_combo_set_popdown_strings(
        GTK_COMBO(lookup_widget(main_window, "combo_search")),
        new_hist);

    /* free old list structure */
    g_list_free(list_search_history);

    list_search_history = new_hist;
}


/**
 * Flag whether search is enabled.
 */
void
gui_search_set_enabled(struct search *sch, gboolean enabled)
{
	gboolean was_enabled = sch->enabled;

	if (was_enabled == enabled)
		return;

	sch->enabled = enabled;

	if (enabled)
		guc_search_start(sch->search_handle);
	else
		guc_search_stop(sch->search_handle);

	/* Marks this entry as active/inactive in the searches list. */
	gui_search_force_update_tab_label(sch, time(NULL));
}


/**
 *	Expand all nodes in tree for current search
 */
void
search_gui_expand_all(void)
{
	search_t *current_search = search_gui_get_current_search();
	gtk_tree_view_expand_all(GTK_TREE_VIEW(current_search->tree_view));
}


/**
 *	Collapse all nodes in tree for current search
 */
void
search_gui_collapse_all(void)
{
	search_t *current_search = search_gui_get_current_search();
	gtk_tree_view_collapse_all(GTK_TREE_VIEW(current_search->tree_view));
}

void
search_gui_start_massive_update(search_t *sch)
{
	g_assert(sch);

   	if (sch == search_gui_get_current_search() || sch->massive_update)
		return;

	g_object_ref(sch->model);
	gtk_tree_view_set_model(GTK_TREE_VIEW(sch->tree_view), NULL);
	sch->massive_update = TRUE;
}

void
search_gui_end_massive_update(search_t *sch)
{
	g_assert(sch);

	if (!sch->massive_update)
		return;

    gui_search_force_update_tab_label(sch, time(NULL));
	gtk_tree_view_set_model(GTK_TREE_VIEW(sch->tree_view),
		GTK_TREE_MODEL(sch->model));
	g_object_unref(GTK_TREE_MODEL(sch->model));
	sch->massive_update = FALSE;
}

/**
 *
 * Update the search displays with the correct meta-data
 *
 */
void
search_gui_metadata_update(const bitzi_data_t *data)
{
	GList *l;
	gchar *text;

	text = bitzi_gui_get_metadata(data);

	/*
	 * Fill in the columns in each search that contains a reference
	 */

	for (l = searches; l != NULL; l = g_list_next(l)) {
		search_t *search = l->data;
    	GtkTreeIter *parent;

#if 0
		g_message("search_metadata_cb: search %p, iter = %p",search, iter);
#endif

	   	parent = find_parent_with_sha1(search->parents, data->urnsha1);
		if (parent) {
			gtk_tree_store_set(GTK_TREE_STORE(search->model), parent,
					c_sr_meta, text ? text : _("Not in database"),
					(-1));
		}
	}

	/* free the string */
	g_free(text);
}

/* vi: set ts=4 sw=4 cindent: */
