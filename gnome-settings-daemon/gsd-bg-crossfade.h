/* gsd-bg-crossfade.h - fade window background between two surfaces

   Copyright 2008, Red Hat, Inc.

   This file is part of the Gnome Library.

   The Gnome Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Gnome Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Gnome Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.

   Author: Ray Strode <rstrode@redhat.com>
*/

#ifndef __GSD_BG_CROSSFADE_H__
#define __GSD_BG_CROSSFADE_H__


#include <gdk/gdk.h>

G_BEGIN_DECLS

#define GSD_TYPE_BG_CROSSFADE            (gsd_bg_crossfade_get_type ())
#define GSD_BG_CROSSFADE(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GSD_TYPE_BG_CROSSFADE, GsdBGCrossfade))
#define GSD_BG_CROSSFADE_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  GSD_TYPE_BG_CROSSFADE, GsdBGCrossfadeClass))
#define GSD_IS_BG_CROSSFADE(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GSD_TYPE_BG_CROSSFADE))
#define GSD_IS_BG_CROSSFADE_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  GSD_TYPE_BG_CROSSFADE))
#define GSD_BG_CROSSFADE_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  GSD_TYPE_BG_CROSSFADE, GsdBGCrossfadeClass))

typedef struct _GsdBGCrossfadePrivate GsdBGCrossfadePrivate;
typedef struct _GsdBGCrossfade GsdBGCrossfade;
typedef struct _GsdBGCrossfadeClass GsdBGCrossfadeClass;

struct _GsdBGCrossfade
{
	GObject parent_object;

	GsdBGCrossfadePrivate *priv;
};

struct _GsdBGCrossfadeClass
{
	GObjectClass parent_class;

	void (* finished) (GsdBGCrossfade *fade, GdkWindow *window);
};

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsdBGCrossfade, g_object_unref)

GType             gsd_bg_crossfade_get_type              (void);
GsdBGCrossfade *gsd_bg_crossfade_new (int width, int height);
gboolean          gsd_bg_crossfade_set_start_surface (GsdBGCrossfade *fade,
                                                        cairo_surface_t *surface);
gboolean          gsd_bg_crossfade_set_end_surface (GsdBGCrossfade *fade,
                                                      cairo_surface_t *surface);
void              gsd_bg_crossfade_start (GsdBGCrossfade *fade,
                                            GdkWindow        *window);
gboolean          gsd_bg_crossfade_is_started (GsdBGCrossfade *fade);
void              gsd_bg_crossfade_stop (GsdBGCrossfade *fade);

G_END_DECLS

#endif
