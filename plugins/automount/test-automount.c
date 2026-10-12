/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Tests for the automount manager's lock gate: the fail-closed predicate,
 * the start-up gate, the queue drain and the autorun gate. The manager's
 * source is included so its static functions can be called; gsd-autorun.c
 * is replaced by the stubs below, which record calls and set or clear the
 * same "gsd-allow-autorun" object data as the real functions.
 *
 * No bus is used: each case starts from a fresh manager and sets only the
 * state it names.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "config.h"

#include <gtk/gtk.h>

/* GtkMountOperation needs a session bus at construction, which the package
 * build does not have; the tests only check whether a mount operation is
 * passed, so a plain GMountOperation stands in for it. */
#define gtk_mount_operation_new(parent) g_mount_operation_new ()

#include "gsd-automount-manager.c"

/* --- gsd-autorun stubs ------------------------------------------------ */

static guint autorun_calls;

void
gsd_autorun (GMount *mount,
             GSettings *settings,
             GsdAutorunAllowed allowed_func,
             GsdAutorunOpenWindow open_window_func,
             gpointer user_data)
{
        autorun_calls++;
}

void
gsd_allow_autorun_for_volume (GVolume *volume)
{
        g_object_set_data (G_OBJECT (volume), "gsd-allow-autorun", GINT_TO_POINTER (1));
}

void
gsd_allow_autorun_for_volume_finish (GVolume *volume)
{
}

void
gsd_disallow_autorun_for_volume (GVolume *volume)
{
        g_object_set_data (G_OBJECT (volume), "gsd-allow-autorun", NULL);
}

static guint close_dialogs_calls;

void
gsd_autorun_close_dialogs (void)
{
        close_dialogs_calls++;
}

static gboolean
allows_autorun (gpointer object)
{
        return g_object_get_data (G_OBJECT (object), "gsd-allow-autorun") != NULL;
}

/* --- a fake volume ------------------------------------------------------ */

typedef struct {
        GObject parent;
        gboolean should_automount;
        gboolean can_mount;
        GMount *mount;
        guint mounts;             /* g_volume_mount() calls */
        gboolean last_had_op;     /* the last call passed a GMountOperation */
        gboolean last_allowed;    /* the allow flag was set at that call */
        gboolean hold;            /* keep the mount in flight until finished */
        GTask *held_task;
        GMountOperation *held_op;
} FakeVolume;

typedef struct {
        GObjectClass parent_class;
} FakeVolumeClass;

static GType fake_volume_get_type (void);
static void fake_volume_iface_init (GVolumeIface *iface);

G_DEFINE_TYPE_WITH_CODE (FakeVolume, fake_volume, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME, fake_volume_iface_init))

static void
fake_volume_finalize (GObject *object)
{
        g_clear_object (&((FakeVolume *) object)->mount);
        g_clear_object (&((FakeVolume *) object)->held_task);
        g_clear_object (&((FakeVolume *) object)->held_op);
        G_OBJECT_CLASS (fake_volume_parent_class)->finalize (object);
}

static void
fake_volume_class_init (FakeVolumeClass *klass)
{
        G_OBJECT_CLASS (klass)->finalize = fake_volume_finalize;
}

static void
fake_volume_init (FakeVolume *volume)
{
        volume->should_automount = TRUE;
        volume->can_mount = TRUE;
}

static char *
fake_volume_get_name (GVolume *volume)
{
        return g_strdup ("fake volume");
}

static GMount *
fake_volume_get_mount (GVolume *volume)
{
        FakeVolume *fake = (FakeVolume *) volume;

        return fake->mount != NULL ? g_object_ref (fake->mount) : NULL;
}

static gboolean
fake_volume_can_mount (GVolume *volume)
{
        return ((FakeVolume *) volume)->can_mount;
}

static gboolean
fake_volume_should_automount (GVolume *volume)
{
        return ((FakeVolume *) volume)->should_automount;
}

static void
fake_volume_mount (GVolume             *volume,
                   GMountMountFlags     flags,
                   GMountOperation     *mount_operation,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
        FakeVolume *fake = (FakeVolume *) volume;
        GTask *task;

        fake->mounts++;
        fake->last_had_op = mount_operation != NULL;
        fake->last_allowed = allows_autorun (volume);

        task = g_task_new (volume, cancellable, callback, user_data);
        if (fake->hold) {
                fake->held_task = task;
                fake->held_op = mount_operation != NULL ? g_object_ref (mount_operation) : NULL;
                return;
        }
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
}

/* Finish a held mount: success, or a plain failure (not "handled"). */
static void
fake_volume_finish (FakeVolume *fake, gboolean ok)
{
        GTask *task = fake->held_task;

        fake->held_task = NULL;
        if (ok)
                g_task_return_boolean (task, TRUE);
        else
                g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "fake failure");
        g_object_unref (task);
}

static gboolean
fake_volume_mount_finish (GVolume       *volume,
                          GAsyncResult  *result,
                          GError       **error)
{
        return g_task_propagate_boolean (G_TASK (result), error);
}

static void
fake_volume_iface_init (GVolumeIface *iface)
{
        iface->get_name = fake_volume_get_name;
        iface->get_mount = fake_volume_get_mount;
        iface->can_mount = fake_volume_can_mount;
        iface->should_automount = fake_volume_should_automount;
        iface->mount_fn = fake_volume_mount;
        iface->mount_finish = fake_volume_mount_finish;
}

static FakeVolume *
fake_volume_new (gboolean should_automount)
{
        FakeVolume *volume = g_object_new (fake_volume_get_type (), NULL);

        volume->should_automount = should_automount;
        return volume;
}

/* --- a fake mount, for mount-added -------------------------------------- */

typedef struct {
        GObject parent;
        GVolume *volume;
} FakeMount;

typedef struct {
        GObjectClass parent_class;
} FakeMountClass;

static GType fake_mount_get_type (void);
static void fake_mount_iface_init (GMountIface *iface);

G_DEFINE_TYPE_WITH_CODE (FakeMount, fake_mount, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT, fake_mount_iface_init))

static void
fake_mount_finalize (GObject *object)
{
        g_clear_object (&((FakeMount *) object)->volume);
        G_OBJECT_CLASS (fake_mount_parent_class)->finalize (object);
}

static void
fake_mount_class_init (FakeMountClass *klass)
{
        G_OBJECT_CLASS (klass)->finalize = fake_mount_finalize;
}

static void
fake_mount_init (FakeMount *mount)
{
}

static GVolume *
fake_mount_get_volume (GMount *mount)
{
        FakeMount *fake = (FakeMount *) mount;

        return fake->volume != NULL ? g_object_ref (fake->volume) : NULL;
}

static void
fake_mount_iface_init (GMountIface *iface)
{
        iface->get_volume = fake_mount_get_volume;
}

static GMount *
fake_mount_new (GVolume *volume)
{
        FakeMount *mount = g_object_new (fake_mount_get_type (), NULL);

        mount->volume = g_object_ref (volume);
        return G_MOUNT (mount);
}

/* --- a fake volume monitor, for the start-up scan ------------------------ */

typedef struct {
        GVolumeMonitor parent;
        GList *volumes;
} FakeMonitor;

typedef struct {
        GVolumeMonitorClass parent_class;
} FakeMonitorClass;

static GType fake_monitor_get_type (void);

G_DEFINE_TYPE (FakeMonitor, fake_monitor, G_TYPE_VOLUME_MONITOR)

static GList *
fake_monitor_get_volumes (GVolumeMonitor *monitor)
{
        GList *copy = g_list_copy (((FakeMonitor *) monitor)->volumes);

        g_list_foreach (copy, (GFunc) g_object_ref, NULL);
        return copy;
}

static void
fake_monitor_finalize (GObject *object)
{
        g_list_free_full (((FakeMonitor *) object)->volumes, g_object_unref);
        G_OBJECT_CLASS (fake_monitor_parent_class)->finalize (object);
}

static void
fake_monitor_class_init (FakeMonitorClass *klass)
{
        G_OBJECT_CLASS (klass)->finalize = fake_monitor_finalize;
        G_VOLUME_MONITOR_CLASS (klass)->get_volumes = fake_monitor_get_volumes;
}

static void
fake_monitor_init (FakeMonitor *monitor)
{
}

/* --- helpers --------------------------------------------------------------- */

static void
run_pending (void)
{
        while (g_main_context_iteration (NULL, FALSE))
                ;
}

/* A fresh manager with its settings; no bus, no watchers. */
static GsdAutomountManager *
manager_new (void)
{
        GsdAutomountManager *manager = gsd_automount_manager_new ();

        manager->priv->settings = g_settings_new ("org.gnome.desktop.media-handling");
        return manager;
}

static void
manager_free (GsdAutomountManager *manager)
{
        GsdAutomountManagerPrivate *p = manager->priv;

        g_list_free_full (p->volume_queue, g_object_unref);
        p->volume_queue = NULL;
        g_list_free_full (p->startup_queue, g_object_unref);
        p->startup_queue = NULL;
        g_clear_object (&p->volume_monitor);
        g_clear_object (&p->settings);
        g_object_unref (manager);
}

/* The state in which every source is known and says "unlocked": logind
 * read, both lock providers resolved and absent. */
static void
set_known_unlocked (GsdAutomountManager *manager)
{
        GsdAutomountManagerPrivate *p = manager->priv;

        p->locked_hint_known = TRUE;
        p->locked_hint = FALSE;
        p->ss_resolved = TRUE;
        p->us_resolved = TRUE;
        p->ss_present = FALSE;
        p->us_present = FALSE;
}

/* --- predicate ------------------------------------------------------------ */

static void
test_predicate_fresh_manager (void)
{
        GsdAutomountManager *manager = manager_new ();

        g_assert_false (lock_state_allows_mount (manager));
        manager_free (manager);
}

static void
test_predicate_logind_not_read (void)
{
        GsdAutomountManager *manager = manager_new ();

        /* presence resolved, nothing present, but LockedHint never read */
        manager->priv->ss_resolved = TRUE;
        manager->priv->us_resolved = TRUE;
        g_assert_false (lock_state_allows_mount (manager));
        manager_free (manager);
}

static void
test_predicate_known_unlocked (void)
{
        GsdAutomountManager *manager = manager_new ();

        set_known_unlocked (manager);
        g_assert_true (lock_state_allows_mount (manager));
        manager_free (manager);
}

static void
test_predicate_locked_hint (void)
{
        GsdAutomountManager *manager = manager_new ();

        set_known_unlocked (manager);
        manager->priv->locked_hint = TRUE;
        g_assert_false (lock_state_allows_mount (manager));
        manager_free (manager);
}

static void
test_predicate_watchers_not_reported (void)
{
        GsdAutomountManager *manager;

        manager = manager_new ();
        set_known_unlocked (manager);
        manager->priv->ss_resolved = FALSE;
        g_assert_false (lock_state_allows_mount (manager));
        manager_free (manager);

        manager = manager_new ();
        set_known_unlocked (manager);
        manager->priv->us_resolved = FALSE;
        g_assert_false (lock_state_allows_mount (manager));
        manager_free (manager);
}

static void
test_predicate_present_unknown (void)
{
        GsdAutomountManager *manager;

        manager = manager_new ();
        set_known_unlocked (manager);
        manager->priv->ss_present = TRUE;
        g_assert_false (lock_state_allows_mount (manager));
        manager->priv->ss_known = TRUE;
        g_assert_true (lock_state_allows_mount (manager));
        manager->priv->screensaver_active = TRUE;
        g_assert_false (lock_state_allows_mount (manager));
        manager_free (manager);

        manager = manager_new ();
        set_known_unlocked (manager);
        manager->priv->us_present = TRUE;
        g_assert_false (lock_state_allows_mount (manager));
        manager->priv->us_known = TRUE;
        g_assert_true (lock_state_allows_mount (manager));
        manager->priv->lockscreen_active = TRUE;
        g_assert_false (lock_state_allows_mount (manager));
        manager_free (manager);
}

/* --- start-up gate and queue drain ------------------------------------------ */

static void
test_startup_volume_waits_for_state (void)
{
        GsdAutomountManager *manager = manager_new ();
        FakeMonitor *monitor = g_object_new (fake_monitor_get_type (), NULL);
        FakeVolume *volume = fake_volume_new (TRUE);

        monitor->volumes = g_list_append (NULL, g_object_ref (volume));
        manager->priv->volume_monitor = G_VOLUME_MONITOR (monitor);

        /* the lock state is unknown: queued, not mounted */
        automount_all_volumes (manager);
        run_pending ();
        g_assert_cmpuint (volume->mounts, ==, 0);

        /* known unlocked: mounted once, as at start-up (no mount
         * operation, no autorun permission) */
        set_known_unlocked (manager);
        check_volume_queue (manager);
        run_pending ();
        g_assert_cmpuint (volume->mounts, ==, 1);
        g_assert_false (volume->last_had_op);
        g_assert_false (volume->last_allowed);
        g_assert_null (manager->priv->startup_queue);

        g_object_unref (volume);
        manager_free (manager);
}

static void
test_queue_drains_each_volume_once (void)
{
        GsdAutomountManager *manager = manager_new ();
        FakeVolume *volumes[3];
        guint i;

        manager->priv->session_is_active = TRUE;
        set_known_unlocked (manager);
        manager->priv->locked_hint = TRUE;

        for (i = 0; i < G_N_ELEMENTS (volumes); i++) {
                volumes[i] = fake_volume_new (TRUE);
                check_screen_lock_and_mount (manager, G_VOLUME (volumes[i]));
        }
        run_pending ();
        for (i = 0; i < G_N_ELEMENTS (volumes); i++)
                g_assert_cmpuint (volumes[i]->mounts, ==, 0);
        g_assert_cmpuint (g_list_length (manager->priv->volume_queue), ==, 3);

        /* unlocked: each queued volume is mounted once, with a mount
         * operation and the autorun permission (it was inserted into the
         * user's session) */
        manager->priv->locked_hint = FALSE;
        check_volume_queue (manager);
        run_pending ();
        for (i = 0; i < G_N_ELEMENTS (volumes); i++) {
                g_assert_cmpuint (volumes[i]->mounts, ==, 1);
                g_assert_true (volumes[i]->last_had_op);
                g_assert_true (volumes[i]->last_allowed);
        }
        g_assert_null (manager->priv->volume_queue);

        for (i = 0; i < G_N_ELEMENTS (volumes); i++)
                g_object_unref (volumes[i]);
        manager_free (manager);
}

/* --- autorun gate ----------------------------------------------------------- */

static void
test_autorun_mount_added_while_locked (void)
{
        GsdAutomountManager *manager = manager_new ();
        FakeVolume *volume = fake_volume_new (TRUE);
        GMount *mount = fake_mount_new (G_VOLUME (volume));

        manager->priv->session_is_active = TRUE;
        set_known_unlocked (manager);
        manager->priv->locked_hint = TRUE;
        gsd_allow_autorun_for_volume (G_VOLUME (volume));

        autorun_calls = 0;
        mount_added_callback (NULL, mount, manager);
        g_assert_cmpuint (autorun_calls, ==, 0);
        /* the permission given earlier does not survive the refusal */
        g_assert_false (allows_autorun (volume));

        g_object_unref (mount);
        g_object_unref (volume);
        manager_free (manager);
}

static void
test_autorun_mount_added_lock_state_unknown (void)
{
        GsdAutomountManager *manager = manager_new ();
        FakeVolume *volume = fake_volume_new (TRUE);
        GMount *mount = fake_mount_new (G_VOLUME (volume));

        /* session active, but logind and the providers not read yet */
        manager->priv->session_is_active = TRUE;
        gsd_allow_autorun_for_volume (G_VOLUME (volume));

        autorun_calls = 0;
        mount_added_callback (NULL, mount, manager);
        g_assert_cmpuint (autorun_calls, ==, 0);

        g_object_unref (mount);
        g_object_unref (volume);
        manager_free (manager);
}

static void
test_autorun_mount_added_unlocked (void)
{
        GsdAutomountManager *manager = manager_new ();
        FakeVolume *volume = fake_volume_new (TRUE);
        GMount *mount = fake_mount_new (G_VOLUME (volume));

        manager->priv->session_is_active = TRUE;
        set_known_unlocked (manager);

        autorun_calls = 0;
        mount_added_callback (NULL, mount, manager);
        g_assert_cmpuint (autorun_calls, ==, 1);

        g_object_unref (mount);
        g_object_unref (volume);
        manager_free (manager);
}

static void
test_autorun_mount_added_inactive (void)
{
        GsdAutomountManager *manager = manager_new ();
        FakeVolume *volume = fake_volume_new (TRUE);
        GMount *mount = fake_mount_new (G_VOLUME (volume));

        manager->priv->session_is_active = FALSE;
        set_known_unlocked (manager);

        autorun_calls = 0;
        mount_added_callback (NULL, mount, manager);
        g_assert_cmpuint (autorun_calls, ==, 0);

        g_object_unref (mount);
        g_object_unref (volume);
        manager_free (manager);
}

static void
test_autorun_permission_not_given_while_locked (void)
{
        GsdAutomountManager *manager = manager_new ();
        FakeVolume *volume = fake_volume_new (FALSE);   /* not automounted */

        manager->priv->session_is_active = TRUE;
        set_known_unlocked (manager);
        manager->priv->locked_hint = TRUE;

        volume_added_callback (NULL, G_VOLUME (volume), manager);
        g_assert_false (allows_autorun (volume));
        g_assert_cmpuint (volume->mounts, ==, 0);

        g_object_unref (volume);
        manager_free (manager);
}

static void
test_autorun_permission_given_when_unlocked (void)
{
        GsdAutomountManager *manager = manager_new ();
        FakeVolume *volume = fake_volume_new (FALSE);   /* not automounted */

        manager->priv->session_is_active = TRUE;
        set_known_unlocked (manager);

        volume_added_callback (NULL, G_VOLUME (volume), manager);
        g_assert_true (allows_autorun (volume));
        g_assert_cmpuint (volume->mounts, ==, 0);

        g_object_unref (volume);
        manager_free (manager);
}


/* --- prompts and dialogs follow the session state ------------------------- */

typedef struct {
        guint probes;
        guint replies;
        guint aborted;
        GMountOperationResult last_result;
} OpWatch;

static void
watch_probe (GMountOperation *op, const char *m, const char *u, const char *d,
             GAskPasswordFlags f, OpWatch *w)
{
        w->probes++;
}

static void
watch_reply (GMountOperation *op, GMountOperationResult result, OpWatch *w)
{
        w->replies++;
        w->last_result = result;
}

static void
watch_aborted (GMountOperation *op, OpWatch *w)
{
        w->aborted++;
}

static void set_locked_hint (GsdAutomountManager *manager, gboolean locked);

/* Start a mount that stays in flight, in an active, unlocked session, and
 * watch its operation. */
static FakeVolume *
start_held_mount (GsdAutomountManager *manager, OpWatch *w)
{
        FakeVolume *volume = fake_volume_new (TRUE);

        volume->hold = TRUE;
        manager->priv->session_is_active = TRUE;
        set_known_unlocked (manager);
        /* tell the manager, so it has seen the session allowed */
        set_locked_hint (manager, FALSE);
        check_screen_lock_and_mount (manager, G_VOLUME (volume));
        g_assert_nonnull (volume->held_op);

        /* an after-handler stands for the dialog of the class handler */
        g_signal_connect_after (volume->held_op, "ask-password", G_CALLBACK (watch_probe), w);
        g_signal_connect (volume->held_op, "reply", G_CALLBACK (watch_reply), w);
        g_signal_connect (volume->held_op, "aborted", G_CALLBACK (watch_aborted), w);
        return volume;
}

static void
held_ask_password (FakeVolume *volume)
{
        g_signal_emit_by_name (volume->held_op, "ask-password", "Password?", "", "",
                               G_ASK_PASSWORD_NEED_PASSWORD);
}

static void
set_locked_hint (GsdAutomountManager *manager, gboolean locked)
{
        GVariantBuilder b;
        GVariant *changed;

        g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
        g_variant_builder_add (&b, "{sv}", "LockedHint", g_variant_new_boolean (locked));
        changed = g_variant_ref_sink (g_variant_builder_end (&b));
        login_session_properties_changed (NULL, changed, NULL, manager);
        g_variant_unref (changed);
}

static void
end_held_mount (GsdAutomountManager *manager, FakeVolume *volume)
{
        fake_volume_finish (volume, TRUE);
        run_pending ();
        g_object_unref (volume);
        manager_free (manager);
}

static void
test_prompt_refused_after_lock (void)
{
        GsdAutomountManager *manager = manager_new ();
        OpWatch w = { 0 };
        FakeVolume *volume = start_held_mount (manager, &w);

        set_locked_hint (manager, TRUE);
        held_ask_password (volume);
        g_assert_cmpuint (w.probes, ==, 0);
        g_assert_cmpuint (w.replies, ==, 1);
        g_assert_cmpint (w.last_result, ==, G_MOUNT_OPERATION_ABORTED);
        end_held_mount (manager, volume);
}

static void
test_prompt_withdrawn_when_locked (void)
{
        GsdAutomountManager *manager = manager_new ();
        OpWatch w = { 0 };
        FakeVolume *volume = start_held_mount (manager, &w);

        held_ask_password (volume);
        g_assert_cmpuint (w.probes, ==, 1);

        set_locked_hint (manager, TRUE);
        g_assert_cmpuint (w.aborted, ==, 1);
        g_assert_cmpuint (w.replies, ==, 1);
        g_assert_cmpint (w.last_result, ==, G_MOUNT_OPERATION_ABORTED);
        end_held_mount (manager, volume);
}

static void
test_prompt_withdrawn_when_inactive (void)
{
        GsdAutomountManager *manager = manager_new ();
        OpWatch w = { 0 };
        FakeVolume *volume = start_held_mount (manager, &w);
        GVariantBuilder b;
        GVariant *changed;

        held_ask_password (volume);
        g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
        g_variant_builder_add (&b, "{sv}", "SessionIsActive", g_variant_new_boolean (FALSE));
        changed = g_variant_ref_sink (g_variant_builder_end (&b));
        session_state_changed (NULL, changed, NULL, manager);
        g_variant_unref (changed);

        g_assert_cmpuint (w.aborted, ==, 1);
        g_assert_cmpuint (w.replies, ==, 1);
        end_held_mount (manager, volume);
}

static void
test_prompt_withdrawn_when_hint_invalidated (void)
{
        GsdAutomountManager *manager = manager_new ();
        OpWatch w = { 0 };
        FakeVolume *volume = start_held_mount (manager, &w);
        const char *invalidated[] = { "LockedHint", NULL };
        GVariant *changed;

        held_ask_password (volume);
        changed = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0));
        login_session_properties_changed (NULL, changed, (char **) invalidated, manager);
        g_variant_unref (changed);

        g_assert_cmpuint (w.aborted, ==, 1);
        g_assert_cmpuint (w.replies, ==, 1);
        end_held_mount (manager, volume);
}

static gboolean
set_flag (gpointer data)
{
        *(gboolean *) data = TRUE;
        return G_SOURCE_REMOVE;
}

/* The Unity session name (re)appears: its lock state is unknown again
 * until the proxy answers, so a prompt that is up is withdrawn. The
 * callback starts a proxy on the bus, hence a private test bus. */
static void
test_prompt_withdrawn_when_provider_appears (void)
{
        GTestDBus *bus = g_test_dbus_new (G_TEST_DBUS_NONE);
        GDBusConnection *connection;
        GsdAutomountManager *manager;
        OpWatch w = { 0 };
        FakeVolume *volume;
        gboolean done = FALSE;

        g_test_dbus_up (bus);
        connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
        g_assert_nonnull (connection);

        manager = manager_new ();
        volume = start_held_mount (manager, &w);
        held_ask_password (volume);
        g_assert_cmpuint (w.probes, ==, 1);

        unity_appeared_callback (connection, UNITY_NAME, ":1.99", manager);
        g_assert_cmpuint (w.aborted, ==, 1);
        g_assert_cmpuint (w.replies, ==, 1);
        g_assert_cmpint (w.last_result, ==, G_MOUNT_OPERATION_ABORTED);

        /* let the superseded proxy call finish before the manager goes */
        g_cancellable_cancel (manager->priv->us_cancellable);
        g_timeout_add (200, set_flag, &done);
        while (!done)
                g_main_context_iteration (NULL, TRUE);
        clear_provider_proxy (manager, &manager->priv->us_proxy);
        g_clear_object (&manager->priv->us_cancellable);

        end_held_mount (manager, volume);
        g_object_unref (connection);
        g_test_dbus_down (bus);
        g_object_unref (bus);
}

static guint
count_toplevels (void)
{
        GList *l = gtk_window_list_toplevels ();
        guint n = g_list_length (l);

        g_list_free (l);
        return n;
}

static void
test_no_error_dialog_while_locked (void)
{
        GsdAutomountManager *manager = manager_new ();
        OpWatch w = { 0 };
        FakeVolume *volume = start_held_mount (manager, &w);
        guint before = count_toplevels ();

        set_locked_hint (manager, TRUE);
        fake_volume_finish (volume, FALSE);
        run_pending ();
        g_assert_cmpuint (count_toplevels (), ==, before);

        g_object_unref (volume);
        manager_free (manager);
}

int
main (int argc, char **argv)
{
        /* the error-dialog case counts GTK toplevels; the package build runs
         * the tests under xvfb-run */
        gtk_init (&argc, &argv);
        g_test_init (&argc, &argv, NULL);
        /* the helper logs a refused dialog as a warning; keep warnings
         * non-fatal, criticals fatal */
        g_log_set_always_fatal (G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

        g_test_add_func ("/automount/predicate/fresh-manager", test_predicate_fresh_manager);
        g_test_add_func ("/automount/predicate/logind-not-read", test_predicate_logind_not_read);
        g_test_add_func ("/automount/predicate/known-unlocked", test_predicate_known_unlocked);
        g_test_add_func ("/automount/predicate/locked-hint", test_predicate_locked_hint);
        g_test_add_func ("/automount/predicate/watchers-not-reported", test_predicate_watchers_not_reported);
        g_test_add_func ("/automount/predicate/present-unknown", test_predicate_present_unknown);
        g_test_add_func ("/automount/startup/waits-for-state", test_startup_volume_waits_for_state);
        g_test_add_func ("/automount/queue/drains-each-once", test_queue_drains_each_volume_once);
        g_test_add_func ("/automount/autorun/mount-added-locked", test_autorun_mount_added_while_locked);
        g_test_add_func ("/automount/autorun/mount-added-unknown", test_autorun_mount_added_lock_state_unknown);
        g_test_add_func ("/automount/autorun/mount-added-unlocked", test_autorun_mount_added_unlocked);
        g_test_add_func ("/automount/autorun/mount-added-inactive", test_autorun_mount_added_inactive);
        g_test_add_func ("/automount/autorun/permission-locked", test_autorun_permission_not_given_while_locked);
        g_test_add_func ("/automount/autorun/permission-unlocked", test_autorun_permission_given_when_unlocked);
        g_test_add_func ("/automount/prompt/refused-after-lock", test_prompt_refused_after_lock);
        g_test_add_func ("/automount/prompt/withdrawn-when-locked", test_prompt_withdrawn_when_locked);
        g_test_add_func ("/automount/prompt/withdrawn-when-inactive", test_prompt_withdrawn_when_inactive);
        g_test_add_func ("/automount/prompt/withdrawn-when-hint-invalidated", test_prompt_withdrawn_when_hint_invalidated);
        g_test_add_func ("/automount/prompt/withdrawn-when-provider-appears", test_prompt_withdrawn_when_provider_appears);
        g_test_add_func ("/automount/dialog/no-error-while-locked", test_no_error_dialog_while_locked);

        return g_test_run ();
}
