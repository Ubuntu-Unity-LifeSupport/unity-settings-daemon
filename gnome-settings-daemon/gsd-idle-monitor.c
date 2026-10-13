/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright 2013 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Adapted from gnome-session/gnome-session/gs-idle-monitor.c and
 *         from gnome-desktop/libgnome-desktop/gnome-idle-monitor.c
 */

/**
 * SECTION:idle-monitor
 * @title: GsdIdleMonitor
 * @short_description: Mutter idle counter (similar to X's IDLETIME)
 */

#include "config.h"

#include <string.h>
#include <X11/Xlib.h>
#include <X11/extensions/sync.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>

#include "gsd-idle-monitor.h"
#include "gsd-idle-monitor-private.h"
#include "meta-dbus-idle-monitor.h"
#include "muffin-dbus-idle-monitor.h"

G_STATIC_ASSERT(sizeof(unsigned long) == sizeof(gpointer));

struct _GsdIdleMonitor
{
  GObject parent_instance;

  GHashTable  *watches;
  GHashTable  *alarms;
  int          device_id;

  /* X11 implementation */
  Display     *display;
  int          sync_event_base;

  XSyncCounter counter;
  XSyncAlarm   user_active_alarm;
};

struct _GsdIdleMonitorClass
{
  GObjectClass parent_class;
};

typedef struct
{
  GsdIdleMonitor          *monitor;
  guint	                    id;
  GsdIdleMonitorWatchFunc  callback;
  gpointer		    user_data;
  GDestroyNotify            notify;
  guint64                   timeout_msec;

  /* x11 */
  XSyncAlarm                xalarm;
  int                       idle_source_id;
} GsdIdleMonitorWatch;

typedef struct
{
        /* X11 implementation */
        Display     *display;
        int          sync_event_base;
        int          sync_error_base;
        unsigned int have_xsync : 1;

}GsdXSync;

static GsdXSync *xsync;

enum
{
  PROP_0,
  PROP_DEVICE_ID,
  PROP_LAST,
};

static GParamSpec *obj_props[PROP_LAST];

G_DEFINE_TYPE (GsdIdleMonitor, gsd_idle_monitor, G_TYPE_OBJECT)

static GsdIdleMonitor  *device_monitors[256];
static int              device_id_max;

static gint64
_xsyncvalue_to_int64 (XSyncValue value)
{
  return ((guint64) XSyncValueHigh32 (value)) << 32
    | (guint64) XSyncValueLow32 (value);
}

#define GUINT64_TO_XSYNCVALUE(value, ret) XSyncIntsToValue (ret, (value) & 0xFFFFFFFF, ((guint64)(value)) >> 32)

static void
fire_watch (GsdIdleMonitorWatch *watch)
{
  GsdIdleMonitor *monitor;
  guint id;
  gboolean is_user_active_watch;

  monitor = watch->monitor;
  g_object_ref (monitor);

  if (watch->idle_source_id)
    {
      g_source_remove (watch->idle_source_id);
      watch->idle_source_id = 0;
    }

  id = watch->id;
  is_user_active_watch = (watch->timeout_msec == 0);

  if (watch->callback)
    watch->callback (monitor, id, watch->user_data);

  if (is_user_active_watch)
    gsd_idle_monitor_remove_watch (monitor, id);

  g_object_unref (monitor);
}

static XSyncAlarm
_xsync_alarm_set (GsdIdleMonitor	*monitor,
		  XSyncTestType          test_type,
		  guint64                interval,
		  gboolean               want_events)
{
  XSyncAlarmAttributes attr;
  XSyncValue	     delta;
  guint		     flags;

  flags = XSyncCACounter | XSyncCAValueType | XSyncCATestType |
    XSyncCAValue | XSyncCADelta | XSyncCAEvents;

  XSyncIntToValue (&delta, 0);
  attr.trigger.counter = monitor->counter;
  attr.trigger.value_type = XSyncAbsolute;
  attr.delta = delta;
  attr.events = want_events;

  GUINT64_TO_XSYNCVALUE (interval, &attr.trigger.wait_value);
  attr.trigger.test_type = test_type;
  return XSyncCreateAlarm (monitor->display, flags, &attr);
}

static void
ensure_alarm_rescheduled (Display    *dpy,
			  XSyncAlarm  alarm)
{
  XSyncAlarmAttributes attr;

  /* Some versions of Xorg have an issue where alarms aren't
   * always rescheduled. Calling XSyncChangeAlarm, even
   * without any attributes, will reschedule the alarm. */
  XSyncChangeAlarm (dpy, alarm, 0, &attr);
}

static void
set_alarm_enabled (Display    *dpy,
		   XSyncAlarm  alarm,
		   gboolean    enabled)
{
  XSyncAlarmAttributes attr;
  attr.events = enabled;
  XSyncChangeAlarm (dpy, alarm, XSyncCAEvents, &attr);
}

static void
check_x11_watch (gpointer data,
                 gpointer user_data)
{
  GsdIdleMonitorWatch *watch = data;
  XSyncAlarm alarm = (XSyncAlarm) user_data;

  if (watch->xalarm != alarm)
    return;

  fire_watch (watch);
}

static void
gsd_idle_monitor_handle_xevent (GsdIdleMonitor       *monitor,
                                 XSyncAlarmNotifyEvent *alarm_event)
{
  XSyncAlarm alarm;
  GList *watches;
  gboolean has_alarm;


  if (alarm_event->state != XSyncAlarmActive)
    return;

  alarm = alarm_event->alarm;

  has_alarm = FALSE;

  if (alarm == monitor->user_active_alarm)
    {
      set_alarm_enabled (monitor->display,
                         alarm,
                         FALSE);
      has_alarm = TRUE;
    }
  else if (g_hash_table_contains (monitor->alarms, (gpointer) alarm))
    {
      ensure_alarm_rescheduled (monitor->display,
                                alarm);
      has_alarm = TRUE;
    }

  if (has_alarm)
    {
      watches = g_hash_table_get_values (monitor->watches);

      g_list_foreach (watches, check_x11_watch, (gpointer) alarm);
      g_list_free (watches);
    }
}

void
gsd_idle_monitor_handle_xevent_all (XEvent *xevent)
{
  int i;

  for (i = 0; i <= device_id_max; i++)
    if (device_monitors[i])
      gsd_idle_monitor_handle_xevent (device_monitors[i], (XSyncAlarmNotifyEvent*)xevent);
}

static char *
counter_name_for_device (int device_id)
{
  if (device_id > 0)
    return g_strdup_printf ("DEVICEIDLETIME %d", device_id);

  return g_strdup ("IDLETIME");
}

static XSyncCounter
find_idletime_counter (GsdIdleMonitor *monitor)
{
  int		      i;
  int		      ncounters;
  XSyncSystemCounter *counters;
  XSyncCounter        counter = None;
  char               *counter_name;

  counter_name = counter_name_for_device (monitor->device_id);
  counters = XSyncListSystemCounters (monitor->display, &ncounters);
  for (i = 0; i < ncounters; i++)
    {
      if (counters[i].name != NULL && strcmp (counters[i].name, counter_name) == 0)
        {
          counter = counters[i].counter;
          break;
        }
    }
  XSyncFreeSystemCounterList (counters);
  g_free (counter_name);

  return counter;
}

static guint32
get_next_watch_serial (void)
{
  static guint32 serial = 0;
  g_atomic_int_inc (&serial);
  return serial;
}

static void
idle_monitor_watch_free (GsdIdleMonitorWatch *watch)
{
  GsdIdleMonitor *monitor;

  if (watch == NULL)
    return;

  monitor = watch->monitor;
  g_object_ref (monitor);

  if (watch->idle_source_id)
    {
      g_source_remove (watch->idle_source_id);
      watch->idle_source_id = 0;
    }

  if (watch->notify != NULL)
    watch->notify (watch->user_data);

  if (watch->xalarm != monitor->user_active_alarm &&
      watch->xalarm != None)
    {
      XSyncDestroyAlarm (monitor->display, watch->xalarm);
      g_hash_table_remove (monitor->alarms, (gpointer) watch->xalarm);
    }

  g_object_unref (monitor);
  g_slice_free (GsdIdleMonitorWatch, watch);
}

static GdkFilterReturn
xevent_filter (GdkXEvent *xevent,
               GdkEvent  *event,
               gpointer   user_data)
{
  XEvent *ev;

  ev = xevent;
  if (ev->xany.type == xsync->sync_event_base + XSyncAlarmNotify) {
      gsd_idle_monitor_handle_xevent_all (ev);
  }
  return GDK_FILTER_CONTINUE;
}

static void
init_xsync (GsdIdleMonitor *monitor)
{

  monitor->counter = find_idletime_counter (monitor);
  /* IDLETIME counter not found? */
  if (monitor->counter == None)
    {
      g_warning ("IDLETIME counter not found\n");
      return;
    }

  monitor->user_active_alarm = _xsync_alarm_set (monitor, XSyncNegativeTransition, 1, FALSE);
}

static void
gsd_idle_monitor_dispose (GObject *object)
{
  GsdIdleMonitor *monitor;

  monitor = gsd_idle_monitor (object);

  g_clear_pointer (&monitor->watches, g_hash_table_destroy);
  g_clear_pointer (&monitor->alarms, g_hash_table_destroy);

  if (monitor->user_active_alarm != None)
    {
      XSyncDestroyAlarm (monitor->display, monitor->user_active_alarm);
      monitor->user_active_alarm = None;
    }

  /* The device in device_monitors is cleared when the device is
   * removed. Ensure that the object is not deleted before that. */
  g_assert_null (device_monitors[monitor->device_id]);

  G_OBJECT_CLASS (gsd_idle_monitor_parent_class)->dispose (object);
}

static void
gsd_idle_monitor_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  GsdIdleMonitor *monitor = gsd_idle_monitor (object);

  switch (prop_id)
    {
    case PROP_DEVICE_ID:
      g_value_set_int (value, monitor->device_id);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gsd_idle_monitor_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  GsdIdleMonitor *monitor = gsd_idle_monitor (object);
  switch (prop_id)
    {
    case PROP_DEVICE_ID:
      monitor->device_id = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gsd_idle_monitor_constructed (GObject *object)
{
  GsdIdleMonitor *monitor = gsd_idle_monitor (object);

  monitor->display = GDK_DISPLAY_XDISPLAY (gdk_display_get_default ());
  init_xsync (monitor);
}

static void
gsd_idle_monitor_class_init (GsdIdleMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gsd_idle_monitor_dispose;
  object_class->constructed = gsd_idle_monitor_constructed;
  object_class->get_property = gsd_idle_monitor_get_property;
  object_class->set_property = gsd_idle_monitor_set_property;

  /**
   * GsdIdleMonitor:device_id:
   *
   * The device to listen to idletime on.
   */
  obj_props[PROP_DEVICE_ID] =
    g_param_spec_int ("device-id",
                      "Device ID",
                      "The device to listen to idletime on",
                      0, 255, 0,
                      G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
  g_object_class_install_property (object_class, PROP_DEVICE_ID, obj_props[PROP_DEVICE_ID]);
}

static void
gsd_idle_monitor_init (GsdIdleMonitor *monitor)
{
  monitor->watches = g_hash_table_new_full (NULL,
                                                  NULL,
                                                  NULL,
                                                  (GDestroyNotify)idle_monitor_watch_free);

  monitor->alarms = g_hash_table_new (NULL, NULL);
}

static void
ensure_device_monitor (int device_id)
{
  if (device_monitors[device_id])
    return;

  device_monitors[device_id] = g_object_new (GSD_TYPE_IDLE_MONITOR, "device-id", device_id, NULL);
  device_id_max = MAX (device_id_max, device_id);
}

/**
 * gsd_idle_monitor_get_core:
 *
 * Returns: (transfer none): the #GsdIdleMonitor that tracks the server-global
 * idletime for all devices. To track device-specific idletime,
 * use gsd_idle_monitor_get_for_device().
 */
GsdIdleMonitor *
gsd_idle_monitor_get_core (void)
{
  ensure_device_monitor (0);
  return device_monitors[0];
}

/**
 * gsd_idle_monitor_get_for_device:
 * @device_id: the device to get the idle time for.
 *
 * Returns: (transfer none): a new #GsdIdleMonitor that tracks the
 * device-specific idletime for @device. To track server-global idletime
 * for all devices, use gsd_idle_monitor_get_core().
 */
GsdIdleMonitor *
gsd_idle_monitor_get_for_device (int device_id)
{
  g_return_val_if_fail (device_id > 0 && device_id < 256, NULL);

  ensure_device_monitor (device_id);
  return device_monitors[device_id];
}

static gboolean
fire_watch_idle (gpointer data)
{
  GsdIdleMonitorWatch *watch = data;

  watch->idle_source_id = 0;
  fire_watch (watch);

  return FALSE;
}

static GsdIdleMonitorWatch *
make_watch (GsdIdleMonitor           *monitor,
            guint64                    timeout_msec,
	    GsdIdleMonitorWatchFunc   callback,
	    gpointer                   user_data,
	    GDestroyNotify             notify)
{
  GsdIdleMonitorWatch *watch;

  watch = g_slice_new0 (GsdIdleMonitorWatch);
  watch->monitor = monitor;
  watch->id = get_next_watch_serial ();
  watch->callback = callback;
  watch->user_data = user_data;
  watch->notify = notify;
  watch->timeout_msec = timeout_msec;

  if (timeout_msec != 0)
    {
      watch->xalarm = _xsync_alarm_set (monitor, XSyncPositiveTransition, timeout_msec, TRUE);

      g_hash_table_add (monitor->alarms, (gpointer) watch->xalarm);

      if (gsd_idle_monitor_get_idletime (monitor) > (gint64)timeout_msec)
        watch->idle_source_id = g_idle_add (fire_watch_idle, watch);
    }
  else if (monitor->user_active_alarm != None)
    {
      watch->xalarm = monitor->user_active_alarm;

      set_alarm_enabled (monitor->display, monitor->user_active_alarm, TRUE);
    }

  g_hash_table_insert (monitor->watches,
                       GUINT_TO_POINTER (watch->id),
                       watch);
  return watch;
}

/**
 * gsd_idle_monitor_add_idle_watch:
 * @monitor: A #GsdIdleMonitor
 * @interval_msec: The idletime interval, in milliseconds
 * @callback: (allow-none): The callback to call when the user has
 *     accumulated @interval_msec milliseconds of idle time.
 * @user_data: (allow-none): The user data to pass to the callback
 * @notify: A #GDestroyNotify
 *
 * Returns: a watch id
 *
 * Adds a watch for a specific idle time. The callback will be called
 * when the user has accumulated @interval_msec milliseconds of idle time.
 * This function will return an ID that can either be passed to
 * gsd_idle_monitor_remove_watch(), or can be used to tell idle time
 * watches apart if you have more than one.
 *
 * Also note that this function will only care about positive transitions
 * (user's idle time exceeding a certain time). If you want to know about
 * when the user has become active, use
 * gsd_idle_monitor_add_user_active_watch().
 */
guint
gsd_idle_monitor_add_idle_watch (GsdIdleMonitor	       *monitor,
                                  guint64	                interval_msec,
                                  GsdIdleMonitorWatchFunc      callback,
                                  gpointer			user_data,
                                  GDestroyNotify		notify)
{
  GsdIdleMonitorWatch *watch;

  g_return_val_if_fail (GSD_IS_IDLE_MONITOR (monitor), 0);
  g_return_val_if_fail (interval_msec > 0, 0);

  watch = make_watch (monitor,
                      interval_msec,
                      callback,
                      user_data,
                      notify);

  return watch->id;
}

/**
 * gsd_idle_monitor_add_user_active_watch:
 * @monitor: A #GsdIdleMonitor
 * @callback: (allow-none): The callback to call when the user is
 *     active again.
 * @user_data: (allow-none): The user data to pass to the callback
 * @notify: A #GDestroyNotify
 *
 * Returns: a watch id
 *
 * Add a one-time watch to know when the user is active again.
 * Note that this watch is one-time and will de-activate after the
 * function is called, for efficiency purposes. It's most convenient
 * to call this when an idle watch, as added by
 * gsd_idle_monitor_add_idle_watch(), has triggered.
 */
guint
gsd_idle_monitor_add_user_active_watch (GsdIdleMonitor          *monitor,
                                         GsdIdleMonitorWatchFunc  callback,
                                         gpointer		   user_data,
                                         GDestroyNotify	           notify)
{
  GsdIdleMonitorWatch *watch;

  g_return_val_if_fail (GSD_IS_IDLE_MONITOR (monitor), 0);

  watch = make_watch (monitor,
                      0,
                      callback,
                      user_data,
                      notify);

  return watch->id;
}

/**
 * gsd_idle_monitor_remove_watch:
 * @monitor: A #GsdIdleMonitor
 * @id: A watch ID
 *
 * Removes an idle time watcher, previously added by
 * gsd_idle_monitor_add_idle_watch() or
 * gsd_idle_monitor_add_user_active_watch().
 */
void
gsd_idle_monitor_remove_watch (GsdIdleMonitor *monitor,
                                guint	         id)
{
  g_return_if_fail (GSD_IS_IDLE_MONITOR (monitor));

  g_object_ref (monitor);
  g_hash_table_remove (monitor->watches,
                       GUINT_TO_POINTER (id));
  
  g_object_unref (monitor);
}

/**
 * gsd_idle_monitor_get_idletime:
 * @monitor: A #GsdIdleMonitor
 *
 * Returns: The current idle time, in milliseconds, or -1 for not supported
 */
gint64
gsd_idle_monitor_get_idletime (GsdIdleMonitor *monitor)
{
  XSyncValue value;

  g_return_val_if_fail (GSD_IS_IDLE_MONITOR (monitor), -1);

  if (monitor->counter == None)
    return -1;

  if (!XSyncQueryCounter (monitor->display, monitor->counter, &value))
    return -1;

  return _xsyncvalue_to_int64 (value);
}

static gboolean
handle_get_idletime (MetaDBusIdleMonitor   *skeleton,
                     GDBusMethodInvocation *invocation,
                     GsdIdleMonitor       *monitor)
{
  guint64 idletime;

  idletime = gsd_idle_monitor_get_idletime (monitor);
  meta_dbus_idle_monitor_complete_get_idletime (skeleton, invocation, idletime);

  return TRUE;
}

/* In a Unity session the same monitors are also exported under Muffin's
 * name, org.cinnamon.Muffin.IdleMonitor, which libcinnamon-desktop's
 * GnomeIdleMonitor (and so cinnamon-session's presence) is a client of.
 * Both object managers serve the same GsdIdleMonitor objects: a watch added
 * through either is a watch on the same XSync counter. Muffin's
 * ResetIdletime is not provided.
 *
 * Carried over unchanged: on_device_added takes a reference on the device
 * monitor that is never released, on_device_removed clears the slot while
 * the exported skeletons keep the monitor alive, and dispose asserts that the
 * slot is already cleared. The Muffin export takes no reference of its own
 * on the monitors. */

#define MUTTER_IDLE_MONITOR_PATH "/org/gnome/Mutter/IdleMonitor"
#define MUFFIN_IDLE_MONITOR_NAME "org.cinnamon.Muffin.IdleMonitor"
#define MUFFIN_IDLE_MONITOR_PATH "/org/cinnamon/Muffin/IdleMonitor"

static GDBusObjectManagerServer *mutter_manager;
static GDBusObjectManagerServer *muffin_manager;

/* watches added through the Muffin interface: watch id -> DBusWatch */
static GHashTable *muffin_watches;

typedef struct {
  GDBusInterfaceSkeleton *skeleton;
  GsdIdleMonitor *monitor;
  char *dbus_name;
  guint watch_id;
  guint name_watcher_id;
  gboolean muffin;
} DBusWatch;

static void
destroy_dbus_watch (gpointer data)
{
  DBusWatch *watch = data;

  if (watch->muffin &&
      g_hash_table_lookup (muffin_watches, GUINT_TO_POINTER (watch->watch_id)) == watch)
    g_hash_table_remove (muffin_watches, GUINT_TO_POINTER (watch->watch_id));

  g_object_unref (watch->skeleton);
  g_object_unref (watch->monitor);
  g_free (watch->dbus_name);
  g_bus_unwatch_name (watch->name_watcher_id);

  g_slice_free (DBusWatch, watch);
}

static void
dbus_idle_callback (GsdIdleMonitor *monitor,
                    guint            watch_id,
                    gpointer         user_data)
{
  DBusWatch *watch = user_data;
  GDBusInterfaceSkeleton *skeleton = watch->skeleton;

  /* to the client that added the watch, on the object and the interface it
   * added it through */
  g_dbus_connection_emit_signal (g_dbus_interface_skeleton_get_connection (skeleton),
                                 watch->dbus_name,
                                 g_dbus_interface_skeleton_get_object_path (skeleton),
                                 g_dbus_interface_skeleton_get_info (skeleton)->name,
                                 "WatchFired",
                                 g_variant_new ("(u)", watch_id),
                                 NULL);
}

static void
name_vanished_callback (GDBusConnection *connection,
                        const char      *name,
                        gpointer         user_data)
{
  DBusWatch *watch = user_data;

  gsd_idle_monitor_remove_watch (watch->monitor, watch->watch_id);
}

static DBusWatch *
make_dbus_watch (GDBusInterfaceSkeleton *skeleton,
                 GDBusMethodInvocation  *invocation,
                 GsdIdleMonitor         *monitor,
                 gboolean                muffin)
{
  DBusWatch *watch;

  watch = g_slice_new0 (DBusWatch);
  watch->skeleton = g_object_ref (skeleton);
  watch->monitor = g_object_ref (monitor);
  watch->muffin = muffin;
  watch->dbus_name = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  watch->name_watcher_id = g_bus_watch_name_on_connection (g_dbus_method_invocation_get_connection (invocation),
                                                           watch->dbus_name,
                                                           G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                           NULL, /* appeared */
                                                           name_vanished_callback,
                                                           watch, NULL);

  return watch;
}

static void
register_muffin_watch (DBusWatch *watch)
{
  if (!watch->muffin)
    return;

  if (muffin_watches == NULL)
    muffin_watches = g_hash_table_new (NULL, NULL);
  g_hash_table_insert (muffin_watches, GUINT_TO_POINTER (watch->watch_id), watch);
}

static guint
add_dbus_idle_watch (GDBusInterfaceSkeleton *skeleton,
                     GDBusMethodInvocation  *invocation,
                     guint64                 interval,
                     GsdIdleMonitor         *monitor,
                     gboolean                muffin)
{
  DBusWatch *watch;

  watch = make_dbus_watch (skeleton, invocation, monitor, muffin);
  watch->watch_id = gsd_idle_monitor_add_idle_watch (monitor, interval,
                                                      dbus_idle_callback, watch, destroy_dbus_watch);
  register_muffin_watch (watch);

  return watch->watch_id;
}

static guint
add_dbus_user_active_watch (GDBusInterfaceSkeleton *skeleton,
                            GDBusMethodInvocation  *invocation,
                            GsdIdleMonitor         *monitor,
                            gboolean                muffin)
{
  DBusWatch *watch;

  watch = make_dbus_watch (skeleton, invocation, monitor, muffin);
  watch->watch_id = gsd_idle_monitor_add_user_active_watch (monitor,
                                                             dbus_idle_callback, watch,
                                                             destroy_dbus_watch);
  register_muffin_watch (watch);

  return watch->watch_id;
}

static gboolean
handle_add_idle_watch (MetaDBusIdleMonitor   *skeleton,
                       GDBusMethodInvocation *invocation,
                       guint64                interval,
                       GsdIdleMonitor       *monitor)
{
  guint id;

  id = add_dbus_idle_watch (G_DBUS_INTERFACE_SKELETON (skeleton), invocation,
                            interval, monitor, FALSE);
  meta_dbus_idle_monitor_complete_add_idle_watch (skeleton, invocation, id);

  return TRUE;
}

static gboolean
handle_add_user_active_watch (MetaDBusIdleMonitor   *skeleton,
                              GDBusMethodInvocation *invocation,
                              GsdIdleMonitor       *monitor)
{
  guint id;

  id = add_dbus_user_active_watch (G_DBUS_INTERFACE_SKELETON (skeleton), invocation,
                                   monitor, FALSE);
  meta_dbus_idle_monitor_complete_add_user_active_watch (skeleton, invocation, id);

  return TRUE;
}

static gboolean
handle_remove_watch (MetaDBusIdleMonitor   *skeleton,
                     GDBusMethodInvocation *invocation,
                     guint                  id,
                     GsdIdleMonitor       *monitor)
{
  gsd_idle_monitor_remove_watch (monitor, id);
  meta_dbus_idle_monitor_complete_remove_watch (skeleton, invocation);

  return TRUE;
}

static gboolean
muffin_handle_get_idletime (MuffinDBusIdleMonitor *skeleton,
                            GDBusMethodInvocation *invocation,
                            GsdIdleMonitor        *monitor)
{
  guint64 idletime;

  idletime = gsd_idle_monitor_get_idletime (monitor);
  muffin_dbus_idle_monitor_complete_get_idletime (skeleton, invocation, idletime);

  return TRUE;
}

static gboolean
muffin_handle_add_idle_watch (MuffinDBusIdleMonitor *skeleton,
                              GDBusMethodInvocation *invocation,
                              guint64                interval,
                              GsdIdleMonitor        *monitor)
{
  guint id;

  id = add_dbus_idle_watch (G_DBUS_INTERFACE_SKELETON (skeleton), invocation,
                            interval, monitor, TRUE);
  muffin_dbus_idle_monitor_complete_add_idle_watch (skeleton, invocation, id);

  return TRUE;
}

static gboolean
muffin_handle_add_user_active_watch (MuffinDBusIdleMonitor *skeleton,
                                     GDBusMethodInvocation *invocation,
                                     GsdIdleMonitor        *monitor)
{
  guint id;

  id = add_dbus_user_active_watch (G_DBUS_INTERFACE_SKELETON (skeleton), invocation,
                                   monitor, TRUE);
  muffin_dbus_idle_monitor_complete_add_user_active_watch (skeleton, invocation, id);

  return TRUE;
}

/* Watch ids come from one counter for the whole process, shared with the
 * plugins' own watches: through this interface a client removes only the
 * watches it added through it, on this monitor. Any other id is ignored,
 * as an unknown id is. */
static gboolean
muffin_handle_remove_watch (MuffinDBusIdleMonitor *skeleton,
                            GDBusMethodInvocation *invocation,
                            guint                  id,
                            GsdIdleMonitor        *monitor)
{
  DBusWatch *watch = NULL;

  if (muffin_watches != NULL)
    watch = g_hash_table_lookup (muffin_watches, GUINT_TO_POINTER (id));

  if (watch != NULL &&
      watch->monitor == monitor &&
      g_strcmp0 (watch->dbus_name, g_dbus_method_invocation_get_sender (invocation)) == 0)
    gsd_idle_monitor_remove_watch (monitor, id);
  else
    g_debug ("Ignoring RemoveWatch (%u) from %s: not a watch it added on %s",
             id, g_dbus_method_invocation_get_sender (invocation),
             g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (skeleton)));

  muffin_dbus_idle_monitor_complete_remove_watch (skeleton, invocation);

  return TRUE;
}

static void
create_monitor_skeleton (GDBusObjectManagerServer *manager,
                         GsdIdleMonitor          *monitor,
                         const char               *path)
{
  MetaDBusIdleMonitor *skeleton;
  MetaDBusObjectSkeleton *object;

  skeleton = meta_dbus_idle_monitor_skeleton_new ();
  g_signal_connect_object (skeleton, "handle-add-idle-watch",
                           G_CALLBACK (handle_add_idle_watch), monitor, 0);
  g_signal_connect_object (skeleton, "handle-add-user-active-watch",
                           G_CALLBACK (handle_add_user_active_watch), monitor, 0);
  g_signal_connect_object (skeleton, "handle-remove-watch",
                           G_CALLBACK (handle_remove_watch), monitor, 0);
  g_signal_connect_object (skeleton, "handle-get-idletime",
                           G_CALLBACK (handle_get_idletime), monitor, 0);

  object = meta_dbus_object_skeleton_new (path);
  meta_dbus_object_skeleton_set_idle_monitor (object, skeleton);

  g_dbus_object_manager_server_export (manager, G_DBUS_OBJECT_SKELETON (object));

  g_object_unref (skeleton);
  g_object_unref (object);
}

static void
create_muffin_monitor_skeleton (GDBusObjectManagerServer *manager,
                                GsdIdleMonitor           *monitor,
                                const char               *path)
{
  MuffinDBusIdleMonitor *skeleton;
  MuffinDBusObjectSkeleton *object;

  skeleton = muffin_dbus_idle_monitor_skeleton_new ();
  g_signal_connect_object (skeleton, "handle-add-idle-watch",
                           G_CALLBACK (muffin_handle_add_idle_watch), monitor, 0);
  g_signal_connect_object (skeleton, "handle-add-user-active-watch",
                           G_CALLBACK (muffin_handle_add_user_active_watch), monitor, 0);
  g_signal_connect_object (skeleton, "handle-remove-watch",
                           G_CALLBACK (muffin_handle_remove_watch), monitor, 0);
  g_signal_connect_object (skeleton, "handle-get-idletime",
                           G_CALLBACK (muffin_handle_get_idletime), monitor, 0);

  object = muffin_dbus_object_skeleton_new (path);
  muffin_dbus_object_skeleton_set_idle_monitor (object, skeleton);

  g_dbus_object_manager_server_export (manager, G_DBUS_OBJECT_SKELETON (object));

  g_object_unref (skeleton);
  g_object_unref (object);
}

static void
export_monitor (GsdIdleMonitor *monitor,
                const char     *name)
{
  char *path;

  path = g_strdup_printf (MUTTER_IDLE_MONITOR_PATH "/%s", name);
  create_monitor_skeleton (mutter_manager, monitor, path);
  g_free (path);

  if (muffin_manager == NULL)
    return;

  path = g_strdup_printf (MUFFIN_IDLE_MONITOR_PATH "/%s", name);
  create_muffin_monitor_skeleton (muffin_manager, monitor, path);
  g_free (path);
}

static void
on_device_added (GdkDeviceManager *device_manager,
                 GdkDevice        *device,
                 gpointer          user_data)
{

  GsdIdleMonitor *monitor;
  int device_id;
  char *name;

  device_id = gdk_x11_device_get_id (device);
  monitor = gsd_idle_monitor_get_for_device (device_id);
  g_object_ref(monitor);
  name = g_strdup_printf ("Device%d", device_id);

  export_monitor (monitor, name);
  g_free (name);
}

static void
on_device_removed (GdkDeviceManager *device_manager,
                   GdkDevice        *device,
                   gpointer          user_data)
{
  int device_id;
  char *path;

  device_id = gdk_x11_device_get_id (device);
  path = g_strdup_printf (MUTTER_IDLE_MONITOR_PATH "/Device%d", device_id);
  g_dbus_object_manager_server_unexport (mutter_manager, path);
  g_free (path);

  if (muffin_manager != NULL)
    {
      path = g_strdup_printf (MUFFIN_IDLE_MONITOR_PATH "/Device%d", device_id);
      g_dbus_object_manager_server_unexport (muffin_manager, path);
      g_free (path);
    }

  g_clear_object (&device_monitors[device_id]);
  if (device_id == device_id_max)
    device_id_max--;
}

static gboolean
in_desktop (const gchar *name)
{
  const gchar *desktop_name_list;
  gchar **names;
  gboolean in_list = FALSE;
  gint i;

  desktop_name_list = g_getenv ("XDG_CURRENT_DESKTOP");
  if (!desktop_name_list)
    return FALSE;

  names = g_strsplit (desktop_name_list, ":", -1);
  for (i = 0; names[i] && !in_list; i++)
    if (strcmp (names[i], name) == 0)
      in_list = TRUE;
  g_strfreev (names);

  return in_list;
}

static void
on_muffin_name_acquired (GDBusConnection *connection,
                         const char      *name,
                         gpointer         user_data)
{
  g_debug ("Acquired name %s\n", name);
}

/* Muffin itself has the name (Muffin running in this session), or the
 * process is exiting. This is also called at start when the request was
 * only queued behind Muffin: there are no watches then. Watches added
 * through this interface while the name was ours are dropped: their clients
 * now talk to the new owner and would ignore a WatchFired from us. The
 * objects stay exported; the queued request gets the name back when Muffin
 * goes away. */
static void
on_muffin_name_lost (GDBusConnection *connection,
                     const char      *name,
                     gpointer         user_data)
{
  GList *watches, *l;

  g_debug ("Lost or failed to acquire name %s\n", name);

  if (muffin_watches == NULL)
    return;

  /* removing a watch takes it out of the table */
  watches = g_hash_table_get_values (muffin_watches);
  for (l = watches; l != NULL; l = l->next)
    {
      DBusWatch *watch = l->data;

      gsd_idle_monitor_remove_watch (watch->monitor, watch->watch_id);
    }
  g_list_free (watches);
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const char      *name,
                 gpointer         user_data)
{
  GdkDeviceManager *device_manager;
  GsdIdleMonitor *monitor;
  GList *devices, *iter;

  mutter_manager = g_dbus_object_manager_server_new (MUTTER_IDLE_MONITOR_PATH);

  /* Muffin's name only in a Unity session, where Muffin does not run */
  if (in_desktop ("Unity"))
    muffin_manager = g_dbus_object_manager_server_new (MUFFIN_IDLE_MONITOR_PATH);

  /* We never clear the core monitor, as that's supposed to cumulate idle times from
     all devices */
  monitor = gsd_idle_monitor_get_core ();
  export_monitor (monitor, "Core");

  device_manager =  gdk_display_get_device_manager (gdk_display_get_default ());
  g_object_ref (device_manager);

  devices =  gdk_device_manager_list_devices (device_manager, GDK_DEVICE_TYPE_MASTER);
  devices = g_list_concat (devices, gdk_device_manager_list_devices (device_manager, GDK_DEVICE_TYPE_SLAVE));
  devices = g_list_concat (devices, gdk_device_manager_list_devices (device_manager, GDK_DEVICE_TYPE_FLOATING));

  for (iter = devices; iter; iter = iter->next)
    on_device_added (device_manager, iter->data, NULL);

  g_list_free (devices);

  /* one handler each, for both managers */
  g_signal_connect (device_manager, "device-added",
                    G_CALLBACK (on_device_added), NULL);
  g_signal_connect (device_manager, "device-removed",
                    G_CALLBACK (on_device_removed), NULL);

  g_dbus_object_manager_server_set_connection (mutter_manager, connection);

  if (muffin_manager == NULL)
    return;

  /* The objects are exported before the name is requested, so a client
   * that sees the name finds them. Never REPLACE: if Muffin runs, it keeps
   * its name. */
  g_dbus_object_manager_server_set_connection (muffin_manager, connection);
  g_bus_own_name_on_connection (connection,
                                MUFFIN_IDLE_MONITOR_NAME,
                                G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT,
                                on_muffin_name_acquired,
                                on_muffin_name_lost,
                                NULL, NULL);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const char      *name,
                  gpointer         user_data)
{
  g_debug ("Acquired name %s\n", name);
}

static void
on_name_lost (GDBusConnection *connection,
              const char      *name,
              gpointer         user_data)
{
  g_warning ("Lost or failed to acquire name %s\n", name);
}

static void
init_xsync_global (void)
{
    int major, minor;
    xsync->display = GDK_DISPLAY_XDISPLAY (gdk_display_get_default ());
    xsync->have_xsync = FALSE;

    xsync->sync_error_base = 0;
    xsync->sync_event_base = 0;

    /* I don't think we really have to fill these in */
    major = SYNC_MAJOR_VERSION;
    minor = SYNC_MINOR_VERSION;

    if (!XSyncQueryExtension (xsync->display,
                              &xsync->sync_event_base,
                              &xsync->sync_error_base) ||
        !XSyncInitialize (xsync->display,
                          &major, &minor))
      {
        xsync->sync_error_base = 0;
        xsync->sync_event_base = 0;
      }
    else
      {
        xsync->have_xsync = TRUE;
        XSyncSetPriority (xsync->display, None, 10);
      }
}

void
gsd_idle_monitor_init_dbus (gboolean replace)
{
  static int dbus_name_id;

  if (dbus_name_id > 0)
    return;

  xsync = g_slice_new0 (GsdXSync);
  init_xsync_global();

  /* Alarm events drive every idle and user-active watch in this process
   * (cursor, power, screensaver-proxy), not only the D-Bus export, so the
   * filter must not depend on owning the bus name: it used to be added in
   * on_bus_acquired and removed in on_name_lost, and a name lost and then
   * acquired again (two instances starting at once) left it removed for
   * good - the pointer stayed hidden and no idle watch fired again. */
  gdk_window_add_filter (NULL, xevent_filter, NULL);

  dbus_name_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                 "org.gnome.Mutter.IdleMonitor",
                                 G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT |
                                 (replace ?
                                  G_BUS_NAME_OWNER_FLAGS_REPLACE : 0),
                                 on_bus_acquired,
                                 on_name_acquired,
                                 on_name_lost,
                                 NULL, NULL);
}

