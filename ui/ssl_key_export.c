/* export_sslkeys.c
 *
 * Export SSL Session Keys dialog
 * by Sake Blok <sake@euronet.nl> (20110526)
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib.h>

#include <epan/address.h>
#include <epan/dissectors/packet-tls.h>
#include <epan/dissectors/packet-tls-utils.h>

#include "ui/ssl_key_export.h"

int
ssl_session_key_count(void)
{
    return g_hash_table_size(ssl_session_hash) +
           g_hash_table_size(ssl_crandom_hash);
}

static void
ssl_export_sessions_func(gpointer key, gpointer value, gpointer user_data)
{
    guint i;
    StringInfo *sslid = (StringInfo *)key;
    StringInfo *master_secret = (StringInfo *)value;
    GString *keylist = (GString *)user_data;

    g_string_append(keylist, "RSA Session-ID:");

    for (i = 0; i < sslid->data_len; i++) {
        g_string_append_printf(keylist, "%.2x", sslid->data[i]);
    }

    g_string_append(keylist, " Master-Key:");

    for (i = 0; i < master_secret->data_len; i++) {
        g_string_append_printf(keylist, "%.2x", master_secret->data[i]);
    }

    g_string_append_c(keylist, '\n');
}

static void
ssl_export_client_randoms_func(gpointer key, gpointer value, gpointer user_data)
{
    guint i;
    StringInfo *client_random = (StringInfo *)key;
    StringInfo *master_secret = (StringInfo *)value;
    GString *keylist = (GString *)user_data;

    g_string_append(keylist, "CLIENT_RANDOM ");

    for (i = 0; i < client_random->data_len; i++) {
        g_string_append_printf(keylist, "%.2x", client_random->data[i]);
    }

    g_string_append_c(keylist, ' ');

    for (i = 0; i < master_secret->data_len; i++) {
        g_string_append_printf(keylist, "%.2x", master_secret->data[i]);
    }

    g_string_append_c(keylist, '\n');
}

gchar*
ssl_export_sessions(gsize *length)
{
    /* Output format is:
     * "RSA Session-ID:xxxx Master-Key:yyyy\n"
     * Where xxxx is the session ID in hex (max 64 chars)
     * Where yyyy is the Master Key in hex (always 96 chars)
     * So in total max 3+1+11+64+1+11+96+2 = 189 chars
     * or
     * "CLIENT_RANDOM zzzz yyyy\n"
     * Where zzzz is the client random (always 64 chars)
     * Where yyyy is same as above
     * So length will always be 13+1+64+1+96+2 = 177 chars
     *
     * Wireshark can read CLIENT_RANDOM since v1.8.0.
     * Both values are exported in case you use the Session-ID for resuming a
     * session in a different capture.
     */
    gsize len = 189 * g_hash_table_size(ssl_session_hash) +
                177 * g_hash_table_size(ssl_crandom_hash);
    GString *keylist = g_string_sized_new(len);

    g_hash_table_foreach(ssl_session_hash, ssl_export_sessions_func, (gpointer)keylist);
    g_hash_table_foreach(ssl_crandom_hash, ssl_export_client_randoms_func, (gpointer)keylist);

    *length = keylist->len;
    return g_string_free(keylist, FALSE);
}
