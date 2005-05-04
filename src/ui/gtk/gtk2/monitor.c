/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
 *
 * GUI stuff used by share.c
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

#include "gtk/gui.h"

RCSID("$Id$");

#include "gtk/monitor.h"
#include "gtk/monitor_cb.h"

#include "if/gui_property_priv.h"
#include "if/bridge/ui2c.h"

#include "lib/utf8.h"
#include "lib/glib-missing.h"
#include "lib/override.h"		/* Must be the last header included */

static guint32 monitor_items = 0;
static GtkListStore *monitor_model = NULL;

enum {
   QUERY_COLUMN = 0,
   MONITOR_COLUMNS
};



/***
 *** Callbacks
 ***/

static void
monitor_gui_append(query_type_t type, const gchar *item,
	guint32 ip, guint16 port)
{
	(void) ip;
	(void) port;

	/* The user might have changed the max. number of items to
	 * show, that's why we don't just the remove the first item. */
	for (/* empty */; monitor_items >= monitor_max_items; monitor_items--) {
		GtkTreeIter iter;

        /* Get the first iter in the list */
        if (
			!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(monitor_model),
					&iter)
			) {
				break;
			}

		gtk_list_store_remove(monitor_model, &iter);
    }

	if (monitor_max_items > 0) {
		GtkTreeIter iter;
		gchar buf[128];
		const gchar *s;

    	/* Aquire an iterator */
    	gtk_list_store_append(monitor_model, &iter);
    	monitor_items++;

		/* If the query is empty and we have a SHA1 extension,
	 	 * we print a urn:sha1-query instead. */
		concat_strings(buf, sizeof buf,
			QUERY_SHA1 == type ? "urn:sha1:" : "", item, (void *) 0);

		s = lazy_locale_to_utf8(buf, 0);
   		gtk_list_store_set(monitor_model, &iter, QUERY_COLUMN, s, (-1));
	}
}

/***
 *** Public functions
 ***/

void
monitor_gui_init(void)
{
    GtkWidget *tree;
	GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;

    /* Create a model.  We are using the store model for now, though we
     * could use any other GtkTreeModel */
    monitor_model = gtk_list_store_new(MONITOR_COLUMNS, G_TYPE_STRING);

    /* Get the monitor widget */
    tree = lookup_widget(main_window, "treeview_monitor");

    gtk_tree_view_set_model
        (GTK_TREE_VIEW(tree), GTK_TREE_MODEL(monitor_model));

    /* The view now holds a reference.  We can get rid of our own
     * reference */
    g_object_unref(G_OBJECT(monitor_model));

    /* Create a column, associating the "text" attribute of the
     * cell_renderer to the first column of the model */
    renderer = gtk_cell_renderer_text_new();
	g_object_set(renderer, "ypad", GUI_CELL_RENDERER_YPAD, (void *) 0);
    column = gtk_tree_view_column_new_with_attributes
        (_("Query"), renderer, "text", QUERY_COLUMN, (void *) 0);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);

    /* Add the column to the view. */
    gtk_tree_view_append_column (GTK_TREE_VIEW(tree), column);

	g_signal_connect(G_OBJECT(tree), "button_press_event",
		G_CALLBACK(on_treeview_monitor_button_press_event), NULL);
}

void
monitor_gui_shutdown(void)
{
    monitor_gui_enable_monitor(FALSE);
}

#if 0
/**
 * Remove all but the first n items from the monitor.
 */
void
share_gui_clear_monitor(void)
{
    gtk_list_store_clear(monitor_model);
	monitor_items = 0;
}
#endif

/**
 * Enable/disable monitor.
 */
void monitor_gui_enable_monitor(const gboolean val)
{
    static gboolean registered = FALSE;

    if (val != registered) {
        if (val)
            guc_share_add_search_request_listener(monitor_gui_append);
        else
            guc_share_remove_search_request_listener(monitor_gui_append);
        registered = val;
    }
}

/* vi: set ts=4 sw=4 cindent: */
