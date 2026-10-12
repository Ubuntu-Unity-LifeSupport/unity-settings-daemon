/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Tests for gsd_autorun() (the real gsd-autorun.c): whether autorun is
 * still allowed is asked again when the content type of the mount is
 * known, right before anything is launched or shown. The fake mount below
 * completes its content-type guess only when the test says so, so the
 * test can change the answer in between.
 *
 * The fixture reaches the "open folder" action: autorun-never is false,
 * automount-open is true (memory settings backend), the volume carries the
 * autorun permission, the mount root is a local, non-hidden path and the
 * guess finds no content type.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "config.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include "gsd-autorun.h"

/* --- a fake volume ------------------------------------------------------ */

typedef struct {
        GObject parent;
} FakeVolume;

typedef struct {
        GObjectClass parent_class;
} FakeVolumeClass;

static GType fake_volume_get_type (void);
static void fake_volume_iface_init (GVolumeIface *iface);

G_DEFINE_TYPE_WITH_CODE (FakeVolume, fake_volume, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME, fake_volume_iface_init))

static void
fake_volume_class_init (FakeVolumeClass *klass)
{
}

static void
fake_volume_init (FakeVolume *volume)
{
}

static gboolean
fake_volume_should_automount (GVolume *volume)
{
        return TRUE;
}

static void
fake_volume_iface_init (GVolumeIface *iface)
{
        iface->should_automount = fake_volume_should_automount;
}

/* --- a fake mount whose content-type guess completes on request ---------- */

typedef struct {
        GObject parent;
        GVolume *volume;
        GTask *pending_guess;
        const char *content_type;   /* the guess result; NULL = none found */
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
        FakeMount *mount = (FakeMount *) object;

        g_clear_object (&mount->volume);
        g_clear_object (&mount->pending_guess);
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
        return g_object_ref (((FakeMount *) mount)->volume);
}

static GFile *
fake_mount_get_root (GMount *mount)
{
        return g_file_new_for_path ("/media/fake-medium");
}

static char *
fake_mount_get_name (GMount *mount)
{
        return g_strdup ("fake medium");
}

static void
fake_mount_guess_content_type (GMount              *mount,
                               gboolean             force_rescan,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
        FakeMount *fake = (FakeMount *) mount;

        g_assert_null (fake->pending_guess);
        fake->pending_guess = g_task_new (mount, cancellable, callback, user_data);
}

static char **
fake_mount_guess_content_type_finish (GMount        *mount,
                                      GAsyncResult  *result,
                                      GError       **error)
{
        return g_task_propagate_pointer (G_TASK (result), error);
}

static GIcon *
fake_mount_get_icon (GMount *mount)
{
        return g_themed_icon_new ("media-optical");
}

/* g_mount_can_eject() calls this without a NULL check; the autorun
 * dialog asks it to choose between its Eject and Unmount buttons. */
static gboolean
fake_mount_can_eject (GMount *mount)
{
        return FALSE;
}

static void
fake_mount_iface_init (GMountIface *iface)
{
        iface->can_eject = fake_mount_can_eject;
        iface->get_icon = fake_mount_get_icon;
        iface->get_volume = fake_mount_get_volume;
        iface->get_root = fake_mount_get_root;
        iface->get_name = fake_mount_get_name;
        iface->guess_content_type = fake_mount_guess_content_type;
        iface->guess_content_type_finish = fake_mount_guess_content_type_finish;
}

/* Complete the pending guess with the mount's content type, if any. */
static void
fake_mount_complete_guess (FakeMount *mount)
{
        GTask *task = mount->pending_guess;
        char **types = g_new0 (char *, 2);

        g_assert_nonnull (task);
        mount->pending_guess = NULL;
        types[0] = g_strdup (mount->content_type);
        g_task_return_pointer (task, types, (GDestroyNotify) g_strfreev);
        g_object_unref (task);
}

/* --- the test --------------------------------------------------------------- */

typedef struct {
        gboolean allowed;
        guint opened;
} Recorder;

static gboolean
allowed_cb (gpointer user_data)
{
        return ((Recorder *) user_data)->allowed;
}

static void
open_window_cb (GMount *mount, gpointer user_data)
{
        ((Recorder *) user_data)->opened++;
}

static void
run_pending (void)
{
        while (g_main_context_iteration (NULL, FALSE))
                ;
}

static void
run_case (gboolean allowed_at_completion, guint expected_opened)
{
        GSettings *settings;
        FakeMount *mount;
        Recorder recorder = { TRUE, 0 };

        settings = g_settings_new ("org.gnome.desktop.media-handling");
        g_settings_set_boolean (settings, "autorun-never", FALSE);
        g_settings_set_boolean (settings, "automount-open", TRUE);

        mount = g_object_new (fake_mount_get_type (), NULL);
        mount->volume = g_object_new (fake_volume_get_type (), NULL);
        gsd_allow_autorun_for_volume (mount->volume);

        /* allowed when the mount appears; the guess is pending */
        gsd_autorun (G_MOUNT (mount), settings, allowed_cb, open_window_cb, &recorder);
        g_assert_nonnull (mount->pending_guess);

        recorder.allowed = allowed_at_completion;
        fake_mount_complete_guess (mount);
        run_pending ();

        g_assert_cmpuint (recorder.opened, ==, expected_opened);
        /* the content type is cached either way */
        g_assert_nonnull (g_object_get_data (G_OBJECT (mount), "gsd-content-type-cache"));

        g_object_unref (mount);
        g_object_unref (settings);
}

static void
test_autorun_allowed_at_completion (void)
{
        run_case (TRUE, 1);
}

static void
test_autorun_refused_at_completion (void)
{
        /* the session was locked while the content type was guessed */
        run_case (FALSE, 0);
}

static guint
count_visible_dialogs (void)
{
        GList *l, *toplevels = gtk_window_list_toplevels ();
        guint n = 0;

        for (l = toplevels; l != NULL; l = l->next)
                if (GTK_IS_DIALOG (l->data) && gtk_widget_get_visible (l->data))
                        n++;
        g_list_free (toplevels);
        return n;
}

/* An autorun dialog that is open when the session locks is closed. */
static void
test_autorun_dialog_closed (void)
{
        GSettings *settings;
        FakeMount *mount;
        Recorder recorder = { TRUE, 0 };
        guint before;

        settings = g_settings_new ("org.gnome.desktop.media-handling");
        g_settings_set_boolean (settings, "autorun-never", FALSE);

        mount = g_object_new (fake_mount_get_type (), NULL);
        mount->volume = g_object_new (fake_volume_get_type (), NULL);
        /* a content type with no remembered choice: the dialog asks */
        mount->content_type = "x-content/audio-cdda";
        gsd_allow_autorun_for_volume (mount->volume);

        before = count_visible_dialogs ();
        gsd_autorun (G_MOUNT (mount), settings, allowed_cb, open_window_cb, &recorder);
        fake_mount_complete_guess (mount);
        run_pending ();
        g_assert_cmpuint (count_visible_dialogs (), ==, before + 1);

        gsd_autorun_close_dialogs ();
        run_pending ();
        g_assert_cmpuint (count_visible_dialogs (), ==, before);
        g_assert_cmpuint (recorder.opened, ==, 0);

        g_object_unref (mount);
        g_object_unref (settings);
}

int
main (int argc, char **argv)
{
        gtk_init (&argc, &argv);
        g_test_init (&argc, &argv, NULL);

        g_test_add_func ("/autorun/allowed-at-completion", test_autorun_allowed_at_completion);
        g_test_add_func ("/autorun/refused-at-completion", test_autorun_refused_at_completion);
        g_test_add_func ("/autorun/dialog-closed", test_autorun_dialog_closed);

        return g_test_run ();
}
