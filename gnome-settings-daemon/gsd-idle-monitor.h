/* -*- mode: C; c-file-style: "linux"; indent-tabs-mode: t -*-
 *
 * Adapted from gnome-session/gnome-session/gs-idle-monitor.h
 *
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Authors: William Jon McCann <mccann@jhu.edu>
 */

#ifndef __GSD_IDLE_MONITOR_H__
#define __GSD_IDLE_MONITOR_H__

#include <glib-object.h>
#include <gdk/gdk.h>

G_BEGIN_DECLS

#define GSD_TYPE_IDLE_MONITOR         (gsd_idle_monitor_get_type ())
#define GSD_IDLE_MONITOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), GSD_TYPE_IDLE_MONITOR, GsdIdleMonitor))
#define GSD_IDLE_MONITOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), GSD_TYPE_IDLE_MONITOR, GsdIdleMonitorClass))
#define GSD_IS_IDLE_MONITOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), GSD_TYPE_IDLE_MONITOR))
#define GSD_IS_IDLE_MONITOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), GSD_TYPE_IDLE_MONITOR))
#define GSD_IDLE_MONITOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), GSD_TYPE_IDLE_MONITOR, GsdIdleMonitorClass))

typedef struct _GsdIdleMonitor GsdIdleMonitor;
typedef struct _GsdIdleMonitorClass GsdIdleMonitorClass;
typedef struct _GsdIdleMonitorPrivate GsdIdleMonitorPrivate;

struct _GsdIdleMonitor
{
        GObject                  parent;
        GsdIdleMonitorPrivate *priv;
};

struct _GsdIdleMonitorClass
{
        GObjectClass          parent_class;
};

typedef void (*GsdIdleMonitorWatchFunc) (GsdIdleMonitor      *monitor,
                                           guint                  id,
                                           gpointer               user_data);

GType              gsd_idle_monitor_get_type     (void);

GsdIdleMonitor * gsd_idle_monitor_new          (void);
GsdIdleMonitor * gsd_idle_monitor_new_for_device (GdkDevice *device);

guint              gsd_idle_monitor_add_idle_watch    (GsdIdleMonitor         *monitor,
							 guint64                   interval_msec,
							 GsdIdleMonitorWatchFunc callback,
							 gpointer                  user_data,
							 GDestroyNotify            notify);

guint              gsd_idle_monitor_add_user_active_watch (GsdIdleMonitor          *monitor,
							     GsdIdleMonitorWatchFunc  callback,
							     gpointer		     user_data,
							     GDestroyNotify	     notify);

void               gsd_idle_monitor_remove_watch (GsdIdleMonitor         *monitor,
                                                    guint                     id);

gint64             gsd_idle_monitor_get_idletime (GsdIdleMonitor         *monitor);

G_END_DECLS

#endif /* __GSD_IDLE_MONITOR_H__ */
