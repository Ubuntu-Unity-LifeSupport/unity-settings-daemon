/* gsd-bg-slide_show.h - fade window background between two surfaces

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

#ifndef __GSD_BG_SLIDE_SHOW_H__
#define __GSD_BG_SLIDE_SHOW_H__


#include <gdk/gdk.h>

G_BEGIN_DECLS

#define GSD_BG_TYPE_SLIDE_SHOW            (gsd_bg_slide_show_get_type ())
#define GSD_BG_SLIDE_SHOW(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GSD_BG_TYPE_SLIDE_SHOW, GsdBGSlideShow))
#define GSD_BG_SLIDE_SHOW_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  GSD_BG_TYPE_SLIDE_SHOW, GsdBGSlideShowClass))
#define GSD_BG_IS_SLIDE_SHOW(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GSD_BG_TYPE_SLIDE_SHOW))
#define GSD_BG_IS_SLIDE_SHOW_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  GSD_BG_TYPE_SLIDE_SHOW))
#define GSD_BG_SLIDE_SHOW_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  GSD_BG_TYPE_SLIDE_SHOW, GsdBGSlideShowClass))

typedef struct _GsdBGSlideShowPrivate GsdBGSlideShowPrivate;
typedef struct _GsdBGSlideShow GsdBGSlideShow;
typedef struct _GsdBGSlideShowClass GsdBGSlideShowClass;

struct _GsdBGSlideShow
{
	GObject parent_object;

	GsdBGSlideShowPrivate *priv;
};

struct _GsdBGSlideShowClass
{
	GObjectClass parent_class;
};

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsdBGSlideShow, g_object_unref)

GType             gsd_bg_slide_show_get_type (void);
GsdBGSlideShow *gsd_bg_slide_show_new (const char *filename);
gboolean          gsd_bg_slide_show_load (GsdBGSlideShow  *self,
                                            GError           **error);

void              gsd_bg_slide_show_load_async (GsdBGSlideShow    *self,
                                                  GCancellable        *cancellable,
                                                  GAsyncReadyCallback  callback,
                                                  gpointer             user_data);
gboolean          gsd_bg_slide_show_get_slide (GsdBGSlideShow *self,
                                                 int               frame_number,
                                                 int               width,
                                                 int               height,
                                                 gdouble          *progress,
                                                 double           *duration,
                                                 gboolean         *is_fixed,
                                                 const char      **file1,
                                                 const char      **file2);

void              gsd_bg_slide_show_get_current_slide (GsdBGSlideShow  *self,
                                                         int                width,
                                                         int                height,
                                                         gdouble           *progress,
                                                         double            *duration,
                                                         gboolean          *is_fixed,
                                                         const char       **file1,
                                                         const char       **file2);


double gsd_bg_slide_show_get_start_time (GsdBGSlideShow *self);
double gsd_bg_slide_show_get_total_duration (GsdBGSlideShow *self);
gboolean gsd_bg_slide_show_get_has_multiple_sizes (GsdBGSlideShow *self);
int  gsd_bg_slide_show_get_num_slides (GsdBGSlideShow *self);
G_END_DECLS

#endif
