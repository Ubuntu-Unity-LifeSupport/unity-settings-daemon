/* gsd-bg.h - 

   Copyright 2007, Red Hat, Inc.

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

   Author: Soren Sandmann <sandmann@redhat.com>
*/

#ifndef __GSD_BG_H__
#define __GSD_BG_H__

#define GNOME_DESKTOP_USE_UNSTABLE_API

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <gdesktop-enums.h>
#include <libgnome-desktop/gnome-desktop-thumbnail.h>
#include <gdesktop-enums.h>

#include "gsd-bg-crossfade.h"

G_BEGIN_DECLS

#define GSD_TYPE_BG            (gsd_bg_get_type ())
#define GSD_BG(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GSD_TYPE_BG, GsdBG))
#define GSD_BG_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  GSD_TYPE_BG, GsdBGClass))
#define GSD_IS_BG(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GSD_TYPE_BG))
#define GSD_IS_BG_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  GSD_TYPE_BG))
#define GSD_BG_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  GSD_TYPE_BG, GsdBGClass))

typedef struct _GsdBG GsdBG;
typedef struct _GsdBGClass GsdBGClass;

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsdBG, g_object_unref)

GType            gsd_bg_get_type              (void);
GsdBG *        gsd_bg_new                   (void);
void             gsd_bg_load_from_preferences (GsdBG               *bg,
						 GSettings             *settings);
void             gsd_bg_save_to_preferences   (GsdBG               *bg,
						 GSettings             *settings);
/* Setters */
void             gsd_bg_set_filename          (GsdBG               *bg,
						 const char            *filename);
void             gsd_bg_set_placement         (GsdBG               *bg,
						 GDesktopBackgroundStyle placement);
void             gsd_bg_set_rgba              (GsdBG               *bg,
						 GDesktopBackgroundShading type,
						 GdkRGBA               *primary,
						 GdkRGBA               *secondary);
void             gsd_bg_set_draw_background   (GsdBG               *bg,
						 gboolean               draw_background);

/* Getters */
GDesktopBackgroundStyle gsd_bg_get_placement  (GsdBG               *bg);
gboolean         gsd_bg_get_draw_background   (GsdBG               *bg);
void		 gsd_bg_get_rgba              (GsdBG               *bg,
						 GDesktopBackgroundShading *type,
						 GdkRGBA               *primary,
						 GdkRGBA               *secondary);
const gchar *    gsd_bg_get_filename          (GsdBG               *bg);

/* Drawing and thumbnailing */
void             gsd_bg_draw                  (GsdBG               *bg,
						 GdkPixbuf             *dest,
						 GdkScreen	       *screen,
                                                 gboolean               is_root);
cairo_surface_t *gsd_bg_create_surface        (GsdBG               *bg,
						 GdkWindow             *window,
						 int                    width,
						 int                    height,
						 gboolean               root);
gboolean         gsd_bg_get_image_size        (GsdBG               *bg,
						 GnomeDesktopThumbnailFactory *factory,
                                                 int                    best_width,
                                                 int                    best_height,
						 int                   *width,
						 int                   *height);
GdkPixbuf *      gsd_bg_create_thumbnail      (GsdBG               *bg,
						 GnomeDesktopThumbnailFactory *factory,
						 GdkScreen             *screen,
						 int                    dest_width,
						 int                    dest_height);
gboolean         gsd_bg_is_dark               (GsdBG               *bg,
                                                 int                    dest_width,
						 int                    dest_height);
gboolean         gsd_bg_has_multiple_sizes    (GsdBG               *bg);
gboolean         gsd_bg_changes_with_time     (GsdBG               *bg);
GdkPixbuf *      gsd_bg_create_frame_thumbnail (GsdBG              *bg,
						 GnomeDesktopThumbnailFactory *factory,
						 GdkScreen             *screen,
						 int                    dest_width,
						 int                    dest_height,
						 int                    frame_num);

/* Set a surface as root - not a GsdBG method. At some point
 * if we decide to stabilize the API then we may want to make
 * these object methods, drop gsd_bg_create_surface, etc.
 */
void             gsd_bg_set_surface_as_root   (GdkScreen             *screen,
						 cairo_surface_t       *surface);

GsdBGCrossfade *gsd_bg_set_surface_as_root_with_crossfade (GdkScreen *screen,
                                                              cairo_surface_t *surface);
cairo_surface_t *gsd_bg_get_surface_from_root (GdkScreen *screen);

G_END_DECLS

#endif
