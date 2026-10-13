/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Tests of the idle monitor's D-Bus export, as its clients see it: the
 * org.gnome.Mutter.IdleMonitor interface and, in a Unity session, the same
 * monitors as org.cinnamon.Muffin.IdleMonitor (the name libcinnamon-desktop's
 * GnomeIdleMonitor, and so cinnamon-session's presence, is a client of).
 *
 * Runs under xvfb-run (debian/rules) on a private session bus. Every client
 * call is asynchronous with a bounded wait, because the exported objects are
 * served from this same thread.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "config.h"

#include <string.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>

#include "gsd-idle-monitor-private.h"

#define MUTTER_NAME  "org.gnome.Mutter.IdleMonitor"
#define MUTTER_CORE  "/org/gnome/Mutter/IdleMonitor/Core"
#define MUFFIN_NAME  "org.cinnamon.Muffin.IdleMonitor"
#define MUFFIN_ROOT  "/org/cinnamon/Muffin/IdleMonitor"
#define MUFFIN_CORE  "/org/cinnamon/Muffin/IdleMonitor/Core"

#define WAIT_MS 5000

static GTestDBus *bus;

typedef struct {
        GDBusConnection *connection;
        const char      *iface;
        const char      *path;
        guint            subscription;
        GArray          *fired;     /* watch ids from WatchFired */
        GArray          *wrong;     /* WatchFired on another interface or path */
} Client;

static gboolean
timeout_cb (gpointer data)
{
        *(gboolean *) data = TRUE;
        return G_SOURCE_REMOVE;
}

/* iterate until *flag or the bounded wait is over */
static gboolean
wait_for (gboolean *flag, guint ms)
{
        gboolean timed_out = FALSE;
        guint id = g_timeout_add (ms, timeout_cb, &timed_out);

        while (!*flag && !timed_out)
                g_main_context_iteration (NULL, TRUE);
        if (!timed_out)
                g_source_remove (id);
        return *flag;
}

static void
spin (guint ms)
{
        gboolean never = FALSE;

        wait_for (&never, ms);
}

typedef struct {
        gboolean  done;
        GVariant *reply;
        GError   *error;
} Call;

static void
call_done (GObject *source, GAsyncResult *res, gpointer data)
{
        Call *call = data;

        call->reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &call->error);
        call->done = TRUE;
}

/* a method call on the monitor; NULL on error or timeout */
static GVariant *
call (GDBusConnection *connection, const char *name, const char *path, const char *iface,
      const char *method, GVariant *args, const char *reply_type)
{
        Call c = { FALSE, NULL, NULL };

        g_dbus_connection_call (connection, name, path, iface, method, args,
                                reply_type ? G_VARIANT_TYPE (reply_type) : NULL,
                                G_DBUS_CALL_FLAGS_NO_AUTO_START, WAIT_MS, NULL, call_done, &c);
        if (!wait_for (&c.done, WAIT_MS + 1000))
                return NULL;
        if (c.error != NULL) {
                g_test_message ("%s.%s: %s", iface, method, c.error->message);
                g_clear_error (&c.error);
                return NULL;
        }
        return c.reply;
}

static gboolean
name_has_owner (const char *name)
{
        GDBusConnection *connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
        GVariant *reply;
        gboolean owned = FALSE;

        reply = call (connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                      "org.freedesktop.DBus", "NameHasOwner", g_variant_new ("(s)", name), "(b)");
        if (reply != NULL) {
                g_variant_get (reply, "(b)", &owned);
                g_variant_unref (reply);
        }
        g_object_unref (connection);
        return owned;
}

static gboolean
wait_for_owner (const char *name)
{
        gint64 end = g_get_monotonic_time () + WAIT_MS * 1000;

        while (g_get_monotonic_time () < end) {
                if (name_has_owner (name))
                        return TRUE;
                spin (50);
        }
        return FALSE;
}

static void
watch_fired (GDBusConnection *connection,
             const char      *sender,
             const char      *path,
             const char      *iface,
             const char      *signal,
             GVariant        *parameters,
             gpointer         data)
{
        Client *client = data;
        guint id;

        g_variant_get (parameters, "(u)", &id);
        if (g_strcmp0 (iface, client->iface) == 0 && g_strcmp0 (path, client->path) == 0)
                g_array_append_val (client->fired, id);
        else
                g_array_append_val (client->wrong, id);
}

/* a client on its own connection (its own unique name), listening for
 * WatchFired on every interface and path, as a match rule would let it */
static Client *
client_new (const char *iface, const char *path)
{
        Client *client = g_new0 (Client, 1);
        GError *error = NULL;

        client->connection = g_dbus_connection_new_for_address_sync (g_test_dbus_get_bus_address (bus),
                                                                      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                                      G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                                      NULL, NULL, &error);
        g_assert_no_error (error);
        client->iface = iface;
        client->path = path;
        client->fired = g_array_new (FALSE, FALSE, sizeof (guint));
        client->wrong = g_array_new (FALSE, FALSE, sizeof (guint));
        client->subscription = g_dbus_connection_signal_subscribe (client->connection, NULL, NULL,
                                                                   "WatchFired", NULL, NULL,
                                                                   G_DBUS_SIGNAL_FLAGS_NONE,
                                                                   watch_fired, client, NULL);
        return client;
}

static void
client_free (Client *client)
{
        g_dbus_connection_signal_unsubscribe (client->connection, client->subscription);
        g_dbus_connection_close_sync (client->connection, NULL, NULL);
        g_object_unref (client->connection);
        g_array_unref (client->fired);
        g_array_unref (client->wrong);
        g_free (client);
}

static gboolean
client_has_fired (Client *client, guint id)
{
        guint i;

        for (i = 0; i < client->fired->len; i++)
                if (g_array_index (client->fired, guint, i) == id)
                        return TRUE;
        return FALSE;
}

static gboolean
client_got_any (Client *client, guint id)
{
        guint i;

        if (client_has_fired (client, id))
                return TRUE;
        for (i = 0; i < client->wrong->len; i++)
                if (g_array_index (client->wrong, guint, i) == id)
                        return TRUE;
        return FALSE;
}

/* wait until the client got WatchFired for id on its own interface and path */
static gboolean
wait_fired (Client *client, guint id, guint ms)
{
        gint64 end = g_get_monotonic_time () + (gint64) ms * 1000;

        while (!client_has_fired (client, id) && g_get_monotonic_time () < end)
                spin (20);
        return client_has_fired (client, id);
}

static guint
add_idle_watch (Client *client, guint64 interval)
{
        const char *name = g_str_equal (client->iface, MUFFIN_NAME) ? MUFFIN_NAME : MUTTER_NAME;
        GVariant *reply;
        guint id = 0;

        reply = call (client->connection, name, client->path, client->iface,
                      "AddIdleWatch", g_variant_new ("(t)", interval), "(u)");
        if (reply != NULL) {
                g_variant_get (reply, "(u)", &id);
                g_variant_unref (reply);
        }
        return id;
}

static gboolean
remove_watch (Client *client, guint id)
{
        const char *name = g_str_equal (client->iface, MUFFIN_NAME) ? MUFFIN_NAME : MUTTER_NAME;
        GVariant *reply;

        reply = call (client->connection, name, client->path, client->iface,
                      "RemoveWatch", g_variant_new ("(u)", id), NULL);
        if (reply == NULL)
                return FALSE;
        g_variant_unref (reply);
        return TRUE;
}

/* user activity: the X idle counter starts again from 0 */
static void
reset_idle (void)
{
        Display *display = GDK_DISPLAY_XDISPLAY (gdk_display_get_default ());

        XResetScreenSaver (display);
        XSync (display, False);
}

/* (a) the Muffin name is owned and its object manager has /Core */
typedef struct {
        gboolean            done;
        GDBusObjectManager *manager;
} ManagerResult;

static void
manager_ready (GObject *source, GAsyncResult *res, gpointer data)
{
        ManagerResult *result = data;

        result->manager = g_dbus_object_manager_client_new_finish (res, NULL);
        result->done = TRUE;
}

static void
test_muffin_core_object (void)
{
        Client *client = client_new (MUFFIN_NAME, MUFFIN_CORE);
        ManagerResult result = { FALSE, NULL };
        GDBusObject *core;
        GDBusInterface *iface;

        g_assert_true (wait_for_owner (MUFFIN_NAME));

        /* as libcinnamon-desktop's GnomeIdleMonitor does */
        g_dbus_object_manager_client_new (client->connection,
                                          G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
                                          MUFFIN_NAME, MUFFIN_ROOT,
                                          NULL, NULL, NULL, NULL, manager_ready, &result);
        g_assert_true (wait_for (&result.done, WAIT_MS));
        g_assert_nonnull (result.manager);
        /* the client reads the objects once the name owner answered */
        spin (200);
        core = g_dbus_object_manager_get_object (result.manager, MUFFIN_CORE);
        g_assert_nonnull (core);
        iface = g_dbus_object_get_interface (core, MUFFIN_NAME);
        g_assert_nonnull (iface);

        g_object_unref (iface);
        g_object_unref (core);
        g_object_unref (result.manager);
        client_free (client);
}

/* (b) GetIdletime answers on the Muffin interface */
static void
test_muffin_get_idletime (void)
{
        Client *client = client_new (MUFFIN_NAME, MUFFIN_CORE);
        GVariant *reply;
        guint64 idletime = 0;

        reset_idle ();
        spin (300);
        reply = call (client->connection, MUFFIN_NAME, MUFFIN_CORE, MUFFIN_NAME,
                      "GetIdletime", NULL, "(t)");
        g_assert_nonnull (reply);
        g_variant_get (reply, "(t)", &idletime);
        g_variant_unref (reply);
        g_assert_cmpuint (idletime, >=, 200);
        g_assert_cmpuint (idletime, <, 5000);
        client_free (client);
}

/* (c) an idle watch fires on the Muffin interface, after the interval and
 * at once when the user is already idle longer than the interval */
static void
test_muffin_idle_watch_fires (void)
{
        Client *client = client_new (MUFFIN_NAME, MUFFIN_CORE);
        guint id;

        reset_idle ();
        id = add_idle_watch (client, 500);
        g_assert_cmpuint (id, >, 0);
        spin (200);
        g_assert_false (client_got_any (client, id));
        g_assert_true (wait_fired (client, id, 3000));
        g_assert_true (remove_watch (client, id));

        /* already idle for ~1 s: fires without new idle time */
        reset_idle ();
        spin (1000);
        id = add_idle_watch (client, 300);
        g_assert_cmpuint (id, >, 0);
        g_assert_true (wait_fired (client, id, 1000));
        g_assert_true (remove_watch (client, id));
        g_assert_cmpuint (client->wrong->len, ==, 0);
        client_free (client);
}

/* (d) a watch fires only for the client that added it, and only on the
 * interface and object path it was added through */
static void
test_watch_reaches_only_its_client (void)
{
        Client *one = client_new (MUFFIN_NAME, MUFFIN_CORE);
        Client *two = client_new (MUFFIN_NAME, MUFFIN_CORE);
        Client *mutter = client_new (MUTTER_NAME, MUTTER_CORE);
        guint id_one, id_two, id_mutter;

        reset_idle ();
        id_one = add_idle_watch (one, 400);
        id_two = add_idle_watch (two, 400);
        id_mutter = add_idle_watch (mutter, 400);
        g_assert_cmpuint (id_one, >, 0);
        g_assert_cmpuint (id_two, >, 0);
        g_assert_cmpuint (id_mutter, >, 0);

        g_assert_true (wait_fired (one, id_one, 3000));
        g_assert_true (wait_fired (two, id_two, 3000));
        g_assert_true (wait_fired (mutter, id_mutter, 3000));
        spin (200);

        g_assert_false (client_got_any (one, id_two));
        g_assert_false (client_got_any (one, id_mutter));
        g_assert_false (client_got_any (two, id_one));
        g_assert_false (client_got_any (two, id_mutter));
        g_assert_false (client_got_any (mutter, id_one));
        g_assert_false (client_got_any (mutter, id_two));
        g_assert_cmpuint (one->wrong->len, ==, 0);
        g_assert_cmpuint (two->wrong->len, ==, 0);
        g_assert_cmpuint (mutter->wrong->len, ==, 0);

        remove_watch (one, id_one);
        remove_watch (two, id_two);
        remove_watch (mutter, id_mutter);
        client_free (one);
        client_free (two);
        client_free (mutter);
}

/* (e) through the Muffin interface a client removes only its own watches:
 * not one added through the Mutter interface, not another client's */
static void
test_muffin_remove_only_own_watch (void)
{
        Client *one = client_new (MUFFIN_NAME, MUFFIN_CORE);
        Client *two = client_new (MUFFIN_NAME, MUFFIN_CORE);
        Client *mutter = client_new (MUTTER_NAME, MUTTER_CORE);
        guint id_two, id_mutter;

        reset_idle ();
        id_two = add_idle_watch (two, 600);
        id_mutter = add_idle_watch (mutter, 600);
        g_assert_cmpuint (id_two, >, 0);
        g_assert_cmpuint (id_mutter, >, 0);

        g_assert_true (remove_watch (one, id_two));
        g_assert_true (remove_watch (one, id_mutter));

        g_assert_true (wait_fired (two, id_two, 3000));
        g_assert_true (wait_fired (mutter, id_mutter, 3000));

        remove_watch (two, id_two);
        remove_watch (mutter, id_mutter);
        client_free (one);
        client_free (two);
        client_free (mutter);
}

/* (f) the Mutter interface works as before */
static void
test_mutter_unchanged (void)
{
        Client *client = client_new (MUTTER_NAME, MUTTER_CORE);
        GVariant *reply;
        guint id;

        g_assert_true (wait_for_owner (MUTTER_NAME));
        reply = call (client->connection, MUTTER_NAME, MUTTER_CORE, MUTTER_NAME,
                      "GetIdletime", NULL, "(t)");
        g_assert_nonnull (reply);
        g_variant_unref (reply);

        reset_idle ();
        id = add_idle_watch (client, 300);
        g_assert_cmpuint (id, >, 0);
        g_assert_true (wait_fired (client, id, 3000));
        g_assert_true (remove_watch (client, id));
        g_assert_cmpuint (client->wrong->len, ==, 0);
        client_free (client);
}

/* (g) outside a Unity session the Muffin name is not taken */
static void
test_no_muffin_outside_unity (void)
{
        g_assert_true (wait_for_owner (MUTTER_NAME));
        spin (500);
        g_assert_false (name_has_owner (MUFFIN_NAME));
}

#ifdef IDLE_MONITOR_REGISTRY_TEST
/* (h) a client's watches go when its connection closes */
static void
test_muffin_watches_go_with_client (void)
{
        Client *client = client_new (MUFFIN_NAME, MUFFIN_CORE);
        gint64 end;

        g_assert_true (wait_for_owner (MUFFIN_NAME));
        g_assert_cmpuint (_idle_monitor_muffin_watch_count (), ==, 0);
        reset_idle ();
        g_assert_cmpuint (add_idle_watch (client, 60000), >, 0);
        g_assert_cmpuint (add_idle_watch (client, 120000), >, 0);
        g_assert_cmpuint (_idle_monitor_muffin_watch_count (), ==, 2);

        client_free (client);
        end = g_get_monotonic_time () + WAIT_MS * 1000;
        while (_idle_monitor_muffin_watch_count () > 0 && g_get_monotonic_time () < end)
                spin (20);
        g_assert_cmpuint (_idle_monitor_muffin_watch_count (), ==, 0);
}
#endif

typedef struct {
        const char *path;
        void      (*func) (void);
        const char *desktop;
} Case;

static const Case cases[] = {
#ifdef IDLE_MONITOR_REGISTRY_TEST
        { "/idle-monitor/muffin-watches-go-with-client", test_muffin_watches_go_with_client, "Unity:Unity7:ubuntu" },
#else
        { "/idle-monitor/mutter-unchanged", test_mutter_unchanged, "Unity:Unity7:ubuntu" },
        { "/idle-monitor/muffin-core-object", test_muffin_core_object, "Unity:Unity7:ubuntu" },
        { "/idle-monitor/muffin-get-idletime", test_muffin_get_idletime, "Unity:Unity7:ubuntu" },
        { "/idle-monitor/muffin-idle-watch-fires", test_muffin_idle_watch_fires, "Unity:Unity7:ubuntu" },
        { "/idle-monitor/watch-reaches-only-its-client", test_watch_reaches_only_its_client, "Unity:Unity7:ubuntu" },
        { "/idle-monitor/muffin-remove-only-own-watch", test_muffin_remove_only_own_watch, "Unity:Unity7:ubuntu" },
        { "/idle-monitor/no-muffin-outside-unity", test_no_muffin_outside_unity, "GNOME" },
#endif
};

/* Each case runs in its own process, with its own private bus: the export is
 * set up once per process, and a failing case must not hide the others. */
static void
run_case (gconstpointer data)
{
        const Case *c = data;

        if (g_test_subprocess ()) {
                bus = g_test_dbus_new (G_TEST_DBUS_NONE);
                g_test_dbus_up (bus);
                g_setenv ("XDG_CURRENT_DESKTOP", c->desktop, TRUE);
                gsd_idle_monitor_init_dbus (FALSE);
                c->func ();
                /* the export keeps the shared connection for the life of the
                 * process, so stop the daemon instead of waiting for it */
                g_test_dbus_stop (bus);
                return;
        }

        g_test_trap_subprocess (NULL, 60 * G_USEC_PER_SEC,
                                G_TEST_SUBPROCESS_INHERIT_STDOUT | G_TEST_SUBPROCESS_INHERIT_STDERR);
        if (!g_test_trap_has_passed ())
                g_test_fail ();
}

int
main (int argc, char **argv)
{
        guint i;

        g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
        gtk_test_init (&argc, &argv, NULL);

        for (i = 0; i < G_N_ELEMENTS (cases); i++)
                g_test_add_data_func (cases[i].path, &cases[i], run_case);

        return g_test_run ();
}
