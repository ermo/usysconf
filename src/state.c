/*
 * This file is part of usysconf.
 *
 * Copyright © 2017-2018 Solus Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "files.h"
#include "state.h"

DEF_AUTOFREE(FILE, fclose)

/**
 * Each entry is just a list node with a ptr (registered interest) and
 * an mtime for when it was last modified on disk
 */
typedef struct UscStateEntry {
        struct UscStateEntry *next; /**<Next guy in the chain */
        char *ptr;                  /**<Registered interest */
        time_t mtime;               /**<Last modified stamp */
} UscStateEntry;

/**
 * Opaque implementation details.
 */
struct UscStateTracker {
        const char *state_file;
        UscStateEntry *entry; /**<Root entry in the list */
};

UscStateTracker *usc_state_tracker_new(void)
{
        UscStateTracker *ret = NULL;

        ret = calloc(1, sizeof(UscStateTracker));
        if (!ret) {
                return NULL;
        }
        ret->state_file = USYSCONF_STATUS_FILE;
        return ret;
}

/**
 * Lookup the path within the internal list and find any matching node for it.
 *
 * This may return NULL.
 */
static UscStateEntry *usc_state_tracker_lookup(UscStateTracker *self, const char *path)
{
        for (UscStateEntry *entry = self->entry; entry; entry = entry->next) {
                if (entry->ptr && strcmp(entry->ptr, path) == 0) {
                        return entry;
                }
        }
        return NULL;
}

/**
 * Internal logic for pushing or updating an existing path.
 */
static bool usc_state_tracker_put_entry(UscStateTracker *self, char *path, time_t mtime)
{
        UscStateEntry *entry = NULL;

        /* Does this entry already exist */
        entry = usc_state_tracker_lookup(self, path);
        if (entry) {
                entry->mtime = mtime;
                return true;
        }

        /* Must insert a new entry now */
        entry = calloc(1, sizeof(UscStateEntry));
        if (!entry) {
                fputs("OOM\n", stderr);
                return false;
        }
        entry->ptr = strdup(path);
        if (!entry->ptr) {
                fputs("OOM\n", stderr);
                free(entry);
                return false;
        }

        /* Merge list reversed */
        entry->mtime = mtime;
        entry->next = self->entry;
        self->entry = entry;
        return true;
}

bool usc_state_tracker_push_path(UscStateTracker *self, const char *path)
{
        struct stat st = { 0 };

        autofree(char) *dup = NULL;

        /* Make sure it actually exists */
        dup = realpath(path, NULL);
        if (!dup) {
                return false;
        }

        if (stat(dup, &st) != 0) {
                return false;
        }

        return usc_state_tracker_put_entry(self, dup, st.st_mtime);
}

static void usc_state_entry_free(UscStateEntry *entry)
{
        if (!entry) {
                return;
        }
        /* Free next dude in the chain */
        usc_state_entry_free(entry->next);
        free(entry->ptr);
        free(entry);
}

void usc_state_tracker_free(UscStateTracker *self)
{
        if (!self) {
                return;
        }
        usc_state_entry_free(self->entry);
        free(self);
}

bool usc_state_tracker_write(UscStateTracker *self)
{
        autofree(FILE) *fp = NULL;

        if (!usc_file_exists(USYSCONF_TRACK_DIR) && mkdir(USYSCONF_TRACK_DIR, 00755) != 0) {
                fprintf(stderr, "mkdir(): %s %s\n", USYSCONF_TRACK_DIR, strerror(errno));
                return false;
        }

        fp = fopen(self->state_file, "w");
        if (!fp) {
                fprintf(stderr, "fopen(): %s %s\n", self->state_file, strerror(errno));
                return false;
        }

        fprintf(fp, "# This file is automatically generated. DO NOT EDIT\n");

        /* Walk nodes */
        for (UscStateEntry *entry = self->entry; entry; entry = entry->next) {
                /* Drop stale entries here */
                if (!entry->ptr || !usc_file_exists(entry->ptr)) {
                        continue;
                }
                if (fprintf(fp, "%ld:%s\n", entry->mtime, entry->ptr) < 0) {
                        fprintf(stderr, "fprintf(): %s %s\n", self->state_file, strerror(errno));
                        return false;
                }
        }

        return true;
}

bool usc_state_tracker_load(UscStateTracker *self)
{
        autofree(FILE) *fp = NULL;
        char *bfr = NULL;
        size_t n = 0;
        ssize_t read = 0;

        fp = fopen(self->state_file, "r");
        if (!fp) {
                /* If it doesn't exist, *meh* */
                if (errno == ENOENT) {
                        errno = 0;
                        return true;
                }
                fprintf(stderr,
                        "Failed to load state file %s: %s\n",
                        self->state_file,
                        strerror(errno));
                return false;
        }

        errno = 0;

        /* Walk the line. */
        while ((read = getline(&bfr, &n, fp)) > 0) {
                char *c = NULL;
                char *part = NULL;
                autofree(char) *snd = NULL;
                time_t t = 0;

                if (read < 1) {
                        continue;
                }
                /* Strip the newline from it */
                if (bfr[read - 1] == '\n') {
                        bfr[read - 1] = '\0';
                        --read;
                }

                /* Skip comments */
                if (*bfr == '#') {
                        continue;
                }

                c = memchr(bfr, ':', (size_t)read);
                if (!c) {
                        fprintf(stderr, "Erronous line in input misses colon: '%s'\n", bfr);
                        errno = EINVAL;
                        break;
                }

                if (c - bfr < 1) {
                        fprintf(stderr, "Missing filename in line: '%s'\n", bfr);
                        errno = EINVAL;
                        break;
                }

                snd = strdup(c + 1);

                /* Now break the string here at the colon */
                bfr[c - bfr] = '\0';

                t = strtoll(bfr, &part, 10);
                if (!part) {
                        fprintf(stderr, "Invalid timestamp '%s'\n", bfr);
                        errno = EINVAL;
                        break;
                }

                /* Drop old cache entries */
                if (!usc_file_exists(snd)) {
                        errno = 0;
                        goto next;
                }

                /* Ensure we can actually push this guy. */
                if (!usc_state_tracker_put_entry(self, snd, t)) {
                        fprintf(stderr, "Failed to push entry: %s\n", snd);
                        errno = EINVAL;
                        break;
                }

        next:
                free(bfr);
                bfr = NULL;
        }

        if (bfr) {
                free(bfr);
        }

        /* Janky ready. */
        if (errno != 0) {
                fprintf(stderr,
                        "Failed to parse state: %s %s\n",
                        self->state_file,
                        strerror(errno));
                usc_state_entry_free(self->entry);
                self->entry = NULL;
                return false;
        }

        /* Successfully loaded */
        return true;
}

bool usc_state_tracker_needs_update(UscStateTracker *self, const char *path, bool force)
{
        time_t mtime = 0;
        UscStateEntry *entry = NULL;
        autofree(char) *real = NULL;

        real = realpath(path, NULL);
        if (!real) {
                return false;
        }

        /* Don't know about this guy? Needs an update */
        entry = usc_state_tracker_lookup(self, real);
        if (!entry) {
                return true;
        }

        /* Some fs bork, do it anyway */
        if (!usc_file_mtime(real, &mtime)) {
                return true;
        }

        if (force) {
                return true;
        }

        /* If our record mtime is older than the current mtime, update it */
        if (entry->mtime < mtime) {
                return true;
        }

        /* Nothing to be done here.. */
        return false;
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 8
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=8 tabstop=8 expandtab:
 * :indentSize=8:tabSize=8:noTabs=true:
 */
