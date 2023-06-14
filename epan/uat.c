/*
 *  uat.c
 *
 *  User Accessible Tables
 *  Maintain an array of user accessible data structures
 *
 * (c) 2007, Luis E. Garcia Ontanon <luis@ontanon.org>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 2001 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>

#include <glib.h>

#include <wsutil/file_util.h>
#include <wsutil/str_util.h>
#include <wsutil/report_message.h>
#include <wsutil/ws_assert.h>

#include <wsutil/filesystem.h>
#include <epan/packet.h>
#include <epan/range.h>

#include "uat-int.h"

/*
 * XXX Files are encoded as ASCII. We might want to encode them as UTF8
 * instead.
 */

static GPtrArray* all_uats = NULL;

uat_t* uat_new(const char* name,
               size_t size,
               const char* filename,
               gboolean from_profile,
               void* data_ptr,
               guint* numitems_ptr,
               guint flags,
               const char* help,
               uat_copy_cb_t copy_cb,
               uat_update_cb_t update_cb,
               uat_free_cb_t free_cb,
               uat_post_update_cb_t post_update_cb,
               uat_reset_cb_t reset_cb,
               uat_field_t* flds_array) {
    /* Create new uat */
    uat_t* uat = g_new(uat_t, 1);
    guint i;

    /* Add to global array of uats */
    if (!all_uats)
        all_uats = g_ptr_array_new();

    g_ptr_array_add(all_uats,uat);

    /* Check params */
    ws_assert(name && size && filename && data_ptr && numitems_ptr);

    /* Set uat values from inputs */
    uat->name = g_strdup(name);
    uat->record_size = size;
    uat->filename = g_strdup(filename);
    uat->from_profile = from_profile;
    /* Callers of uat_new() pass in (void*) for data_ptr, because
     * that is the "universal" pointer type that can be cast to
     * anything. However, for our purposes, we want a (void**).
     * So, we cast (void*) data_ptr to (void**) here. That keeps
     * gcc -fstrict-aliasing from complaining. */
    uat->user_ptr = (void**) data_ptr;
    uat->nrows_p = numitems_ptr;
    uat->copy_cb = copy_cb;
    uat->update_cb = update_cb;
    uat->free_cb = free_cb;
    uat->post_update_cb = post_update_cb;
    uat->reset_cb = reset_cb;
    uat->fields = flds_array;
    uat->default_values = NULL;
    uat->user_data = g_array_new(FALSE,FALSE,(guint)uat->record_size);
    uat->raw_data = g_array_new(FALSE,FALSE,(guint)uat->record_size);
    uat->valid_data = g_array_new(FALSE,FALSE,sizeof(gboolean));
    uat->changed = FALSE;
    uat->loaded = FALSE;
    uat->from_global = FALSE;
    uat->rep = NULL;
    uat->free_rep = NULL;
    uat->help = g_strdup(help);
    uat->flags = flags;

    for (i=0;flds_array[i].title;i++) {
        fld_data_t* f = g_new(fld_data_t, 1);

        f->colnum = i+1;
        f->rep = NULL;
        f->free_rep = NULL;

        flds_array[i].priv = f;
    }

    uat->ncols = i;

    *((void**)data_ptr) = NULL;
    *numitems_ptr = 0;

    return uat;
}

void* uat_add_record(uat_t* uat, const void* data, gboolean valid_rec) {
    void* rec;
    gboolean* valid;

    uat_insert_record_idx(uat, uat->raw_data->len, data);

    if (valid_rec) {
        /* Add a "known good" record to the list to be used by the dissector */
        g_array_append_vals (uat->user_data, data, 1);

        rec = UAT_USER_INDEX_PTR(uat, uat->user_data->len - 1);

        if (uat->copy_cb) {
            uat->copy_cb(rec, data, (unsigned int) uat->record_size);
        }

        UAT_UPDATE(uat);

        valid = &g_array_index(uat->valid_data, gboolean, uat->valid_data->len-1);
        *valid = valid_rec;
    } else {
        rec = NULL;
    }

    return rec;
}

/* Updates the validity of a record. */
void uat_update_record(uat_t *uat, const void *record, gboolean valid_rec) {
    guint pos;
    gboolean *valid;

    /* Locate internal UAT data pointer. */
    for (pos = 0; pos < uat->raw_data->len; pos++) {
        if (UAT_INDEX_PTR(uat, pos) == record) {
            break;
        }
    }
    if (pos == uat->raw_data->len) {
        /* Data is not within list?! */
        ws_assert_not_reached();
    }

    valid = &g_array_index(uat->valid_data, gboolean, pos);
    *valid = valid_rec;
}

void uat_swap(uat_t* uat, guint a, guint b) {
    size_t s = uat->record_size;
    void* tmp;
    gboolean tmp_bool;

    ws_assert( a < uat->raw_data->len && b < uat->raw_data->len );

    if (a == b) return;

    tmp = g_malloc(s);
    memcpy(tmp, UAT_INDEX_PTR(uat,a), s);
    memcpy(UAT_INDEX_PTR(uat,a), UAT_INDEX_PTR(uat,b), s);
    memcpy(UAT_INDEX_PTR(uat,b), tmp, s);
    g_free(tmp);

    tmp_bool = *(gboolean*)(uat->valid_data->data + (sizeof(gboolean) * (a)));
    *(gboolean*)(uat->valid_data->data + (sizeof(gboolean) * (a))) = *(gboolean*)(uat->valid_data->data + (sizeof(gboolean) * (b)));
    *(gboolean*)(uat->valid_data->data + (sizeof(gboolean) * (b))) = tmp_bool;


}

void uat_insert_record_idx(uat_t* uat, guint idx, const void *src_record) {
    /* Allow insert before an existing item or append after the last item. */
    ws_assert( idx <= uat->raw_data->len );

    /* Store a copy of the record and invoke copy_cb to clone pointers too. */
    g_array_insert_vals(uat->raw_data, idx, src_record, 1);
    void *rec = UAT_INDEX_PTR(uat, idx);
    if (uat->copy_cb) {
        uat->copy_cb(rec, src_record, (unsigned int) uat->record_size);
    } else {
        memcpy(rec, src_record, (unsigned int) uat->record_size);
    }

    /* Initially assume that the record is invalid, it is not copied to the
     * user-visible records list. */
    gboolean valid_rec = FALSE;
    g_array_insert_val(uat->valid_data, idx, valid_rec);
}

void uat_remove_record_idx(uat_t* uat, guint idx) {

    ws_assert( idx < uat->raw_data->len );

    if (uat->free_cb) {
        uat->free_cb(UAT_INDEX_PTR(uat,idx));
    }

    g_array_remove_index(uat->raw_data, idx);
    g_array_remove_index(uat->valid_data, idx);
}

void uat_move_index(uat_t * uat, guint old_idx, guint new_idx)
{
    guint dir = 1;
    guint start = old_idx;
    if ( old_idx > new_idx )
        dir = -1;

    while ( start != new_idx )
    {
        uat_swap(uat, start, start + dir);
        start += dir;
    }
}

/* The returned filename was g_malloc()'d so the caller must free it */
gchar* uat_get_actual_filename(uat_t* uat, gboolean for_writing) {
    gchar *pers_fname = NULL;

    if (! uat->from_global) {
        pers_fname =  get_persconffile_path(uat->filename, uat->from_profile);
    }

    if ((! for_writing ) && (! file_exists(pers_fname) )) {
        gchar* data_fname = get_datafile_path(uat->filename);

        if (file_exists(data_fname)) {
            g_free(pers_fname);
            return data_fname;
        }

        g_free(data_fname);
        g_free(pers_fname);
        return NULL;
    }

    return pers_fname;
}

uat_t* uat_get_table_by_name(const char* name) {
    guint i;

    for (i=0; i < all_uats->len; i++) {
        uat_t* u = (uat_t *)g_ptr_array_index(all_uats,i);
        if ( g_str_equal(u->name,name) ) {
            return (u);
        }
    }

    return NULL;
}

void uat_set_default_values(uat_t *uat_in, const char *default_values[])
{
    uat_in->default_values = default_values;
}

char *uat_fld_tostr(void *rec, uat_field_t *f) {
    guint        len;
    char       *ptr;
    char       *out;

    f->cb.tostr(rec, &ptr, &len, f->cbdata.tostr, f->fld_data);

    switch(f->mode) {
        case PT_TXTMOD_NONE:
        case PT_TXTMOD_STRING:
        case PT_TXTMOD_ENUM:
        case PT_TXTMOD_BOOL:
        case PT_TXTMOD_FILENAME:
        case PT_TXTMOD_DIRECTORYNAME:
        case PT_TXTMOD_DISPLAY_FILTER:
        case PT_TXTMOD_COLOR:
        case PT_TXTMOD_PROTO_FIELD:
            out = g_strndup(ptr, len);
            break;
        case PT_TXTMOD_HEXBYTES: {
            GString *s = g_string_sized_new( len*2 + 1 );
            guint i;

            for (i=0; i<len;i++) g_string_append_printf(s, "%.2X", ((const guint8*)ptr)[i]);

            out = g_strdup(s->str);

            g_string_free(s, TRUE);
            break;
        }
        default:
            ws_assert_not_reached();
            out = NULL;
            break;
    }

    g_free(ptr);
    return out;
}

static void putfld(FILE* fp, void* rec, uat_field_t* f) {
    guint fld_len;
    char* fld_ptr;

    f->cb.tostr(rec,&fld_ptr,&fld_len,f->cbdata.tostr,f->fld_data);

    switch(f->mode){
        case PT_TXTMOD_NONE:
        case PT_TXTMOD_ENUM:
        case PT_TXTMOD_FILENAME:
        case PT_TXTMOD_DIRECTORYNAME:
        case PT_TXTMOD_DISPLAY_FILTER:
        case PT_TXTMOD_PROTO_FIELD:
        case PT_TXTMOD_COLOR:
        case PT_TXTMOD_STRING:
        case PT_TXTMOD_DISSECTOR:
        {
            guint i;

            putc('"',fp);

            for(i=0;i<fld_len;i++) {
                char c = fld_ptr[i];

                if (c == '"' || c == '\\' || ! g_ascii_isprint((guchar)c) ) {
                    fprintf(fp,"\\x%02x", (guchar) c);
                } else {
                    putc(c,fp);
                }
            }

            putc('"',fp);
            break;
        }
        case PT_TXTMOD_HEXBYTES: {
            guint i;

            for(i=0;i<fld_len;i++) {
                fprintf(fp,"%02x", (guchar)fld_ptr[i]);
            }

            break;
        }
        case PT_TXTMOD_BOOL: {
            fprintf(fp,"\"%s\"", fld_ptr);
            break;
        }
        default:
            ws_assert_not_reached();
    }

    g_free(fld_ptr);
}

gboolean uat_save(uat_t* uat, char** error) {
    guint i;
    gchar* fname = uat_get_actual_filename(uat,TRUE);
    FILE* fp;

    if (! fname ) return FALSE;

    fp = ws_fopen(fname,"w");

    if (!fp && errno == ENOENT) {
        /* Parent directory does not exist, try creating first */
        gchar *pf_dir_path = NULL;
        if (create_persconffile_dir(&pf_dir_path) != 0) {
            *error = ws_strdup_printf("uat_save: error creating '%s'", pf_dir_path);
            g_free (pf_dir_path);
            return FALSE;
        }
        fp = ws_fopen(fname,"w");
    }

    if (!fp) {
        *error = ws_strdup_printf("uat_save: error opening '%s': %s",fname,g_strerror(errno));
        return FALSE;
    }

    *error = NULL;
    g_free (fname);

    /* Ensure raw_data is synced with user_data and all "good" entries have been accounted for */

    /* Start by clearing current user_data */
    for ( i = 0 ; i < uat->user_data->len ; i++ ) {
        if (uat->free_cb) {
            uat->free_cb(UAT_USER_INDEX_PTR(uat,i));
        }
    }
    g_array_set_size(uat->user_data,0);

    *((uat)->user_ptr) = NULL;
    *((uat)->nrows_p) = 0;

    /* Now copy "good" raw_data entries to user_data */
    for ( i = 0 ; i < uat->raw_data->len ; i++ ) {
        void *rec = UAT_INDEX_PTR(uat, i);
        gboolean valid = g_array_index(uat->valid_data, gboolean, i);
        if (valid) {
            g_array_append_vals(uat->user_data, rec, 1);
            if (uat->copy_cb) {
                uat->copy_cb(UAT_USER_INDEX_PTR(uat, uat->user_data->len - 1),
                             rec, (unsigned int) uat->record_size);
            }

            UAT_UPDATE(uat);
        }
    }


    fprintf(fp,"# This file is automatically generated, DO NOT MODIFY.\n");

    for ( i = 0 ; i < uat->user_data->len ; i++ ) {
        void* rec = uat->user_data->data + (uat->record_size * i);
        uat_field_t* f;
        guint j;

        f = uat->fields;


        for( j=0 ; j < uat->ncols ; j++ ) {
            putfld(fp, rec, &(f[j]));
            fputs((j == uat->ncols - 1) ? "\n" : "," ,fp);
        }

    }

    fclose(fp);

    uat->changed = FALSE;

    return TRUE;
}

uat_t *uat_find(gchar *name) {
    guint i;

    for (i=0; i < all_uats->len; i++) {
        uat_t* u = (uat_t *)g_ptr_array_index(all_uats,i);

        if (strcmp(u->name, name) == 0 || strcmp(u->filename, name) == 0) {
            return u;
        }
    }
    return NULL;
}

void uat_clear(uat_t* uat) {
    guint i;

    for ( i = 0 ; i < uat->user_data->len ; i++ ) {
        if (uat->free_cb) {
            uat->free_cb(UAT_USER_INDEX_PTR(uat,i));
        }
    }

    for ( i = 0 ; i < uat->raw_data->len ; i++ ) {
        if (uat->free_cb) {
            uat->free_cb(UAT_INDEX_PTR(uat,i));
        }
    }

    g_array_set_size(uat->raw_data,0);
    g_array_set_size(uat->user_data,0);
    g_array_set_size(uat->valid_data,0);

    *((uat)->user_ptr) = NULL;
    *((uat)->nrows_p) = 0;

    if (uat->reset_cb) {
        uat->reset_cb();
    }
}

void uat_unload_all(void) {
    guint i;

    for (i=0; i < all_uats->len; i++) {
        uat_t* u = (uat_t *)g_ptr_array_index(all_uats,i);
        /* Do not unload if not in profile */
        if (u->from_profile) {
            uat_clear(u);
            u->loaded = FALSE;
        }
    }
}

void uat_cleanup(void) {
    guint i;
    guint j;
    uat_t* uat;

    for (i = 0; i < all_uats->len; i++) {
        uat = (uat_t *)g_ptr_array_index(all_uats, i);
        uat_clear(uat);
        g_free(uat->help);
        g_free(uat->name);
        g_free(uat->filename);
        g_array_free(uat->user_data, TRUE);
        g_array_free(uat->raw_data, TRUE);
        g_array_free(uat->valid_data, TRUE);
        for (j = 0; uat->fields[j].title; j++)
            g_free(uat->fields[j].priv);
        g_free(uat);
    }

    g_ptr_array_free(all_uats,TRUE);
}

void uat_foreach_table(uat_cb_t cb,void* user_data) {
    guint i;

    for (i=0; i < all_uats->len; i++)
        cb(g_ptr_array_index(all_uats,i), user_data);

}

void uat_load_all(void) {
    guint i;
    gchar* err;

    for (i=0; i < all_uats->len; i++) {
        uat_t* u = (uat_t *)g_ptr_array_index(all_uats,i);

        if (!u->loaded) {
            err = NULL;
            if (!uat_load(u, NULL, &err)) {
                report_failure("Error loading table '%s': %s",u->name,err);
                g_free(err);
            }
        }
    }
}


gboolean uat_fld_chk_str(void* u1 _U_, const char* strptr, guint len _U_, const void* u2 _U_, const void* u3 _U_, char** err) {
    if (strptr == NULL) {
        *err = g_strdup("NULL pointer");
        return FALSE;
    }

    *err = NULL;
    return TRUE;
}

gboolean uat_fld_chk_oid(void* u1 _U_, const char* strptr, guint len, const void* u2 _U_, const void* u3 _U_, char** err) {
  unsigned int i;
    *err = NULL;

    if (strptr == NULL) {
      *err = g_strdup("NULL pointer");
      return FALSE;
    }

    if (len == 0) {
      *err = g_strdup("Empty OID");
      return FALSE;
    }

    for(i = 0; i < len; i++)
      if(!(g_ascii_isdigit(strptr[i]) || strptr[i] == '.')) {
        *err = g_strdup("Only digits [0-9] and \".\" allowed in an OID");
        return FALSE;
      }

    if(strptr[len-1] == '.') {
      *err = g_strdup("OIDs must not be terminated with a \".\"");
      return FALSE;
    }

    if(!((*strptr == '0' || *strptr == '1' || *strptr =='2') && (len > 1 && strptr[1] == '.'))) {
      *err = g_strdup("OIDs must start with \"0.\" (ITU-T assigned), \"1.\" (ISO assigned) or \"2.\" (joint ISO/ITU-T assigned)");
      return FALSE;
    }

    /* should also check that the second arc is in the range 0-39 */

    return *err == NULL;
}

gboolean uat_fld_chk_proto(void* u1 _U_, const char* strptr, guint len, const void* u2 _U_, const void* u3 _U_, char** err) {
    if (len) {
        char* name = g_strndup(strptr,len);
        g_strstrip(name);

        if (find_dissector(name)) {
            *err = NULL;
            g_free(name);
            return TRUE;
        } else {
            *err = g_strdup("dissector not found");
            g_free(name);
            return FALSE;
        }
    } else {
        *err = NULL;
        return TRUE;
    }
}

static gboolean uat_fld_chk_num_check_result(gboolean result, const char* strn, char** err) {
    if (result && ((*strn != '\0') && (*strn != ' '))) {
        /* string valid, but followed by something other than a space */
        result = FALSE;
        errno = EINVAL;
    }
    if (!result) {
        switch (errno) {

        case EINVAL:
            *err = g_strdup("Invalid value");
            break;

        case ERANGE:
            *err = g_strdup("Value too large");
            break;

        default:
            *err = g_strdup(g_strerror(errno));
            break;
        }
    }

    return result;
}

static gboolean uat_fld_chk_num(int base, const char* strptr, guint len, char** err) {
    if (len > 0) {
        char* str = g_strndup(strptr, len);
        const char* strn;
        gboolean result;
        guint32 value;

        result = ws_basestrtou32(str, &strn, &value, base);
        result = uat_fld_chk_num_check_result(result, strn, err);
        g_free(str);
        return result;
    }

    *err = NULL;
    return TRUE;
}

static gboolean uat_fld_chk_num64(int base, const char* strptr, guint len, char** err) {
    if (len > 0) {
        char* str = g_strndup(strptr, len);
        const char* strn;
        gboolean result;
        guint64 value64;

        result = ws_basestrtou64(str, &strn, &value64, base);
        result = uat_fld_chk_num_check_result(result, strn, err);
        g_free(str);
        return result;
    }

    *err = NULL;
    return TRUE;
}

gboolean uat_fld_chk_num_dec(void* u1 _U_, const char* strptr, guint len, const void* u2 _U_, const void* u3 _U_, char** err) {
    return uat_fld_chk_num(10, strptr, len, err);
}

gboolean uat_fld_chk_num_hex(void* u1 _U_, const char* strptr, guint len, const void* u2 _U_, const void* u3 _U_, char** err) {
    return uat_fld_chk_num(16, strptr, len, err);
}

gboolean uat_fld_chk_num_dec64(void* u1 _U_, const char* strptr, guint len, const void* u2 _U_, const void* u3 _U_, char** err) {
    return uat_fld_chk_num64(10, strptr, len, err);
}

gboolean uat_fld_chk_num_hex64(void* u1 _U_, const char* strptr, guint len, const void* u2 _U_, const void* u3 _U_, char** err) {
    return uat_fld_chk_num64(16, strptr, len, err);
}

gboolean uat_fld_chk_num_signed_dec(void* u1 _U_, const char* strptr, guint len, const void* u2 _U_, const void* u3 _U_, char** err) {
    if (len > 0) {
        char* str = g_strndup(strptr,len);
        const char* strn;
        gboolean result;
        gint32 value;

        result = ws_strtoi32(str, &strn, &value);
        result = uat_fld_chk_num_check_result(result, strn, err);
        g_free(str);

        return result;
    }

    *err = NULL;
    return TRUE;
}

gboolean uat_fld_chk_num_signed_dec64(void* u1 _U_, const char* strptr, guint len, const void* u2 _U_, const void* u3 _U_, char** err) {
    if (len > 0) {
        char* str = g_strndup(strptr, len);
        const char* strn;
        gboolean result;
        gint64 value;

        result = ws_strtoi64(str, &strn, &value);
        result = uat_fld_chk_num_check_result(result, strn, err);
        g_free(str);

        return result;
    }

    *err = NULL;
    return TRUE;
}

gboolean uat_fld_chk_bool(void* u1 _U_, const char* strptr, guint len, const void* u2 _U_, const void* u3 _U_, char** err)
{
    char* str = g_strndup(strptr,len);

    if ((g_strcmp0(str, "TRUE") == 0) ||
        (g_strcmp0(str, "FALSE") == 0)) {
        *err = NULL;
        g_free(str);
        return TRUE;
    }

    *err = ws_strdup_printf("invalid value: %s (must be TRUE or FALSE)", str);
    g_free(str);
    return FALSE;
}


gboolean uat_fld_chk_enum(void* u1 _U_, const char* strptr, guint len, const void* v, const void* u3 _U_, char** err) {
    char* str = g_strndup(strptr,len);
    guint i;
    const value_string* vs = (const value_string *)v;

    for(i=0;vs[i].strptr;i++) {
        if (g_strcmp0(vs[i].strptr,str) == 0) {
            *err = NULL;
            g_free(str);
            return TRUE;
        }
    }

    *err = ws_strdup_printf("invalid value: %s",str);
    g_free(str);
    return FALSE;
}

gboolean uat_fld_chk_range(void* u1 _U_, const char* strptr, guint len, const void* v _U_, const void* u3, char** err) {
    char* str = g_strndup(strptr,len);
    range_t* r = NULL;
    convert_ret_t ret = range_convert_str(NULL, &r, str,GPOINTER_TO_UINT(u3));
    gboolean ret_value = FALSE;

    switch (  ret ) {
        case CVT_NO_ERROR:
            *err = NULL;
            ret_value = TRUE;
            break;
        case CVT_SYNTAX_ERROR:
            *err = ws_strdup_printf("syntax error in range: %s",str);
            ret_value = FALSE;
            break;
        case CVT_NUMBER_TOO_BIG:
            *err = ws_strdup_printf("value too large in range: '%s' (max = %u)",str,GPOINTER_TO_UINT(u3));
            ret_value = FALSE;
            break;
        default:
            *err = g_strdup("Unable to convert range. Please report this to wireshark-dev@wireshark.org");
            ret_value = FALSE;
            break;
    }

    g_free(str);
    wmem_free(NULL, r);
    return ret_value;
}

gboolean uat_fld_chk_color(void* u1 _U_, const char* strptr, guint len, const void* v _U_, const void* u3 _U_, char** err) {

    if ((len != 7) || (*strptr != '#')) {
        *err = g_strdup("Color must be of the format #RRGGBB");
        return FALSE;
    }

    /* Color is just # followed by hex string, so use hex verification */
    return uat_fld_chk_num(16, strptr + 1, len - 1, err);
}

char* uat_unbinstring(const char* si, guint in_len, guint* len_p) {
    guint8* buf;
    guint len = in_len/2;
    int i = 0;
    int d0, d1;

    if (in_len%2) {
        return NULL;
    }

    buf= (guint8 *)g_malloc0(len+1);
    if (len_p) *len_p = len;

    while(in_len) {
        d1 = ws_xton(*(si++));
        d0 = ws_xton(*(si++));

        buf[i++] = (d1 * 16) + d0;

        in_len -= 2;
    }

    return (char*)buf;
}

char* uat_unesc(const char* si, guint in_len, guint* len_p) {
    char* buf = (char *)g_malloc0(in_len+1);
    char* p = buf;
    guint len = 0;
    const char* s;
    const char* in_end = si+in_len;

    for (s = si; s < in_end; s++) {
        switch(*s) {
            case '\\':
                switch(*(++s)) {
                    case 'a': *(p++) = '\a'; len++; break;
                    case 'b': *(p++) = '\b'; len++; break;
                    case 'e': *(p++) = '\033' /* '\e' is non ANSI-C */; len++; break;
                    case 'f': *(p++) = '\f'; len++; break;
                    case 'n': *(p++) = '\n'; len++; break;
                    case 'r': *(p++) = '\r'; len++; break;
                    case 't': *(p++) = '\t'; len++; break;
                    case 'v': *(p++) = '\v'; len++; break;

                    case '0':
                    case '1':
                    case '2':
                    case '3':
                    case '4':
                    case '5':
                    case '6':
                    case '7':
                    {
                        int c0 = 0;
                        int c1 = 0;
                        int c2 = 0;
                        int c = 0;

                        c0 = (*s) - '0';

                        if ( s[1] >= '0' && s[1] <= '7' ) {
                            c1 = c0;
                            c0 = (*++s) - '0';

                            if ( s[1] >= '0' && s[1] <= '7' ) {
                                c2 = c1;
                                c1 = c0;
                                c0 = (*++s) - '0';
                            }
                        }
                        c = (64 * c2) + (8 * c1) + c0;
                        *(p++) = (char) (c > 255 ? 255 : c);
                        len++;
                        break;
                    }

                    case 'x':
                    {
                        char c1 = *(s+1);
                        char c0 = *(s+2);

                        if (g_ascii_isxdigit(c1) && g_ascii_isxdigit(c0)) {
                            *(p++) = (ws_xton(c1) * 0x10) + ws_xton(c0);
                            s += 2;
                        } else {
                            *(p++) = *s;
                        }
                        len++;
                        break;
                    }
                    default:
                        *p++ = *s;
                        break;
                }
                break;
            default:
                *(p++) = *s;
                len++;
                break;
        }
    }

    if (len_p) *len_p = len;
    return buf;
}

char* uat_undquote(const char* si, guint in_len, guint* len_p) {
    return uat_unesc(si+1,in_len-2,len_p);
}


char* uat_esc(const char* buf, guint len) {
    const guint8* end = ((const guint8*)buf)+len;
    char* out = (char *)g_malloc0((4*len)+1);
    const guint8* b;
    char* s = out;

    for (b = (const guint8 *)buf; b < end; b++) {
        if (*b == '"' || *b == '\\' || ! g_ascii_isprint(*b) ) {
            snprintf(s,5,"\\x%02x",((guint)*b));
            s+=4;
        } else {
            *(s++) = (*b);
        }
    }

    return out;

}

gboolean uat_fld_chk_str_isprint(void* u1 _U_, const char* strptr, guint len, const void* u2 _U_, const void* u3 _U_, char** err) {
    guint i;

    for (i = 0; i < len; i++) {
        char c = strptr[i];
        if (! g_ascii_isprint(c)) {
            *err = ws_strdup_printf("invalid char pos=%d value=%02x", i, (guchar) c);
            return FALSE;
        }
    }
    *err = NULL;
    return TRUE;
}

gboolean uat_fld_chk_str_isalpha(void* u1 _U_, const char* strptr, guint len, const void* u2 _U_, const void* u3 _U_, char** err) {
    guint i;

    for (i = 0; i < len; i++) {
        char c = strptr[i];
        if (! g_ascii_isalpha(c)) {
            *err = ws_strdup_printf("invalid char pos=%d value=%02x", i, (guchar) c);
            return FALSE;
        }
    }
    *err = NULL;
    return TRUE;
}

gboolean uat_fld_chk_str_isalnum(void* u1 _U_, const char* strptr, guint len, const void* u2 _U_, const void* u3 _U_, char** err) {
    guint i;

    for (i = 0; i < len; i++) {
        char c = strptr[i];
        if (! g_ascii_isalnum(c)) {
            *err = ws_strdup_printf("invalid char pos=%d value=%02x", i, (guchar) c);
            return FALSE;
        }
    }
    *err = NULL;
    return TRUE;
}

gboolean uat_fld_chk_str_isdigit(void* u1 _U_, const char* strptr, guint len, const void* u2 _U_, const void* u3 _U_, char** err) {
    guint i;

    for (i = 0; i < len; i++) {
        char c = strptr[i];
        if (! g_ascii_isdigit(c)) {
            *err = ws_strdup_printf("invalid char pos=%d value=%02x", i, (guchar) c);
            return FALSE;
        }
    }
    *err = NULL;
    return TRUE;
}

gboolean uat_fld_chk_str_isxdigit(void* u1 _U_, const char* strptr, guint len, const void* u2 _U_, const void* u3 _U_, char** err) {
    guint i;

    for (i = 0; i < len; i++) {
        char c = strptr[i];
        if (! g_ascii_isxdigit(c)) {
            *err = ws_strdup_printf("invalid char pos=%d value=%02x", i, (guchar) c);
            return FALSE;
        }
    }
    *err = NULL;
    return TRUE;
}


/*
 * Editor modelines
 *
 * Local Variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * ex: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
