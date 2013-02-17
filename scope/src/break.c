/*
 *  break.c
 *
 *  Copyright 2012 Dimitar Toshkov Zhekov <dimitar.zhekov@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <gdk/gdkkeysyms.h>

#include "common.h"

enum
{
	BREAK_ID,
	BREAK_FILE,
	BREAK_LINE,
	BREAK_SCID,
	BREAK_TYPE,
	BREAK_ENABLED,
	BREAK_DISPLAY,
	BREAK_FUNC,
	BREAK_ADDR,
	BREAK_TIMES,
	BREAK_IGNORE,
	BREAK_COND,
	BREAK_SCRIPT,
	BREAK_PENDING,
	BREAK_LOCATION,
	BREAK_RUN_APPLY,
	BREAK_TEMPORARY,
	BREAK_DISCARD,
	BREAK_MISSING
};

static gint break_id_compare(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
	G_GNUC_UNUSED gpointer gdata)
{
	char *s1, *s2;
	gint result;

	gtk_tree_model_get(model, a, BREAK_ID, &s1, -1);
	gtk_tree_model_get(model, b, BREAK_ID, &s2, -1);
	result = utils_atoi0(s1) - utils_atoi0(s2);

	if (!result && s1 && s2)
	{
		const char *p1, *p2;

		for (p1 = s1; isdigit(*p1); p1++);
		for (p2 = s2; isdigit(*p2); p2++);

		result = atoi(p1 + (*p1 == '.')) - atoi(p2 + (*p2 == '.'));
	}

	g_free(s1);
	g_free(s2);
	return result;
}

static gint break_location_compare(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
	G_GNUC_UNUSED gpointer gdata)
{
	gint result = model_seek_compare(model, a, b, NULL);
	return result ? result : model_string_compare(model, a, b, BREAK_LOCATION);
}

static const char
	*const BP_CHARS   = "bhtfwwwaarrc?",
	*const BP_BREAKS  = "bh",
	*const BP_TRACES  = "tf",
	*const BP_HARDWS  = "hf",
	*const BP_BORTS   = "bhtf",
	*const BP_KNOWNS  = "btfwar",
	*const BP_WATCHES = "war",
	*const BP_WATOPTS = "ar";

typedef struct _BreakType
{
	const char *text;
	const char *type;
} BreakType;

static const BreakType break_types[] =
{
	{ "breakpoint",      "break" },
	{ "hw breakpoint",   "hbreak" },
	{ "tracepoint",      "trace" },
	{ "fast tracepoint", "ftrace" },
	{ "wpt",             "watch" },
	{ "watchpoint",      "watch" },
	{ "hw watchpoint",   "watch" },
	{ "hw-awpt",         "access" },
	{ "acc watchpoint",  "access" },
	{ "hw-rwpt",         "read" },
	{ "read watchpoint", "read" },
	{ "catchpoint",      "catch" },
	{ NULL,              "??" }
};

static void break_type_set_data_func(G_GNUC_UNUSED GtkTreeViewColumn *column,
	GtkCellRenderer *cell, GtkTreeModel *model, GtkTreeIter *iter,
	G_GNUC_UNUSED gpointer gdata)
{
	char type;
	gboolean temporary;
	GString *string = g_string_sized_new(0x0F);

	gtk_tree_model_get(model, iter, BREAK_TYPE, &type, BREAK_TEMPORARY, &temporary, -1);
	g_string_append(string, break_types[strchr(BP_CHARS, type) - BP_CHARS].type);

	if (temporary)
		g_string_append(string, ",t");

	g_object_set(cell, "text", string->str, NULL);
	g_string_free(string, TRUE);
}

static GtkListStore *store;
static GtkTreeModel *model;
static GtkTreeSortable *sortable;
static GtkTreeSelection *selection;
static gint scid_gen = 0;

static void break_mark(GtkTreeIter *iter, gboolean mark)
{
	char *file;
	gint line;
	gboolean enabled;

	gtk_tree_model_get(model, iter, BREAK_FILE, &file, BREAK_LINE, &line, BREAK_ENABLED,
		&enabled, -1);
	utils_mark(file, line, mark, MARKER_BREAKPT + enabled);
	g_free(file);
}

static void break_enable(GtkTreeIter *iter, gboolean enable)
{
	break_mark(iter, FALSE);
	gtk_list_store_set(store, iter, BREAK_ENABLED, enable, -1);
	break_mark(iter, TRUE);
}

static void on_break_enabled_toggled(G_GNUC_UNUSED GtkCellRendererToggle *renderer,
	gchar *path_str, G_GNUC_UNUSED gpointer gdata)
{
	GtkTreeIter iter;
	DebugState state = debug_state();
	char *id;
	gint scid;
	gboolean enabled;

	gtk_tree_model_get_iter_from_string(model, &iter, path_str);
	gtk_tree_model_get(model, &iter, BREAK_ID, &id, BREAK_SCID, &scid, BREAK_ENABLED,
		&enabled, -1);
	enabled ^= TRUE;

	if (state == DS_INACTIVE || !id)
	{
		break_enable(&iter, enabled);
	}
	else if (state & DS_SENDABLE)
	{
		debug_send_format(N, "02%d%d-break-%sable %s", enabled, scid, enabled ? "en" :
			"dis", id);
	}
	else
		plugin_beep();

	g_free(id);
}

#define EDITCOLS 3

static const char *break_command(gint index, char type)
{
	static const char *const break_commands[EDITCOLS] = { "after", "condition", "commands" };
	return !index && strchr(BP_TRACES, type) ? "passcount" : break_commands[index];
}

static void on_break_column_edited(G_GNUC_UNUSED GtkCellRendererText *renderer,
	gchar *path_str, gchar *new_text, gpointer gdata)
{
	gint index = GPOINTER_TO_INT(gdata) - 1;
	const gchar *set_text = validate_column(new_text, index > 0);
	GtkTreeIter iter;
	char *id;
	char type;

	gtk_tree_model_get_iter_from_string(model, &iter, path_str);
	gtk_tree_model_get(model, &iter, BREAK_ID, &id, BREAK_TYPE, &type, -1);

	if (id && (debug_state() & DS_SENDABLE))
	{
		char *locale = utils_get_locale_from_display(new_text, HB_DEFAULT);

		debug_send_format(F, "022%s-break-%s %s %s", id, break_command(index, type), id,
			locale ? locale : index ? "" : "0");
		g_free(locale);
	}
	else if (!id)
		gtk_list_store_set(store, &iter, index + BREAK_IGNORE, set_text, -1);
	else
		plugin_beep();

	g_free(id);
}

static void on_break_ignore_editing_started(G_GNUC_UNUSED GtkCellRenderer *cell,
	GtkCellEditable *editable, G_GNUC_UNUSED const gchar *path, G_GNUC_UNUSED gpointer gdata)
{
	if (GTK_IS_EDITABLE(editable))
		validator_attach(GTK_EDITABLE(editable), VALIDATOR_NUMERIC);

	if (GTK_IS_ENTRY(editable))
		gtk_entry_set_max_length(GTK_ENTRY(editable), 10);
}

static const TreeCell break_cells[] =
{
	{ "break_enabled", G_CALLBACK(on_break_enabled_toggled) },
	{ "break_ignore",  G_CALLBACK(on_break_column_edited)   },
	{ "break_cond",    G_CALLBACK(on_break_column_edited)   },
	{ "break_script",  G_CALLBACK(on_break_column_edited)   },
	{ NULL, NULL }
};

static void append_script_command(const ParseNode *node, GString *string)
{
	iff (node->type == PT_VALUE, "script: contains array")
	{
		gchar *display = utils_get_display_from_7bit((char *) node->value, HB_DEFAULT);
		const gchar *s;

		if (string->len)
			g_string_append_c(string, ' ');
		g_string_append_c(string, '"');

		for (s = display; *s; s++)
		{
			if (*s == '"' || *s == '\\')
				g_string_append_c(string, '\\');
			g_string_append_c(string, *s);
		}

		g_string_append_c(string, '"');
		g_free(display);
	}
}

typedef enum _BreakStage
{
	BG_PERSIST,
	BG_DISCARD,
	BG_APPLY,
	BG_GOTO,
	BG_NEXT
} BreakStage;

typedef struct _BreakData
{
	GtkTreeIter iter;
	char type;
	BreakStage stage;
} BreakData;

static void break_iter_applied(GtkTreeIter *iter, const char *id)
{
	gchar *columns[EDITCOLS];
	gboolean enabled;
	char type;
	gint index;

	gtk_tree_model_get(model, iter, BREAK_ENABLED, &enabled, BREAK_IGNORE, &columns[0],
		BREAK_COND, &columns[1], BREAK_SCRIPT, &columns[2], BREAK_TYPE, &type, -1);

	if (strchr(BP_BORTS, type))
	{
		if (strchr(BP_BREAKS, type))
		{
			g_free(columns[0]);
			columns[0] = NULL;
		}
		g_free(columns[1]);
		columns[1] = NULL;
	}
	else if (!enabled)
		debug_send_format(N, "-break-disable %s", id);

	for (index = 0; index < EDITCOLS; index++)
	{
		char *locale = utils_get_locale_from_display(columns[index], HB_DEFAULT);

		if (locale)
		{
			debug_send_format(F, "-break-%s %s %s", break_command(index, type), id,
				locale);
			g_free(locale);
		}
		g_free(columns[index]);
	}
}

static void break_node_parse(const ParseNode *node, BreakData *bd)
{
	GArray *nodes = (GArray *) node->value;
	const char *id;

	if (node->type == PT_VALUE)
	{
		dc_error("breaks: contains value");
		bd->stage = BG_DISCARD;
	}
	else if ((id = parse_find_value(nodes, "number")) == NULL)
	{
		dc_error("no number");
		bd->stage = BG_DISCARD;
	}
	else  /* enough data to parse */
	{
		const char *text_type = parse_find_value(nodes, "type");
		const BreakType *bt;
		char type;
		gboolean leading = !strchr(id, '.');
		gboolean borts;
		ParseLocation loc;
		gboolean enabled = g_strcmp0(parse_find_value(nodes, "enabled"), "n");
		GtkTreeIter *const iter = &bd->iter;
		const char *times = parse_find_value(nodes, "times");
		gboolean temporary = !g_strcmp0(parse_find_value(nodes, "disp"), "del");

		if (!text_type)
			text_type = node->name;

		for (bt = break_types; bt->text; bt++)
			if (!strcmp(text_type, bt->text))
				break;

		type = BP_CHARS[bt - break_types];

		if (leading || bd->stage != BG_NEXT || type != '?')
			bd->type = type;
		else
			type = bd->type;

		borts = strchr(BP_BORTS, type) != NULL;
		parse_location(nodes, &loc);

		if (bd->stage != BG_APPLY)
		{
			const ParseNode *script = parse_find_node(nodes, "script");
			GtkTreeIter iter1;

			if (model_find(model, &iter1, BREAK_ID, id))
			{
				bd->iter = iter1;
				break_mark(iter, FALSE);
			}
			else  /* new breakpoint */
			{
				const char *location = parse_find_locale(nodes, "original-location");
				char *original = g_strdup(location);
				gchar *display;
				gboolean persist = leading && bd->stage == BG_PERSIST;
				gboolean pending = parse_find_locale(nodes, "pending") != NULL;

				if (original)
				{
					char *split = strchr(original, ':');

					if (g_path_is_absolute(original) && split > original &&
						split[1] != ':')
					{
						*split++ = '\0';

						if (!loc.file)
							loc.file = original;

						if (isdigit(*split) && !loc.line)
							loc.line = atoi(split);
					}
				}
				else if (strchr(BP_WATCHES, type))
				{
					if ((location = parse_find_locale(nodes, "exp")) == NULL)
						location = parse_find_locale(nodes, "what");
				}

				if (!location || !strchr(BP_KNOWNS, type))
				{
					persist = FALSE;  /* can't create apply command */
					if (!location)
						location = loc.func;
				}

				display = borts ? utils_get_utf8_basename(location) :
					utils_get_display_from_locale(location, HB_DEFAULT);

				if (leading)
					gtk_list_store_append(store, iter);
				else
				{
					gtk_list_store_insert_after(store, &iter1, iter);
					bd->iter = iter1;
				}

				gtk_list_store_set(store, iter, BREAK_SCID, ++scid_gen, BREAK_TYPE,
					type, BREAK_DISPLAY, display, BREAK_PENDING, pending,
					BREAK_LOCATION, location, BREAK_RUN_APPLY, leading && borts,
					BREAK_DISCARD, !persist, -1);

				if (persist)
					utils_tree_set_cursor(selection, iter, 0.5);

				g_free(original);
				g_free(display);
			}

			utils_mark(loc.file, loc.line, TRUE, MARKER_BREAKPT + enabled);

			if (script)
			{
				GString *string = g_string_sized_new(0x3F);

				if (script->type == PT_VALUE)
					append_script_command(script, string);
				else
				{
					array_foreach((GArray *) script->value,
						(GFunc) append_script_command, string);
				}

				gtk_list_store_set(store, iter, BREAK_SCRIPT, string->str, -1);
				g_string_free(string, TRUE);
			}
			else
				gtk_list_store_set(store, iter, BREAK_SCRIPT, NULL, -1);
		}

		if (borts || bd->stage != BG_APPLY)
		{
			gchar *cond = utils_get_display_from_7bit(parse_find_value(nodes, "cond"),
				HB_DEFAULT);
			const char *ignore = parse_find_value(nodes, "ignore");

			gtk_list_store_set(store, iter, BREAK_ENABLED, enabled, BREAK_COND, cond,
				strchr(BP_BREAKS, type) || bd->stage != BG_APPLY ? BREAK_IGNORE : -1,
				ignore ? ignore : parse_find_value(nodes, "pass"), -1);
			g_free(cond);
		}

		gtk_list_store_set(store, iter, BREAK_ID, id, BREAK_FILE, loc.file, BREAK_LINE,
			loc.line, BREAK_FUNC, loc.func, BREAK_ADDR, loc.addr, BREAK_TIMES,
			utils_atoi0(times), BREAK_MISSING, FALSE, BREAK_TEMPORARY, temporary, -1);

		parse_location_free(&loc);

		if (bd->stage == BG_APPLY)
			break_iter_applied(iter, id);
		else if (bd->stage == BG_GOTO)
			debug_send_thread("-exec-continue");

		bd->stage = BG_NEXT;
	}
}

void on_break_inserted(GArray *nodes)
{
	const char *token = parse_grab_token(nodes);
	BreakData bd;

	bd.stage = BG_PERSIST;

	if (token)
	{
		if (*token == '0')
			bd.stage = BG_GOTO;
		else if (*token)
		{
			iff (model_find(model, &bd.iter, BREAK_SCID, token), "%s: b_scid not found",
				token)
			{
				bd.stage = BG_APPLY;
			}
		}
		else
			bd.stage = BG_DISCARD;
	}

	array_foreach(nodes, (GFunc) break_node_parse, &bd);
}

static void break_apply(GtkTreeIter *iter, gboolean thread)
{
	GString *command = g_string_sized_new(0x1FF);
	gint scid;
	char type;
	char *ignore, *location;
	gboolean enabled, pending, temporary;
	gchar *cond;
	gboolean borts;

	gtk_tree_model_get(model, iter, BREAK_SCID, &scid, BREAK_TYPE, &type, BREAK_ENABLED,
		&enabled, BREAK_IGNORE, &ignore, BREAK_COND, &cond, BREAK_LOCATION, &location,
		BREAK_PENDING, &pending, BREAK_TEMPORARY, &temporary, -1);

	borts = strchr(BP_BORTS, type) != NULL;
	g_string_append_printf(command, "02%d-break-%s", scid, borts ? "insert" : "watch");

	if (borts)
	{
		if (temporary)
			g_string_append(command, " -t");

		if (strchr(BP_HARDWS, type))
			g_string_append(command, " -h");

		if (strchr(BP_BREAKS, type))
		{
			if (ignore)
				g_string_append_printf(command, " -i %s", ignore);
		}
		else
			g_string_append(command, " -a");

		if (!enabled)
			g_string_append(command, " -d");

		if (cond)
		{
			char *locale = utils_get_locale_from_display(cond, HB_DEFAULT);
			g_string_append_printf(command, " -c \"%s\"", locale);
			g_free(locale);
		}

		if (pending)
			g_string_append(command, " -f");

		if (thread && thread_id)
			g_string_append_printf(command, " -p %s", thread_id);
	}
	else if (strchr(BP_WATOPTS, type))
		g_string_append_printf(command, " -%c", type);

	g_string_append_printf(command, " %s", location);
	debug_send_command(F, command->str);
	g_string_free(command, TRUE);
	g_free(ignore);
	g_free(location);
	g_free(cond);
}

static void break_clear(GtkTreeIter *iter)
{
	char type;

	gtk_tree_model_get(model, iter, BREAK_TYPE, &type, -1);
	gtk_list_store_set(store, iter, BREAK_ID, NULL, BREAK_ADDR, NULL,
		strchr(BP_BORTS, type) ? -1 : BREAK_TEMPORARY, FALSE, -1);
}

static gboolean break_remove(GtkTreeIter *iter)
{
	break_mark(iter, FALSE);
	return gtk_list_store_remove(store, iter);
}

static gboolean break_remove_all(const char *pref, gboolean force)
{
	GtkTreeIter iter;
	int len = strlen(pref);
	gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
	gboolean found = FALSE;

	while (valid)
	{
		char *id;
		gboolean discard;

		gtk_tree_model_get(model, &iter, BREAK_ID, &id, BREAK_DISCARD, &discard, -1);

		if (id && !strncmp(id, pref, len) && strchr(".", id[len]))
		{
			found = TRUE;

			if (discard || force)
			{
				valid = break_remove(&iter);
				g_free(id);
				continue;
			}

			break_clear(&iter);
		}

		g_free(id);
		valid = gtk_tree_model_iter_next(model, &iter);
	}

	return found;
}

void on_break_done(GArray *nodes)
{
	const char *token = parse_grab_token(nodes);
	GtkTreeIter iter;

	switch (*token)
	{
		case '0' :
		case '1' :
		{
			iff (model_find(model, &iter, BREAK_SCID, token + 1), "%s: b_scid not found",
				token)
			{
				break_enable(&iter, *token == '1');
			}
			break;
		}
		case '2' :
		{
			debug_send_format(N, "-break-info %s", token + 1);
			break;
		}
		case '3' :
		{
			if (!break_remove_all(token + 1, TRUE))
				dc_error("%s: bid not found", token);
			break;
		}
		default : dc_error("%s: invalid b_oper", token);
	}
}

static void break_iter_missing(GtkTreeIter *iter, G_GNUC_UNUSED gpointer gdata)
{
	gtk_list_store_set(store, iter, BREAK_MISSING, TRUE, -1);
}

static void breaks_missing(void)
{
	GtkTreeIter iter;
	gboolean valid = gtk_tree_model_get_iter_first(model, &iter);

	while (valid)
	{
		char *id;
		gboolean discard, missing;

		gtk_tree_model_get(model, &iter, BREAK_ID, &id, BREAK_DISCARD, &discard,
			BREAK_MISSING, &missing, -1);
		g_free(id);

		if (id && missing)
		{
			if (discard)
			{
				valid = break_remove(&iter);
				g_free(id);
				continue;
			}

			break_clear(&iter);
		}

		valid = gtk_tree_model_iter_next(model, &iter);
	}
}

void on_break_list(GArray *nodes)
{
	iff ((nodes = parse_find_array(parse_lead_array(nodes), "body")) != NULL, "no body")
	{
		gboolean refresh = parse_grab_token(nodes) != NULL;
		BreakData bd;

		if (refresh)
			model_foreach(model, (GFunc) break_iter_missing, NULL);

		bd.stage = BG_DISCARD;
		array_foreach(nodes, (GFunc) break_node_parse, &bd);

		if (refresh)
			breaks_missing();
	}
}

gint break_async = -1;

void on_break_stopped(GArray *nodes)
{
	if (break_async < TRUE)
	{
		const char *id = parse_find_value(nodes, "bkptno");
		const char *disp = parse_find_value(nodes, "disp");

		if (id && disp)
		{
			if (!strcmp(disp, "dis"))
			{
				GtkTreeIter iter;

				if (model_find(model, &iter, BREAK_ID, id))
					break_enable(&iter, FALSE);
			}
			else if (!strcmp(disp, "del"))
				break_remove_all(id, FALSE);
		}
	}

	on_thread_stopped(nodes);
}

void on_break_created(GArray *nodes)
{
#ifndef G_OS_UNIX
	if (!pref_async_break_bugs)
#endif
	{
		BreakData bd;
		bd.stage = BG_DISCARD;
		array_foreach(nodes, (GFunc) break_node_parse, &bd);
	}

	break_async = TRUE;
}

void on_break_deleted(GArray *nodes)
{
	break_remove_all(parse_lead_value(nodes), FALSE);
	break_async = TRUE;
}

static void break_feature_node_check(const ParseNode *node, G_GNUC_UNUSED gpointer gdata)
{
	if (!strcmp((const char *) node->value, "breakpoint-notifications"))
		break_async = TRUE;
}

void on_break_features(GArray *nodes)
{
	array_foreach(parse_lead_array(nodes), (GFunc) break_feature_node_check, NULL);
}

static void break_delete(GtkTreeIter *iter)
{
	char *id;

	gtk_tree_model_get(model, iter, BREAK_ID, &id, -1);

	if (debug_state() == DS_INACTIVE || !id)
		break_remove(iter);
	else
		debug_send_format(N, "023%s-break-delete %s", id, id);

	g_free(id);
}

static void break_iter_mark(GtkTreeIter *iter, GeanyDocument *doc)
{
	char *file;
	gint line;
	gboolean enabled;

	gtk_tree_model_get(model, iter, BREAK_FILE, &file, BREAK_LINE, &line,
		BREAK_ENABLED, &enabled, -1);

	if (line && !utils_filenamecmp(file, doc->real_path))
		sci_set_marker_at_line(doc->editor->sci, line - 1, MARKER_BREAKPT + enabled);
	g_free(file);
}

void breaks_mark(GeanyDocument *doc)
{
	if (doc->real_path)
		model_foreach(model, (GFunc) break_iter_mark, doc);
}

void breaks_clear(void)
{
	GtkTreeIter iter;
	gboolean valid = gtk_tree_model_get_iter_first(model, &iter);

	while (valid)
	{
		gboolean discard;

		gtk_tree_model_get(model, &iter, BREAK_DISCARD, &discard, -1);

		if (discard)
			valid = break_remove(&iter);
		else
		{
			break_clear(&iter);
			valid = gtk_tree_model_iter_next(model, &iter);
		}
	}
}

static void break_iter_reset(GtkTreeIter *iter, G_GNUC_UNUSED gpointer gdata)
{
	gtk_list_store_set(store, iter, BREAK_TIMES, 0, -1);
}

void breaks_reset(void)
{
	model_foreach(model, (GFunc) break_iter_reset, NULL);
}

static void break_iter_apply(GtkTreeIter *iter, G_GNUC_UNUSED gpointer gdata)
{
	gboolean run_apply;

	gtk_tree_model_get(model, iter, BREAK_RUN_APPLY, &run_apply, -1);

	if (run_apply)
		break_apply(iter, FALSE);
}

void breaks_apply(void)
{
	model_foreach(model, (GFunc) break_iter_apply, NULL);
}

void breaks_query_async(GString *commands)
{
	if (break_async == -1)
	{
		break_async = FALSE;
		g_string_append(commands, "05-list-features\n");
	}
}

static void break_relocate(GtkTreeIter *iter, const char *real_path, gint line)
{
	char *location = g_strdup_printf("%s:%d", real_path, line);
	gchar *display = utils_get_utf8_basename(location);

	gtk_list_store_set(store, iter, BREAK_FILE, real_path, BREAK_LINE, line, BREAK_DISPLAY,
		display, BREAK_LOCATION, location, -1);

	g_free(display);
	g_free(location);
}

void breaks_delta(ScintillaObject *sci, const char *real_path, gint start, gint delta,
	gboolean active)
{
	GtkTreeIter iter;
	gboolean valid = gtk_tree_model_get_iter_first(model, &iter);

	while (valid)
	{
		char *file;
		gint line;
		gboolean enabled;
		char *location;

		gtk_tree_model_get(model, &iter, BREAK_FILE, &file, BREAK_LINE, &line,
			BREAK_ENABLED, &enabled, BREAK_LOCATION, &location, -1);

		if (--line >= 0 && start <= line && !utils_filenamecmp(file, real_path))
		{
			if (active)
			{
				utils_move_mark(sci, line, start, delta, MARKER_BREAKPT + enabled);
			}
			else if (delta > 0 || start - delta <= line)
			{
				char *split = strchr(location, ':');

				line += delta + 1;

				if (split && isdigit(split[1]))
					break_relocate(&iter, real_path, line);
				else
					gtk_list_store_set(store, &iter, BREAK_LINE, line, -1);
			}
			else
			{
				sci_delete_marker_at_line(sci, start, MARKER_BREAKPT + enabled);
				valid = gtk_list_store_remove(store, &iter);
				g_free(file);
				g_free(location);
				continue;
			}
		}

		g_free(file);
		g_free(location);
		valid = gtk_tree_model_iter_next(model, &iter);
	}
}

static void break_iter_check(GtkTreeIter *iter, guint *active)
{
	char *id;
	gboolean enabled;

	gtk_tree_model_get(model, iter, BREAK_ID, &id, BREAK_ENABLED, &enabled, -1);
	*active += enabled && id;
	g_free(id);
}

guint breaks_active(void)
{
	guint active = 0;
	model_foreach(model, (GFunc) break_iter_check, &active);
	return active;
}

void on_break_toggle(G_GNUC_UNUSED const MenuItem *menu_item)
{
	GeanyDocument *doc = document_get_current();
	gint doc_line = utils_current_line(doc);
	GtkTreeIter iter, iter1;
	gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
	gint found = 0;

	while (valid)
	{
		char *id, *file;
		gint line;

		gtk_tree_model_get(model, &iter, BREAK_ID, &id, BREAK_FILE, &file, BREAK_LINE,
			&line, -1);

		if (line == doc_line && !utils_filenamecmp(file, doc->real_path))
		{
			if (found && found != utils_atoi0(id))
			{
				dialogs_show_msgbox(GTK_MESSAGE_INFO,
					_("There are two or more breakpoints at %s:%d.\n"
					"Use the breakpoint list to remove the exact one."),
					doc->file_name, doc_line);
				g_free(id);
				g_free(file);
				return;
			}

			found = id ? atoi(id) : -1;
			iter1 = iter;
		}

		g_free(id);
		g_free(file);
		valid = gtk_tree_model_iter_next(model, &iter);
	}

	if (found)
		break_delete(&iter1);
	else if (debug_state() != DS_INACTIVE)
		debug_send_format(N, "-break-insert %s:%d", doc->real_path, doc_line);
	else
	{
		gtk_list_store_append(store, &iter);
		break_relocate(&iter, doc->real_path, doc_line);
		gtk_list_store_set(store, &iter, BREAK_SCID, ++scid_gen, BREAK_TYPE, 'b',
			BREAK_ENABLED, TRUE, BREAK_RUN_APPLY, TRUE, -1);
		utils_tree_set_cursor(selection, &iter, 0.5);
		sci_set_marker_at_line(doc->editor->sci, doc_line - 1, MARKER_BREAKPT + TRUE);
	}
}

gboolean breaks_update(void)
{
	debug_send_command(N, "04-break-list");
	return TRUE;
}

static void break_iter_unmark(GtkTreeIter *iter, G_GNUC_UNUSED gpointer gdata)
{
	break_mark(iter, FALSE);
}

void breaks_delete_all(void)
{
	model_foreach(model, (GFunc) break_iter_unmark, NULL);
	gtk_list_store_clear(store);
	scid_gen = 0;
}

enum
{
	STRING_FILE,
	STRING_DISPLAY,
	STRING_FUNC,
	STRING_IGNORE,
	STRING_COND,
	STRING_SCRIPT,
	STRING_LOCATION,
	STRING_COUNT
};

static const char *string_names[STRING_COUNT] = { "file", "display", "func", "ignore", "cond",
	"script", "location" };

static gboolean break_load(GKeyFile *config, const char *section)
{
	guint i;
	gint line, type;
	gboolean enabled, pending, run_apply, temporary;
	char *strings[STRING_COUNT];
	gboolean valid = FALSE;

	line = utils_get_setting_integer(config, section, "line", 0);
	type = utils_get_setting_integer(config, section, "type", 0);
	enabled = utils_get_setting_boolean(config, section, "enabled", TRUE);
	pending = utils_get_setting_boolean(config, section, "pending", FALSE);
	run_apply = utils_get_setting_boolean(config, section, "run_apply",
		strchr(BP_BORTS, type) != NULL);
	temporary = utils_get_setting_boolean(config, section, "temporary", FALSE);
	for (i = 0; i < STRING_COUNT; i++)
		strings[i] = utils_key_file_get_string(config, section, string_names[i]);

	if (type && strchr(BP_KNOWNS, type) && strings[STRING_LOCATION] && line >= 0)
	{
		GtkTreeIter iter;
		char *ignore = validate_column(strings[STRING_IGNORE], FALSE);

		if (!strings[STRING_FILE])
			line = 0;

		gtk_list_store_append(store, &iter);
		gtk_list_store_set(store, &iter, BREAK_FILE, strings[STRING_FILE], BREAK_LINE,
			line, BREAK_SCID, ++scid_gen, BREAK_TYPE, type, BREAK_ENABLED, enabled,
			BREAK_DISPLAY, strings[STRING_DISPLAY], BREAK_FUNC, strings[STRING_FUNC],
			BREAK_IGNORE, ignore, BREAK_COND, strings[STRING_COND], BREAK_SCRIPT,
			strings[STRING_SCRIPT], BREAK_PENDING, pending, BREAK_LOCATION,
			strings[STRING_LOCATION], BREAK_RUN_APPLY, run_apply, BREAK_TEMPORARY,
			temporary, -1);
		break_mark(&iter, TRUE);
		valid = TRUE;
	}

	for (i = 0; i < STRING_COUNT; i++)
		g_free(strings[i]);

	return valid;
}

void breaks_load(GKeyFile *config)
{
	breaks_delete_all();
	utils_load(config, "break", break_load);
}

static gboolean break_save(GKeyFile *config, const char *section, GtkTreeIter *iter)
{
	gboolean discard;

	gtk_tree_model_get(model, iter, BREAK_DISCARD, &discard, -1);

	if (!discard)
	{
		guint i;
		gint line;
		char type;
		gboolean enabled, pending, run_apply, temporary;
		char *strings[STRING_COUNT];

		gtk_tree_model_get(model, iter, BREAK_FILE, &strings[STRING_FILE], BREAK_LINE,
			&line, BREAK_TYPE, &type, BREAK_ENABLED, &enabled, BREAK_DISPLAY,
			&strings[STRING_DISPLAY], BREAK_FUNC, &strings[STRING_FUNC], BREAK_IGNORE,
			&strings[STRING_IGNORE], BREAK_COND, &strings[STRING_COND], BREAK_SCRIPT,
			&strings[STRING_SCRIPT], BREAK_PENDING, &pending, BREAK_LOCATION,
			&strings[STRING_LOCATION], BREAK_RUN_APPLY, &run_apply, BREAK_TEMPORARY,
			&temporary, -1);

		if (line)
			g_key_file_set_integer(config, section, "line", line);
		else
			g_key_file_remove_key(config, section, "line", NULL);

		g_key_file_set_integer(config, section, "type", type);
		g_key_file_set_boolean(config, section, "enabled", enabled);
		g_key_file_set_boolean(config, section, "pending", pending);
		g_key_file_set_boolean(config, section, "run_apply", run_apply);

		for (i = 0; i < STRING_COUNT; i++)
		{
			if (strings[i])
				utils_key_file_set_string(config, section, string_names[i], strings[i]);
			else
				g_key_file_remove_key(config, section, string_names[i], NULL);
		}

		if (strchr(BP_BORTS, type))
			g_key_file_set_boolean(config, section, "temporary", temporary);
		else
			g_key_file_remove_key(config, section, "temporary", NULL);

		return TRUE;
	}

	return FALSE;
}

void breaks_save(GKeyFile *config)
{
	model_save(model, config, "break", break_save);
}

static GObject *block_cells[EDITCOLS];

static void on_break_selection_changed(GtkTreeSelection *selection,
	G_GNUC_UNUSED gpointer gdata)
{
	GtkTreeIter iter;

	if (gtk_tree_selection_get_selected(selection, NULL, &iter))
	{
		char *id;
		gboolean editable;
		gint index;

		gtk_tree_model_get(model, &iter, BREAK_ID, &id, -1);
		editable = !id || !strchr(id, '.');
		for (index = 0; index < EDITCOLS; index++)
			g_object_set(block_cells[index], "editable", editable, NULL);
		g_free(id);
	}
}

static GtkTreeView *tree;

static gboolean on_break_query_tooltip(G_GNUC_UNUSED GtkWidget *widget, gint x, gint y,
	gboolean keyboard_tip, GtkTooltip *tooltip, GtkTreeViewColumn *break_display_column)
{
	GtkTreeIter iter;
	gboolean has_tip = FALSE;

	if (gtk_tree_view_get_tooltip_context(tree, &x, &y, keyboard_tip, NULL, NULL, &iter))
	{
		char *file, *func;
		gint line;
		GString *text = g_string_sized_new(0xFF);

		gtk_tree_view_set_tooltip_cell(tree, tooltip, NULL, break_display_column, NULL);
		gtk_tree_model_get(model, &iter, BREAK_FILE, &file, BREAK_LINE, &line, BREAK_FUNC,
			&func, -1);

		if (file)
		{
			g_string_append(text, file);
			if (line)
				g_string_append_printf(text, ":%d", line);
			has_tip = TRUE;
			g_free(file);
		}

		if (func)
		{
			if (has_tip)
				g_string_append(text, ", ");
			g_string_append(text, func);
			has_tip = TRUE;
			g_free(func);
		}

		gtk_tooltip_set_text(tooltip, text->str);
		g_string_free(text, TRUE);
	}

	return has_tip;
}

static void on_break_refresh(G_GNUC_UNUSED const MenuItem *menu_item)
{
	debug_send_command(N, "02-break-list");
}

static void on_break_unsorted(G_GNUC_UNUSED const MenuItem *menu_item)
{
	gtk_tree_sortable_set_sort_column_id(sortable, BREAK_SCID, GTK_SORT_ASCENDING);
}

static void on_break_insert(G_GNUC_UNUSED const MenuItem *menu_item)
{
	GeanyDocument *doc = document_get_current();
	GString *command = g_string_new("-break-insert ");

	if (doc && utils_source_document(doc))
		g_string_append_printf(command, "%s:%d", doc->file_name, utils_current_line(doc));

	view_command_line(command->str, _("Add Breakpoint"), " ", TRUE);
	g_string_free(command, TRUE);
}

static void on_break_watch(G_GNUC_UNUSED const MenuItem *menu_item)
{
	gchar *expr = utils_get_default_selection();
	GString *command = g_string_new("-break-watch ");

	if (expr)
	{
		g_string_append(command, expr);
		g_free(expr);
	}

	view_command_line(command->str, _("Add Watchpoint"), " ", TRUE);
	g_string_free(command, TRUE);
}

static void on_break_apply(const MenuItem *menu_item)
{
	if (menu_item || thread_id)
	{
		GtkTreeIter iter;
		gtk_tree_selection_get_selected(selection, NULL, &iter);
		break_apply(&iter, !menu_item);
	}
	else
		plugin_beep();
}

static void on_break_run_apply(const MenuItem *menu_item)
{
	GtkTreeIter iter;

	gtk_tree_selection_get_selected(selection, NULL, &iter);
	gtk_list_store_set(store, &iter, BREAK_RUN_APPLY,
		gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menu_item->widget)), -1);
}

static void on_break_delete(G_GNUC_UNUSED const MenuItem *menu_item)
{
	GtkTreeIter iter;
	gtk_tree_selection_get_selected(selection, NULL, &iter);
	break_delete(&iter);
}

static void break_seek_selected(gboolean focus)
{
	GtkTreeViewColumn *column;

	gtk_tree_view_get_cursor(tree, NULL, &column);

	if (column)
	{
		static const char *unseeks[] = { "break_enabled_column", "break_ignore_column",
			"break_cond_column", "break_script_column", NULL };
		const char *name = gtk_buildable_get_name(GTK_BUILDABLE(column));
		const char **unseek;

		for (unseek = unseeks; *unseek; unseek++)
			if (!strcmp(*unseek, name))
				return;
	}

	view_seek_selected(selection, focus, SK_DEFAULT);
}

static void on_break_view_source(G_GNUC_UNUSED const MenuItem *menu_item)
{
	view_seek_selected(selection, FALSE, SK_DEFAULT);
}

#define DS_VIEWABLE (DS_BASICS | DS_EXTRA_2)
#define DS_APPLIABLE (DS_SENDABLE | DS_EXTRA_1)
#define DS_RUN_APPLY (DS_BASICS | DS_EXTRA_3)
#define DS_DELETABLE (DS_NOT_BUSY | DS_EXTRA_3)

static MenuItem break_menu_items[] =
{
	{ "break_refresh",     on_break_refresh,     DS_SENDABLE,  NULL, NULL },
	{ "break_unsorted",    on_break_unsorted,    0,            NULL, NULL },
	{ "break_view_source", on_break_view_source, DS_VIEWABLE,  NULL, NULL },
	{ "break_insert",      on_break_insert,      DS_SENDABLE,  NULL, NULL },
	{ "break_watch",       on_break_watch,       DS_SENDABLE,  NULL, NULL },
	{ "break_apply",       on_break_apply,       DS_APPLIABLE, NULL, NULL },
	{ "break_run_apply",   on_break_run_apply,   DS_RUN_APPLY, NULL, NULL },
	{ "break_delete",      on_break_delete,      DS_DELETABLE, NULL, NULL },
	{ NULL, NULL, 0, NULL, NULL }
};

static guint break_menu_extra_state(void)
{
	GtkTreeIter iter;

	if (gtk_tree_selection_get_selected(selection, NULL, &iter))
	{
		char *id, *file;

		gtk_tree_model_get(model, &iter, BREAK_ID, &id, BREAK_FILE, &file, -1);
		g_free(id);
		g_free(file);

		return (!id << DS_INDEX_1) | ((file != NULL) << DS_INDEX_2) |
			((!id || !strchr(id, '.')) << DS_INDEX_3);
	}

	return 0;
}

static MenuInfo break_menu_info = { break_menu_items, break_menu_extra_state, 0 };

static gboolean on_break_key_press(GtkWidget *widget, GdkEventKey *event,
	G_GNUC_UNUSED gpointer gdata)
{
	return menu_insert_delete(event, &break_menu_info, "break_insert", "break_delete") ||
		on_view_key_press(widget, event, break_seek_selected);
}

static void on_break_menu_show(G_GNUC_UNUSED GtkWidget *widget, const MenuItem *menu_item)
{
	GtkTreeIter iter;

	if (gtk_tree_selection_get_selected(selection, NULL, &iter))
	{
		gboolean run_apply;
		gtk_tree_model_get(model, &iter, BREAK_RUN_APPLY, &run_apply, -1);
		menu_item_set_active(menu_item, run_apply);
	}
}

static void on_break_apply_button_release(GtkWidget *widget, GdkEventButton *event,
	GtkWidget *menu)
{
	menu_shift_button_release(widget, event, menu, on_break_apply);
}

void break_init(void)
{
	GtkWidget *menu;
	guint i;

	tree = view_connect("break_view", &model, &selection, break_cells, "break_window", NULL);
	store = GTK_LIST_STORE(model);
	sortable = GTK_TREE_SORTABLE(store);
	gtk_tree_view_column_set_cell_data_func(get_column("break_type_column"),
		GTK_CELL_RENDERER(get_object("break_type")), break_type_set_data_func, NULL, NULL);
	g_signal_connect(get_object("break_ignore"), "editing-started",
		G_CALLBACK(on_break_ignore_editing_started), NULL);
	view_set_sort_func(sortable, BREAK_ID, break_id_compare);
	view_set_sort_func(sortable, BREAK_IGNORE, model_gint_compare);
	view_set_sort_func(sortable, BREAK_LOCATION, break_location_compare);

	for (i = 0; i < EDITCOLS; i++)
		block_cells[i] = get_object(break_cells[i + 1].name);
	g_signal_connect(selection, "changed", G_CALLBACK(on_break_selection_changed), NULL);
	gtk_widget_set_has_tooltip(GTK_WIDGET(tree), TRUE);
	g_signal_connect(tree, "query-tooltip", G_CALLBACK(on_break_query_tooltip),
		get_column("break_display_column"));

	menu = menu_select("break_menu", &break_menu_info, selection);
	g_signal_connect(tree, "key-press-event", G_CALLBACK(on_break_key_press), NULL);
	g_signal_connect(tree, "button-press-event", G_CALLBACK(on_view_button_1_press),
		break_seek_selected);
	g_signal_connect(menu, "show", G_CALLBACK(on_break_menu_show),
		(gpointer) menu_item_find(break_menu_items, "break_run_apply"));
	g_signal_connect(get_widget("break_apply"), "button-release-event",
		G_CALLBACK(on_break_apply_button_release), menu);
}

void break_finalize(void)
{
	model_foreach(model, (GFunc) break_iter_unmark, NULL);
}
