/**
 * kalu - Copyright (C) 2012-2018 Olivier Brunel
 *
 * kalu-alpm.c
 * Copyright (C) 2012-2017 Olivier Brunel <jjk@jjacky.com>
 *
 * This file is part of kalu.
 *
 * kalu is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * kalu is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * kalu. If not, see http://www.gnu.org/licenses/
 */

#include <config.h>

/* C */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <utime.h>

/* alpm */
#include <alpm.h>
#include <alpm_list.h>

/* glib */
#include <glib-2.0/glib.h>

/* kalu */
#include "kalu.h"
#include "kalu-alpm.h"
#include "util.h"
#include "conf.h"

/* global variable */
unsigned short alpm_verbose;


static kalu_alpm_t *alpm;
static gchar *tmp_dbpath = NULL;
static gboolean is_tmp_dbpath_set = FALSE;

static gboolean copy_file (const gchar *from, const gchar *to);
static gboolean create_local_db (const gchar *dbpath, gchar **newpath,
        GString **_synced_dbs, GError **error);



static gboolean
copy_file (const gchar *from, const gchar *to)
{
    gchar *contents;
    gsize  length;

    debug ("copying %s to %s", from, to);

    if (!g_file_get_contents (from, &contents, &length, NULL))
    {
        debug ("cannot read %s", from);
        return FALSE;
    }

    if (!g_file_set_contents (to, contents, (gssize) length, NULL))
    {
        debug ("cannot write %s", to);
        g_free (contents);
        return FALSE;
    }

    debug ("..done");
    g_free (contents);
    return TRUE;
}

static gboolean
create_local_db (const gchar *_dbpath, gchar **newpath, GString **_synced_dbs, GError **error)
{
    gchar    buf[PATH_MAX];
    gchar    buf2[PATH_MAX];
    gchar    buf_dbpath[PATH_MAX];
    gchar   *dbpath = NULL;
    size_t   l = 0;
    gchar   *folder;
    GDir    *dir;
    const gchar    *file;
    struct stat     filestat;
    struct utimbuf  times;
    gboolean create_tmpdir = TRUE;

    if (tmp_dbpath)
    {
        debug ("checking local db %s", tmp_dbpath);

        if (stat (tmp_dbpath, &filestat) != 0)
        {
            if (errno != ENOENT)
            {
                g_set_error (error, KALU_ERROR, 1, _("Failed to stat %s: %s"),
                        tmp_dbpath, strerror (errno));
                return FALSE;
            }
            debug ("..doesn't exist");
        }
        else
        {
            if (S_ISDIR (filestat.st_mode))
            {
                ssize_t len;

                debug ("..folder found, getting dbpath");
                if (snprintf (buf, PATH_MAX, "%s/local", tmp_dbpath) >= PATH_MAX)
                {
                    g_set_error (error, KALU_ERROR, 1, _("Internal error: Path too long"));
                    return FALSE;
                }
                len = readlink (buf, buf_dbpath, PATH_MAX - 1);
                if (len > 6)
                {
                    buf_dbpath[len] = '\0';
                    if (!streq (buf_dbpath + len - 6, "/local"))
                    {
                        debug ("symlink local invalid (%s)", buf_dbpath);
                        len = -1;
                    }
                    else
                    {
                        gboolean same;

                        len -= 6;
                        buf_dbpath[len] = '\0';
                        l = strlen (_dbpath) - 1;
                        if (_dbpath[l] == '/')
                            same = (size_t) len == l && streqn (buf_dbpath, _dbpath, l);
                        else
                            same = streq (buf_dbpath, _dbpath);
                        if (same)
                        {
                            debug ("same dbpath (%s), re-using", buf_dbpath);
                            create_tmpdir = FALSE;
                        }
                        else
                        {
                            debug ("different dbpath (%s vs %s)",
                                    buf_dbpath, _dbpath);
                            len = -1;
                        }
                    }
                }
                else
                {
                    debug ("symlink 'local' not found or invalid");
                    len = -1;
                }

                if (len < 0)
                {
                    debug ("removing tmp_dbpath (%s)", tmp_dbpath);
                    rmrf (tmp_dbpath);
                }
            }
            else
            {
                debug ("..not a folder");
            }
        }

        if (create_tmpdir)
        {
            /* reset_kalpm_synced_dbs */
            if (_synced_dbs && *_synced_dbs)
                (*_synced_dbs)->len = 0;
        }
    }

    if (create_tmpdir)
    {
        debug ("creating local db");

        if (is_tmp_dbpath_set)
        {
            if (mkdir (tmp_dbpath, 0700) < 0)
            {
                debug ("mkdir failed: %s", strerror (errno));
                g_set_error (error, KALU_ERROR, 1, _("Unable to create temp folder"));
                return FALSE;
            }
            folder = tmp_dbpath;
        }
        else
        {
            /* create folder in tmp dir */
            if (NULL == (folder = g_dir_make_tmp ("kalu-XXXXXX", NULL)))
            {
                g_set_error (error, KALU_ERROR, 1, _("Unable to create temp folder"));
                return FALSE;
            }
            debug ("created tmp folder %s", folder);
        }

        /* dbpath will not be slash-terminated */
        l = strlen (_dbpath);
        if (_dbpath[l - 1] == '/')
            --l;
        if (l >= PATH_MAX)
        {
            g_set_error (error, KALU_ERROR, 1, _("Internal error: Path too long"));
            goto error;
        }
        memcpy (buf_dbpath, _dbpath, l);
        buf_dbpath[l] = '\0';
        dbpath = buf_dbpath;

        /* symlink local */
        if (snprintf (buf, PATH_MAX, "%s/local", dbpath) >= PATH_MAX
                || snprintf (buf2, PATH_MAX, "%s/local", folder) >= PATH_MAX)
        {
            g_set_error (error, KALU_ERROR, 1, _("Internal error: Path too long"));
            goto error;
        }
        if (0 != symlink (buf, buf2))
        {
            g_set_error (error, KALU_ERROR, 1,
                    _("Unable to create symlink %s"),
                    buf2);
            goto error;
        }
        debug ("created symlink %s", buf2);

        /* copy databases in sync */
        if (snprintf (buf, PATH_MAX, "%s/sync", folder) >= PATH_MAX)
        {
            g_set_error (error, KALU_ERROR, 1, _("Internal error: Path too long"));
            goto error;
        }
        if (0 != mkdir (buf, 0700))
        {
            g_set_error (error, KALU_ERROR, 1,
                    _("Unable to create folder %s"),
                    buf);
            goto error;
        }
        debug ("created folder %s", buf);
    }
    else
    {
        /* re-using tmp folder; we assume local symlinks and sync folders have
         * already been taken care of last time, as they should */
        folder = tmp_dbpath;
        dbpath = buf_dbpath;
    }

    if (snprintf (buf, PATH_MAX, "%s/sync", dbpath) >= PATH_MAX)
    {
        g_set_error (error, KALU_ERROR, 1, _("Internal error: Path too long"));
        goto error;
    }
    if (NULL == (dir = g_dir_open (buf, 0, NULL)))
    {
        g_set_error (error, KALU_ERROR, 1,
                _("Unable to open folder %s"),
                buf);
        goto error;
    }

    while ((file = g_dir_read_name (dir)))
    {
        l = (size_t) snprintf (buf, PATH_MAX, "%s/sync/%s", dbpath, file);
        if (l >= PATH_MAX)
        {
            g_set_error (error, KALU_ERROR, 1, _("Internal error"));
            goto error;
        }

        /* stat so we copy files only. also, we need to preserve modified date,
         * used to determine if DBs are up to date or not by libalpm */
        if (0 == stat (buf, &filestat))
        {
            if (S_ISREG (filestat.st_mode))
            {
                gboolean do_copy = TRUE;
                int l2 = 0;

                for (;;)
                {
                    struct stat st2;

                    /* when re-using tmpdir, for DBs we have some special
                     * handling to see whether to preserve existing file or not */
                    if (create_tmpdir ||
                            (!streq (buf + l - 3, ".db")
                             && !streq (buf +l - 7, ".db.sig")))
                    {
                        break;
                    }

                    /* construct name of our timestamp file for that db */
                    l2 = snprintf (buf2, PATH_MAX, "%s/sync/%s.ts", folder, file);
                    if (l2 >= PATH_MAX)
                    {
                        g_set_error (error, KALU_ERROR, 1, _("Internal error: Path too long"));
                        goto error;
                    }

                    /* first see if we already have that db */
                    buf2[l2 - 3] = '\0';
                    if (stat (buf2, &st2) < 0)
                    {
                        break;
                    }
                    /* then check there's a timestamp file */
                    buf2[l2 - 3] = '.';
                    if (stat (buf2, &st2) < 0)
                    {
                        break;
                    }
                    /* check if DB was modified since last time */
                    if (filestat.st_mtime != st2.st_mtime)
                    {
                        break;
                    }

                    /* we already have a DB, with a timestamp file indicating it
                     * hasn't changed since last time, so we'll keep our
                     * version (which might have been sync-d since) */
                    do_copy = FALSE;
                    break;
                }

                if (do_copy)
                {
                    if (l2 == 0)
                    {
                        l2 = snprintf (buf2, PATH_MAX, "%s/sync/%s.ts", folder, file);
                        if (l2 >= PATH_MAX)
                        {
                            g_set_error (error, KALU_ERROR, 1,
                                    _("Internal error: Path too long"));
                            goto error;
                        }
                    }
                    buf2[l2 - 3] = '\0';
                    if (!copy_file (buf, buf2))
                    {
                        g_set_error (error, KALU_ERROR, 1,
                                _("Copy failed for %s"),
                                buf);
                        g_dir_close (dir);
                        goto error;
                    }
                    if (streq (buf + l - 3, ".db") || streq (buf + l - 7, ".db.sig"))
                    {
                        gint fd;

                        /* preserve time */
                        times.actime = filestat.st_atime;
                        times.modtime = filestat.st_mtime;
                        if (0 != utime (buf2, &times))
                        {
                            /* sucks, but no fail, we'll just have to download this db */
                            debug ("Unable to change time of %s", buf2);
                        }
                        else
                        {
                            debug ("updated time for %s", buf2);
                        }
                        /* create our timestamp file */
                        buf2[l2 - 3] = '.';
                        do
                            fd = open (buf2, O_RDONLY | O_CREAT, 0644);
                        while (fd < 0 && errno == EINTR);
                        if (fd >= 0)
                        {
                            close (fd);
                        }
                        /* set time on our timestamp file */
                        if (0 != utime (buf2, &times))
                        {
                            /* sucks, but no fail, we'll just have to download this db */
                            debug ("Unable to change time of %s", buf2);
                        }
                        else
                        {
                            debug ("updated time for %s", buf2);
                        }

                        /* if this was a synced db, reset it */
                        if (_synced_dbs && *_synced_dbs && (*_synced_dbs)->len > 0
                                /* i.e. it's a file.db (not file.db.sig) */
                                && buf[l - 1] == 'b')
                        {
                            GString *str = *_synced_dbs;
                            size_t len = strlen (file) - 3;
                            gchar *dbname = buf + l - len - 3;
                            size_t j;

                            buf[l - 3] = '\0';
                            for (j = 0; j <= str->len; j += strlen (str->str + j) + 1)
                                if (streq (dbname, str->str + j))
                                {
                                    g_string_erase (str, (gssize) j, (gssize) len + 1);
                                    break;
                                }
                        }
                    }
                }
                else
                {
                    buf2[l2 - 3] = '\0';
                    debug ("keeping current %s", buf2);
                }
            }
            else
            {
                debug ("ignoring non-regular file: %s", buf);
            }
        }
        else
        {
            g_set_error (error, KALU_ERROR, 1, _("Unable to stat %s\n"), buf);
            g_dir_close (dir);
            goto error;
        }
    }
    g_dir_close (dir);

    if (create_tmpdir && tmp_dbpath != folder)
    {
        g_free (tmp_dbpath);
        tmp_dbpath = folder;
    }
    *newpath = g_strdup (folder);
    return TRUE;

error:
    if (create_tmpdir)
    {
        g_free (folder);
    }
    return FALSE;
}

static void
log_cb (alpm_loglevel_t level, const char *fmt, va_list args)
{
    gchar *s;
    gsize l;

    if (!fmt || *fmt == '\0')
    {
        return;
    }

    if (config->is_debug == 2 && (level & (ALPM_LOG_DEBUG | ALPM_LOG_FUNCTION)))
    {
        return;
    }

    s = g_strdup_vprintf (fmt, args);
    l = strlen (s);
    if (s[--l] == '\n')
        s[l] = '\0';

    debug ("ALPM: %s", s);
    g_free (s);
}

gboolean
kalu_alpm_set_tmp_dbpath (const gchar *path)
{
    g_free (tmp_dbpath);
    if (path)
        tmp_dbpath = g_strdup (path);
    else
        tmp_dbpath = NULL;
    is_tmp_dbpath_set = !!path;
    return TRUE;
}

gboolean
kalu_alpm_load (kalu_simul_t    *simulation,
                const gchar     *conffile,
                GString        **_synced_dbs,
                GError         **error)
{
    GError             *local_err = NULL;
    gchar              *newpath;
    enum _alpm_errno_t  err;
    pacman_config_t    *pac_conf = NULL;
    gchar              *section = NULL;

    /* parse pacman.conf */
    debug ("parsing pacman.conf (%s) for options", conffile);
    if (!parse_pacman_conf (conffile, &section, 0, 0, &pac_conf, &local_err))
    {
        g_propagate_error (error, local_err);
        free_pacman_config (pac_conf);
        return FALSE;
    }

    debug ("setting up libalpm");
    alpm = new0 (kalu_alpm_t, 1);

    /* create tmp copy of db (so we can sync w/out being root) */
    if (!create_local_db (pac_conf->dbpath, &newpath, _synced_dbs, &local_err))
    {
        g_set_error (error, KALU_ERROR, 1,
                _("Unable to create local copy of database: %s"),
                local_err->message);
        g_clear_error (&local_err);
        free_pacman_config (pac_conf);
        kalu_alpm_free ();
        return FALSE;
    }
    alpm->dbpath = newpath;

    /* init libalpm */
    alpm->handle = alpm_initialize (pac_conf->rootdir, alpm->dbpath, &err);
    if (alpm->handle == NULL)
    {
        g_set_error (error, KALU_ERROR, 1,
                _("Failed to initialize alpm library: %s"),
                alpm_strerror (err));
        free_pacman_config (pac_conf);
        kalu_alpm_free ();
        return FALSE;
    }

    /* set arch & some options (what to ignore during update) */
    alpm_option_set_arch (alpm->handle, pac_conf->arch);
    alpm_option_set_ignorepkgs (alpm->handle, pac_conf->ignorepkgs);
    alpm_option_set_ignoregroups (alpm->handle, pac_conf->ignoregroups);
    alpm_option_set_default_siglevel (alpm->handle, pac_conf->siglevel);
    /* set GnuPG's rootdir */
    if (alpm_option_set_gpgdir (alpm->handle, pac_conf->gpgdir) != 0)
    {
        g_set_error (error, KALU_ERROR, 1,
                _("Failed to set GPGDir in ALPM: %s"),
                    alpm_strerror (alpm_errno (alpm->handle)));
        free_pacman_config (pac_conf);
        kalu_alpm_free ();
        return FALSE;
    }
    /* cachedirs are used when determining download size */
    alpm_option_set_cachedirs (alpm->handle, pac_conf->cachedirs);

#ifndef DISABLE_UPDATER
    if (simulation)
    {
        alpm->simulation = simulation;
        alpm_option_set_dlcb (alpm->handle, simulation->dl_progress_cb);
        alpm_option_set_questioncb (alpm->handle, simulation->question_cb);
        alpm_option_set_logcb (alpm->handle, simulation->log_cb);
        simulation->pac_conf = pac_conf;
    }
    else
#endif
    if (config->is_debug > 1)
        alpm_option_set_logcb (alpm->handle, log_cb);

    /* now we need to add dbs */
    alpm_list_t *i;
    FOR_LIST (i, pac_conf->databases)
    {
        database_t  *db_conf = i->data;
        alpm_db_t   *db;

        /* register db */
        debug ("register %s", db_conf->name);
        db = alpm_register_syncdb (alpm->handle, db_conf->name,
                db_conf->siglevel);
        if (db == NULL)
        {
            g_set_error (error, KALU_ERROR, 1,
                    _("Could not register database %s: %s"),
                    db_conf->name, alpm_strerror (alpm_errno (alpm->handle)));
            free_pacman_config (pac_conf);
            kalu_alpm_free ();
            return FALSE;
        }

        /* add servers */
        alpm_list_t *j;
        FOR_LIST (j, db_conf->servers)
        {
            char        *value  = j->data;
            const char  *dbname = alpm_db_get_name (db);
            /* let's attempt a replacement for the current repo */
            char        *temp   = strreplace (value, "$repo", dbname);
            /* let's attempt a replacement for the arch */
            const char  *arch   = pac_conf->arch;
            char        *server;

            if (arch)
            {
                server = strreplace (temp, "$arch", arch);
                free (temp);
            }
            else
            {
                if (strstr (temp, "$arch"))
                {
                    g_set_error (error, KALU_ERROR, 1,
                            _("Server %s contains the $arch variable, "
                                "but no Architecture was defined"),
                            value);
                    free (temp);
                    free (value);
                    free_pacman_config (pac_conf);
                    kalu_alpm_free ();
                    return FALSE;
                }
                server = temp;
            }

            debug ("add server %s into %s", server, dbname);
            if (alpm_db_add_server (db, server) != 0)
            {
                /* pm_errno is set by alpm_db_setserver */
                g_set_error (error, KALU_ERROR, 1,
                        _("Could not add server %s to database %s: %s"),
                        server,
                        dbname,
                        alpm_strerror (alpm_errno (alpm->handle)));
                free (server);
                free (value);
                free_pacman_config (pac_conf);
                kalu_alpm_free ();
                return FALSE;
            }

            free (server);
        }
    }

    /* set global var */
    alpm_verbose = pac_conf->verbosepkglists;

    if (!simulation)
        free_pacman_config (pac_conf);
    return TRUE;
}

gboolean
kalu_alpm_syncdbs (GString **_synced_dbs, GError **error)
{
    alpm_list_t     *sync_dbs   = NULL;
    alpm_list_t     *i;
    GError          *local_err  = NULL;
    int              ret;

    if (!check_syncdbs (alpm, 1, 0, &local_err))
    {
        g_propagate_error (error, local_err);
        return FALSE;
    }

    sync_dbs = alpm_get_syncdbs (alpm->handle);
#ifndef DISABLE_UPDATER
    if (alpm->simulation)
        alpm->simulation->on_sync_dbs (NULL, (gint) alpm_list_count (sync_dbs));
#endif
    FOR_LIST (i, sync_dbs)
    {
        alpm_db_t *db = i->data;

#ifndef DISABLE_UPDATER
        if (alpm->simulation)
            alpm->simulation->on_sync_db_start (NULL, alpm_db_get_name (db));
#endif
        ret = alpm_db_update (0, db);
        if (ret < 0)
        {
            g_set_error (error, KALU_ERROR, 1,
                    _("Failed to update %s: %s"),
                    alpm_db_get_name (db),
                    alpm_strerror (alpm_errno (alpm->handle)));
            return FALSE;
        }
        else if (ret == 1)
        {
            debug ("%s is up to date", alpm_db_get_name (db));
        }
        else
        {
            if (_synced_dbs)
            {
                GString *str = *_synced_dbs;
                const char *dbname = alpm_db_get_name (db);
                size_t j;

                if (!str)
                    str = *_synced_dbs = g_string_sized_new (63);

                for (j = 0; j <= str->len; j += strlen (str->str + j) + 1)
                    if (streq (dbname, str->str + j))
                        break;

                if (j > str->len)
                {
                    g_string_append (str, dbname);
                    g_string_append_c (str, '\0');
                }
            }
            debug ("%s was updated", alpm_db_get_name (db));
        }
#ifndef DISABLE_UPDATER
        if (alpm->simulation)
        {
            /* keep in sync with kupdater.h */
            enum {
                SYNC_SUCCESS,
                SYNC_FAILURE,
                SYNC_NOT_NEEDED
            };

            alpm->simulation->on_sync_db_end (NULL,
                    (ret < 0) ? SYNC_FAILURE : (ret == 1) ? SYNC_NOT_NEEDED : SYNC_SUCCESS);
        }
#endif
    }

    return TRUE;
}

gboolean
kalu_alpm_has_updates (alpm_list_t **packages, GError **error)
{
    alpm_list_t *i;
    alpm_list_t *data       = NULL;
    GError      *local_err  = NULL;

    if (!check_syncdbs (alpm, 1, 1, &local_err))
    {
        g_propagate_error (error, local_err);
        return FALSE;
    }

    if (!trans_init (alpm, alpm->flags, 1, &local_err))
    {
        g_propagate_error (error, local_err);
        return FALSE;
    }

    if (alpm_sync_sysupgrade (alpm->handle, 0) == -1)
    {
        g_set_error (error, KALU_ERROR, 1, "%s",
                alpm_strerror (alpm_errno (alpm->handle)));
        goto cleanup;
    }

    if (alpm_trans_prepare (alpm->handle, &data) == -1)
    {
        int len = 1024;
        gchar buf[255], err[len--];
        err[0] = '\0';
        switch (alpm_errno (alpm->handle))
        {
            case ALPM_ERR_PKG_INVALID_ARCH:
                FOR_LIST (i, data)
                {
                    char *pkg = i->data;
                    len -= snprintf (buf, 255,
                            _("- Package %s does not have a valid architecture\n"),
                            pkg);
                    if (len >= 0)
                    {
                        strncat (err, buf, (size_t) len);
                    }
                    free (pkg);
                }
                break;
            case ALPM_ERR_UNSATISFIED_DEPS:
                FOR_LIST (i, data)
                {
                    alpm_depmissing_t *miss = i->data;
                    char *depstring = alpm_dep_compute_string (miss->depend);
                    len -= snprintf (buf, 255,
                            _("- %s requires %s\n"),
                            miss->target,
                            depstring);
                    if (len >= 0)
                    {
                        strncat (err, buf, (size_t) len);
                    }
                    free (depstring);
                    alpm_depmissing_free (miss);
                }
                break;
            case ALPM_ERR_CONFLICTING_DEPS:
                FOR_LIST (i, data)
                {
                    alpm_conflict_t *conflict = i->data;
                    /* only print reason if it contains new information */
                    if (conflict->reason->mod == ALPM_DEP_MOD_ANY)
                    {
                        len -= snprintf (buf, 255,
                                _("- %s and %s are in conflict\n"),
                                conflict->package1,
                                conflict->package2);
                        if (len >= 0)
                        {
                            strncat (err, buf, (size_t) len);
                        }
                    }
                    else
                    {
                        char *reason;
                        reason = alpm_dep_compute_string (conflict->reason);
                        len -= snprintf (buf, 255,
                                _("- %s and %s are in conflict (%s)\n"),
                                conflict->package1,
                                conflict->package2,
                                reason);
                        if (len >= 0)
                        {
                            strncat (err, buf, (size_t) len);
                        }
                        free (reason);
                    }
                    alpm_conflict_free (conflict);
                }
                break;
            default:
                break;
        }
        g_set_error (error, KALU_ERROR, 2,
                _("Failed to prepare transaction: %s\n%s"),
                alpm_strerror (alpm_errno (alpm->handle)),
                err);
        goto cleanup;
    }

    alpm_db_t *db_local = alpm_get_localdb (alpm->handle);
    FOR_LIST (i, alpm_trans_get_add (alpm->handle))
    {
        alpm_pkg_t *pkg = i->data;
        alpm_pkg_t *old = alpm_db_get_pkg (db_local, alpm_pkg_get_name (pkg));
        kalu_package_t *package;

        package = new0 (kalu_package_t, 1);
        package->repo = strdup (alpm_db_get_name (alpm_pkg_get_db (pkg)));
        package->name = strdup (alpm_pkg_get_name (pkg));
        package->desc = strdup (alpm_pkg_get_desc (pkg));
        package->new_version = strdup (alpm_pkg_get_version (pkg));
        package->dl_size = (guint) alpm_pkg_download_size (pkg);
        package->new_size = (guint) alpm_pkg_get_isize (pkg);
        /* we might not have an old package, when an update requires to
         * install a new package (e.g. after a split) */
        if (old)
        {
            package->old_version = strdup (alpm_pkg_get_version (old));
            package->old_size = (guint) alpm_pkg_get_isize (old);
        }
        else
        {
            /* TRANSLATORS: no previous version */
            package->old_version = strdup (_("none"));
            package->old_size = 0;
        }

        *packages = alpm_list_add (*packages, package);
    }

#ifndef DISABLE_UPDATER
    /* packages don't get removed automatically during a sysupgrade, however in
     * simulation the user will have chosen to remove/replace a package, so we
     * need to include them as well */
    if (alpm->simulation)
        FOR_LIST (i, alpm_trans_get_remove (alpm->handle))
        {
            alpm_pkg_t *pkg = i->data;
            alpm_pkg_t *old = alpm_db_get_pkg (db_local, alpm_pkg_get_name (pkg));
            kalu_package_t *package;

            package = new0 (kalu_package_t, 1);
            package->repo = strdup (alpm_db_get_name (alpm_pkg_get_db (pkg)));
            package->name = strdup (alpm_pkg_get_name (pkg));
            package->desc = strdup (alpm_pkg_get_desc (pkg));
            package->new_version = strdup (_("none"));
            package->dl_size = 0;
            package->new_size = 0;
            package->old_version = strdup (alpm_pkg_get_version (old));
            package->old_size = (guint) alpm_pkg_get_isize (old);

            *packages = alpm_list_add (*packages, package);
        }
#endif

cleanup:
    alpm_list_free (data);
    trans_release (alpm, NULL);

    return (*packages != NULL);
}

gboolean
kalu_alpm_has_updates_watched (alpm_list_t **packages, alpm_list_t *watched,
        GError **error)
{
    alpm_list_t *sync_dbs = alpm_get_syncdbs (alpm->handle);
    alpm_list_t *i, *j;
    GError *local_err = NULL;

    if (!check_syncdbs (alpm, 1, 1, &local_err))
    {
        g_propagate_error (error, local_err);
        return FALSE;
    }

    FOR_LIST (i, watched)
    {
        alpm_pkg_t *pkg = NULL;
        watched_package_t *w_pkg = i->data;
        kalu_package_t *package;
        const char *s;

        /* is the name actually a repo/name to restrict to a specific repo? */
        s = strchr (w_pkg->name, '/');

        FOR_LIST (j, sync_dbs)
        {
            if (s)
            {
                const char *dbname = alpm_db_get_name ((alpm_db_t *) j->data);
                size_t len = strlen (dbname);

                /* we only want the package in a specific repo */
                if (len != s - w_pkg->name || !streqn (w_pkg->name, dbname, len))
                    continue;
                ++s;
            }

            pkg = alpm_db_get_pkg ((alpm_db_t *) j->data, (s) ? s : w_pkg->name);
            if (pkg)
            {
                if (alpm_pkg_vercmp (alpm_pkg_get_version (pkg),
                            w_pkg->version) > 0)
                {
                    package = new0 (kalu_package_t, 1);

                    package->repo = strdup (alpm_db_get_name (alpm_pkg_get_db (pkg)));
                    /* we want to keep the name as "repo/name" (despite the
                     * "oddity" of it) so it is processed correctly in the
                     * watched list, as well as to indicate it was restricted to
                     * this specific repo */
                    package->name = strdup ((s) ? w_pkg->name : alpm_pkg_get_name (pkg));
                    package->desc = strdup (alpm_pkg_get_desc (pkg));
                    package->old_version = strdup (w_pkg->version);
                    package->new_version = strdup (alpm_pkg_get_version (pkg));
                    package->dl_size = (guint) alpm_pkg_download_size (pkg);
                    package->new_size = (guint) alpm_pkg_get_isize (pkg);
		    package->ignored = (guint) alpm_pkg_should_ignore(alpm->handle, pkg);

                    *packages = alpm_list_add (*packages, package);
                    debug ("found watched update %s: %s -> %s", package->name,
                            package->old_version, package->new_version);
                }
                break;
            }
        }

        if (!pkg)
        {
            package = new0 (kalu_package_t, 1);

            package->name = strdup (w_pkg->name);
            package->desc = strdup (_("<package not found>"));
            package->old_version = strdup (w_pkg->version);
            package->new_version = strdup ("-");
            package->dl_size = 0;
            package->new_size = 0;

            *packages = alpm_list_add (*packages, package);
            debug ("watched package not found: %s", package->name);
        }
    }

    return (*packages != NULL);
}

gboolean
kalu_alpm_has_foreign (alpm_list_t **packages, alpm_list_t *ignore,
        GError **error)
{
    alpm_db_t *dblocal;
    alpm_list_t *sync_dbs, *i, *j;
    gboolean found;
    GError *local_err = NULL;

    if (!check_syncdbs (alpm, 1, 1, &local_err))
    {
        g_propagate_error (error, local_err);
        return FALSE;
    }

    dblocal  = alpm_get_localdb (alpm->handle);
    sync_dbs = alpm_get_syncdbs (alpm->handle);

    FOR_LIST (i, alpm_db_get_pkgcache (dblocal))
    {
        alpm_pkg_t *pkg = i->data;
        const char *pkgname = alpm_pkg_get_name (pkg);
        found = FALSE;

        if (NULL != alpm_list_find_str (ignore, pkgname))
        {
            continue;
        }

        FOR_LIST (j, sync_dbs)
        {
            if (alpm_db_get_pkg ((alpm_db_t *) j->data, pkgname))
            {
                found = TRUE;
                break;
            }
        }
        if (!found)
        {
            *packages = alpm_list_add (*packages, pkg);
        }
    }

    return (*packages != NULL);
}

const gchar *
kalu_alpm_get_dbpath (void)
{
    return (alpm) ? alpm->dbpath : NULL;
}

void
kalu_alpm_rmdb (gboolean keep_tmp_dbpath)
{
    if (!tmp_dbpath)
        return;
    if (!keep_tmp_dbpath)
        rmrf (tmp_dbpath);
    g_free (tmp_dbpath);
    tmp_dbpath = NULL;
}

void
kalu_alpm_free (void)
{
    if (alpm == NULL)
    {
        return;
    }

    if (alpm->handle != NULL)
    {
        alpm_release (alpm->handle);
    }
    free (alpm->dbpath);
    g_free (alpm);
    alpm = NULL;
}
