/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2026 Ubuntu Unity Life Support
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GSD_MOUNT_GUARD_H__
#define __GSD_MOUNT_GUARD_H__

#include <gio/gio.h>

/* Keeps the prompts of the helper's mount operations (password, question,
 * processes using the volume) tied to the session state: a prompt is only
 * let through while allowed_func says yes, and a prompt that is up when it
 * says no is withdrawn. */

typedef gboolean (*GsdMountGuardAllowed) (gpointer user_data);

void gsd_mount_guard_attach         (GMountOperation      *op,
                                     GsdMountGuardAllowed  allowed_func,
                                     gpointer              user_data);
void gsd_mount_guard_detach         (GMountOperation      *op);
void gsd_mount_guard_withdraw_all   (void);
void gsd_mount_guard_detach_all_for (gpointer              user_data);

#endif /* __GSD_MOUNT_GUARD_H__ */
