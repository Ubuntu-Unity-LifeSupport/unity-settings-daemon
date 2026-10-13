/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2026 Ubuntu Unity Life Support
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "config.h"

#include "gsd-mount-guard.h"

/* GMountOperation's ask-password, ask-question and show-processes signals
 * are G_SIGNAL_RUN_LAST: a handler connected normally runs before the class
 * handler that shows the dialog (GtkMountOperation), so stopping the
 * emission there keeps the dialog from appearing. Replying
 * G_MOUNT_OPERATION_ABORTED is the cancel path of GIO (the backend fails
 * the operation with G_IO_ERROR_FAILED_HANDLED), and "aborted" is what makes
 * GtkMountOperation destroy a dialog that is already shown.
 *
 * At most one reply is sent per question: "pending" is set when a prompt
 * is let through and cleared by any reply (also the dialog's own OK or
 * Cancel) and by "aborted" (also the backend's own). */

typedef struct {
        GMountOperation      *op;
        GsdMountGuardAllowed  allowed_func;
        gpointer              user_data;
        gboolean              pending;
        gulong                handler_ids[5];
} GuardEntry;

static GList *entries;

static GuardEntry *
find_entry (GMountOperation *op)
{
        GList *l;

        for (l = entries; l != NULL; l = l->next) {
                GuardEntry *entry = l->data;

                if (entry->op == op)
                        return entry;
        }
        return NULL;
}

/* Withdraw the prompt that is up: the dialog goes away, the question gets
 * its one reply. "pending" is read first, since our own "aborted" handler
 * clears it. */
static void
withdraw (GuardEntry *entry)
{
        GMountOperation *op = g_object_ref (entry->op);
        gboolean pending = entry->pending;

        g_signal_emit_by_name (op, "aborted");
        if (pending)
                g_mount_operation_reply (op, G_MOUNT_OPERATION_ABORTED);
        g_object_unref (op);
}

static void
prompt_requested (GuardEntry *entry,
                  const char *signal_name)
{
        if (entry->allowed_func (entry->user_data)) {
                entry->pending = TRUE;
                return;
        }

        g_debug ("Mount operation %p: %s refused, the session is locked or inactive",
                 entry->op, signal_name);
        g_signal_stop_emission_by_name (entry->op, signal_name);
        if (entry->pending) {
                /* a prompt of this operation is still up (a repeated
                 * show-processes, say): withdraw it now, with its reply */
                withdraw (entry);
        } else {
                g_mount_operation_reply (entry->op, G_MOUNT_OPERATION_ABORTED);
        }
}

static void
on_ask_password (GMountOperation   *op,
                 const char        *message,
                 const char        *default_user,
                 const char        *default_domain,
                 GAskPasswordFlags  flags,
                 GuardEntry        *entry)
{
        prompt_requested (entry, "ask-password");
}

static void
on_ask_question (GMountOperation  *op,
                 const char       *message,
                 const char      **choices,
                 GuardEntry       *entry)
{
        prompt_requested (entry, "ask-question");
}

static void
on_show_processes (GMountOperation  *op,
                   const char       *message,
                   GArray           *processes,
                   const char      **choices,
                   GuardEntry       *entry)
{
        prompt_requested (entry, "show-processes");
}

static void
on_reply (GMountOperation       *op,
          GMountOperationResult  result,
          GuardEntry            *entry)
{
        entry->pending = FALSE;
}

static void
on_aborted (GMountOperation *op,
            GuardEntry      *entry)
{
        entry->pending = FALSE;
}

void
gsd_mount_guard_attach (GMountOperation      *op,
                        GsdMountGuardAllowed  allowed_func,
                        gpointer              user_data)
{
        GuardEntry *entry;

        g_return_if_fail (G_IS_MOUNT_OPERATION (op));
        g_return_if_fail (allowed_func != NULL);
        g_return_if_fail (find_entry (op) == NULL);

        entry = g_new0 (GuardEntry, 1);
        entry->op = g_object_ref (op);
        entry->allowed_func = allowed_func;
        entry->user_data = user_data;
        entry->handler_ids[0] = g_signal_connect (op, "ask-password",
                                                  G_CALLBACK (on_ask_password), entry);
        entry->handler_ids[1] = g_signal_connect (op, "ask-question",
                                                  G_CALLBACK (on_ask_question), entry);
        entry->handler_ids[2] = g_signal_connect (op, "show-processes",
                                                  G_CALLBACK (on_show_processes), entry);
        entry->handler_ids[3] = g_signal_connect (op, "reply",
                                                  G_CALLBACK (on_reply), entry);
        entry->handler_ids[4] = g_signal_connect (op, "aborted",
                                                  G_CALLBACK (on_aborted), entry);
        entries = g_list_prepend (entries, entry);
}

static void
entry_free (GuardEntry *entry)
{
        guint i;

        for (i = 0; i < G_N_ELEMENTS (entry->handler_ids); i++)
                g_signal_handler_disconnect (entry->op, entry->handler_ids[i]);
        g_object_unref (entry->op);
        g_free (entry);
}

/* Called when the operation has finished (before its owner drops it): a
 * prompt still up loses its dialog; nothing waits for a reply any more. */
void
gsd_mount_guard_detach (GMountOperation *op)
{
        GuardEntry *entry = find_entry (op);

        if (entry == NULL)
                return;
        entries = g_list_remove (entries, entry);
        if (entry->pending) {
                entry->pending = FALSE;
                g_signal_emit_by_name (entry->op, "aborted");
        }
        entry_free (entry);
}

/* The session was locked or left: withdraw every prompt that is up and no
 * longer allowed. */
void
gsd_mount_guard_withdraw_all (void)
{
        GList *copy, *l;

        copy = g_list_copy (entries);
        for (l = copy; l != NULL; l = l->next) {
                GuardEntry *entry = l->data;

                /* an earlier withdraw may have detached it */
                if (g_list_find (entries, entry) == NULL)
                        continue;
                if (entry->pending && !entry->allowed_func (entry->user_data))
                        withdraw (entry);
        }
        g_list_free (copy);
}

/* The owner goes away: withdraw its prompts and forget its operations, so
 * allowed_func is never called with a stale user_data. */
void
gsd_mount_guard_detach_all_for (gpointer user_data)
{
        GList *copy, *l;

        copy = g_list_copy (entries);
        for (l = copy; l != NULL; l = l->next) {
                GuardEntry *entry = l->data;
                GMountOperation *op;

                if (g_list_find (entries, entry) == NULL || entry->user_data != user_data)
                        continue;
                /* hold the operation while it is withdrawn and detached */
                op = g_object_ref (entry->op);
                if (entry->pending)
                        withdraw (entry);
                gsd_mount_guard_detach (op);
                g_object_unref (op);
        }
        g_list_free (copy);
}
