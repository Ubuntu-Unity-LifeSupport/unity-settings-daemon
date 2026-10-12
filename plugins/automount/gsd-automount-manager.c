/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2010 Red Hat, Inc.
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Tomas Bzatek <tbzatek@redhat.com>
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "gnome-settings-profile.h"
#include "gnome-settings-bus.h"
#include "gsd-automount-manager.h"
#include "gsd-autorun.h"
#include "gsd-mount-guard.h"

#define GSD_AUTOMOUNT_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GSD_TYPE_AUTOMOUNT_MANAGER, GsdAutomountManagerPrivate))

struct GsdAutomountManagerPrivate
{
        GSettings   *settings;

	GVolumeMonitor *volume_monitor;
	unsigned int automount_idle_id;

        GDBusProxy *session;
        gboolean session_is_active;
        /* autorun_allowed() as last seen by update_allowed() */
        gboolean was_allowed;
        /* error dialogs shown, destroyed when the session locks */
        GList *error_dialogs;
        gboolean screensaver_active;
        gboolean lockscreen_active;
        guint ss_watch_id;
        guint us_watch_id;
        GDBusProxy *ss_proxy;
        GDBusProxy *us_proxy;
        /* one per lock provider name, renewed on every appearance and
         * cancelled when it vanishes: replies meant for an earlier owner
         * are dropped */
        GCancellable *ss_cancellable;
        GCancellable *us_cancellable;

        /* The lock state is only trusted once it is known: a present
         * ScreenSaver or Unity owner must have answered, and logind's
         * LockedHint for our session must have been read. Until then,
         * and whenever a read fails, volumes are queued (fail closed). */
        gboolean ss_resolved;   /* the name watcher has reported once */
        gboolean us_resolved;
        gboolean ss_present;
        gboolean us_present;
        gboolean ss_known;
        gboolean us_known;
        GDBusProxy *login_session;
        gboolean locked_hint;
        gboolean locked_hint_known;
        GCancellable *cancellable;

        GList *volume_queue;
        /* volumes present at start-up, mounted without autorun as before */
        GList *startup_queue;
};

static void     gsd_automount_manager_class_init  (GsdAutomountManagerClass *klass);
static void     gsd_automount_manager_init        (GsdAutomountManager      *gsd_automount_manager);

G_DEFINE_TYPE (GsdAutomountManager, gsd_automount_manager, G_TYPE_OBJECT)

static gboolean autorun_allowed (gpointer user_data);

static void
error_dialog_destroyed (GtkWidget           *dialog,
                        GsdAutomountManager *manager)
{
        manager->priv->error_dialogs = g_list_remove (manager->priv->error_dialogs, dialog);
}

static void
close_error_dialogs (GsdAutomountManager *manager)
{
        /* each destroy removes the dialog from the list */
        while (manager->priv->error_dialogs != NULL)
                gtk_widget_destroy (GTK_WIDGET (manager->priv->error_dialogs->data));
}

/* An error dialog, only while the session is active and unlocked;
 * otherwise the error is logged. */
static GtkDialog *
show_error_dialog (GsdAutomountManager *manager,
		   const char          *primary_text,
		   const char          *secondary_text)
{
	GtkWidget *dialog;

	if (!autorun_allowed (manager)) {
		g_warning ("%s: %s", primary_text, secondary_text);
		return NULL;
	}

	dialog = gtk_message_dialog_new (NULL,
					 0,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_OK,
					 "%s", "");

	g_object_set (dialog,
		      "text", primary_text,
		      "secondary-text", secondary_text,
		      NULL);

	gtk_widget_show (GTK_WIDGET (dialog));

	g_signal_connect (dialog, "response",
			  G_CALLBACK (gtk_widget_destroy), NULL);
	g_signal_connect (dialog, "destroy",
			  G_CALLBACK (error_dialog_destroyed), manager);
	manager->priv->error_dialogs = g_list_prepend (manager->priv->error_dialogs, dialog);

	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

	return GTK_DIALOG (dialog);
}

static void
startup_volume_mount_cb (GObject *source_object,
			 GAsyncResult *res,
			 gpointer user_data)
{
	g_volume_mount_finish (G_VOLUME (source_object), res, NULL);
}

/* Whether the screen lock state allows mounting now. Every condition must
 * be known and say "unlocked": logind's LockedHint for our session was read
 * and is false, and each present lock provider (org.gnome.ScreenSaver,
 * com.canonical.Unity) has answered and reports not active / not locked.
 * Anything unknown means no (the caller queues the volume). */
static gboolean
lock_state_allows_mount (GsdAutomountManager *manager)
{
        GsdAutomountManagerPrivate *p = manager->priv;

        if (!p->locked_hint_known || p->locked_hint)
                return FALSE;
        /* until a name watcher has reported, presence itself is unknown */
        if (!p->ss_resolved || !p->us_resolved)
                return FALSE;
        /* presence follows the bus name, not the proxy: an owner whose
         * proxy could not be made has not answered, so it is unknown */
        if (p->ss_present && (!p->ss_known || p->screensaver_active))
                return FALSE;
        if (p->us_present && (!p->us_known || p->lockscreen_active))
                return FALSE;

        return TRUE;
}

/* Whether autorun may act for a medium now: the session is the active one
 * and known to be unlocked. Asked when a mount appears and again right
 * before autorun launches or shows anything. */
static gboolean
autorun_allowed (gpointer user_data)
{
        GsdAutomountManager *manager = user_data;

        return manager->priv->session_is_active &&
               lock_state_allows_mount (manager);
}

static void
automount_all_volumes (GsdAutomountManager *manager)
{
	GList *volumes, *l;
	GMount *mount;
	GVolume *volume;

	if (g_settings_get_boolean (manager->priv->settings, "automount")) {
		/* automount all mountable volumes at start-up */
		volumes = g_volume_monitor_get_volumes (manager->priv->volume_monitor);
		for (l = volumes; l != NULL; l = l->next) {
			volume = l->data;

			if (!g_volume_should_automount (volume) ||
			    !g_volume_can_mount (volume)) {
				continue;
			}

			mount = g_volume_get_mount (volume);
			if (mount != NULL) {
				g_object_unref (mount);
				continue;
			}

			if (!lock_state_allows_mount (manager)) {
				/* the lock state is not known yet, or the screen
				 * is locked: mount it once it is known to be
				 * unlocked */
				g_debug ("Queuing start-up volume %p", volume);
				manager->priv->startup_queue =
					g_list_prepend (manager->priv->startup_queue,
							g_object_ref (volume));
				continue;
			}

			/* pass NULL as GMountOperation to avoid user interaction */
			g_volume_mount (volume, 0, NULL, NULL, startup_volume_mount_cb, NULL);
		}
		g_list_free_full (volumes, g_object_unref);
	}
}

static gboolean
automount_all_volumes_idle_cb (gpointer data)
{
	GsdAutomountManager *manager = GSD_AUTOMOUNT_MANAGER (data);

	automount_all_volumes (manager);

	manager->priv->automount_idle_id = 0;
	return FALSE;
}

typedef struct {
        GsdAutomountManager *manager;
        GMountOperation     *mount_op;
} MountData;

static void
volume_mount_cb (GObject *source_object,
		 GAsyncResult *res,
		 gpointer user_data)
{
	MountData *data = user_data;
	GError *error;
	char *primary;
	char *name;

	gsd_mount_guard_detach (data->mount_op);

	error = NULL;
	gsd_allow_autorun_for_volume_finish (G_VOLUME (source_object));
	if (!g_volume_mount_finish (G_VOLUME (source_object), res, &error)) {
		/* not mounted (also a prompt withdrawn at a lock): no autorun */
		gsd_disallow_autorun_for_volume (G_VOLUME (source_object));
		if (error->code != G_IO_ERROR_FAILED_HANDLED) {
			name = g_volume_get_name (G_VOLUME (source_object));
			primary = g_strdup_printf (_("Unable to mount %s"), name);
			g_free (name);
			show_error_dialog (data->manager, primary,
				           error->message);
			g_free (primary);
		}
		g_error_free (error);
	}

	g_object_unref (data->mount_op);
	g_free (data);
}

static void
do_mount_volume (GsdAutomountManager *manager,
                 GVolume             *volume)
{
	MountData *data;

	data = g_new0 (MountData, 1);
	data->manager = manager;
	data->mount_op = gtk_mount_operation_new (NULL);
	g_mount_operation_set_password_save (data->mount_op, G_PASSWORD_SAVE_FOR_SESSION);
	/* its prompts follow the session state */
	gsd_mount_guard_attach (data->mount_op, autorun_allowed, manager);

	gsd_allow_autorun_for_volume (volume);
	g_volume_mount (volume, 0, data->mount_op, NULL, volume_mount_cb, data);
}

static void
check_volume_queue (GsdAutomountManager *manager)
{
        GList *queue, *startup, *l;

        if (!lock_state_allows_mount (manager))
                return;

        /* Take both lists before mounting anything: the old loop read
         * l->next after g_list_remove() had freed l. */
        queue = manager->priv->volume_queue;
        manager->priv->volume_queue = NULL;
        startup = manager->priv->startup_queue;
        manager->priv->startup_queue = NULL;

        for (l = queue; l != NULL; l = l->next)
                do_mount_volume (manager, G_VOLUME (l->data));
        g_list_free_full (queue, g_object_unref);

        for (l = startup; l != NULL; l = l->next) {
                GVolume *volume = G_VOLUME (l->data);
                GMount *mount = g_volume_get_mount (volume);

                if (mount != NULL) {
                        g_object_unref (mount);
                        continue;
                }
                /* as at start-up: no GMountOperation, no autorun */
                g_volume_mount (volume, 0, NULL, NULL, startup_volume_mount_cb, NULL);
        }
        g_list_free_full (startup, g_object_unref);
}

/* When the session stops being active and unlocked (autorun_allowed()
 * turns false), withdraw the prompts that are up and close the helper's
 * dialogs. Called wherever an input of autorun_allowed() changes. */
static void
update_allowed (GsdAutomountManager *manager)
{
        gboolean allowed = autorun_allowed (manager);

        if (manager->priv->was_allowed && !allowed) {
                g_debug ("Session locked or inactive: withdrawing prompts and dialogs");
                gsd_mount_guard_withdraw_all ();
                gsd_autorun_close_dialogs ();
                close_error_dialogs (manager);
        }
        manager->priv->was_allowed = allowed;
}

/* A lock provider or logind answered: handle the transition, then mount
 * what waits for the session to be known unlocked. */
static void
lock_state_changed (GsdAutomountManager *manager)
{
        update_allowed (manager);
        check_volume_queue (manager);
}

static void
clear_volume_queue (GsdAutomountManager *manager)
{
        if (!manager->priv->volume_queue)
                return;

        g_list_free_full (manager->priv->volume_queue, g_object_unref);
        manager->priv->volume_queue = NULL;
}

static void
check_screen_lock_and_mount (GsdAutomountManager *manager,
                             GVolume *volume)
{
        if (!manager->priv->session_is_active)
                return;

        if (!lock_state_allows_mount (manager)) {
                /* queue the volume, to mount it once the session is known to be unlocked */
                g_debug ("Queuing volume %p", volume);
                manager->priv->volume_queue = g_list_prepend (manager->priv->volume_queue,
                                                              g_object_ref (volume));
        } else {
                /* mount it immediately */
                do_mount_volume (manager, volume);
        }
}

static void
volume_removed_callback (GVolumeMonitor *monitor,
                         GVolume *volume,
                         GsdAutomountManager *manager)
{
        g_debug ("Volume %p removed, removing from the queue", volume);

        /* clear it from the queues, if present, and drop their reference */
        if (g_list_find (manager->priv->volume_queue, volume)) {
                manager->priv->volume_queue =
                        g_list_remove (manager->priv->volume_queue, volume);
                g_object_unref (volume);
        }
        if (g_list_find (manager->priv->startup_queue, volume)) {
                manager->priv->startup_queue =
                        g_list_remove (manager->priv->startup_queue, volume);
                g_object_unref (volume);
        }
}

static void
volume_added_callback (GVolumeMonitor *monitor,
		       GVolume *volume,
		       GsdAutomountManager *manager)
{
	if (g_settings_get_boolean (manager->priv->settings, "automount") &&
	    g_volume_should_automount (volume) &&
	    g_volume_can_mount (volume)) {
                check_screen_lock_and_mount (manager, volume);
	} else if (autorun_allowed (manager)) {
		/* Allow gsd_autorun() to run. When the mount is later
		 * added programmatically (i.e. for a blank CD),
		 * gsd_autorun() will be called by mount_added_callback().
		 * Not while the session is locked or inactive. */
		gsd_allow_autorun_for_volume (volume);
		gsd_allow_autorun_for_volume_finish (volume);
	}
}

static void
autorun_show_window (GMount *mount, gpointer user_data)
{
	GFile *location;
        char *uri;
        GError *error;
	char *primary;
	char *name;

	location = g_mount_get_root (mount);
        uri = g_file_get_uri (location);

        error = NULL;
	/* use default folder handler */
        if (! gtk_show_uri (NULL, uri, GDK_CURRENT_TIME, &error)) {
		name = g_mount_get_name (mount);
		primary = g_strdup_printf (_("Unable to open a folder for %s"), name);
		g_free (name);
		show_error_dialog (GSD_AUTOMOUNT_MANAGER (user_data), primary,
				   error->message);
		g_free (primary);
		g_error_free (error);
        }

        g_free (uri);
	g_object_unref (location);
}

static void
mount_added_callback (GVolumeMonitor *monitor,
		      GMount *mount,
		      GsdAutomountManager *manager)
{
        /* don't autorun unless the session is active and unlocked; a
         * permission given to the volume earlier is dropped */
        if (!autorun_allowed (manager)) {
                GVolume *volume = g_mount_get_volume (mount);

                if (volume != NULL) {
                        gsd_disallow_autorun_for_volume (volume);
                        g_object_unref (volume);
                }
                return;
        }

	gsd_autorun (mount, manager->priv->settings, autorun_allowed,
		     autorun_show_window, manager);
}


static void
session_state_changed (GDBusProxy      *session,
                       GVariant        *changed,
                       char           **invalidated,
                       GsdAutomountManager *manager)
{
        GsdAutomountManagerPrivate *p = manager->priv;

        GVariant *v;

        v = g_variant_lookup_value (changed, "SessionIsActive", G_VARIANT_TYPE_BOOLEAN);

        if (v) {
                gboolean active;

                active = g_variant_get_boolean (v);
                g_debug ("Received session is active change: now %s", active ? "active" : "inactive");
                /* when doing the fast-user-switch into a new account,
                 * ensure the new account is undimmed and with the backlight on */
                if (active)
                        p->session_is_active = TRUE;
                else
                        p->session_is_active = FALSE;

                g_variant_unref (v);

        }

        if (!p->session_is_active) {
                if (p->volume_queue != NULL) {
                        g_list_free_full (p->volume_queue, g_object_unref);
                        p->volume_queue = NULL;
                }
        }

        update_allowed (manager);
}

static gboolean
is_session_active (GsdAutomountManager *manager)
{
        GVariant *variant;
        gboolean is_session_active = FALSE;

        variant = g_dbus_proxy_get_cached_property (manager->priv->session,
                                                    "SessionIsActive");
        if (variant) {
                is_session_active = g_variant_get_boolean (variant);
                g_variant_unref (variant);
        }

        return is_session_active;
}


static void
do_initialize_session (GsdAutomountManager *manager)
{
        manager->priv->session = G_DBUS_PROXY (gnome_settings_bus_get_session_proxy ());
        g_signal_connect (manager->priv->session, "g-properties-changed",
                          G_CALLBACK (session_state_changed),
                          manager);
        manager->priv->session_is_active = is_session_active (manager);
}

#define SCREENSAVER_NAME "org.gnome.ScreenSaver"

/* An asynchronous call to a lock provider. The callback checks the
 * provider's cancellable it was started with, not GTask's own check: a
 * cancelled call, or one whose provider vanished or appeared again since,
 * returns without touching the manager. */
typedef struct {
        GsdAutomountManager *manager;
        GCancellable        *cancellable;
} ProviderCall;

static ProviderCall *
provider_call_new (GsdAutomountManager *manager,
                   GCancellable        *cancellable)
{
        ProviderCall *call = g_new0 (ProviderCall, 1);

        call->manager = manager;
        call->cancellable = g_object_ref (cancellable);
        return call;
}

/* Frees the call; returns the manager, or NULL if the call was superseded. */
static GsdAutomountManager *
provider_call_finish (ProviderCall *call)
{
        GsdAutomountManager *manager = NULL;

        if (!g_cancellable_is_cancelled (call->cancellable))
                manager = call->manager;
        g_object_unref (call->cancellable);
        g_free (call);
        return manager;
}

static GCancellable *
renew_cancellable (GCancellable **cancellable)
{
        if (*cancellable != NULL) {
                g_cancellable_cancel (*cancellable);
                g_object_unref (*cancellable);
        }
        *cancellable = g_cancellable_new ();
        return *cancellable;
}

static void
drop_cancellable (GCancellable **cancellable)
{
        if (*cancellable != NULL) {
                g_cancellable_cancel (*cancellable);
                g_clear_object (cancellable);
        }
}

static void
clear_provider_proxy (GsdAutomountManager *manager,
                      GDBusProxy         **proxy)
{
        if (*proxy != NULL) {
                g_signal_handlers_disconnect_by_data (*proxy, manager);
                g_clear_object (proxy);
        }
}

static void
screensaver_signal_callback (GDBusProxy *proxy,
                             const gchar *sender_name,
                             const gchar *signal_name,
                             GVariant *parameters,
                             gpointer user_data)
{
        GsdAutomountManager *manager = user_data;

        if (g_strcmp0 (signal_name, "ActiveChanged") == 0) {
                g_variant_get (parameters, "(b)", &manager->priv->screensaver_active);
                manager->priv->ss_known = TRUE;
                g_debug ("Screensaver active changed to %d", manager->priv->screensaver_active);

                lock_state_changed (manager);
        }
}

static void
screensaver_get_active_ready_cb (GObject *source,
                                 GAsyncResult *res,
                                 gpointer user_data)
{
        GsdAutomountManager *manager;
        GVariant *result;
        GError *error = NULL;

        result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source),
                                           res,
                                           &error);

        manager = provider_call_finish (user_data);
        if (manager == NULL) {
                /* meant for an earlier owner, or stopped */
                g_clear_error (&error);
                if (result != NULL)
                        g_variant_unref (result);
                return;
        }

        if (error != NULL) {
                /* the state stays unknown: volumes stay queued */
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Can't call GetActive() on the ScreenSaver object: %s",
                                   error->message);
                g_error_free (error);

                return;
        }

        g_variant_get (result, "(b)", &manager->priv->screensaver_active);
        g_variant_unref (result);
        manager->priv->ss_known = TRUE;

        g_debug ("Screensaver GetActive() returned %d", manager->priv->screensaver_active);
        lock_state_changed (manager);
}

static void
screensaver_appeared_callback (GDBusConnection *connection,
                               const gchar *name,
                               const gchar *name_owner,
                               gpointer user_data)
{
        GsdAutomountManager *manager = user_data;

        g_debug ("ScreenSaver name appeared");

        manager->priv->ss_resolved = TRUE;
        manager->priv->ss_present = TRUE;
        manager->priv->ss_known = FALSE;
        update_allowed (manager);
        renew_cancellable (&manager->priv->ss_cancellable);
        clear_provider_proxy (manager, &manager->priv->ss_proxy);
        manager->priv->ss_proxy = G_DBUS_PROXY (gnome_settings_bus_get_screen_saver_proxy ());

        if (manager->priv->ss_proxy == NULL) {
                g_warning ("Can't get proxy for the ScreenSaver object");
                return;
        }

        g_debug ("ScreenSaver proxy ready");
        g_signal_connect (manager->priv->ss_proxy, "g-signal",
                          G_CALLBACK (screensaver_signal_callback), manager);

        g_dbus_proxy_call (manager->priv->ss_proxy,
                           "GetActive",
                           NULL,
                           G_DBUS_CALL_FLAGS_NO_AUTO_START,
                           -1,
                           manager->priv->ss_cancellable,
                           screensaver_get_active_ready_cb,
                           provider_call_new (manager, manager->priv->ss_cancellable));
}

static void
screensaver_vanished_callback (GDBusConnection *connection,
                               const gchar *name,
                               gpointer user_data)
{
        GsdAutomountManager *manager = user_data;

        g_debug ("ScreenSaver name vanished");

        manager->priv->ss_resolved = TRUE;
        manager->priv->ss_present = FALSE;
        manager->priv->ss_known = FALSE;
        manager->priv->screensaver_active = FALSE;
        drop_cancellable (&manager->priv->ss_cancellable);
        clear_provider_proxy (manager, &manager->priv->ss_proxy);

        if (!manager->priv->ss_proxy && !manager->priv->us_proxy) {
                /* in this case force a clear of the volume queue, without
                 * mounting them.
                 */
                clear_volume_queue (manager);
        }
        /* the start-up queue is kept; it mounts once the state is known */
        lock_state_changed (manager);
}

static void
do_initialize_screensaver (GsdAutomountManager *manager)
{
        GsdAutomountManagerPrivate *p = manager->priv;

        p->ss_watch_id =
                g_bus_watch_name (G_BUS_TYPE_SESSION,
                                  SCREENSAVER_NAME,
                                  G_BUS_NAME_WATCHER_FLAGS_NONE,
                                  screensaver_appeared_callback,
                                  screensaver_vanished_callback,
                                  manager,
                                  NULL);
}

#define UNITY_NAME "com.canonical.Unity"
#define UNITY_SESSION_PATH "/com/canonical/Unity/Session"
#define UNITY_SESSION_INTERFACE "com.canonical.Unity.Session"

static void
unity_session_signal_callback (GDBusProxy *proxy,
                               const gchar *sender_name,
                               const gchar *signal_name,
                               GVariant *parameters,
                               gpointer user_data)
{
        GsdAutomountManager *manager = user_data;

        if (g_strcmp0 (signal_name, "Locked") == 0) {
                manager->priv->lockscreen_active = TRUE;
                manager->priv->us_known = TRUE;
                lock_state_changed (manager);
                g_debug ("Unity.Session Locked");
        } else if (g_strcmp0 (signal_name, "Unlocked") == 0) {
                manager->priv->lockscreen_active = FALSE;
                manager->priv->us_known = TRUE;
                lock_state_changed (manager);
                g_debug ("Unity.Session Unlocked");
        }
}

static void
unity_session_is_locked_cb (GObject *source,
                            GAsyncResult *res,
                            gpointer user_data)
{
        GsdAutomountManager *manager;
        GVariant *result;
        GError *error = NULL;

        result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source),
                                           res,
                                           &error);

        manager = provider_call_finish (user_data);
        if (manager == NULL) {
                /* meant for an earlier owner, or stopped */
                g_clear_error (&error);
                if (result != NULL)
                        g_variant_unref (result);
                return;
        }

        if (error != NULL) {
                /* the state stays unknown: volumes stay queued */
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Can't call IsLocked() on the Unity.Session object: %s",
                                   error->message);
                g_error_free (error);

                return;
        }

        g_variant_get (result, "(b)", &manager->priv->lockscreen_active);
        g_variant_unref (result);
        manager->priv->us_known = TRUE;

        g_debug ("Unity Session IsLocked() returned %d", manager->priv->lockscreen_active);
        lock_state_changed (manager);
}

static void
unity_session_proxy_ready_cb (GObject *source,
                              GAsyncResult *res,
                              gpointer user_data)
{
        GsdAutomountManager *manager;
        GError *error = NULL;
        GDBusProxy *us_proxy;

        us_proxy = g_dbus_proxy_new_finish (res, &error);

        manager = provider_call_finish (user_data);
        if (manager == NULL) {
                /* meant for an earlier owner, or stopped */
                g_clear_error (&error);
                g_clear_object (&us_proxy);
                return;
        }

        if (error != NULL) {
                /* the Unity state stays unknown while the name is present */
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Can't get proxy for the Unity Session object: %s",
                                   error->message);
                g_error_free (error);

                return;
        }

        g_debug ("Unity Session proxy ready");

        clear_provider_proxy (manager, &manager->priv->us_proxy);
        manager->priv->us_proxy = us_proxy;

        g_signal_connect (us_proxy, "g-signal",
                          G_CALLBACK (unity_session_signal_callback), manager);

        g_dbus_proxy_call (us_proxy,
                           "IsLocked",
                           NULL,
                           G_DBUS_CALL_FLAGS_NO_AUTO_START,
                           -1,
                           manager->priv->us_cancellable,
                           unity_session_is_locked_cb,
                           provider_call_new (manager, manager->priv->us_cancellable));
}

static void
unity_appeared_callback (GDBusConnection *connection,
                         const gchar *name,
                         const gchar *name_owner,
                         gpointer user_data)
{
        GsdAutomountManager *manager = user_data;

        g_debug ("Unity name appeared");

        manager->priv->us_resolved = TRUE;
        manager->priv->us_present = TRUE;
        manager->priv->us_known = FALSE;
        manager->priv->lockscreen_active = FALSE;
        update_allowed (manager);
        renew_cancellable (&manager->priv->us_cancellable);
        clear_provider_proxy (manager, &manager->priv->us_proxy);

        g_dbus_proxy_new (connection,
                          G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                          NULL,
                          name,
                          UNITY_SESSION_PATH,
                          UNITY_SESSION_INTERFACE,
                          manager->priv->us_cancellable,
                          unity_session_proxy_ready_cb,
                          provider_call_new (manager, manager->priv->us_cancellable));
}

static void
unity_vanished_callback (GDBusConnection *connection,
                               const gchar *name,
                               gpointer user_data)
{
        GsdAutomountManager *manager = user_data;

        g_debug ("Unity name vanished");

        manager->priv->us_resolved = TRUE;
        manager->priv->us_present = FALSE;
        manager->priv->us_known = FALSE;
        manager->priv->lockscreen_active = FALSE;
        drop_cancellable (&manager->priv->us_cancellable);
        clear_provider_proxy (manager, &manager->priv->us_proxy);

        if (!manager->priv->ss_proxy && !manager->priv->us_proxy) {
                /* in this case force a clear of the volume queue, without
                 * mounting them.
                 */
                clear_volume_queue (manager);
        }
        /* the start-up queue is kept; it mounts once the state is known */
        lock_state_changed (manager);
}

static void
do_initialize_lockscreen (GsdAutomountManager *manager)
{
        GsdAutomountManagerPrivate *p = manager->priv;

        p->us_watch_id =
                g_bus_watch_name (G_BUS_TYPE_SESSION,
                                  UNITY_NAME,
                                  G_BUS_NAME_WATCHER_FLAGS_NONE,
                                  unity_appeared_callback,
                                  unity_vanished_callback,
                                  manager,
                                  NULL);
}

#define LOGIN1_NAME "org.freedesktop.login1"
#define LOGIN1_PATH "/org/freedesktop/login1"
#define LOGIN1_MANAGER_INTERFACE "org.freedesktop.login1.Manager"
#define LOGIN1_SESSION_INTERFACE "org.freedesktop.login1.Session"
#define LOGIN1_USER_SELF_PATH "/org/freedesktop/login1/user/self"
#define LOGIN1_USER_INTERFACE "org.freedesktop.login1.User"

/* logind's LockedHint for our session: set by the screen locker while the
 * screen is locked. Read and watched as a third, independent source; the
 * plugin runs as a systemd user service, so the session is found through
 * XDG_SESSION_ID or, failing that, the user's display session. */

static void
login_session_properties_changed (GDBusProxy          *proxy,
                                  GVariant            *changed,
                                  char               **invalidated,
                                  GsdAutomountManager *manager)
{
        GVariant *v;

        v = g_variant_lookup_value (changed, "LockedHint", G_VARIANT_TYPE_BOOLEAN);
        if (v != NULL) {
                manager->priv->locked_hint = g_variant_get_boolean (v);
                manager->priv->locked_hint_known = TRUE;
                g_variant_unref (v);
                g_debug ("logind LockedHint changed to %d", manager->priv->locked_hint);
                lock_state_changed (manager);
        } else if (invalidated != NULL && g_strv_contains ((const char * const *) invalidated, "LockedHint")) {
                manager->priv->locked_hint_known = FALSE;
                g_debug ("logind LockedHint invalidated");
                update_allowed (manager);
        }
}

static void
login_session_proxy_ready_cb (GObject      *source,
                              GAsyncResult *res,
                              gpointer      user_data)
{
        GsdAutomountManager *manager;
        GDBusProxy *proxy;
        GVariant *v;
        GError *error = NULL;

        proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
        if (proxy == NULL) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Can't get proxy for the logind session, automount stays held: %s",
                                   error->message);
                g_error_free (error);
                return;
        }

        manager = GSD_AUTOMOUNT_MANAGER (user_data);
        manager->priv->login_session = proxy;
        g_signal_connect (proxy, "g-properties-changed",
                          G_CALLBACK (login_session_properties_changed), manager);

        v = g_dbus_proxy_get_cached_property (proxy, "LockedHint");
        if (v == NULL) {
                g_warning ("logind session has no LockedHint property, automount stays held");
                return;
        }
        manager->priv->locked_hint = g_variant_get_boolean (v);
        manager->priv->locked_hint_known = TRUE;
        g_variant_unref (v);

        g_debug ("logind LockedHint is %d", manager->priv->locked_hint);
        lock_state_changed (manager);
}

static void
login_session_path_found (GsdAutomountManager *manager,
                          const char          *path)
{
        if (path == NULL || !g_variant_is_object_path (path) || g_strcmp0 (path, "/") == 0) {
                g_warning ("No logind session for this process, automount stays held");
                return;
        }

        g_debug ("logind session %s", path);
        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                  G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                  NULL,
                                  LOGIN1_NAME,
                                  path,
                                  LOGIN1_SESSION_INTERFACE,
                                  manager->priv->cancellable,
                                  login_session_proxy_ready_cb,
                                  manager);
}

static void
login_display_session_cb (GObject      *source,
                          GAsyncResult *res,
                          gpointer      user_data)
{
        GVariant *result, *display = NULL;
        GError *error = NULL;
        const char *path = NULL;

        result = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
        if (result == NULL) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Can't read the user's display session from logind, automount stays held: %s",
                                   error->message);
                g_error_free (error);
                return;
        }

        g_variant_get (result, "(v)", &display);
        if (display != NULL && g_variant_is_of_type (display, G_VARIANT_TYPE ("(so)")))
                g_variant_get (display, "(&s&o)", NULL, &path);

        login_session_path_found (GSD_AUTOMOUNT_MANAGER (user_data), path);

        if (display != NULL)
                g_variant_unref (display);
        g_variant_unref (result);
}

static void
login_query_display_session (GsdAutomountManager *manager,
                             GDBusConnection     *bus)
{
        g_dbus_connection_call (bus,
                                LOGIN1_NAME,
                                LOGIN1_USER_SELF_PATH,
                                "org.freedesktop.DBus.Properties",
                                "Get",
                                g_variant_new ("(ss)", LOGIN1_USER_INTERFACE, "Display"),
                                G_VARIANT_TYPE ("(v)"),
                                G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                -1,
                                manager->priv->cancellable,
                                login_display_session_cb,
                                manager);
}

static void
login_get_session_cb (GObject      *source,
                      GAsyncResult *res,
                      gpointer      user_data)
{
        GVariant *result;
        GError *error = NULL;
        const char *path = NULL;

        result = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
        if (result == NULL) {
                if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                        g_error_free (error);
                        return;
                }
                g_debug ("logind GetSession failed (%s), trying the display session", error->message);
                g_error_free (error);
                login_query_display_session (GSD_AUTOMOUNT_MANAGER (user_data), G_DBUS_CONNECTION (source));
                return;
        }

        g_variant_get (result, "(&o)", &path);
        login_session_path_found (GSD_AUTOMOUNT_MANAGER (user_data), path);
        g_variant_unref (result);
}

static void
login_bus_ready_cb (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
        GsdAutomountManager *manager;
        GDBusConnection *bus;
        GError *error = NULL;
        const char *session_id;

        bus = g_bus_get_finish (res, &error);
        if (bus == NULL) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Can't reach the system bus, automount stays held: %s", error->message);
                g_error_free (error);
                return;
        }

        manager = GSD_AUTOMOUNT_MANAGER (user_data);
        session_id = g_getenv ("XDG_SESSION_ID");
        if (session_id != NULL && *session_id != '\0') {
                g_dbus_connection_call (bus,
                                        LOGIN1_NAME,
                                        LOGIN1_PATH,
                                        LOGIN1_MANAGER_INTERFACE,
                                        "GetSession",
                                        g_variant_new ("(s)", session_id),
                                        G_VARIANT_TYPE ("(o)"),
                                        G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                        -1,
                                        manager->priv->cancellable,
                                        login_get_session_cb,
                                        manager);
        } else {
                login_query_display_session (manager, bus);
        }
        g_object_unref (bus);
}

static void
do_initialize_logind (GsdAutomountManager *manager)
{
        manager->priv->cancellable = g_cancellable_new ();
        g_bus_get (G_BUS_TYPE_SYSTEM, manager->priv->cancellable,
                   login_bus_ready_cb, manager);
}

static void
setup_automounter (GsdAutomountManager *manager)
{
        do_initialize_session (manager);
        do_initialize_logind (manager);
        do_initialize_screensaver (manager);
        do_initialize_lockscreen (manager);

	manager->priv->volume_monitor = g_volume_monitor_get ();
	g_signal_connect_object (manager->priv->volume_monitor, "mount-added",
				 G_CALLBACK (mount_added_callback), manager, 0);
	g_signal_connect_object (manager->priv->volume_monitor, "volume-added",
				 G_CALLBACK (volume_added_callback), manager, 0);
	g_signal_connect_object (manager->priv->volume_monitor, "volume-removed",
				 G_CALLBACK (volume_removed_callback), manager, 0);

	manager->priv->automount_idle_id =
		g_idle_add_full (G_PRIORITY_LOW,
				 automount_all_volumes_idle_cb,
				 manager, NULL);
}

gboolean
gsd_automount_manager_start (GsdAutomountManager *manager,
                                       GError              **error)
{
        g_debug ("Starting automounting manager");
        gnome_settings_profile_start (NULL);

        manager->priv->settings = g_settings_new ("org.gnome.desktop.media-handling");
        setup_automounter (manager);

        gnome_settings_profile_end (NULL);

        return TRUE;
}

void
gsd_automount_manager_stop (GsdAutomountManager *manager)
{
        GsdAutomountManagerPrivate *p = manager->priv;

        g_debug ("Stopping automounting manager");

        /* nothing of ours may prompt or call back into a stopped manager */
        gsd_mount_guard_detach_all_for (manager);
        gsd_autorun_close_dialogs ();
        close_error_dialogs (manager);
        p->was_allowed = FALSE;

        if (p->ss_proxy) {
                g_signal_handlers_disconnect_by_data (p->ss_proxy, manager);
        }
        if (p->us_proxy) {
                g_signal_handlers_disconnect_by_data (p->us_proxy, manager);
        }
        if (p->login_session) {
                g_signal_handlers_disconnect_by_data (p->login_session, manager);
        }
        if (p->cancellable) {
                g_cancellable_cancel (p->cancellable);
                g_clear_object (&p->cancellable);
        }
        drop_cancellable (&p->ss_cancellable);
        drop_cancellable (&p->us_cancellable);
        g_clear_object (&p->login_session);
        p->locked_hint_known = FALSE;
        p->ss_resolved = p->us_resolved = FALSE;
        p->ss_present = p->us_present = FALSE;
        p->ss_known = p->us_known = FALSE;

        if (p->startup_queue != NULL) {
                g_list_free_full (p->startup_queue, g_object_unref);
                p->startup_queue = NULL;
        }

        g_clear_object (&p->session);
        g_clear_object (&p->volume_monitor);
        g_clear_object (&p->settings);
        g_clear_object (&p->ss_proxy);
        g_clear_object (&p->us_proxy);

        g_bus_unwatch_name (p->ss_watch_id);
        g_bus_unwatch_name (p->us_watch_id);

        if (p->volume_queue != NULL) {
                g_list_free_full (p->volume_queue, g_object_unref);
                p->volume_queue = NULL;
        }

        if (p->automount_idle_id != 0) {
                g_source_remove (p->automount_idle_id);
                p->automount_idle_id = 0;
        }
}

static void
gsd_automount_manager_class_init (GsdAutomountManagerClass *klass)
{
        g_type_class_add_private (klass, sizeof (GsdAutomountManagerPrivate));
}

static void
gsd_automount_manager_init (GsdAutomountManager *manager)
{
        manager->priv = GSD_AUTOMOUNT_MANAGER_GET_PRIVATE (manager);
}

GsdAutomountManager *
gsd_automount_manager_new (void)
{
        return GSD_AUTOMOUNT_MANAGER (g_object_new (GSD_TYPE_AUTOMOUNT_MANAGER, NULL));
}
