/** @file
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 2001 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef DFILTER_H
#define DFILTER_H

#include <wireshark.h>

#include "dfilter-loc.h"
#include <epan/proto.h>

/* Passed back to user */
typedef struct epan_dfilter dfilter_t;

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct epan_dissect;

#define DF_ERROR_GENERIC		-1
#define DF_ERROR_UNEXPECTED_END		-2

typedef struct {
	int code;
	char *msg;
	df_loc_t loc;
} df_error_t;

df_error_t *
df_error_new(int code, char *msg, df_loc_t *loc);

df_error_t *
df_error_new_printf(int code, df_loc_t *loc, const char *fmt, ...)
G_GNUC_PRINTF(3, 4);

#define df_error_new_msg(msg) \
	df_error_new_printf(DF_ERROR_GENERIC, NULL, "%s", msg)

df_error_t *
df_error_new_vprintf(int code, df_loc_t *loc, const char *fmt, va_list ap);

WS_DLL_PUBLIC
void
df_error_free(df_error_t **ep);

/* Module-level initialization */
void
dfilter_init(void);

/* Module-level cleanup */
void
dfilter_cleanup(void);

/* Perform macro expansion. */
WS_DLL_PUBLIC
char *
dfilter_expand(const char *expr, df_error_t **err_ret);

/* Save textual representation of syntax tree (for debugging purposes). */
#define DF_SAVE_TREE		(1U << 0)
/* Perform macro substitution on filter text. */
#define DF_EXPAND_MACROS	(1U << 1)
/* Do an optimization pass on the compiled filter. */
#define DF_OPTIMIZE		(1U << 2)
/* Enable debug trace for flex. */
#define DF_DEBUG_FLEX		(1U << 3)
/* Enable debug trace for lemon. */
#define DF_DEBUG_LEMON		(1U << 4)

/* Compiles a string to a dfilter_t.
 * On success, sets the dfilter* pointed to by dfp
 * to either a NULL pointer (if the filter is a null
 * filter, as generated by an all-blank string) or to
 * a pointer to the newly-allocated dfilter_t
 * structure.
 *
 * On failure, *err_msg is set to point to the error
 * message.  This error message is allocated with
 * g_malloc(), and must be freed with g_free().
 * The dfilter* will be set to NULL after a failure.
 *
 * Returns TRUE on success, FALSE on failure.
 */
WS_DLL_PUBLIC
gboolean
dfilter_compile_full(const gchar *text, dfilter_t **dfp,
			df_error_t **errpp, unsigned flags,
			const char *caller);

#define dfilter_compile(text, dfp, errp) \
	dfilter_compile_full(text, dfp, errp, \
				DF_EXPAND_MACROS|DF_OPTIMIZE, \
				__func__)

/* Frees all memory used by dfilter, and frees
 * the dfilter itself. */
WS_DLL_PUBLIC
void
dfilter_free(dfilter_t *df);

/* Apply compiled dfilter */
WS_DLL_PUBLIC
gboolean
dfilter_apply_edt(dfilter_t *df, struct epan_dissect *edt);

/* Apply compiled dfilter */
gboolean
dfilter_apply(dfilter_t *df, proto_tree *tree);

/* Prime a proto_tree using the fields/protocols used in a dfilter. */
void
dfilter_prime_proto_tree(const dfilter_t *df, proto_tree *tree);

/* Refresh references in a compiled display filter. */
WS_DLL_PUBLIC
void
dfilter_load_field_references(const dfilter_t *df, proto_tree *tree);

/* Refresh references in a compiled display filter. */
WS_DLL_PUBLIC
void
dfilter_load_field_references_edt(const dfilter_t *df, struct epan_dissect *edt);

/* Check if dfilter has interesting fields */
gboolean
dfilter_has_interesting_fields(const dfilter_t *df);

/* Check if dfilter is interested in a given field
 *
 * @param df The dfilter
 * @param hfid The header field info ID to check
 * @return TRUE if the field is interesting to the dfilter
 */
gboolean
dfilter_interested_in_field(const dfilter_t *df, int hfid);

/* Check if dfilter is interested in a given protocol
 *
 * @param df The dfilter
 * @param hfid The protocol ID to check
 * @return TRUE if the dfilter is interested in a field whose
 * parent is proto_id
 */
gboolean
dfilter_interested_in_proto(const dfilter_t *df, int proto_id);

WS_DLL_PUBLIC
GPtrArray *
dfilter_deprecated_tokens(dfilter_t *df);

WS_DLL_PUBLIC
GSList *
dfilter_get_warnings(dfilter_t *df);

#define DF_DUMP_REFERENCES	(1U << 0)
#define DF_DUMP_SHOW_FTYPE	(1U << 1)

/* Print bytecode of dfilter to fp */
WS_DLL_PUBLIC
void
dfilter_dump(FILE *fp, dfilter_t *df, uint16_t flags);

/* Text after macro expansion. */
WS_DLL_PUBLIC
const char *
dfilter_text(dfilter_t *df);

/* Text representation of syntax tree (if it was saved, NULL oterwise). */
WS_DLL_PUBLIC
const char *
dfilter_syntax_tree(dfilter_t *df);

/* Print bytecode of dfilter to log */
WS_DLL_PUBLIC
void
dfilter_log_full(const char *domain, enum ws_log_level level,
			const char *file, long line, const char *func,
			dfilter_t *dfcode, const char *msg);

#ifdef WS_DEBUG
#define dfilter_log(dfcode, msg) \
	dfilter_log_full(LOG_DOMAIN_DFILTER, LOG_LEVEL_NOISY,	\
				__FILE__, __LINE__, __func__,	\
				dfcode, msg)
#else
#define dfilter_log(dfcode, msg) (void)0
#endif

#define DFILTER_DEBUG_HERE(dfcode) \
	dfilter_log_full(LOG_DOMAIN_DFILTER, LOG_LEVEL_ECHO,	\
				__FILE__, __LINE__, __func__,	\
				dfcode, #dfcode);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* DFILTER_H */
