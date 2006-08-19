/*
 * $Id$
 *
 * Copyright (c) 2004, Raphael Manfredi
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

#ifndef _gtk_ui_h_
#define _gtk_ui_h_

#include "common.h"

#ifdef USE_TOPLESS

/* Diverse dummy definitions */
#define settings_gui_save_if_dirty()

#define main_gui_early_init(argc, argv)
#define main_gui_init()
#define main_gui_timer(x)
#define main_gui_update_coords()
#define main_gui_shutdown()
#define main_gui_shutdown_tick(remain)
#define settings_gui_shutdown()

#define drop_init()
#define drop_close()

#define search_gui_store_searches()

#define icon_timer()

static inline void
main_gui_run(void)
{
	GMainLoop *ml;

#if defined(USE_GLIB1)
	ml = g_main_new(FALSE);
	g_main_run(ml);
#elif defined(USE_GLIB2)
	ml = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(ml);
#endif /* GLIB */
}

#else	/* !USE_TOPLESS */

#include <gtk/gtk.h>

#ifdef USE_GTK1
#include "gtk1/support-glade.h"
#endif
#ifdef USE_GTK2
#include "gtk2/support-glade.h"
#endif

#include "main.h"

#ifdef USE_GTK1
#define g_ascii_strcasecmp g_strcasecmp
#define gdk_drawable_get_size gdk_window_get_size
#endif

/* Common padding values for GtkCellRenderer */
#define GUI_CELL_RENDERER_XPAD ((guint) 4U)
#define GUI_CELL_RENDERER_YPAD ((guint) 0U)

/**
 * Sorting constants.
 */

enum sorting_order {
	SORT_DESC = -1,
	SORT_NONE = 0,
	SORT_ASC = 1,
	SORT_NO_COL = 2		/**< No column chosen yet */
};

#endif	/* USE_TOPLESS */
#endif /* _gtk_ui_h_ */

/* vi: set ts=4 sw=4 cindent: */
