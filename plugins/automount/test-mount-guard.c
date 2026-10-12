/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Tests for gsd-mount-guard.c with a plain GMountOperation: prompts are let
 * through only while allowed, a refused or withdrawn prompt gets exactly
 * one reply, and a prompt that is up when the session stops being allowed
 * is withdrawn ("aborted", then the reply).
 *
 * An after-handler ("probe") stands for the dialog: GtkMountOperation shows
 * its dialog from the RUN_LAST class handler, which an emission stopped by
 * the guard never reaches either. The main loop is not run, so
 * GMountOperation's own "unhandled" reply (scheduled in an idle by its
 * class handler) does not interfere.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "config.h"

#include <gio/gio.h>

#include "gsd-mount-guard.h"

typedef struct {
        gboolean allowed;
        guint probes;
        guint replies;
        guint aborted;
        GMountOperationResult last_result;
} Fixture;

static gboolean
allowed_cb (gpointer user_data)
{
        return ((Fixture *) user_data)->allowed;
}

static void
probe_password (GMountOperation *op, const char *m, const char *u, const char *d,
                GAskPasswordFlags f, Fixture *fx)
{
        fx->probes++;
}

static void
probe_processes (GMountOperation *op, const char *m, GArray *p, const char **c, Fixture *fx)
{
        fx->probes++;
}

static void
count_reply (GMountOperation *op, GMountOperationResult result, Fixture *fx)
{
        fx->replies++;
        fx->last_result = result;
}

static void
count_aborted (GMountOperation *op, Fixture *fx)
{
        fx->aborted++;
}

static GMountOperation *
op_new (Fixture *fx)
{
        GMountOperation *op = g_mount_operation_new ();

        g_signal_connect_after (op, "ask-password", G_CALLBACK (probe_password), fx);
        g_signal_connect_after (op, "show-processes", G_CALLBACK (probe_processes), fx);
        /* connected before the guard attaches: sees every reply/abort */
        g_signal_connect (op, "reply", G_CALLBACK (count_reply), fx);
        g_signal_connect (op, "aborted", G_CALLBACK (count_aborted), fx);
        gsd_mount_guard_attach (op, allowed_cb, fx);
        return op;
}

static void
ask_password (GMountOperation *op)
{
        g_signal_emit_by_name (op, "ask-password", "Password?", "user", "domain",
                               G_ASK_PASSWORD_NEED_PASSWORD);
}

static void
show_processes (GMountOperation *op)
{
        GArray *processes = g_array_new (FALSE, FALSE, sizeof (GPid));
        const char *choices[] = { "Unmount Anyway", "Cancel", NULL };
        GPid pid = 1;

        g_array_append_val (processes, pid);
        g_signal_emit_by_name (op, "show-processes", "Busy", processes, choices);
        g_array_unref (processes);
}

static void
op_free (GMountOperation *op)
{
        gsd_mount_guard_detach (op);
        g_object_unref (op);
}

static void
test_refused_when_not_allowed (void)
{
        Fixture fx = { FALSE };
        GMountOperation *op = op_new (&fx);

        ask_password (op);
        g_assert_cmpuint (fx.probes, ==, 0);
        g_assert_cmpuint (fx.replies, ==, 1);
        g_assert_cmpint (fx.last_result, ==, G_MOUNT_OPERATION_ABORTED);
        op_free (op);
}

static void
test_let_through_when_allowed (void)
{
        Fixture fx = { TRUE };
        GMountOperation *op = op_new (&fx);

        ask_password (op);
        g_assert_cmpuint (fx.probes, ==, 1);
        g_assert_cmpuint (fx.replies, ==, 0);
        op_free (op);
}

static void
test_withdrawn_on_transition (void)
{
        Fixture fx = { TRUE };
        GMountOperation *op = op_new (&fx);

        ask_password (op);
        g_assert_cmpuint (fx.probes, ==, 1);

        fx.allowed = FALSE;
        gsd_mount_guard_withdraw_all ();
        g_assert_cmpuint (fx.aborted, ==, 1);
        g_assert_cmpuint (fx.replies, ==, 1);
        g_assert_cmpint (fx.last_result, ==, G_MOUNT_OPERATION_ABORTED);

        /* nothing left to withdraw */
        gsd_mount_guard_withdraw_all ();
        g_assert_cmpuint (fx.aborted, ==, 1);
        g_assert_cmpuint (fx.replies, ==, 1);
        op_free (op);
}

static void
test_no_second_reply_after_cancel (void)
{
        Fixture fx = { TRUE };
        GMountOperation *op = op_new (&fx);

        ask_password (op);
        /* the user answers the dialog (Cancel) */
        g_mount_operation_reply (op, G_MOUNT_OPERATION_ABORTED);
        g_assert_cmpuint (fx.replies, ==, 1);

        fx.allowed = FALSE;
        gsd_mount_guard_withdraw_all ();
        g_assert_cmpuint (fx.replies, ==, 1);
        g_assert_cmpuint (fx.aborted, ==, 0);
        op_free (op);
}

static void
test_repeat_prompt_after_lock_withdraws (void)
{
        Fixture fx = { TRUE };
        GMountOperation *op = op_new (&fx);

        show_processes (op);
        g_assert_cmpuint (fx.probes, ==, 1);

        /* the session locks; the backend repeats show-processes before the
         * lock hook runs */
        fx.allowed = FALSE;
        show_processes (op);
        g_assert_cmpuint (fx.probes, ==, 1);
        g_assert_cmpuint (fx.aborted, ==, 1);
        g_assert_cmpuint (fx.replies, ==, 1);
        g_assert_cmpint (fx.last_result, ==, G_MOUNT_OPERATION_ABORTED);
        op_free (op);
}

static void
test_detach_while_pending (void)
{
        Fixture fx = { TRUE };
        GMountOperation *op = op_new (&fx);

        ask_password (op);
        gsd_mount_guard_detach (op);
        /* the dialog goes, no reply is sent for a finished operation */
        g_assert_cmpuint (fx.aborted, ==, 1);
        g_assert_cmpuint (fx.replies, ==, 0);

        /* no guard any more: a later emission is not stopped */
        fx.allowed = FALSE;
        ask_password (op);
        g_assert_cmpuint (fx.probes, ==, 2);
        g_assert_cmpuint (fx.replies, ==, 0);
        g_object_unref (op);
}

static void
test_detach_all_for_owner (void)
{
        Fixture mine = { TRUE };
        Fixture other = { TRUE };
        GMountOperation *op1 = op_new (&mine);
        GMountOperation *op2 = op_new (&other);

        ask_password (op1);
        ask_password (op2);

        gsd_mount_guard_detach_all_for (&mine);
        g_assert_cmpuint (mine.aborted, ==, 1);
        g_assert_cmpuint (mine.replies, ==, 1);
        g_assert_cmpuint (other.aborted, ==, 0);

        /* op1 is no longer guarded, op2 still is */
        other.allowed = FALSE;
        gsd_mount_guard_withdraw_all ();
        g_assert_cmpuint (other.aborted, ==, 1);
        g_assert_cmpuint (mine.aborted, ==, 1);

        g_object_unref (op1);
        op_free (op2);
}

int
main (int argc, char **argv)
{
        g_test_init (&argc, &argv, NULL);

        g_test_add_func ("/mount-guard/refused-when-not-allowed", test_refused_when_not_allowed);
        g_test_add_func ("/mount-guard/let-through-when-allowed", test_let_through_when_allowed);
        g_test_add_func ("/mount-guard/withdrawn-on-transition", test_withdrawn_on_transition);
        g_test_add_func ("/mount-guard/no-second-reply-after-cancel", test_no_second_reply_after_cancel);
        g_test_add_func ("/mount-guard/repeat-prompt-after-lock", test_repeat_prompt_after_lock_withdraws);
        g_test_add_func ("/mount-guard/detach-while-pending", test_detach_while_pending);
        g_test_add_func ("/mount-guard/detach-all-for-owner", test_detach_all_for_owner);

        return g_test_run ();
}
