/*
   Editor low level data handling and cursor fundamentals.

   Copyright (C) 1996, 1997, 1998, 2001, 2002, 2003, 2004, 2005, 2006,
   2007, 2008, 2009, 2010, 2011, 2012
   The Free Software Foundation, Inc.

   Written by:
   Paul Sheer 1996, 1997
   Ilia Maslakov <il.smind@gmail.com> 2009, 2010, 2011
   Andrew Borodin <aborodin@vmail.ru> 2012.

   This file is part of the Midnight Commander.

   The Midnight Commander is free software: you can redistribute it
   and/or modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the License,
   or (at your option) any later version.

   The Midnight Commander is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/** \file
 *  \brief Source: editor low level data handling and cursor fundamentals
 *  \author Paul Sheer
 *  \date 1996, 1997
 */

#include <config.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <fcntl.h>

#include "lib/global.h"

#include "lib/tty/color.h"
#include "lib/tty/tty.h"        /* attrset() */
#include "lib/tty/key.h"        /* is_idle() */
#include "lib/skin.h"           /* EDITOR_NORMAL_COLOR */
#include "lib/vfs/vfs.h"
#include "lib/strutil.h"        /* utf string functions */
#include "lib/util.h"           /* load_file_position(), save_file_position() */
#include "lib/timefmt.h"        /* time formatting */
#include "lib/lock.h"
#include "lib/widget.h"

#ifdef HAVE_CHARSET
#include "lib/charsets.h"       /* get_codepage_id */
#endif

#include "src/filemanager/usermenu.h"   /* user_menu_cmd() */

#include "src/setup.h"          /* option_tab_spacing */
#include "src/learn.h"          /* learn_keys */
#include "src/keybind-defaults.h"

#include "edit-impl.h"
#include "editwidget.h"
#ifdef HAVE_ASPELL
#include "spell.h"
#endif

/*** global variables ****************************************************************************/

int option_word_wrap_line_length = DEFAULT_WRAP_LINE_LENGTH;
int option_typewriter_wrap = 0;
int option_auto_para_formatting = 0;
int option_fill_tabs_with_spaces = 0;
int option_return_does_auto_indent = 1;
int option_backspace_through_tabs = 0;
int option_fake_half_tabs = 1;
int option_save_mode = EDIT_QUICK_SAVE;
int option_save_position = 1;
int option_max_undo = 32768;
int option_persistent_selections = 1;
int option_cursor_beyond_eol = 0;
int option_line_state = 0;
int option_line_state_width = 0;
gboolean option_cursor_after_inserted_block = FALSE;

int option_edit_right_extreme = 0;
int option_edit_left_extreme = 0;
int option_edit_top_extreme = 0;
int option_edit_bottom_extreme = 0;
int enable_show_tabs_tws = 1;
int option_check_nl_at_eof = 0;
int option_group_undo = 0;
int show_right_margin = 0;

const char *option_whole_chars_search = "0123456789abcdefghijklmnopqrstuvwxyz_";
char *option_backup_ext = NULL;

unsigned int edit_stack_iterator = 0;
edit_stack_type edit_history_moveto[MAX_HISTORY_MOVETO];
/* magic sequense for say than block is vertical */
const char VERTICAL_MAGIC[] = { '\1', '\1', '\1', '\1', '\n' };

/*** file scope macro definitions ****************************************************************/

#define TEMP_BUF_LEN 1024

#define space_width 1

/*** file scope type declarations ****************************************************************/

/*** file scope variables ************************************************************************/

/* detecting an error on save is easy: just check if every byte has been written. */
/* detecting an error on read, is not so easy 'cos there is not way to tell
   whether you read everything or not. */
/* FIXME: add proper `triple_pipe_open' to read, write and check errors. */
static const struct edit_filters
{
    const char *read, *write, *extension;
} all_filters[] =
{
    /* *INDENT-OFF* */
    { "xz -cd %s 2>&1", "xz > %s", ".xz"},
    { "lzma -cd %s 2>&1", "lzma > %s", ".lzma" },
    { "bzip2 -cd %s 2>&1", "bzip2 > %s", ".bz2" },
    { "gzip -cd %s 2>&1", "gzip > %s", ".gz" },
    { "gzip -cd %s 2>&1", "gzip > %s", ".Z" }
    /* *INDENT-ON* */
};

static off_t last_bracket = -1;

/*** file scope functions ************************************************************************/
/* --------------------------------------------------------------------------------------------- */

/*-
 *
 * here's a quick sketch of the layout: (don't run this through indent.)
 *
 * (b1 is buffers1 and b2 is buffers2)
 *
 *                                       |
 * \0\0\0\0\0m e _ f i l e . \nf i n . \n|T h i s _ i s _ s o\0\0\0\0\0\0\0\0\0
 * ______________________________________|______________________________________
 *                                       |
 * ...  |  b2[2]   |  b2[1]   |  b2[0]   |  b1[0]   |  b1[1]   |  b1[2]   | ...
 *      |->        |->        |->        |->        |->        |->        |
 *                                       |
 *           _<------------------------->|<----------------->_
 *                   WEdit->curs2        |   WEdit->curs1
 *           ^                           |                   ^
 *           |                          ^|^                  |
 *         cursor                       |||                cursor
 *                                      |||
 *                              file end|||file beginning
 *                                       |
 *                                       |
 *
 *           _
 * This_is_some_file
 * fin.
 */

/* --------------------------------------------------------------------------------------------- */
/**
 * Initialize the buffers for an empty files.
 */

static void
edit_init_buffers (WEdit * edit)
{
    int j;

    for (j = 0; j <= MAXBUFF; j++)
    {
        edit->buffers1[j] = NULL;
        edit->buffers2[j] = NULL;
    }

    edit->curs1 = 0;
    edit->curs2 = 0;
    edit->buffers2[0] = g_malloc0 (EDIT_BUF_SIZE);
}

/* --------------------------------------------------------------------------------------------- */

/**
 * Load file OR text into buffers.  Set cursor to the beginning of file.
 *
 * @return FALSE on error.
 */

static gboolean
edit_load_file_fast (WEdit * edit, const vfs_path_t * filename_vpath)
{
    off_t buf, buf2;
    int file = -1;
    gboolean ret = FALSE;

    edit->curs2 = edit->last_byte;
    buf2 = edit->curs2 >> S_EDIT_BUF_SIZE;

    file = mc_open (filename_vpath, O_RDONLY | O_BINARY);
    if (file == -1)
    {
        gchar *errmsg, *filename;

        filename = vfs_path_to_str (filename_vpath);
        errmsg = g_strdup_printf (_("Cannot open %s for reading"), filename);
        g_free (filename);
        edit_error_dialog (_("Error"), errmsg);
        g_free (errmsg);
        return FALSE;
    }

    if (!edit->buffers2[buf2])
        edit->buffers2[buf2] = g_malloc0 (EDIT_BUF_SIZE);

    do
    {
        if (mc_read (file,
                     (char *) edit->buffers2[buf2] + EDIT_BUF_SIZE -
                     (edit->curs2 & M_EDIT_BUF_SIZE), edit->curs2 & M_EDIT_BUF_SIZE) < 0)
            break;

        for (buf = buf2 - 1; buf >= 0; buf--)
        {
            /* edit->buffers2[0] is already allocated */
            if (!edit->buffers2[buf])
                edit->buffers2[buf] = g_malloc0 (EDIT_BUF_SIZE);
            if (mc_read (file, (char *) edit->buffers2[buf], EDIT_BUF_SIZE) < 0)
                break;
        }
        ret = TRUE;
    }
    while (FALSE);

    if (!ret)
    {
        gchar *errmsg, *filename;

        filename = vfs_path_to_str (filename_vpath);
        errmsg = g_strdup_printf (_("Error reading %s"), filename);
        g_free (filename);
        edit_error_dialog (_("Error"), errmsg);
        g_free (errmsg);
    }
    mc_close (file);
    return ret;
}

/* --------------------------------------------------------------------------------------------- */
/** Return index of the filter or -1 is there is no appropriate filter */

static int
edit_find_filter (const vfs_path_t * filename_vpath)
{
    size_t i, l, e;
    char *filename;

    if (filename_vpath == NULL)
        return -1;

    filename = vfs_path_to_str (filename_vpath);
    l = strlen (filename);
    for (i = 0; i < sizeof (all_filters) / sizeof (all_filters[0]); i++)
    {
        e = strlen (all_filters[i].extension);
        if (l > e)
            if (!strcmp (all_filters[i].extension, filename + l - e))
            {
                g_free (filename);
                return i;
            }
    }
    g_free (filename);
    return -1;
}

/* --------------------------------------------------------------------------------------------- */

static char *
edit_get_filter (const vfs_path_t * filename_vpath)
{
    int i;
    char *p, *quoted_name, *filename;

    i = edit_find_filter (filename_vpath);
    if (i < 0)
        return NULL;

    filename = vfs_path_to_str (filename_vpath);
    quoted_name = name_quote (filename, 0);
    g_free (filename);
    p = g_strdup_printf (all_filters[i].read, quoted_name);
    g_free (quoted_name);
    return p;
}

/* --------------------------------------------------------------------------------------------- */

static off_t
edit_insert_stream (WEdit * edit, FILE * f)
{
    int c;
    off_t i = 0;
    while ((c = fgetc (f)) >= 0)
    {
        edit_insert (edit, c);
        i++;
    }
    return i;
}

/* --------------------------------------------------------------------------------------------- */
/**
  * Open file and create it if necessary.
  *
  * @param edit           editor object
  * @param filename_vpath file name
  * @param st             buffer for store stat info
  * @return TRUE for success, FALSE for error.
  */

static gboolean
check_file_access (WEdit * edit, const vfs_path_t * filename_vpath, struct stat *st)
{
    int file;
    gchar *errmsg = NULL;

    /* Try opening an existing file */
    file = mc_open (filename_vpath, O_NONBLOCK | O_RDONLY | O_BINARY, 0666);
    if (file < 0)
    {
        /*
         * Try creating the file. O_EXCL prevents following broken links
         * and opening existing files.
         */
        file = mc_open (filename_vpath, O_NONBLOCK | O_RDONLY | O_BINARY | O_CREAT | O_EXCL, 0666);
        if (file < 0)
        {
            char *filename;

            filename = vfs_path_to_str (filename_vpath);
            errmsg = g_strdup_printf (_("Cannot open %s for reading"), filename);
            g_free (filename);
            goto cleanup;
        }

        /* New file, delete it if it's not modified or saved */
        edit->delete_file = 1;
    }

    /* Check what we have opened */
    if (mc_fstat (file, st) < 0)
    {
        char *filename;

        filename = vfs_path_to_str (filename_vpath);
        errmsg = g_strdup_printf (_("Cannot get size/permissions for %s"), filename);
        g_free (filename);
        goto cleanup;
    }

    /* We want to open regular files only */
    if (!S_ISREG (st->st_mode))
    {
        char *filename;

        filename = vfs_path_to_str (filename_vpath);
        errmsg = g_strdup_printf (_("\"%s\" is not a regular file"), filename);
        g_free (filename);
        goto cleanup;
    }

    /*
     * Don't delete non-empty files.
     * O_EXCL should prevent it, but let's be on the safe side.
     */
    if (st->st_size > 0)
        edit->delete_file = 0;

    if (st->st_size >= SIZE_LIMIT)
    {
        char *filename;

        filename = vfs_path_to_str (filename_vpath);
        errmsg = g_strdup_printf (_("File \"%s\" is too large"), filename);
        g_free (filename);
    }

  cleanup:
    (void) mc_close (file);

    if (errmsg != NULL)
    {
        edit_error_dialog (_("Error"), errmsg);
        g_free (errmsg);
        return FALSE;
    }
    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */

/**
 * Open the file and load it into the buffers, either directly or using
 * a filter.  Return TRUE on success, FALSE on error.
 *
 * Fast loading (edit_load_file_fast) is used when the file size is
 * known.  In this case the data is read into the buffers by blocks.
 * If the file size is not known, the data is loaded byte by byte in
 * edit_insert_file.
 *
 * @param edit editor object
 * @return TRUE if file was successfully opened and loaded to buffers, FALSE otherwise
 */
static gboolean
edit_load_file (WEdit * edit)
{
    gboolean fast_load = TRUE;

    /* Cannot do fast load if a filter is used */
    if (edit_find_filter (edit->filename_vpath) >= 0)
        fast_load = FALSE;

    /*
     * FIXME: line end translation should disable fast loading as well
     * Consider doing fseek() to the end and ftell() for the real size.
     */
    if (edit->filename_vpath != NULL)
    {
        /*
         * VFS may report file size incorrectly, and slow load is not a big
         * deal considering overhead in VFS.
         */
        if (!vfs_file_is_local (edit->filename_vpath))
            fast_load = FALSE;

        /* If we are dealing with a real file, check that it exists */
        if (!check_file_access (edit, edit->filename_vpath, &edit->stat1))
        {
            edit_clean (edit);
            return FALSE;
        }
    }
    else
    {
        /* nothing to load */
        fast_load = FALSE;
    }

    edit_init_buffers (edit);

    if (fast_load)
    {
        edit->last_byte = edit->stat1.st_size;
        edit_load_file_fast (edit, edit->filename_vpath);
        /* If fast load was used, the number of lines wasn't calculated */
        edit->total_lines = edit_count_lines (edit, 0, edit->last_byte);
    }
    else
    {
        edit->last_byte = 0;
        if (edit->filename_vpath != NULL
            && *(vfs_path_get_by_index (edit->filename_vpath, 0)->path) != '\0')
        {
            edit->undo_stack_disable = 1;
            if (edit_insert_file (edit, edit->filename_vpath) < 0)
            {
                edit_clean (edit);
                return FALSE;
            }
            edit->undo_stack_disable = 0;
        }
    }
    edit->lb = LB_ASIS;
    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/** Restore saved cursor position in the file */

static void
edit_load_position (WEdit * edit)
{
    long line, column;
    off_t offset;

    if (edit->filename_vpath == NULL
        || *(vfs_path_get_by_index (edit->filename_vpath, 0)->path) == '\0')
        return;

    load_file_position (edit->filename_vpath, &line, &column, &offset, &edit->serialized_bookmarks);

    if (line > 0)
    {
        edit_move_to_line (edit, line - 1);
        edit->prev_col = column;
    }
    else if (offset > 0)
    {
        edit_cursor_move (edit, offset);
        line = edit->curs_line;
        edit->search_start = edit->curs1;
    }

    book_mark_restore (edit, BOOK_MARK_COLOR);

    edit_move_to_prev_col (edit, edit_bol (edit, edit->curs1));
    edit_move_display (edit, line - (WIDGET (edit)->lines / 2));
}

/* --------------------------------------------------------------------------------------------- */
/** Save cursor position in the file */

static void
edit_save_position (WEdit * edit)
{
    if (edit->filename_vpath == NULL
        || *(vfs_path_get_by_index (edit->filename_vpath, 0)->path) == '\0')
        return;

    book_mark_serialize (edit, BOOK_MARK_COLOR);
    save_file_position (edit->filename_vpath, edit->curs_line + 1, edit->curs_col, edit->curs1,
                        edit->serialized_bookmarks);
    edit->serialized_bookmarks = NULL;
}

/* --------------------------------------------------------------------------------------------- */
/** Clean the WEdit stricture except the widget part */

static void
edit_purge_widget (WEdit * edit)
{
    size_t len = sizeof (WEdit) - sizeof (Widget);
    char *start = (char *) edit + sizeof (Widget);
    memset (start, 0, len);
}

/* --------------------------------------------------------------------------------------------- */

/*
   TODO: if the user undos until the stack bottom, and the stack has not wrapped,
   then the file should be as it was when he loaded up. Then set edit->modified to 0.
 */

static long
edit_pop_undo_action (WEdit * edit)
{
    long c;
    unsigned long sp = edit->undo_stack_pointer;

    if (sp == edit->undo_stack_bottom)
        return STACK_BOTTOM;

    sp = (sp - 1) & edit->undo_stack_size_mask;
    c = edit->undo_stack[sp];
    if (c >= 0)
    {
        /*      edit->undo_stack[sp] = '@'; */
        edit->undo_stack_pointer = (edit->undo_stack_pointer - 1) & edit->undo_stack_size_mask;
        return c;
    }

    if (sp == edit->undo_stack_bottom)
        return STACK_BOTTOM;

    c = edit->undo_stack[(sp - 1) & edit->undo_stack_size_mask];
    if (edit->undo_stack[sp] == -2)
    {
        /*      edit->undo_stack[sp] = '@'; */
        edit->undo_stack_pointer = sp;
    }
    else
        edit->undo_stack[sp]++;

    return c;
}

static long
edit_pop_redo_action (WEdit * edit)
{
    long c;
    unsigned long sp = edit->redo_stack_pointer;

    if (sp == edit->redo_stack_bottom)
        return STACK_BOTTOM;

    sp = (sp - 1) & edit->redo_stack_size_mask;
    c = edit->redo_stack[sp];
    if (c >= 0)
    {
        edit->redo_stack_pointer = (edit->redo_stack_pointer - 1) & edit->redo_stack_size_mask;
        return c;
    }

    if (sp == edit->redo_stack_bottom)
        return STACK_BOTTOM;

    c = edit->redo_stack[(sp - 1) & edit->redo_stack_size_mask];
    if (edit->redo_stack[sp] == -2)
        edit->redo_stack_pointer = sp;
    else
        edit->redo_stack[sp]++;

    return c;
}

static long
get_prev_undo_action (WEdit * edit)
{
    long c;
    unsigned long sp = edit->undo_stack_pointer;

    if (sp == edit->undo_stack_bottom)
        return STACK_BOTTOM;

    sp = (sp - 1) & edit->undo_stack_size_mask;
    c = edit->undo_stack[sp];
    if (c >= 0)
        return c;

    if (sp == edit->undo_stack_bottom)
        return STACK_BOTTOM;

    c = edit->undo_stack[(sp - 1) & edit->undo_stack_size_mask];
    return c;
}

/* --------------------------------------------------------------------------------------------- */
/** is called whenever a modification is made by one of the four routines below */

static void
edit_modification (WEdit * edit)
{
    edit->caches_valid = FALSE;

    /* raise lock when file modified */
    if (!edit->modified && !edit->delete_file)
        edit->locked = lock_file (edit->filename_vpath);
    edit->modified = 1;
}

/* --------------------------------------------------------------------------------------------- */

#ifdef HAVE_CHARSET
static char *
edit_get_byte_ptr (const WEdit * edit, off_t byte_index)
{
    if (byte_index >= (edit->curs1 + edit->curs2) || byte_index < 0)
        return NULL;

    if (byte_index >= edit->curs1)
    {
        off_t p;

        p = edit->curs1 + edit->curs2 - byte_index - 1;
        return (char *) (edit->buffers2[p >> S_EDIT_BUF_SIZE] +
                         (EDIT_BUF_SIZE - (p & M_EDIT_BUF_SIZE) - 1));
    }

    return (char *) (edit->buffers1[byte_index >> S_EDIT_BUF_SIZE] +
                     (byte_index & M_EDIT_BUF_SIZE));
}
#endif

/* --------------------------------------------------------------------------------------------- */

#ifdef HAVE_CHARSET
static int
edit_get_prev_utf (const WEdit * edit, off_t byte_index, int *char_width)
{
    int i, res;
    gchar utf8_buf[3 * UTF8_CHAR_LEN + 1];
    gchar *str;
    gchar *cursor_buf_ptr;

    if (byte_index > (edit->curs1 + edit->curs2) || byte_index <= 0)
    {
        *char_width = 0;
        return 0;
    }

    for (i = 0; i < (3 * UTF8_CHAR_LEN); i++)
        utf8_buf[i] = edit_get_byte (edit, byte_index + i - (2 * UTF8_CHAR_LEN));
    utf8_buf[3 * UTF8_CHAR_LEN] = '\0';

    cursor_buf_ptr = utf8_buf + (2 * UTF8_CHAR_LEN);
    str = g_utf8_find_prev_char (utf8_buf, cursor_buf_ptr);

    if (str == NULL || g_utf8_next_char (str) != cursor_buf_ptr)
    {
        *char_width = 1;
        return *(cursor_buf_ptr - 1);
    }
    else
    {
        res = g_utf8_get_char_validated (str, -1);

        if (res < 0)
        {
            *char_width = 1;
            return *(cursor_buf_ptr - 1);
        }
        else
        {
            *char_width = cursor_buf_ptr - str;
            return res;
        }
    }
}
#endif

/* --------------------------------------------------------------------------------------------- */
/* high level cursor movement commands */
/* --------------------------------------------------------------------------------------------- */
/** check whether cursor is in indent part of line
 *
 * @param edit editor object
 *
 * @return TRUE if cursor is in indent, FALSE otherwise
 */

static gboolean
is_in_indent (const WEdit * edit)
{
    off_t p;

    for (p = edit_bol (edit, edit->curs1); p < edit->curs1; p++)
        if (strchr (" \t", edit_get_byte (edit, p)) == NULL)
            return FALSE;

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/** check whether line in editor is blank or not
 *
 * @param edit editor object
 * @param offset position in file
 *
 * @return TRUE if line in blank, FALSE otherwise
 */

static gboolean
is_blank (const WEdit * edit, off_t offset)
{
    off_t s, f;
    int c;

    s = edit_bol (edit, offset);
    f = edit_eol (edit, offset) - 1;
    while (s <= f)
    {
        c = edit_get_byte (edit, s++);
        if (!isspace (c))
            return FALSE;
    }
    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/** returns the offset of line i */

static off_t
edit_find_line (WEdit * edit, long line)
{
    long i, j = 0;
    long m = 2000000000;        /* what is the magic number? */

    if (!edit->caches_valid)
    {
        memset (edit->line_numbers, 0, sizeof (edit->line_numbers));
        memset (edit->line_offsets, 0, sizeof (edit->line_offsets));
        /* three offsets that we *know* are line 0 at 0 and these two: */
        edit->line_numbers[1] = edit->curs_line;
        edit->line_offsets[1] = edit_bol (edit, edit->curs1);
        edit->line_numbers[2] = edit->total_lines;
        edit->line_offsets[2] = edit_bol (edit, edit->last_byte);
        edit->caches_valid = TRUE;
    }
    if (line >= edit->total_lines)
        return edit->line_offsets[2];
    if (line <= 0)
        return 0;
    /* find the closest known point */
    for (i = 0; i < N_LINE_CACHES; i++)
    {
        long n;

        n = abs (edit->line_numbers[i] - line);
        if (n < m)
        {
            m = n;
            j = i;
        }
    }
    if (m == 0)
        return edit->line_offsets[j];   /* know the offset exactly */
    if (m == 1 && j >= 3)
        i = j;                  /* one line different - caller might be looping, so stay in this cache */
    else
        i = 3 + (rand () % (N_LINE_CACHES - 3));
    if (line > edit->line_numbers[j])
        edit->line_offsets[i] =
            edit_move_forward (edit, edit->line_offsets[j], line - edit->line_numbers[j], 0);
    else
        edit->line_offsets[i] =
            edit_move_backward (edit, edit->line_offsets[j], edit->line_numbers[j] - line);
    edit->line_numbers[i] = line;
    return edit->line_offsets[i];
}

/* --------------------------------------------------------------------------------------------- */
/** moves up until a blank line is reached, or until just
   before a non-blank line is reached */

static void
edit_move_up_paragraph (WEdit * edit, gboolean do_scroll)
{
    long i = 0;

    if (edit->curs_line > 1)
    {
        if (!edit_line_is_blank (edit, edit->curs_line))
        {
            for (i = edit->curs_line - 1; i != 0; i--)
                if (edit_line_is_blank (edit, i))
                    break;
        }
        else if (edit_line_is_blank (edit, edit->curs_line - 1))
        {
            for (i = edit->curs_line - 1; i != 0; i--)
                if (!edit_line_is_blank (edit, i))
                {
                    i++;
                    break;
                }
        }
        else
        {
            for (i = edit->curs_line - 1; i != 0; i--)
                if (edit_line_is_blank (edit, i))
                    break;
        }
    }

    edit_move_up (edit, edit->curs_line - i, do_scroll);
}

/* --------------------------------------------------------------------------------------------- */
/** moves down until a blank line is reached, or until just
   before a non-blank line is reached */

static void
edit_move_down_paragraph (WEdit * edit, gboolean do_scroll)
{
    long i;

    if (edit->curs_line >= edit->total_lines - 1)
        i = edit->total_lines;
    else if (!edit_line_is_blank (edit, edit->curs_line))
    {
        for (i = edit->curs_line + 1; i != 0; i++)
            if (edit_line_is_blank (edit, i) || i >= edit->total_lines)
                break;
    }
    else if (edit_line_is_blank (edit, edit->curs_line + 1))
    {
        for (i = edit->curs_line + 1; i != 0; i++)
            if (!edit_line_is_blank (edit, i) || i > edit->total_lines)
            {
                i--;
                break;
            }
    }
    else
    {
        for (i = edit->curs_line + 1; i != 0; i++)
            if (edit_line_is_blank (edit, i) || i >= edit->total_lines)
                break;
    }
    edit_move_down (edit, i - edit->curs_line, do_scroll);
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_begin_page (WEdit * edit)
{
    edit_update_curs_row (edit);
    edit_move_up (edit, edit->curs_row, 0);
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_end_page (WEdit * edit)
{
    edit_update_curs_row (edit);
    edit_move_down (edit, WIDGET (edit)->lines - edit->curs_row - 1, 0);
}


/* --------------------------------------------------------------------------------------------- */
/** goto beginning of text */

static void
edit_move_to_top (WEdit * edit)
{
    if (edit->curs_line)
    {
        edit_cursor_move (edit, -edit->curs1);
        edit_move_to_prev_col (edit, 0);
        edit->force |= REDRAW_PAGE;
        edit->search_start = 0;
        edit_update_curs_row (edit);
    }
}


/* --------------------------------------------------------------------------------------------- */
/** goto end of text */

static void
edit_move_to_bottom (WEdit * edit)
{
    if (edit->curs_line < edit->total_lines)
    {
        edit_move_down (edit, edit->total_lines - edit->curs_row, 0);
        edit->start_display = edit->last_byte;
        edit->start_line = edit->total_lines;
        edit_scroll_upward (edit, WIDGET (edit)->lines - 1);
        edit->force |= REDRAW_PAGE;
    }
}

/* --------------------------------------------------------------------------------------------- */
/** goto beginning of line */

static void
edit_cursor_to_bol (WEdit * edit)
{
    edit_cursor_move (edit, edit_bol (edit, edit->curs1) - edit->curs1);
    edit->search_start = edit->curs1;
    edit->prev_col = edit_get_col (edit);
    edit->over_col = 0;
}

/* --------------------------------------------------------------------------------------------- */
/** goto end of line */

static void
edit_cursor_to_eol (WEdit * edit)
{
    edit_cursor_move (edit, edit_eol (edit, edit->curs1) - edit->curs1);
    edit->search_start = edit->curs1;
    edit->prev_col = edit_get_col (edit);
    edit->over_col = 0;
}

/* --------------------------------------------------------------------------------------------- */

static unsigned long
my_type_of (int c)
{
    int x, r = 0;
    const char *p, *q;
    const char option_chars_move_whole_word[] =
        "!=&|<>^~ !:;, !'!`!.?!\"!( !) !{ !} !Aa0 !+-*/= |<> ![ !] !\\#! ";

    if (c == 0)
        return 0;
    if (c == '!')
    {
        if (*option_chars_move_whole_word == '!')
            return 2;
        return 0x80000000UL;
    }
    if (g_ascii_isupper ((gchar) c))
        c = 'A';
    else if (g_ascii_islower ((gchar) c))
        c = 'a';
    else if (g_ascii_isalpha (c))
        c = 'a';
    else if (isdigit (c))
        c = '0';
    else if (isspace (c))
        c = ' ';
    q = strchr (option_chars_move_whole_word, c);
    if (!q)
        return 0xFFFFFFFFUL;
    do
    {
        for (x = 1, p = option_chars_move_whole_word; p < q; p++)
            if (*p == '!')
                x <<= 1;
        r |= x;
    }
    while ((q = strchr (q + 1, c)));
    return r;
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_left_word_move (WEdit * edit, int s)
{
    while (TRUE)
    {
        int c1, c2;

        if (edit->column_highlight
            && edit->mark1 != edit->mark2
            && edit->over_col == 0 && edit->curs1 == edit_bol (edit, edit->curs1))
            break;
        edit_cursor_move (edit, -1);
        if (edit->curs1 == 0)
            break;
        c1 = edit_get_byte (edit, edit->curs1 - 1);
        c2 = edit_get_byte (edit, edit->curs1);
        if (c1 == '\n' || c2 == '\n')
            break;
        if ((my_type_of (c1) & my_type_of (c2)) == 0)
            break;
        if (isspace (c1) && !isspace (c2))
            break;
        if (s != 0 && !isspace (c1) && isspace (c2))
            break;
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_left_word_move_cmd (WEdit * edit)
{
    edit_left_word_move (edit, 0);
    edit->force |= REDRAW_PAGE;
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_right_word_move (WEdit * edit, int s)
{
    while (TRUE)
    {
        int c1, c2;

        if (edit->column_highlight
            && edit->mark1 != edit->mark2
            && edit->over_col == 0 && edit->curs1 == edit_eol (edit, edit->curs1))
            break;
        edit_cursor_move (edit, 1);
        if (edit->curs1 >= edit->last_byte)
            break;
        c1 = edit_get_byte (edit, edit->curs1 - 1);
        c2 = edit_get_byte (edit, edit->curs1);
        if (c1 == '\n' || c2 == '\n')
            break;
        if ((my_type_of (c1) & my_type_of (c2)) == 0)
            break;
        if (isspace (c1) && !isspace (c2))
            break;
        if (s != 0 && !isspace (c1) && isspace (c2))
            break;
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_right_word_move_cmd (WEdit * edit)
{
    edit_right_word_move (edit, 0);
    edit->force |= REDRAW_PAGE;
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_right_char_move_cmd (WEdit * edit)
{
    int cw = 1;
    int c = 0;
#ifdef HAVE_CHARSET
    if (edit->utf8)
    {
        c = edit_get_utf (edit, edit->curs1, &cw);
        if (cw < 1)
            cw = 1;
    }
    else
#endif
    {
        c = edit_get_byte (edit, edit->curs1);
    }
    if (option_cursor_beyond_eol && c == '\n')
    {
        edit->over_col++;
    }
    else
    {
        edit_cursor_move (edit, cw);
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_left_char_move_cmd (WEdit * edit)
{
    int cw = 1;
    if (edit->column_highlight
        && option_cursor_beyond_eol
        && edit->mark1 != edit->mark2
        && edit->over_col == 0 && edit->curs1 == edit_bol (edit, edit->curs1))
        return;
#ifdef HAVE_CHARSET
    if (edit->utf8)
    {
        edit_get_prev_utf (edit, edit->curs1, &cw);
        if (cw < 1)
            cw = 1;
    }
#endif
    if (option_cursor_beyond_eol && edit->over_col > 0)
    {
        edit->over_col--;
    }
    else
    {
        edit_cursor_move (edit, -cw);
    }
}

/* --------------------------------------------------------------------------------------------- */
/** Up or down cursor moving.
 direction = TRUE - move up
           = FALSE - move down
*/

static void
edit_move_updown (WEdit * edit, long lines, gboolean do_scroll, gboolean direction)
{
    long p;
    long l = direction ? edit->curs_line : edit->total_lines - edit->curs_line;

    if (lines > l)
        lines = l;

    if (lines == 0)
        return;

    if (lines > 1)
        edit->force |= REDRAW_PAGE;
    if (do_scroll)
    {
        if (direction)
            edit_scroll_upward (edit, lines);
        else
            edit_scroll_downward (edit, lines);
    }
    p = edit_bol (edit, edit->curs1);

    p = direction ? edit_move_backward (edit, p, lines) : edit_move_forward (edit, p, lines, 0);

    edit_cursor_move (edit, p - edit->curs1);

    edit_move_to_prev_col (edit, p);

    /* search start of current multibyte char (like CJK) */
    if (edit->curs1 + 1 < edit->last_byte)
    {
        edit_right_char_move_cmd (edit);
        edit_left_char_move_cmd (edit);
    }

    edit->search_start = edit->curs1;
    edit->found_len = 0;
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_right_delete_word (WEdit * edit)
{
    while (edit->curs1 < edit->last_byte)
    {
        int c1, c2;

        c1 = edit_delete (edit, TRUE);
        c2 = edit_get_byte (edit, edit->curs1);
        if (c1 == '\n' || c2 == '\n')
            break;
        if ((isspace (c1) == 0) != (isspace (c2) == 0))
            break;
        if ((my_type_of (c1) & my_type_of (c2)) == 0)
            break;
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_left_delete_word (WEdit * edit)
{
    while (edit->curs1 > 0)
    {
        int c1, c2;

        c1 = edit_backspace (edit, TRUE);
        c2 = edit_get_byte (edit, edit->curs1 - 1);
        if (c1 == '\n' || c2 == '\n')
            break;
        if ((isspace (c1) == 0) != (isspace (c2) == 0))
            break;
        if ((my_type_of (c1) & my_type_of (c2)) == 0)
            break;
    }
}

/* --------------------------------------------------------------------------------------------- */
/**
   the start column position is not recorded, and hence does not
   undo as it happed. But who would notice.
 */

static void
edit_do_undo (WEdit * edit)
{
    long ac;
    long count = 0;

    edit->undo_stack_disable = 1;       /* don't record undo's onto undo stack! */
    edit->over_col = 0;
    while ((ac = edit_pop_undo_action (edit)) < KEY_PRESS)
    {
        switch ((int) ac)
        {
        case STACK_BOTTOM:
            goto done_undo;
        case CURS_RIGHT:
            edit_cursor_move (edit, 1);
            break;
        case CURS_LEFT:
            edit_cursor_move (edit, -1);
            break;
        case BACKSPACE:
        case BACKSPACE_BR:
            edit_backspace (edit, TRUE);
            break;
        case DELCHAR:
        case DELCHAR_BR:
            edit_delete (edit, TRUE);
            break;
        case COLUMN_ON:
            edit->column_highlight = 1;
            break;
        case COLUMN_OFF:
            edit->column_highlight = 0;
            break;
        }
        if (ac >= 256 && ac < 512)
            edit_insert_ahead (edit, ac - 256);
        if (ac >= 0 && ac < 256)
            edit_insert (edit, ac);

        if (ac >= MARK_1 - 2 && ac < MARK_2 - 2)
        {
            edit->mark1 = ac - MARK_1;
            edit->column1 =
                (long) edit_move_forward3 (edit, edit_bol (edit, edit->mark1), 0, edit->mark1);
        }
        if (ac >= MARK_2 - 2 && ac < MARK_CURS - 2)
        {
            edit->mark2 = ac - MARK_2;
            edit->column2 =
                (long) edit_move_forward3 (edit, edit_bol (edit, edit->mark2), 0, edit->mark2);
        }
        else if (ac >= MARK_CURS - 2 && ac < KEY_PRESS)
        {
            edit->end_mark_curs = ac - MARK_CURS;
        }
        if (count++)
            edit->force |= REDRAW_PAGE; /* more than one pop usually means something big */
    }

    if (edit->start_display > ac - KEY_PRESS)
    {
        edit->start_line -= edit_count_lines (edit, ac - KEY_PRESS, edit->start_display);
        edit->force |= REDRAW_PAGE;
    }
    else if (edit->start_display < ac - KEY_PRESS)
    {
        edit->start_line += edit_count_lines (edit, edit->start_display, ac - KEY_PRESS);
        edit->force |= REDRAW_PAGE;
    }
    edit->start_display = ac - KEY_PRESS;       /* see push and pop above */
    edit_update_curs_row (edit);

  done_undo:
    edit->undo_stack_disable = 0;
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_do_redo (WEdit * edit)
{
    long ac;
    long count = 0;

    if (edit->redo_stack_reset)
        return;

    edit->over_col = 0;
    while ((ac = edit_pop_redo_action (edit)) < KEY_PRESS)
    {
        switch ((int) ac)
        {
        case STACK_BOTTOM:
            goto done_redo;
        case CURS_RIGHT:
            edit_cursor_move (edit, 1);
            break;
        case CURS_LEFT:
            edit_cursor_move (edit, -1);
            break;
        case BACKSPACE:
            edit_backspace (edit, TRUE);
            break;
        case DELCHAR:
            edit_delete (edit, TRUE);
            break;
        case COLUMN_ON:
            edit->column_highlight = 1;
            break;
        case COLUMN_OFF:
            edit->column_highlight = 0;
            break;
        }
        if (ac >= 256 && ac < 512)
            edit_insert_ahead (edit, ac - 256);
        if (ac >= 0 && ac < 256)
            edit_insert (edit, ac);

        if (ac >= MARK_1 - 2 && ac < MARK_2 - 2)
        {
            edit->mark1 = ac - MARK_1;
            edit->column1 =
                (long) edit_move_forward3 (edit, edit_bol (edit, edit->mark1), 0, edit->mark1);
        }
        else if (ac >= MARK_2 - 2 && ac < KEY_PRESS)
        {
            edit->mark2 = ac - MARK_2;
            edit->column2 =
                (long) edit_move_forward3 (edit, edit_bol (edit, edit->mark2), 0, edit->mark2);
        }
        /* more than one pop usually means something big */
        if (count++)
            edit->force |= REDRAW_PAGE;
    }

    if (edit->start_display > ac - KEY_PRESS)
    {
        edit->start_line -= edit_count_lines (edit, ac - KEY_PRESS, edit->start_display);
        edit->force |= REDRAW_PAGE;
    }
    else if (edit->start_display < ac - KEY_PRESS)
    {
        edit->start_line += edit_count_lines (edit, edit->start_display, ac - KEY_PRESS);
        edit->force |= REDRAW_PAGE;
    }
    edit->start_display = ac - KEY_PRESS;       /* see push and pop above */
    edit_update_curs_row (edit);

  done_redo:
    ;
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_group_undo (WEdit * edit)
{
    long ac = KEY_PRESS;
    long cur_ac = KEY_PRESS;
    while (ac != STACK_BOTTOM && ac == cur_ac)
    {
        cur_ac = get_prev_undo_action (edit);
        edit_do_undo (edit);
        ac = get_prev_undo_action (edit);
        /* exit from cycle if option_group_undo is not set,
         * and make single UNDO operation
         */
        if (!option_group_undo)
            ac = STACK_BOTTOM;
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_delete_to_line_end (WEdit * edit)
{
    while (edit_get_byte (edit, edit->curs1) != '\n' && edit->curs2 != 0)
        edit_delete (edit, TRUE);
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_delete_to_line_begin (WEdit * edit)
{
    while (edit_get_byte (edit, edit->curs1 - 1) != '\n' && edit->curs1 != 0)
        edit_backspace (edit, TRUE);
}

/* --------------------------------------------------------------------------------------------- */

static gboolean
is_aligned_on_a_tab (WEdit * edit)
{
    long curs_col;

    edit_update_curs_col (edit);
    curs_col = edit->curs_col % (TAB_SIZE * space_width);
    return (curs_col == 0 || curs_col == (HALF_TAB_SIZE * space_width));
}

/* --------------------------------------------------------------------------------------------- */

static gboolean
right_of_four_spaces (WEdit * edit)
{
    int i, ch = 0;

    for (i = 1; i <= HALF_TAB_SIZE; i++)
        ch |= edit_get_byte (edit, edit->curs1 - i);

    return (ch == ' ' && is_aligned_on_a_tab (edit));
}

/* --------------------------------------------------------------------------------------------- */

static gboolean
left_of_four_spaces (WEdit * edit)
{
    int i, ch = 0;

    for (i = 0; i < HALF_TAB_SIZE; i++)
        ch |= edit_get_byte (edit, edit->curs1 + i);

    return (ch == ' ' && is_aligned_on_a_tab (edit));
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_auto_indent (WEdit * edit)
{
    off_t p;
    char c;

    p = edit->curs1;
    /* use the previous line as a template */
    p = edit_move_backward (edit, p, 1);
    /* copy the leading whitespace of the line */
    while (TRUE)
    {                           /* no range check - the line _is_ \n-terminated */
        c = edit_get_byte (edit, p++);
        if (c != ' ' && c != '\t')
            break;
        edit_insert (edit, c);
    }
}

/* --------------------------------------------------------------------------------------------- */

static inline void
edit_double_newline (WEdit * edit)
{
    edit_insert (edit, '\n');
    if (edit_get_byte (edit, edit->curs1) == '\n' || edit_get_byte (edit, edit->curs1 - 2) == '\n')
        return;
    edit->force |= REDRAW_PAGE;
    edit_insert (edit, '\n');
}

/* --------------------------------------------------------------------------------------------- */

static void
insert_spaces_tab (WEdit * edit, gboolean half)
{
    long i;

    edit_update_curs_col (edit);
    i = option_tab_spacing * space_width;
    if (half)
        i /= 2;
    i = ((edit->curs_col / i) + 1) * i - edit->curs_col;
    while (i > 0)
    {
        edit_insert (edit, ' ');
        i -= space_width;
    }
}

/* --------------------------------------------------------------------------------------------- */

static inline void
edit_tab_cmd (WEdit * edit)
{
    if (option_fake_half_tabs && is_in_indent (edit))
    {
        /* insert a half tab (usually four spaces) unless there is a
           half tab already behind, then delete it and insert a
           full tab. */
        if (option_fill_tabs_with_spaces || !right_of_four_spaces (edit))
            insert_spaces_tab (edit, TRUE);
        else
        {
            int i;

            for (i = 1; i <= HALF_TAB_SIZE; i++)
                edit_backspace (edit, TRUE);
            edit_insert (edit, '\t');
        }
    }
    else if (option_fill_tabs_with_spaces)
        insert_spaces_tab (edit, FALSE);
    else
        edit_insert (edit, '\t');
}

/* --------------------------------------------------------------------------------------------- */

static void
check_and_wrap_line (WEdit * edit)
{
    off_t curs;
    int c;

    if (!option_typewriter_wrap)
        return;
    edit_update_curs_col (edit);
    if (edit->curs_col < option_word_wrap_line_length)
        return;
    curs = edit->curs1;
    while (TRUE)
    {
        curs--;
        c = edit_get_byte (edit, curs);
        if (c == '\n' || curs <= 0)
        {
            edit_insert (edit, '\n');
            return;
        }
        if (c == ' ' || c == '\t')
        {
            off_t current = edit->curs1;
            edit_cursor_move (edit, curs - edit->curs1 + 1);
            edit_insert (edit, '\n');
            edit_cursor_move (edit, current - edit->curs1 + 1);
            return;
        }
    }
}

/* --------------------------------------------------------------------------------------------- */
/** this find the matching bracket in either direction, and sets edit->bracket
 *
 * @param edit editor object
 * @param in_screen seach only on the current screen
 * @param furthest_bracket_search count of the bytes for search
 *
 * @return position of the found bracket (-1 if no match)
 */

static off_t
edit_get_bracket (WEdit * edit, gboolean in_screen, unsigned long furthest_bracket_search)
{
    const char *const b = "{}{[][()(", *p;
    int i = 1, a, inc = -1, c, d, n = 0;
    unsigned long j = 0;
    off_t q;
    edit_update_curs_row (edit);
    c = edit_get_byte (edit, edit->curs1);
    p = strchr (b, c);
    /* no limit */
    if (!furthest_bracket_search)
        furthest_bracket_search--;
    /* not on a bracket at all */
    if (p == NULL)
        return -1;
    /* the matching bracket */
    d = p[1];
    /* going left or right? */
    if (strchr ("{[(", c))
        inc = 1;
    for (q = edit->curs1 + inc;; q += inc)
    {
        /* out of buffer? */
        if (q >= edit->last_byte || q < 0)
            break;
        a = edit_get_byte (edit, q);
        /* don't want to eat CPU */
        if (j++ > furthest_bracket_search)
            break;
        /* out of screen? */
        if (in_screen)
        {
            if (q < edit->start_display)
                break;
            /* count lines if searching downward */
            if (inc > 0 && a == '\n')
                if (n++ >= WIDGET (edit)->lines - edit->curs_row)       /* out of screen */
                    break;
        }
        /* count bracket depth */
        i += (a == c) - (a == d);
        /* return if bracket depth is zero */
        if (i == 0)
            return q;
    }
    /* no match */
    return -1;
}

/* --------------------------------------------------------------------------------------------- */

static inline void
edit_goto_matching_bracket (WEdit * edit)
{
    off_t q;

    q = edit_get_bracket (edit, 0, 0);
    if (q >= 0)
    {
        edit->bracket = edit->curs1;
        edit->force |= REDRAW_PAGE;
        edit_cursor_move (edit, q - edit->curs1);
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_move_block_to_right (WEdit * edit)
{
    off_t start_mark, end_mark;
    long cur_bol, start_bol;

    if (eval_marks (edit, &start_mark, &end_mark))
        return;

    start_bol = edit_bol (edit, start_mark);
    cur_bol = edit_bol (edit, end_mark - 1);

    do
    {
        edit_cursor_move (edit, cur_bol - edit->curs1);
        if (!edit_line_is_blank (edit, edit->curs_line))
        {
            if (option_fill_tabs_with_spaces)
                insert_spaces_tab (edit, option_fake_half_tabs);
            else
                edit_insert (edit, '\t');
            edit_cursor_move (edit, edit_bol (edit, cur_bol) - edit->curs1);
        }

        if (cur_bol == 0)
            break;

        cur_bol = edit_bol (edit, cur_bol - 1);
    }
    while (cur_bol >= start_bol);

    edit->force |= REDRAW_PAGE;
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_move_block_to_left (WEdit * edit)
{
    off_t start_mark, end_mark;
    off_t cur_bol, start_bol;
    int i;

    if (eval_marks (edit, &start_mark, &end_mark))
        return;

    start_bol = edit_bol (edit, start_mark);
    cur_bol = edit_bol (edit, end_mark - 1);

    do
    {
        int del_tab_width;
        int next_char;

        edit_cursor_move (edit, cur_bol - edit->curs1);

        if (option_fake_half_tabs)
            del_tab_width = HALF_TAB_SIZE;
        else
            del_tab_width = option_tab_spacing;

        next_char = edit_get_byte (edit, edit->curs1);
        if (next_char == '\t')
            edit_delete (edit, TRUE);
        else if (next_char == ' ')
            for (i = 1; i <= del_tab_width; i++)
            {
                if (next_char == ' ')
                    edit_delete (edit, TRUE);
                next_char = edit_get_byte (edit, edit->curs1);
            }

        if (cur_bol == 0)
            break;

        cur_bol = edit_bol (edit, cur_bol - 1);
    }
    while (cur_bol >= start_bol);

    edit->force |= REDRAW_PAGE;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * prints at the cursor
 * @return number of chars printed
 */

static size_t
edit_print_string (WEdit * e, const char *s)
{
    size_t i = 0;

    while (s[i] != '\0')
        edit_execute_cmd (e, CK_InsertChar, (unsigned char) s[i++]);
    e->force |= REDRAW_COMPLETELY;
    edit_update_screen (e);
    return i;
}

/* --------------------------------------------------------------------------------------------- */
/*** public functions ****************************************************************************/
/* --------------------------------------------------------------------------------------------- */

/** User edit menu, like user menu (F2) but only in editor. */

void
user_menu (WEdit * edit, const char *menu_file, int selected_entry)
{
    char *block_file;
    int nomark;
    off_t curs;
    off_t start_mark, end_mark;
    struct stat status;
    vfs_path_t *block_file_vpath;

    block_file = mc_config_get_full_path (EDIT_BLOCK_FILE);
    block_file_vpath = vfs_path_from_str (block_file);
    curs = edit->curs1;
    nomark = eval_marks (edit, &start_mark, &end_mark);
    if (nomark == 0)
        edit_save_block (edit, block_file, start_mark, end_mark);

    /* run shell scripts from menu */
    if (user_menu_cmd (edit, menu_file, selected_entry)
        && (mc_stat (block_file_vpath, &status) == 0) && (status.st_size != 0))
    {
        int rc = 0;
        FILE *fd;

        /* i.e. we have marked block */
        if (nomark == 0)
            rc = edit_block_delete_cmd (edit);

        if (rc == 0)
        {
            off_t ins_len;

            ins_len = edit_insert_file (edit, block_file_vpath);
            if (nomark == 0 && ins_len > 0)
                edit_set_markers (edit, start_mark, start_mark + ins_len, 0, 0);
        }
        /* truncate block file */
        fd = fopen (block_file, "w");
        if (fd != NULL)
            fclose (fd);
    }
    g_free (block_file);
    vfs_path_free (block_file_vpath);

    edit_cursor_move (edit, curs - edit->curs1);
    edit->force |= REDRAW_PAGE;
    send_message (edit, NULL, MSG_DRAW, 0, NULL);
}

/* --------------------------------------------------------------------------------------------- */

int
edit_get_byte (const WEdit * edit, off_t byte_index)
{
    off_t p;

    if (byte_index >= (edit->curs1 + edit->curs2) || byte_index < 0)
        return '\n';

    if (byte_index >= edit->curs1)
    {
        p = edit->curs1 + edit->curs2 - byte_index - 1;
        return edit->buffers2[p >> S_EDIT_BUF_SIZE][EDIT_BUF_SIZE - (p & M_EDIT_BUF_SIZE) - 1];
    }

    return edit->buffers1[byte_index >> S_EDIT_BUF_SIZE][byte_index & M_EDIT_BUF_SIZE];
}

/* --------------------------------------------------------------------------------------------- */

#ifdef HAVE_CHARSET
int
edit_get_utf (const WEdit * edit, off_t byte_index, int *char_width)
{
    gchar *str = NULL;
    int res = -1;
    gunichar ch;
    gchar *next_ch = NULL;
    int width = 0;
    gchar utf8_buf[UTF8_CHAR_LEN + 1];

    if (byte_index >= (edit->curs1 + edit->curs2) || byte_index < 0)
    {
        *char_width = 0;
        return '\n';
    }

    str = edit_get_byte_ptr (edit, byte_index);

    if (str == NULL)
    {
        *char_width = 0;
        return 0;
    }

    res = g_utf8_get_char_validated (str, -1);

    if (res < 0)
    {
        /* Retry with explicit bytes to make sure it's not a buffer boundary */
        int i;
        for (i = 0; i < UTF8_CHAR_LEN; i++)
            utf8_buf[i] = edit_get_byte (edit, byte_index + i);
        utf8_buf[UTF8_CHAR_LEN] = '\0';
        str = utf8_buf;
        res = g_utf8_get_char_validated (str, -1);
    }

    if (res < 0)
    {
        ch = *str;
        width = 0;
    }
    else
    {
        ch = res;
        /* Calculate UTF-8 char width */
        next_ch = g_utf8_next_char (str);
        if (next_ch)
        {
            width = next_ch - str;
        }
        else
        {
            ch = 0;
            width = 0;
        }
    }
    *char_width = width;
    return ch;
}
#endif

/* --------------------------------------------------------------------------------------------- */

char *
edit_get_write_filter (const vfs_path_t * write_name_vpath, const vfs_path_t * filename_vpath)
{
    int i;
    char *p, *writename;
    const vfs_path_element_t *path_element;

    i = edit_find_filter (filename_vpath);
    if (i < 0)
        return NULL;

    path_element = vfs_path_get_by_index (write_name_vpath, -1);
    writename = name_quote (path_element->path, 0);
    p = g_strdup_printf (all_filters[i].write, writename);
    g_free (writename);
    return p;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * @param edit   editor object
 * @param f      value of stream file
 * @return       the length of the file
 */

off_t
edit_write_stream (WEdit * edit, FILE * f)
{
    long i;

    if (edit->lb == LB_ASIS)
    {
        for (i = 0; i < edit->last_byte; i++)
            if (fputc (edit_get_byte (edit, i), f) < 0)
                break;
        return i;
    }

    /* change line breaks */
    for (i = 0; i < edit->last_byte; i++)
    {
        unsigned char c = edit_get_byte (edit, i);

        if (!(c == '\n' || c == '\r'))
        {
            /* not line break */
            if (fputc (c, f) < 0)
                return i;
        }
        else
        {                       /* (c == '\n' || c == '\r') */
            unsigned char c1 = edit_get_byte (edit, i + 1);     /* next char */

            switch (edit->lb)
            {
            case LB_UNIX:      /* replace "\r\n" or '\r' to '\n' */
                /* put one line break unconditionally */
                if (fputc ('\n', f) < 0)
                    return i;

                i++;            /* 2 chars are processed */

                if (c == '\r' && c1 == '\n')
                    /* Windows line break; go to the next char */
                    break;

                if (c == '\r' && c1 == '\r')
                {
                    /* two Macintosh line breaks; put second line break */
                    if (fputc ('\n', f) < 0)
                        return i;
                    break;
                }

                if (fputc (c1, f) < 0)
                    return i;
                break;

            case LB_WIN:       /* replace '\n' or '\r' to "\r\n" */
                /* put one line break unconditionally */
                if (fputc ('\r', f) < 0 || fputc ('\n', f) < 0)
                    return i;

                if (c == '\r' && c1 == '\n')
                    /* Windows line break; go to the next char */
                    i++;
                break;

            case LB_MAC:       /* replace "\r\n" or '\n' to '\r' */
                /* put one line break unconditionally */
                if (fputc ('\r', f) < 0)
                    return i;

                i++;            /* 2 chars are processed */

                if (c == '\r' && c1 == '\n')
                    /* Windows line break; go to the next char */
                    break;

                if (c == '\n' && c1 == '\n')
                {
                    /* two Windows line breaks; put second line break */
                    if (fputc ('\r', f) < 0)
                        return i;
                    break;
                }

                if (fputc (c1, f) < 0)
                    return i;
                break;
            case LB_ASIS:      /* default without changes */
                break;
            }
        }
    }

    return edit->last_byte;
}

/* --------------------------------------------------------------------------------------------- */

gboolean
is_break_char (char c)
{
    return (isspace (c) || strchr ("{}[]()<>=|/\\!?~-+`'\",.;:#$%^&*", c));
}

/* --------------------------------------------------------------------------------------------- */

char *
edit_get_word_from_pos (const WEdit * edit, off_t start_pos, off_t * start, gsize * len,
                        gsize * cut)
{
    off_t word_start;
    long cut_len = 0;
    GString *match_expr;
    unsigned char *bufpos;
    int c1, c2;

    for (word_start = start_pos; word_start != 0; word_start--, cut_len++)
    {
        c1 = edit_get_byte (edit, word_start);
        c2 = edit_get_byte (edit, word_start - 1);

        if (is_break_char (c1) != is_break_char (c2) || c1 == '\n' || c2 == '\n')
            break;
    }

    bufpos = &edit->buffers1[word_start >> S_EDIT_BUF_SIZE][word_start & M_EDIT_BUF_SIZE];
    match_expr = g_string_sized_new (16);

    do
    {
        c1 = edit_get_byte (edit, word_start + match_expr->len);
        c2 = edit_get_byte (edit, word_start + match_expr->len + 1);
        g_string_append_c (match_expr, c1);
    }
    while (!(is_break_char (c1) != is_break_char (c2) || c1 == '\n' || c2 == '\n'));

    *len = match_expr->len;
    *start = word_start;
    *cut = cut_len;

    return g_string_free (match_expr, FALSE);
}

/* --------------------------------------------------------------------------------------------- */
/** inserts a file at the cursor, returns count of inserted bytes on success */

long
edit_insert_file (WEdit * edit, const vfs_path_t * filename_vpath)
{
    char *p;
    off_t current;
    off_t ins_len = 0;

    p = edit_get_filter (filename_vpath);
    current = edit->curs1;

    if (p != NULL)
    {
        FILE *f;

        f = (FILE *) popen (p, "r");
        if (f != NULL)
        {
            edit_insert_stream (edit, f);

            /* Place cursor at the end of text selection */
            if (!option_cursor_after_inserted_block)
            {
                ins_len = edit->curs1 - current;
                edit_cursor_move (edit, -ins_len);
            }
            if (pclose (f) > 0)
            {
                char *errmsg;

                errmsg = g_strdup_printf (_("Error reading from pipe: %s"), p);
                edit_error_dialog (_("Error"), errmsg);
                g_free (errmsg);
                ins_len = -1;
            }
        }
        else
        {
            char *errmsg;

            errmsg = g_strdup_printf (_("Cannot open pipe for reading: %s"), p);
            edit_error_dialog (_("Error"), errmsg);
            g_free (errmsg);
            ins_len = -1;
        }
        g_free (p);
    }
    else
    {
        int file;
        off_t blocklen;
        int vertical_insertion = 0;
        char *buf;

        file = mc_open (filename_vpath, O_RDONLY | O_BINARY);
        if (file == -1)
            return -1;

        buf = g_malloc0 (TEMP_BUF_LEN);
        blocklen = mc_read (file, buf, sizeof (VERTICAL_MAGIC));
        if (blocklen > 0)
        {
            /* if contain signature VERTICAL_MAGIC then it vertical block */
            if (memcmp (buf, VERTICAL_MAGIC, sizeof (VERTICAL_MAGIC)) == 0)
                vertical_insertion = 1;
            else
                mc_lseek (file, 0, SEEK_SET);
        }

        if (vertical_insertion)
        {
            off_t mark1, mark2;
            long c1, c2;

            blocklen = edit_insert_column_of_text_from_file (edit, file, &mark1, &mark2, &c1, &c2);
            edit_set_markers (edit, edit->curs1, mark2, c1, c2);

            /* highlight inserted text then not persistent blocks */
            if (!option_persistent_selections && edit->modified)
            {
                if (!edit->column_highlight)
                    edit_push_undo_action (edit, COLUMN_OFF);
                edit->column_highlight = 1;
            }
        }
        else
        {
            off_t i;

            while ((blocklen = mc_read (file, (char *) buf, TEMP_BUF_LEN)) > 0)
            {
                for (i = 0; i < blocklen; i++)
                    edit_insert (edit, buf[i]);
            }
            /* highlight inserted text then not persistent blocks */
            if (!option_persistent_selections && edit->modified)
            {
                edit_set_markers (edit, edit->curs1, current, 0, 0);
                if (edit->column_highlight)
                    edit_push_undo_action (edit, COLUMN_ON);
                edit->column_highlight = 0;
            }

            /* Place cursor at the end of text selection */
            if (!option_cursor_after_inserted_block)
            {
                ins_len = edit->curs1 - current;
                edit_cursor_move (edit, -ins_len);
            }
        }

        edit->force |= REDRAW_PAGE;
        g_free (buf);
        mc_close (file);
        if (blocklen != 0)
            ins_len = 0;
    }

    return ins_len;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Fill in the edit structure.  Return NULL on failure.  Pass edit as
 * NULL to allocate a new structure.
 *
 * If line is 0, try to restore saved position.  Otherwise put the
 * cursor on that line and show it in the middle of the screen.
 */

WEdit *
edit_init (WEdit * edit, int y, int x, int lines, int cols, const vfs_path_t * filename_vpath,
           long line)
{
    gboolean to_free = FALSE;

    option_auto_syntax = 1;     /* Resetting to auto on every invokation */
    option_line_state_width = option_line_state ? LINE_STATE_WIDTH : 0;

    if (edit != NULL)
    {
        /* save some widget parameters */
        gboolean fullscreen = edit->fullscreen;
        int y_prev = edit->y_prev;
        int x_prev = edit->x_prev;
        int lines_prev = edit->lines_prev;
        int cols_prev = edit->cols_prev;

        edit_purge_widget (edit);

        /* restore saved parameters */
        edit->fullscreen = fullscreen;
        edit->y_prev = y_prev;
        edit->x_prev = x_prev;
        edit->lines_prev = lines_prev;
        edit->cols_prev = cols_prev;
    }
    else
    {
#ifdef ENABLE_NLS
        /*
         * Expand option_whole_chars_search by national letters using
         * current locale
         */

        static char option_whole_chars_search_buf[256];

        if (option_whole_chars_search_buf != option_whole_chars_search)
        {
            size_t i;
            size_t len = str_term_width1 (option_whole_chars_search);

            strcpy (option_whole_chars_search_buf, option_whole_chars_search);

            for (i = 1; i <= sizeof (option_whole_chars_search_buf); i++)
            {
                if (g_ascii_islower ((gchar) i) && !strchr (option_whole_chars_search, i))
                {
                    option_whole_chars_search_buf[len++] = i;
                }
            }

            option_whole_chars_search_buf[len] = 0;
            option_whole_chars_search = option_whole_chars_search_buf;
        }
#endif /* ENABLE_NLS */
        edit = g_malloc0 (sizeof (WEdit));
        to_free = TRUE;

        init_widget (WIDGET (edit), y, x, lines, cols, NULL, NULL);
        edit->fullscreen = TRUE;
        edit_save_size (edit);
    }

    edit->drag_state = MCEDIT_DRAG_NORMAL;

    edit->stat1.st_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    edit->stat1.st_uid = getuid ();
    edit->stat1.st_gid = getgid ();
    edit->stat1.st_mtime = 0;

    edit->over_col = 0;
    edit->bracket = -1;
    edit->force |= REDRAW_PAGE;

    /* set file name before load file */
    edit_set_filename (edit, filename_vpath);

    edit->undo_stack_size = START_STACK_SIZE;
    edit->undo_stack_size_mask = START_STACK_SIZE - 1;
    edit->undo_stack = g_malloc0 ((edit->undo_stack_size + 10) * sizeof (long));

    edit->redo_stack_size = START_STACK_SIZE;
    edit->redo_stack_size_mask = START_STACK_SIZE - 1;
    edit->redo_stack = g_malloc0 ((edit->redo_stack_size + 10) * sizeof (long));

#ifdef HAVE_CHARSET
    edit->utf8 = FALSE;
    edit->converter = str_cnv_from_term;
    edit_set_codeset (edit);
#endif

    if (!edit_load_file (edit))
    {
        /* edit_load_file already gives an error message */
        if (to_free)
            g_free (edit);
        return NULL;
    }

    edit->loading_done = 1;
    edit->modified = 0;
    edit->locked = 0;
    edit_load_syntax (edit, NULL, NULL);
    edit_get_syntax_color (edit, -1);

    /* load saved cursor position */
    if ((line == 0) && option_save_position)
        edit_load_position (edit);
    else
    {
        if (line <= 0)
            line = 1;
        edit_move_display (edit, line - 1);
        edit_move_to_line (edit, line - 1);
    }

    edit_load_macro_cmd (edit);

    return edit;
}

/* --------------------------------------------------------------------------------------------- */

/** Clear the edit struct, freeing everything in it.  Return TRUE on success */
gboolean
edit_clean (WEdit * edit)
{
    int j = 0;

    if (edit == NULL)
        return FALSE;

    /* a stale lock, remove it */
    if (edit->locked)
        edit->locked = unlock_file (edit->filename_vpath);

    /* save cursor position */
    if (option_save_position)
        edit_save_position (edit);
    else if (edit->serialized_bookmarks != NULL)
        edit->serialized_bookmarks = (GArray *) g_array_free (edit->serialized_bookmarks, TRUE);

    /* File specified on the mcedit command line and never saved */
    if (edit->delete_file)
        unlink (vfs_path_get_last_path_str (edit->filename_vpath));

    edit_free_syntax_rules (edit);
    book_mark_flush (edit, -1);
    for (; j <= MAXBUFF; j++)
    {
        g_free (edit->buffers1[j]);
        g_free (edit->buffers2[j]);
    }

    g_free (edit->undo_stack);
    g_free (edit->redo_stack);
    vfs_path_free (edit->filename_vpath);
    vfs_path_free (edit->dir_vpath);
    mc_search_free (edit->search);
    edit->search = NULL;

#ifdef HAVE_CHARSET
    if (edit->converter != str_cnv_from_term)
        str_close_conv (edit->converter);
#endif

    edit_purge_widget (edit);

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */

/**
 * Load a new file into the editor and set line.  If it fails, preserve the old file.
 * To do it, allocate a new widget, initialize it and, if the new file
 * was loaded, copy the data to the old widget.
 *
 * @return TRUE on success, FALSE on failure.
 */
gboolean
edit_reload_line (WEdit * edit, const vfs_path_t * filename_vpath, long line)
{
    Widget *w = WIDGET (edit);
    WEdit *e;

    e = g_malloc0 (sizeof (WEdit));
    *WIDGET (e) = *w;
    /* save some widget parameters */
    e->fullscreen = edit->fullscreen;
    e->y_prev = edit->y_prev;
    e->x_prev = edit->x_prev;
    e->lines_prev = edit->lines_prev;
    e->cols_prev = edit->cols_prev;

    if (edit_init (e, w->y, w->x, w->lines, w->cols, filename_vpath, line) == NULL)
    {
        g_free (e);
        return FALSE;
    }

    edit_clean (edit);
    memcpy (edit, e, sizeof (WEdit));
    g_free (e);

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */

#ifdef HAVE_CHARSET
void
edit_set_codeset (WEdit * edit)
{
    const char *cp_id;

    cp_id =
        get_codepage_id (mc_global.source_codepage >=
                         0 ? mc_global.source_codepage : mc_global.display_codepage);

    if (cp_id != NULL)
    {
        GIConv conv;
        conv = str_crt_conv_from (cp_id);
        if (conv != INVALID_CONV)
        {
            if (edit->converter != str_cnv_from_term)
                str_close_conv (edit->converter);
            edit->converter = conv;
        }
    }

    if (cp_id != NULL)
        edit->utf8 = str_isutf8 (cp_id);
}
#endif


/* --------------------------------------------------------------------------------------------- */

/**
 * Recording stack for undo:
 * The following is an implementation of a compressed stack. Identical
 * pushes are recorded by a negative prefix indicating the number of times the
 * same char was pushed. This saves space for repeated curs-left or curs-right
 * delete etc.
 *
 * eg:
 *
 * pushed:       stored:
 *
 * a
 * b             a
 * b            -3
 * b             b
 * c  -->       -4
 * c             c
 * c             d
 * c
 * d
 *
 * If the stack long int is 0-255 it represents a normal insert (from a backspace),
 * 256-512 is an insert ahead (from a delete), If it is betwen 600 and 700 it is one
 * of the cursor functions define'd in edit-impl.h. 1000 through 700'000'000 is to
 * set edit->mark1 position. 700'000'000 through 1400'000'000 is to set edit->mark2
 * position.
 *
 * The only way the cursor moves or the buffer is changed is through the routines:
 * insert, backspace, insert_ahead, delete, and cursor_move.
 * These record the reverse undo movements onto the stack each time they are
 * called.
 *
 * Each key press results in a set of actions (insert; delete ...). So each time
 * a key is pressed the current position of start_display is pushed as
 * KEY_PRESS + start_display. Then for undoing, we pop until we get to a number
 * over KEY_PRESS. We then assign this number less KEY_PRESS to start_display. So undo
 * tracks scrolling and key actions exactly. (KEY_PRESS is about (2^31) * (2/3) = 1400'000'000)
 *
 *
 *
 * @param edit editor object
 * @param c code of the action
 */

void
edit_push_undo_action (WEdit * edit, long c)
{
    unsigned long sp = edit->undo_stack_pointer;
    unsigned long spm1;
    long *t;

    /* first enlarge the stack if necessary */
    if (sp > edit->undo_stack_size - 10)
    {                           /* say */
        if (option_max_undo < 256)
            option_max_undo = 256;
        if (edit->undo_stack_size < (unsigned long) option_max_undo)
        {
            t = g_realloc (edit->undo_stack, (edit->undo_stack_size * 2 + 10) * sizeof (long));
            if (t)
            {
                edit->undo_stack = t;
                edit->undo_stack_size <<= 1;
                edit->undo_stack_size_mask = edit->undo_stack_size - 1;
            }
        }
    }
    spm1 = (edit->undo_stack_pointer - 1) & edit->undo_stack_size_mask;
    if (edit->undo_stack_disable)
    {
        edit_push_redo_action (edit, KEY_PRESS);
        edit_push_redo_action (edit, c);
        return;
    }

    if (edit->redo_stack_reset)
        edit->redo_stack_bottom = edit->redo_stack_pointer = 0;

    if (edit->undo_stack_bottom != sp
        && spm1 != edit->undo_stack_bottom
        && ((sp - 2) & edit->undo_stack_size_mask) != edit->undo_stack_bottom)
    {
        int d;
        if (edit->undo_stack[spm1] < 0)
        {
            d = edit->undo_stack[(sp - 2) & edit->undo_stack_size_mask];
            if (d == c && edit->undo_stack[spm1] > -1000000000)
            {
                if (c < KEY_PRESS)      /* --> no need to push multiple do-nothings */
                    edit->undo_stack[spm1]--;
                return;
            }
        }
        else
        {
            d = edit->undo_stack[spm1];
            if (d == c)
            {
                if (c >= KEY_PRESS)
                    return;     /* --> no need to push multiple do-nothings */
                edit->undo_stack[sp] = -2;
                goto check_bottom;
            }
        }
    }
    edit->undo_stack[sp] = c;

  check_bottom:
    edit->undo_stack_pointer = (edit->undo_stack_pointer + 1) & edit->undo_stack_size_mask;

    /* if the sp wraps round and catches the undo_stack_bottom then erase
     * the first set of actions on the stack to make space - by moving
     * undo_stack_bottom forward one "key press" */
    c = (edit->undo_stack_pointer + 2) & edit->undo_stack_size_mask;
    if ((unsigned long) c == edit->undo_stack_bottom ||
        (((unsigned long) c + 1) & edit->undo_stack_size_mask) == edit->undo_stack_bottom)
        do
        {
            edit->undo_stack_bottom = (edit->undo_stack_bottom + 1) & edit->undo_stack_size_mask;
        }
        while (edit->undo_stack[edit->undo_stack_bottom] < KEY_PRESS
               && edit->undo_stack_bottom != edit->undo_stack_pointer);

    /*If a single key produced enough pushes to wrap all the way round then we would notice that the [undo_stack_bottom] does not contain KEY_PRESS. The stack is then initialised: */
    if (edit->undo_stack_pointer != edit->undo_stack_bottom
        && edit->undo_stack[edit->undo_stack_bottom] < KEY_PRESS)
    {
        edit->undo_stack_bottom = edit->undo_stack_pointer = 0;
    }
}

void
edit_push_redo_action (WEdit * edit, long c)
{
    unsigned long sp = edit->redo_stack_pointer;
    unsigned long spm1;
    long *t;
    /* first enlarge the stack if necessary */
    if (sp > edit->redo_stack_size - 10)
    {                           /* say */
        if (option_max_undo < 256)
            option_max_undo = 256;
        if (edit->redo_stack_size < (unsigned long) option_max_undo)
        {
            t = g_realloc (edit->redo_stack, (edit->redo_stack_size * 2 + 10) * sizeof (long));
            if (t)
            {
                edit->redo_stack = t;
                edit->redo_stack_size <<= 1;
                edit->redo_stack_size_mask = edit->redo_stack_size - 1;
            }
        }
    }
    spm1 = (edit->redo_stack_pointer - 1) & edit->redo_stack_size_mask;

    if (edit->redo_stack_bottom != sp
        && spm1 != edit->redo_stack_bottom
        && ((sp - 2) & edit->redo_stack_size_mask) != edit->redo_stack_bottom)
    {
        int d;
        if (edit->redo_stack[spm1] < 0)
        {
            d = edit->redo_stack[(sp - 2) & edit->redo_stack_size_mask];
            if (d == c && edit->redo_stack[spm1] > -1000000000)
            {
                if (c < KEY_PRESS)      /* --> no need to push multiple do-nothings */
                    edit->redo_stack[spm1]--;
                return;
            }
        }
        else
        {
            d = edit->redo_stack[spm1];
            if (d == c)
            {
                if (c >= KEY_PRESS)
                    return;     /* --> no need to push multiple do-nothings */
                edit->redo_stack[sp] = -2;
                goto redo_check_bottom;
            }
        }
    }
    edit->redo_stack[sp] = c;

  redo_check_bottom:
    edit->redo_stack_pointer = (edit->redo_stack_pointer + 1) & edit->redo_stack_size_mask;

    /* if the sp wraps round and catches the redo_stack_bottom then erase
     * the first set of actions on the stack to make space - by moving
     * redo_stack_bottom forward one "key press" */
    c = (edit->redo_stack_pointer + 2) & edit->redo_stack_size_mask;
    if ((unsigned long) c == edit->redo_stack_bottom ||
        (((unsigned long) c + 1) & edit->redo_stack_size_mask) == edit->redo_stack_bottom)
        do
        {
            edit->redo_stack_bottom = (edit->redo_stack_bottom + 1) & edit->redo_stack_size_mask;
        }
        while (edit->redo_stack[edit->redo_stack_bottom] < KEY_PRESS
               && edit->redo_stack_bottom != edit->redo_stack_pointer);

    /*
     * If a single key produced enough pushes to wrap all the way round then
     * we would notice that the [redo_stack_bottom] does not contain KEY_PRESS.
     * The stack is then initialised:
     */

    if (edit->redo_stack_pointer != edit->redo_stack_bottom
        && edit->redo_stack[edit->redo_stack_bottom] < KEY_PRESS)
        edit->redo_stack_bottom = edit->redo_stack_pointer = 0;
}

/* --------------------------------------------------------------------------------------------- */
/**
   Basic low level single character buffer alterations and movements at the cursor.
   Returns char passed over, inserted or removed.
 */

void
edit_insert (WEdit * edit, int c)
{
    /* check if file has grown to large */
    if (edit->last_byte >= SIZE_LIMIT)
        return;

    /* first we must update the position of the display window */
    if (edit->curs1 < edit->start_display)
    {
        edit->start_display++;
        if (c == '\n')
            edit->start_line++;
    }

    /* Mark file as modified, unless the file hasn't been fully loaded */
    if (edit->loading_done)
        edit_modification (edit);

    /* now we must update some info on the file and check if a redraw is required */
    if (c == '\n')
    {
        book_mark_inc (edit, edit->curs_line);
        edit->curs_line++;
        edit->total_lines++;
        edit->force |= REDRAW_LINE_ABOVE | REDRAW_AFTER_CURSOR;
    }

    /* save the reverse command onto the undo stack */
    /* ordinary char and not space */
    if (c > 32)
        edit_push_undo_action (edit, BACKSPACE);
    else
        edit_push_undo_action (edit, BACKSPACE_BR);
    /* update markers */
    edit->mark1 += (edit->mark1 > edit->curs1);
    edit->mark2 += (edit->mark2 > edit->curs1);
    edit->last_get_rule += (edit->last_get_rule > edit->curs1);

    /* add a new buffer if we've reached the end of the last one */
    if (!(edit->curs1 & M_EDIT_BUF_SIZE))
        edit->buffers1[edit->curs1 >> S_EDIT_BUF_SIZE] = g_malloc0 (EDIT_BUF_SIZE);

    /* perform the insertion */
    edit->buffers1[edit->curs1 >> S_EDIT_BUF_SIZE][edit->curs1 & M_EDIT_BUF_SIZE]
        = (unsigned char) c;

    /* update file length */
    edit->last_byte++;

    /* update cursor position */
    edit->curs1++;
}

/* --------------------------------------------------------------------------------------------- */
/** same as edit_insert and move left */

void
edit_insert_ahead (WEdit * edit, int c)
{
    if (edit->last_byte >= SIZE_LIMIT)
        return;

    if (edit->curs1 < edit->start_display)
    {
        edit->start_display++;
        if (c == '\n')
            edit->start_line++;
    }
    edit_modification (edit);
    if (c == '\n')
    {
        book_mark_inc (edit, edit->curs_line);
        edit->total_lines++;
        edit->force |= REDRAW_AFTER_CURSOR;
    }
    /* ordinary char and not space */
    if (c > 32)
        edit_push_undo_action (edit, DELCHAR);
    else
        edit_push_undo_action (edit, DELCHAR_BR);

    edit->mark1 += (edit->mark1 >= edit->curs1);
    edit->mark2 += (edit->mark2 >= edit->curs1);
    edit->last_get_rule += (edit->last_get_rule >= edit->curs1);

    if (!((edit->curs2 + 1) & M_EDIT_BUF_SIZE))
        edit->buffers2[(edit->curs2 + 1) >> S_EDIT_BUF_SIZE] = g_malloc0 (EDIT_BUF_SIZE);
    edit->buffers2[edit->curs2 >> S_EDIT_BUF_SIZE]
        [EDIT_BUF_SIZE - (edit->curs2 & M_EDIT_BUF_SIZE) - 1] = c;

    edit->last_byte++;
    edit->curs2++;
}


/* --------------------------------------------------------------------------------------------- */

int
edit_delete (WEdit * edit, gboolean byte_delete)
{
    int p = 0;
    int cw = 1;
    int i;

    if (edit->curs2 == 0)
        return 0;

#ifdef HAVE_CHARSET
    /* if byte_delete == TRUE then delete only one byte not multibyte char */
    if (edit->utf8 && !byte_delete)
    {
        edit_get_utf (edit, edit->curs1, &cw);
        if (cw < 1)
            cw = 1;
    }
#else
    (void) byte_delete;
#endif

    if (edit->mark2 != edit->mark1)
        edit_push_markers (edit);

    for (i = 1; i <= cw; i++)
    {
        if (edit->mark1 > edit->curs1)
        {
            edit->mark1--;
            edit->end_mark_curs--;
        }
        if (edit->mark2 > edit->curs1)
            edit->mark2--;
        if (edit->last_get_rule > edit->curs1)
            edit->last_get_rule--;

        p = edit->buffers2[(edit->curs2 - 1) >> S_EDIT_BUF_SIZE][EDIT_BUF_SIZE -
                                                                 ((edit->curs2 -
                                                                   1) & M_EDIT_BUF_SIZE) - 1];

        if (!(edit->curs2 & M_EDIT_BUF_SIZE))
        {
            g_free (edit->buffers2[edit->curs2 >> S_EDIT_BUF_SIZE]);
            edit->buffers2[edit->curs2 >> S_EDIT_BUF_SIZE] = NULL;
        }
        edit->last_byte--;
        edit->curs2--;
        edit_push_undo_action (edit, p + 256);
    }

    edit_modification (edit);
    if (p == '\n')
    {
        book_mark_dec (edit, edit->curs_line);
        edit->total_lines--;
        edit->force |= REDRAW_AFTER_CURSOR;
    }
    if (edit->curs1 < edit->start_display)
    {
        edit->start_display--;
        if (p == '\n')
            edit->start_line--;
    }

    return p;
}

/* --------------------------------------------------------------------------------------------- */

int
edit_backspace (WEdit * edit, gboolean byte_delete)
{
    int p = 0;
    int cw = 1;
    int i;

    if (edit->curs1 == 0)
        return 0;

    if (edit->mark2 != edit->mark1)
        edit_push_markers (edit);

#ifdef HAVE_CHARSET
    if (edit->utf8 && !byte_delete)
    {
        edit_get_prev_utf (edit, edit->curs1, &cw);
        if (cw < 1)
            cw = 1;
    }
#else
    (void) byte_delete;
#endif

    for (i = 1; i <= cw; i++)
    {
        if (edit->mark1 >= edit->curs1)
        {
            edit->mark1--;
            edit->end_mark_curs--;
        }
        if (edit->mark2 >= edit->curs1)
            edit->mark2--;
        if (edit->last_get_rule >= edit->curs1)
            edit->last_get_rule--;

        p = *(edit->buffers1[(edit->curs1 - 1) >> S_EDIT_BUF_SIZE] +
              ((edit->curs1 - 1) & M_EDIT_BUF_SIZE));
        if (((edit->curs1 - 1) & M_EDIT_BUF_SIZE) == 0)
        {
            g_free (edit->buffers1[edit->curs1 >> S_EDIT_BUF_SIZE]);
            edit->buffers1[edit->curs1 >> S_EDIT_BUF_SIZE] = NULL;
        }
        edit->last_byte--;
        edit->curs1--;
        edit_push_undo_action (edit, p);
    }
    edit_modification (edit);
    if (p == '\n')
    {
        book_mark_dec (edit, edit->curs_line);
        edit->curs_line--;
        edit->total_lines--;
        edit->force |= REDRAW_AFTER_CURSOR;
    }

    if (edit->curs1 < edit->start_display)
    {
        edit->start_display--;
        if (p == '\n')
            edit->start_line--;
    }

    return p;
}

/* --------------------------------------------------------------------------------------------- */
/** moves the cursor right or left: increment positive or negative respectively */

void
edit_cursor_move (WEdit * edit, off_t increment)
{
    /* this is the same as a combination of two of the above routines, with only one push onto the undo stack */
    int c;

    if (increment < 0)
    {
        for (; increment < 0; increment++)
        {
            if (edit->curs1 == 0)
                return;

            edit_push_undo_action (edit, CURS_RIGHT);

            c = edit_get_byte (edit, edit->curs1 - 1);
            if (!((edit->curs2 + 1) & M_EDIT_BUF_SIZE))
                edit->buffers2[(edit->curs2 + 1) >> S_EDIT_BUF_SIZE] = g_malloc0 (EDIT_BUF_SIZE);
            edit->buffers2[edit->curs2 >> S_EDIT_BUF_SIZE][EDIT_BUF_SIZE -
                                                           (edit->curs2 & M_EDIT_BUF_SIZE) - 1] = c;
            edit->curs2++;
            c = edit->buffers1[(edit->curs1 - 1) >> S_EDIT_BUF_SIZE][(edit->curs1 -
                                                                      1) & M_EDIT_BUF_SIZE];
            if (!((edit->curs1 - 1) & M_EDIT_BUF_SIZE))
            {
                g_free (edit->buffers1[edit->curs1 >> S_EDIT_BUF_SIZE]);
                edit->buffers1[edit->curs1 >> S_EDIT_BUF_SIZE] = NULL;
            }
            edit->curs1--;
            if (c == '\n')
            {
                edit->curs_line--;
                edit->force |= REDRAW_LINE_BELOW;
            }
        }

    }
    else
    {
        for (; increment > 0; increment--)
        {
            if (edit->curs2 == 0)
                return;

            edit_push_undo_action (edit, CURS_LEFT);

            c = edit_get_byte (edit, edit->curs1);
            if (!(edit->curs1 & M_EDIT_BUF_SIZE))
                edit->buffers1[edit->curs1 >> S_EDIT_BUF_SIZE] = g_malloc0 (EDIT_BUF_SIZE);
            edit->buffers1[edit->curs1 >> S_EDIT_BUF_SIZE][edit->curs1 & M_EDIT_BUF_SIZE] = c;
            edit->curs1++;
            c = edit->buffers2[(edit->curs2 - 1) >> S_EDIT_BUF_SIZE][EDIT_BUF_SIZE -
                                                                     ((edit->curs2 -
                                                                       1) & M_EDIT_BUF_SIZE) - 1];
            if (!(edit->curs2 & M_EDIT_BUF_SIZE))
            {
                g_free (edit->buffers2[edit->curs2 >> S_EDIT_BUF_SIZE]);
                edit->buffers2[edit->curs2 >> S_EDIT_BUF_SIZE] = 0;
            }
            edit->curs2--;
            if (c == '\n')
            {
                edit->curs_line++;
                edit->force |= REDRAW_LINE_ABOVE;
            }
        }
    }
}

/* These functions return positions relative to lines */

/* --------------------------------------------------------------------------------------------- */
/** returns index of last char on line + 1 */

off_t
edit_eol (const WEdit * edit, off_t current)
{
    if (current >= edit->last_byte)
        return edit->last_byte;

    for (; edit_get_byte (edit, current) != '\n'; current++)
        ;

    return current;
}

/* --------------------------------------------------------------------------------------------- */
/** returns index of first char on line */

off_t
edit_bol (const WEdit * edit, off_t current)
{
    if (current <= 0)
        return 0;

    for (; edit_get_byte (edit, current - 1) != '\n'; current--)
        ;

    return current;
}

/* --------------------------------------------------------------------------------------------- */

long
edit_count_lines (const WEdit * edit, off_t current, off_t upto)
{
    long lines = 0;

    if (upto > edit->last_byte)
        upto = edit->last_byte;
    if (current < 0)
        current = 0;
    while (current < upto)
        if (edit_get_byte (edit, current++) == '\n')
            lines++;
    return lines;
}

/* --------------------------------------------------------------------------------------------- */
/* If lines is zero this returns the count of lines from current to upto. */
/* If upto is zero returns index of lines forward current. */

off_t
edit_move_forward (const WEdit * edit, off_t current, long lines, off_t upto)
{
    if (upto != 0)
    {
        return (off_t) edit_count_lines (edit, current, upto);
    }
    else
    {
        long next;
        if (lines < 0)
            lines = 0;
        while (lines-- != 0)
        {
            next = edit_eol (edit, current) + 1;
            if (next > edit->last_byte)
                break;
            else
                current = next;
        }
        return current;
    }
}

/* --------------------------------------------------------------------------------------------- */
/** Returns offset of 'lines' lines up from current */

off_t
edit_move_backward (const WEdit * edit, off_t current, long lines)
{
    if (lines < 0)
        lines = 0;
    current = edit_bol (edit, current);
    while (lines-- != 0 && current != 0)
        current = edit_bol (edit, current - 1);
    return current;
}

/* --------------------------------------------------------------------------------------------- */
/* If cols is zero this returns the count of columns from current to upto. */
/* If upto is zero returns index of cols across from current. */

off_t
edit_move_forward3 (const WEdit * edit, off_t current, long cols, off_t upto)
{
    off_t p, q;
    long col;

    if (upto != 0)
    {
        q = upto;
        cols = -10;
    }
    else
        q = edit->last_byte + 2;

    for (col = 0, p = current; p < q; p++)
    {
        int c, orig_c;

        if (cols != -10)
        {
            if (col == cols)
                return p;
            if (col > cols)
                return p - 1;
        }

        orig_c = c = edit_get_byte (edit, p);

#ifdef HAVE_CHARSET
        if (edit->utf8)
        {
            int utf_ch;
            int cw = 1;

            utf_ch = edit_get_utf (edit, p, &cw);
            if (mc_global.utf8_display)
            {
                if (cw > 1)
                    col -= cw - 1;
                if (g_unichar_iswide (utf_ch))
                    col++;
            }
            else if (cw > 1 && g_unichar_isprint (utf_ch))
                col -= cw - 1;
        }

        c = convert_to_display_c (c);
#endif

        if (c == '\n')
            return (upto != 0 ? (off_t) col : p);
        if (c == '\t')
            col += TAB_SIZE - col % TAB_SIZE;
        else if ((c < 32 || c == 127) && (orig_c == c
#ifdef HAVE_CHARSET
                                          || (!mc_global.utf8_display && !edit->utf8)
#endif
                 ))
            /* '\r' is shown as ^M, so we must advance 2 characters */
            /* Caret notation for control characters */
            col += 2;
        else
            col++;
    }
    return (off_t) col;
}

/* --------------------------------------------------------------------------------------------- */
/** returns the current column position of the cursor */

long
edit_get_col (const WEdit * edit)
{
    return (long) edit_move_forward3 (edit, edit_bol (edit, edit->curs1), 0, edit->curs1);
}

/* --------------------------------------------------------------------------------------------- */
/* Scrolling functions */
/* --------------------------------------------------------------------------------------------- */

void
edit_update_curs_row (WEdit * edit)
{
    edit->curs_row = edit->curs_line - edit->start_line;
}

/* --------------------------------------------------------------------------------------------- */

void
edit_update_curs_col (WEdit * edit)
{
    edit->curs_col = (long) edit_move_forward3 (edit, edit_bol (edit, edit->curs1), 0, edit->curs1);
}

/* --------------------------------------------------------------------------------------------- */

long
edit_get_curs_col (const WEdit * edit)
{
    return edit->curs_col;
}

/* --------------------------------------------------------------------------------------------- */
/** moves the display start position up by i lines */

void
edit_scroll_upward (WEdit * edit, long i)
{
    long lines_above = edit->start_line;

    if (i > lines_above)
        i = lines_above;
    if (i != 0)
    {
        edit->start_line -= i;
        edit->start_display = edit_move_backward (edit, edit->start_display, i);
        edit->force |= REDRAW_PAGE;
        edit->force &= (0xfff - REDRAW_CHAR_ONLY);
    }
    edit_update_curs_row (edit);
}


/* --------------------------------------------------------------------------------------------- */

void
edit_scroll_downward (WEdit * edit, long i)
{
    long lines_below;

    lines_below = edit->total_lines - edit->start_line - (WIDGET (edit)->lines - 1);
    if (lines_below > 0)
    {
        if (i > lines_below)
            i = lines_below;
        edit->start_line += i;
        edit->start_display = edit_move_forward (edit, edit->start_display, i, 0);
        edit->force |= REDRAW_PAGE;
        edit->force &= (0xfff - REDRAW_CHAR_ONLY);
    }
    edit_update_curs_row (edit);
}

/* --------------------------------------------------------------------------------------------- */

void
edit_scroll_right (WEdit * edit, long i)
{
    edit->force |= REDRAW_PAGE;
    edit->force &= (0xfff - REDRAW_CHAR_ONLY);
    edit->start_col -= i;
}

/* --------------------------------------------------------------------------------------------- */

void
edit_scroll_left (WEdit * edit, long i)
{
    if (edit->start_col)
    {
        edit->start_col += i;
        if (edit->start_col > 0)
            edit->start_col = 0;
        edit->force |= REDRAW_PAGE;
        edit->force &= (0xfff - REDRAW_CHAR_ONLY);
    }
}

/* --------------------------------------------------------------------------------------------- */
/* high level cursor movement commands */
/* --------------------------------------------------------------------------------------------- */

void
edit_move_to_prev_col (WEdit * edit, off_t p)
{
    long prev = edit->prev_col;
    long over = edit->over_col;

    edit_cursor_move (edit, edit_move_forward3 (edit, p, prev + edit->over_col, 0) - edit->curs1);

    if (option_cursor_beyond_eol)
    {
        long line_len;

        line_len = (long) edit_move_forward3 (edit, edit_bol (edit, edit->curs1), 0,
                                              edit_eol (edit, edit->curs1));
        if (line_len < prev + edit->over_col)
        {
            edit->over_col = prev + over - line_len;
            edit->prev_col = line_len;
            edit->curs_col = line_len;
        }
        else
        {
            edit->curs_col = prev + over;
            edit->prev_col = edit->curs_col;
            edit->over_col = 0;
        }
    }
    else
    {
        edit->over_col = 0;
        if (option_fake_half_tabs && is_in_indent (edit))
        {
            edit_update_curs_col (edit);
            if (space_width != 0 && edit->curs_col % (HALF_TAB_SIZE * space_width) != 0)
            {
                int q;

                q = edit->curs_col;
                edit->curs_col -= (edit->curs_col % (HALF_TAB_SIZE * space_width));
                p = edit_bol (edit, edit->curs1);
                edit_cursor_move (edit,
                                  edit_move_forward3 (edit, p, edit->curs_col, 0) - edit->curs1);
                if (!left_of_four_spaces (edit))
                    edit_cursor_move (edit, edit_move_forward3 (edit, p, q, 0) - edit->curs1);
            }
        }
    }
}

/* --------------------------------------------------------------------------------------------- */
/** check whether line in editor is blank or not
 *
 * @param edit editor object
 * @param line number of line
 *
 * @return TRUE if line in blank, FALSE otherwise
 */

gboolean
edit_line_is_blank (WEdit * edit, long line)
{
    return is_blank (edit, edit_find_line (edit, line));
}

/* --------------------------------------------------------------------------------------------- */
/** move cursor to line 'line' */

void
edit_move_to_line (WEdit * e, long line)
{
    if (line < e->curs_line)
        edit_move_up (e, e->curs_line - line, 0);
    else
        edit_move_down (e, line - e->curs_line, 0);
    edit_scroll_screen_over_cursor (e);
}

/* --------------------------------------------------------------------------------------------- */
/** scroll window so that first visible line is 'line' */

void
edit_move_display (WEdit * e, long line)
{
    if (line < e->start_line)
        edit_scroll_upward (e, e->start_line - line);
    else
        edit_scroll_downward (e, line - e->start_line);
}

/* --------------------------------------------------------------------------------------------- */
/** save markers onto undo stack */

void
edit_push_markers (WEdit * edit)
{
    edit_push_undo_action (edit, MARK_1 + edit->mark1);
    edit_push_undo_action (edit, MARK_2 + edit->mark2);
    edit_push_undo_action (edit, MARK_CURS + edit->end_mark_curs);
}

/* --------------------------------------------------------------------------------------------- */

void
edit_set_markers (WEdit * edit, off_t m1, off_t m2, long c1, long c2)
{
    edit->mark1 = m1;
    edit->mark2 = m2;
    edit->column1 = c1;
    edit->column2 = c2;
}


/* --------------------------------------------------------------------------------------------- */
/** highlight marker toggle */

void
edit_mark_cmd (WEdit * edit, gboolean unmark)
{
    edit_push_markers (edit);
    if (unmark)
    {
        edit_set_markers (edit, 0, 0, 0, 0);
        edit->force |= REDRAW_PAGE;
    }
    else if (edit->mark2 >= 0)
    {
        edit->end_mark_curs = -1;
        edit_set_markers (edit, edit->curs1, -1, edit->curs_col + edit->over_col,
                          edit->curs_col + edit->over_col);
        edit->force |= REDRAW_PAGE;
    }
    else
    {
        edit->end_mark_curs = edit->curs1;
        edit_set_markers (edit, edit->mark1, edit->curs1, edit->column1,
                          edit->curs_col + edit->over_col);
    }
}

/* --------------------------------------------------------------------------------------------- */
/** highlight the word under cursor */

void
edit_mark_current_word_cmd (WEdit * edit)
{
    long pos;

    for (pos = edit->curs1; pos != 0; pos--)
    {
        int c1, c2;

        c1 = edit_get_byte (edit, pos);
        c2 = edit_get_byte (edit, pos - 1);
        if (!isspace (c1) && isspace (c2))
            break;
        if ((my_type_of (c1) & my_type_of (c2)) == 0)
            break;
    }
    edit->mark1 = pos;

    for (; pos < edit->last_byte; pos++)
    {
        int c1, c2;

        c1 = edit_get_byte (edit, pos);
        c2 = edit_get_byte (edit, pos + 1);
        if (!isspace (c1) && isspace (c2))
            break;
        if ((my_type_of (c1) & my_type_of (c2)) == 0)
            break;
    }
    edit->mark2 = min (pos + 1, edit->last_byte);

    edit->force |= REDRAW_LINE_ABOVE | REDRAW_AFTER_CURSOR;
}

/* --------------------------------------------------------------------------------------------- */

void
edit_mark_current_line_cmd (WEdit * edit)
{
    long pos = edit->curs1;

    edit->mark1 = edit_bol (edit, pos);
    edit->mark2 = edit_eol (edit, pos);

    edit->force |= REDRAW_LINE_ABOVE | REDRAW_AFTER_CURSOR;
}

/* --------------------------------------------------------------------------------------------- */

void
edit_delete_line (WEdit * edit)
{
    /*
     * Delete right part of the line.
     * Note that edit_get_byte() returns '\n' when byte position is
     *   beyond EOF.
     */
    while (edit_get_byte (edit, edit->curs1) != '\n')
        (void) edit_delete (edit, TRUE);

    /*
     * Delete '\n' char.
     * Note that edit_delete() will not corrupt anything if called while
     *   cursor position is EOF.
     */
    (void) edit_delete (edit, TRUE);

    /*
     * Delete left part of the line.
     * Note, that edit_get_byte() returns '\n' when byte position is < 0.
     */
    while (edit_get_byte (edit, edit->curs1 - 1) != '\n')
        (void) edit_backspace (edit, TRUE);
}

/* --------------------------------------------------------------------------------------------- */

long
edit_indent_width (const WEdit * edit, off_t p)
{
    off_t q = p;

    /* move to the end of the leading whitespace of the line */
    while (strchr ("\t ", edit_get_byte (edit, q)) && q < edit->last_byte - 1)
        q++;
    /* count the number of columns of indentation */
    return (long) edit_move_forward3 (edit, p, 0, q);
}

/* --------------------------------------------------------------------------------------------- */

void
edit_insert_indent (WEdit * edit, int indent)
{
    if (!option_fill_tabs_with_spaces)
    {
        while (indent >= TAB_SIZE)
        {
            edit_insert (edit, '\t');
            indent -= TAB_SIZE;
        }
    }
    while (indent-- > 0)
        edit_insert (edit, ' ');
}

/* --------------------------------------------------------------------------------------------- */

void
edit_push_key_press (WEdit * edit)
{
    edit_push_undo_action (edit, KEY_PRESS + edit->start_display);
    if (edit->mark2 == -1)
    {
        edit_push_undo_action (edit, MARK_1 + edit->mark1);
        edit_push_undo_action (edit, MARK_CURS + edit->end_mark_curs);
    }
}

/* --------------------------------------------------------------------------------------------- */

void
edit_find_bracket (WEdit * edit)
{
    edit->bracket = edit_get_bracket (edit, 1, 10000);
    if (last_bracket != edit->bracket)
        edit->force |= REDRAW_PAGE;
    last_bracket = edit->bracket;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * This executes a command as though the user initiated it through a key
 * press.  Callback with MSG_KEY as a message calls this after
 * translating the key press.  This function can be used to pass any
 * command to the editor.  Note that the screen wouldn't update
 * automatically.  Either of command or char_for_insertion must be
 * passed as -1.  Commands are executed, and char_for_insertion is
 * inserted at the cursor.
 */

void
edit_execute_key_command (WEdit * edit, unsigned long command, int char_for_insertion)
{
    if (command == CK_MacroStartRecord || command == CK_RepeatStartRecord
        || (macro_index < 0
            && (command == CK_MacroStartStopRecord || command == CK_RepeatStartStopRecord)))
    {
        macro_index = 0;
        edit->force |= REDRAW_CHAR_ONLY | REDRAW_LINE;
        return;
    }
    if (macro_index != -1)
    {
        edit->force |= REDRAW_COMPLETELY;
        if (command == CK_MacroStopRecord || command == CK_MacroStartStopRecord)
        {
            edit_store_macro_cmd (edit);
            macro_index = -1;
            return;
        }
        if (command == CK_RepeatStopRecord || command == CK_RepeatStartStopRecord)
        {
            edit_repeat_macro_cmd (edit);
            macro_index = -1;
            return;
        }
    }

    if (macro_index >= 0 && macro_index < MAX_MACRO_LENGTH - 1)
    {
        record_macro_buf[macro_index].action = command;
        record_macro_buf[macro_index++].ch = char_for_insertion;
    }
    /* record the beginning of a set of editing actions initiated by a key press */
    if (command != CK_Undo && command != CK_ExtendedKeyMap)
        edit_push_key_press (edit);

    edit_execute_cmd (edit, command, char_for_insertion);
    if (edit->column_highlight)
        edit->force |= REDRAW_PAGE;
}

/* --------------------------------------------------------------------------------------------- */
/**
   This executes a command at a lower level than macro recording.
   It also does not push a key_press onto the undo stack. This means
   that if it is called many times, a single undo command will undo
   all of them. It also does not check for the Undo command.
 */
void
edit_execute_cmd (WEdit * edit, unsigned long command, int char_for_insertion)
{
    Widget *w = WIDGET (edit);

    if (command == CK_WindowFullscreen)
    {
        edit_toggle_fullscreen (edit);
        return;
    }

    /* handle window state */
    if (edit_handle_move_resize (edit, command))
        return;

    edit->force |= REDRAW_LINE;

    /* The next key press will unhighlight the found string, so update
     * the whole page */
    if (edit->found_len || edit->column_highlight)
        edit->force |= REDRAW_PAGE;

    switch (command)
    {
        /* a mark command with shift-arrow */
    case CK_MarkLeft:
    case CK_MarkRight:
    case CK_MarkToWordBegin:
    case CK_MarkToWordEnd:
    case CK_MarkToHome:
    case CK_MarkToEnd:
    case CK_MarkUp:
    case CK_MarkDown:
    case CK_MarkPageUp:
    case CK_MarkPageDown:
    case CK_MarkToFileBegin:
    case CK_MarkToFileEnd:
    case CK_MarkToPageBegin:
    case CK_MarkToPageEnd:
    case CK_MarkScrollUp:
    case CK_MarkScrollDown:
    case CK_MarkParagraphUp:
    case CK_MarkParagraphDown:
        /* a mark command with alt-arrow */
    case CK_MarkColumnPageUp:
    case CK_MarkColumnPageDown:
    case CK_MarkColumnLeft:
    case CK_MarkColumnRight:
    case CK_MarkColumnUp:
    case CK_MarkColumnDown:
    case CK_MarkColumnScrollUp:
    case CK_MarkColumnScrollDown:
    case CK_MarkColumnParagraphUp:
    case CK_MarkColumnParagraphDown:
        edit->column_highlight = 0;
        if (edit->highlight == 0 || (edit->mark2 != -1 && edit->mark1 != edit->mark2))
        {
            edit_mark_cmd (edit, TRUE); /* clear */
            edit_mark_cmd (edit, FALSE);        /* marking on */
        }
        edit->highlight = 1;
        break;

        /* any other command */
    default:
        if (edit->highlight)
            edit_mark_cmd (edit, FALSE);        /* clear */
        edit->highlight = 0;
    }

    /* first check for undo */
    if (command == CK_Undo)
    {
        edit->redo_stack_reset = 0;
        edit_group_undo (edit);
        edit->found_len = 0;
        edit->prev_col = edit_get_col (edit);
        edit->search_start = edit->curs1;
        return;
    }
    /*  check for redo */
    if (command == CK_Redo)
    {
        edit->redo_stack_reset = 0;
        edit_do_redo (edit);
        edit->found_len = 0;
        edit->prev_col = edit_get_col (edit);
        edit->search_start = edit->curs1;
        return;
    }

    edit->redo_stack_reset = 1;

    /* An ordinary key press */
    if (char_for_insertion >= 0)
    {
        /* if non persistent selection and text selected */
        if (!option_persistent_selections && edit->mark1 != edit->mark2)
            edit_block_delete_cmd (edit);

        if (edit->overwrite)
        {
            /* remove char only one time, after input first byte, multibyte chars */
#ifdef HAVE_CHARSET
            if (!mc_global.utf8_display || edit->charpoint == 0)
#endif
                if (edit_get_byte (edit, edit->curs1) != '\n')

                    edit_delete (edit, FALSE);
        }
        if (option_cursor_beyond_eol && edit->over_col > 0)
            edit_insert_over (edit);
#ifdef HAVE_CHARSET
        if (char_for_insertion > 255 && !mc_global.utf8_display)
        {
            unsigned char str[6 + 1];
            size_t i = 0;
            int res;

            res = g_unichar_to_utf8 (char_for_insertion, (char *) str);
            if (res == 0)
            {
                str[0] = '.';
                str[1] = '\0';
            }
            else
            {
                str[res] = '\0';
            }
            while (str[i] != 0 && i <= 6)
            {
                char_for_insertion = str[i];
                edit_insert (edit, char_for_insertion);
                i++;
            }
        }
        else
#endif
            edit_insert (edit, char_for_insertion);

        if (option_auto_para_formatting)
        {
            format_paragraph (edit, 0);
            edit->force |= REDRAW_PAGE;
        }
        else
            check_and_wrap_line (edit);
        edit->found_len = 0;
        edit->prev_col = edit_get_col (edit);
        edit->search_start = edit->curs1;
        edit_find_bracket (edit);
        return;
    }

    switch (command)
    {
    case CK_TopOnScreen:
    case CK_BottomOnScreen:
    case CK_Top:
    case CK_Bottom:
    case CK_PageUp:
    case CK_PageDown:
    case CK_Home:
    case CK_End:
    case CK_Up:
    case CK_Down:
    case CK_Left:
    case CK_Right:
    case CK_WordLeft:
    case CK_WordRight:
        if (!option_persistent_selections && edit->mark2 >= 0)
        {
            if (edit->column_highlight)
                edit_push_undo_action (edit, COLUMN_ON);
            edit->column_highlight = 0;
            edit_mark_cmd (edit, TRUE);
        }
    }

    switch (command)
    {
    case CK_TopOnScreen:
    case CK_BottomOnScreen:
    case CK_MarkToPageBegin:
    case CK_MarkToPageEnd:
    case CK_Up:
    case CK_Down:
    case CK_WordLeft:
    case CK_WordRight:
    case CK_MarkToWordBegin:
    case CK_MarkToWordEnd:
    case CK_MarkUp:
    case CK_MarkDown:
    case CK_MarkColumnUp:
    case CK_MarkColumnDown:
        if (edit->mark2 == -1)
            break;              /*marking is following the cursor: may need to highlight a whole line */
    case CK_Left:
    case CK_Right:
    case CK_MarkLeft:
    case CK_MarkRight:
        edit->force |= REDRAW_CHAR_ONLY;
    }

    /* basic cursor key commands */
    switch (command)
    {
    case CK_BackSpace:
        /* if non persistent selection and text selected */
        if (!option_persistent_selections && edit->mark1 != edit->mark2)
            edit_block_delete_cmd (edit);
        else if (option_cursor_beyond_eol && edit->over_col > 0)
            edit->over_col--;
        else if (option_backspace_through_tabs && is_in_indent (edit))
        {
            while (edit_get_byte (edit, edit->curs1 - 1) != '\n' && edit->curs1 > 0)
                edit_backspace (edit, TRUE);
        }
        else if (option_fake_half_tabs && is_in_indent (edit) && right_of_four_spaces (edit))
        {
            int i;

            for (i = 0; i < HALF_TAB_SIZE; i++)
                edit_backspace (edit, TRUE);
        }
        else
            edit_backspace (edit, FALSE);
        break;
    case CK_Delete:
        /* if non persistent selection and text selected */
        if (!option_persistent_selections && edit->mark1 != edit->mark2)
            edit_block_delete_cmd (edit);
        else
        {
            if (option_cursor_beyond_eol && edit->over_col > 0)
                edit_insert_over (edit);

            if (option_fake_half_tabs && is_in_indent (edit) && left_of_four_spaces (edit))
            {
                int i;

                for (i = 1; i <= HALF_TAB_SIZE; i++)
                    edit_delete (edit, TRUE);
            }
            else
                edit_delete (edit, FALSE);
        }
        break;
    case CK_DeleteToWordBegin:
        edit->over_col = 0;
        edit_left_delete_word (edit);
        break;
    case CK_DeleteToWordEnd:
        if (option_cursor_beyond_eol && edit->over_col > 0)
            edit_insert_over (edit);

        edit_right_delete_word (edit);
        break;
    case CK_DeleteLine:
        edit_delete_line (edit);
        break;
    case CK_DeleteToHome:
        edit_delete_to_line_begin (edit);
        break;
    case CK_DeleteToEnd:
        edit_delete_to_line_end (edit);
        break;
    case CK_Enter:
        edit->over_col = 0;
        if (option_auto_para_formatting)
        {
            edit_double_newline (edit);
            if (option_return_does_auto_indent)
                edit_auto_indent (edit);
            format_paragraph (edit, 0);
        }
        else
        {
            edit_insert (edit, '\n');
            if (option_return_does_auto_indent)
                edit_auto_indent (edit);
        }
        break;
    case CK_Return:
        edit_insert (edit, '\n');
        break;

    case CK_MarkColumnPageUp:
        edit->column_highlight = 1;
    case CK_PageUp:
    case CK_MarkPageUp:
        edit_move_up (edit, w->lines - 1, 1);
        break;
    case CK_MarkColumnPageDown:
        edit->column_highlight = 1;
    case CK_PageDown:
    case CK_MarkPageDown:
        edit_move_down (edit, w->lines - 1, 1);
        break;
    case CK_MarkColumnLeft:
        edit->column_highlight = 1;
    case CK_Left:
    case CK_MarkLeft:
        if (option_fake_half_tabs && is_in_indent (edit) && right_of_four_spaces (edit))
        {
            if (option_cursor_beyond_eol && edit->over_col > 0)
                edit->over_col--;
            else
                edit_cursor_move (edit, -HALF_TAB_SIZE);
            edit->force &= (0xFFF - REDRAW_CHAR_ONLY);
        }
        else
            edit_left_char_move_cmd (edit);
        break;
    case CK_MarkColumnRight:
        edit->column_highlight = 1;
    case CK_Right:
    case CK_MarkRight:
        if (option_fake_half_tabs && is_in_indent (edit) && left_of_four_spaces (edit))
        {
            edit_cursor_move (edit, HALF_TAB_SIZE);
            edit->force &= (0xFFF - REDRAW_CHAR_ONLY);
        }
        else
            edit_right_char_move_cmd (edit);
        break;
    case CK_TopOnScreen:
    case CK_MarkToPageBegin:
        edit_begin_page (edit);
        break;
    case CK_BottomOnScreen:
    case CK_MarkToPageEnd:
        edit_end_page (edit);
        break;
    case CK_WordLeft:
    case CK_MarkToWordBegin:
        edit->over_col = 0;
        edit_left_word_move_cmd (edit);
        break;
    case CK_WordRight:
    case CK_MarkToWordEnd:
        edit->over_col = 0;
        edit_right_word_move_cmd (edit);
        break;
    case CK_MarkColumnUp:
        edit->column_highlight = 1;
    case CK_Up:
    case CK_MarkUp:
        edit_move_up (edit, 1, 0);
        break;
    case CK_MarkColumnDown:
        edit->column_highlight = 1;
    case CK_Down:
    case CK_MarkDown:
        edit_move_down (edit, 1, 0);
        break;
    case CK_MarkColumnParagraphUp:
        edit->column_highlight = 1;
    case CK_ParagraphUp:
    case CK_MarkParagraphUp:
        edit_move_up_paragraph (edit, 0);
        break;
    case CK_MarkColumnParagraphDown:
        edit->column_highlight = 1;
    case CK_ParagraphDown:
    case CK_MarkParagraphDown:
        edit_move_down_paragraph (edit, 0);
        break;
    case CK_MarkColumnScrollUp:
        edit->column_highlight = 1;
    case CK_ScrollUp:
    case CK_MarkScrollUp:
        edit_move_up (edit, 1, 1);
        break;
    case CK_MarkColumnScrollDown:
        edit->column_highlight = 1;
    case CK_ScrollDown:
    case CK_MarkScrollDown:
        edit_move_down (edit, 1, 1);
        break;
    case CK_Home:
    case CK_MarkToHome:
        edit_cursor_to_bol (edit);
        break;
    case CK_End:
    case CK_MarkToEnd:
        edit_cursor_to_eol (edit);
        break;
    case CK_Tab:
        /* if text marked shift block */
        if (edit->mark1 != edit->mark2 && !option_persistent_selections)
        {
            if (edit->mark2 < 0)
                edit_mark_cmd (edit, FALSE);
            edit_move_block_to_right (edit);
        }
        else
        {
            if (option_cursor_beyond_eol)
                edit_insert_over (edit);
            edit_tab_cmd (edit);
            if (option_auto_para_formatting)
            {
                format_paragraph (edit, 0);
                edit->force |= REDRAW_PAGE;
            }
            else
                check_and_wrap_line (edit);
        }
        break;

    case CK_InsertOverwrite:
        edit->overwrite = !edit->overwrite;
        break;

    case CK_Mark:
        if (edit->mark2 >= 0)
        {
            if (edit->column_highlight)
                edit_push_undo_action (edit, COLUMN_ON);
            edit->column_highlight = 0;
        }
        edit_mark_cmd (edit, FALSE);
        break;
    case CK_MarkColumn:
        if (!edit->column_highlight)
            edit_push_undo_action (edit, COLUMN_OFF);
        edit->column_highlight = 1;
        edit_mark_cmd (edit, FALSE);
        break;
    case CK_MarkAll:
        edit_set_markers (edit, 0, edit->last_byte, 0, 0);
        edit->force |= REDRAW_PAGE;
        break;
    case CK_Unmark:
        if (edit->column_highlight)
            edit_push_undo_action (edit, COLUMN_ON);
        edit->column_highlight = 0;
        edit_mark_cmd (edit, TRUE);
        break;
    case CK_MarkWord:
        if (edit->column_highlight)
            edit_push_undo_action (edit, COLUMN_ON);
        edit->column_highlight = 0;
        edit_mark_current_word_cmd (edit);
        break;
    case CK_MarkLine:
        if (edit->column_highlight)
            edit_push_undo_action (edit, COLUMN_ON);
        edit->column_highlight = 0;
        edit_mark_current_line_cmd (edit);
        break;

    case CK_Bookmark:
        book_mark_clear (edit, edit->curs_line, BOOK_MARK_FOUND_COLOR);
        if (book_mark_query_color (edit, edit->curs_line, BOOK_MARK_COLOR))
            book_mark_clear (edit, edit->curs_line, BOOK_MARK_COLOR);
        else
            book_mark_insert (edit, edit->curs_line, BOOK_MARK_COLOR);
        break;
    case CK_BookmarkFlush:
        book_mark_flush (edit, BOOK_MARK_COLOR);
        book_mark_flush (edit, BOOK_MARK_FOUND_COLOR);
        edit->force |= REDRAW_PAGE;
        break;
    case CK_BookmarkNext:
        if (edit->book_mark != NULL)
        {
            edit_book_mark_t *p;

            p = book_mark_find (edit, edit->curs_line);
            if (p->next != NULL)
            {
                p = p->next;
                if (p->line >= edit->start_line + w->lines || p->line < edit->start_line)
                    edit_move_display (edit, p->line - w->lines / 2);
                edit_move_to_line (edit, p->line);
            }
        }
        break;
    case CK_BookmarkPrev:
        if (edit->book_mark != NULL)
        {
            edit_book_mark_t *p;

            p = book_mark_find (edit, edit->curs_line);
            while (p->line == edit->curs_line)
                if (p->prev != NULL)
                    p = p->prev;
            if (p->line >= 0)
            {
                if (p->line >= edit->start_line + w->lines || p->line < edit->start_line)
                    edit_move_display (edit, p->line - w->lines / 2);
                edit_move_to_line (edit, p->line);
            }
        }
        break;

    case CK_Top:
    case CK_MarkToFileBegin:
        edit_move_to_top (edit);
        break;
    case CK_Bottom:
    case CK_MarkToFileEnd:
        edit_move_to_bottom (edit);
        break;

    case CK_Copy:
        if (option_cursor_beyond_eol && edit->over_col > 0)
            edit_insert_over (edit);
        edit_block_copy_cmd (edit);
        break;
    case CK_Remove:
        edit_block_delete_cmd (edit);
        break;
    case CK_Move:
        edit_block_move_cmd (edit);
        break;

    case CK_BlockShiftLeft:
        if (edit->mark1 != edit->mark2)
            edit_move_block_to_left (edit);
        break;
    case CK_BlockShiftRight:
        if (edit->mark1 != edit->mark2)
            edit_move_block_to_right (edit);
        break;
    case CK_Store:
        edit_copy_to_X_buf_cmd (edit);
        break;
    case CK_Cut:
        edit_cut_to_X_buf_cmd (edit);
        break;
    case CK_Paste:
        /* if non persistent selection and text selected */
        if (!option_persistent_selections && edit->mark1 != edit->mark2)
            edit_block_delete_cmd (edit);
        if (option_cursor_beyond_eol && edit->over_col > 0)
            edit_insert_over (edit);
        edit_paste_from_X_buf_cmd (edit);
        if (!option_persistent_selections && edit->mark2 >= 0)
        {
            if (edit->column_highlight)
                edit_push_undo_action (edit, COLUMN_ON);
            edit->column_highlight = 0;
            edit_mark_cmd (edit, TRUE);
        }
        break;
    case CK_History:
        edit_paste_from_history (edit);
        break;

    case CK_SaveAs:
        edit_save_as_cmd (edit);
        break;
    case CK_Save:
        edit_save_confirm_cmd (edit);
        break;
    case CK_BlockSave:
        edit_save_block_cmd (edit);
        break;
    case CK_InsertFile:
        edit_insert_file_cmd (edit);
        break;

    case CK_FilePrev:
        edit_load_back_cmd (edit);
        break;
    case CK_FileNext:
        edit_load_forward_cmd (edit);
        break;

    case CK_SyntaxChoose:
        edit_syntax_dialog (edit);
        break;

    case CK_Search:
        edit_search_cmd (edit, FALSE);
        break;
    case CK_SearchContinue:
        edit_search_cmd (edit, TRUE);
        break;
    case CK_Replace:
        edit_replace_cmd (edit, 0);
        break;
    case CK_ReplaceContinue:
        edit_replace_cmd (edit, 1);
        break;
    case CK_Complete:
        /* if text marked shift block */
        if (edit->mark1 != edit->mark2 && !option_persistent_selections)
            edit_move_block_to_left (edit);
        else
            edit_complete_word_cmd (edit);
        break;
    case CK_Find:
        edit_get_match_keyword_cmd (edit);
        break;

#ifdef HAVE_ASPELL
    case CK_SpellCheckCurrentWord:
        edit_suggest_current_word (edit);
        break;
    case CK_SpellCheck:
        edit_spellcheck_file (edit);
        break;
    case CK_SpellCheckSelectLang:
        edit_set_spell_lang ();
        break;
#endif

    case CK_Date:
        {
            char s[BUF_MEDIUM];
            /* fool gcc to prevent a Y2K warning */
            char time_format[] = "_c";
            time_format[0] = '%';

            FMT_LOCALTIME_CURRENT (s, sizeof (s), time_format);
            edit_print_string (edit, s);
            edit->force |= REDRAW_PAGE;
        }
        break;
    case CK_Goto:
        edit_goto_cmd (edit);
        break;
    case CK_ParagraphFormat:
        format_paragraph (edit, 1);
        edit->force |= REDRAW_PAGE;
        break;
    case CK_MacroDelete:
        edit_delete_macro_cmd (edit);
        break;
    case CK_MatchBracket:
        edit_goto_matching_bracket (edit);
        break;
    case CK_UserMenu:
        user_menu (edit, NULL, -1);
        break;
    case CK_Sort:
        edit_sort_cmd (edit);
        break;
    case CK_ExternalCommand:
        edit_ext_cmd (edit);
        break;
    case CK_Mail:
        edit_mail_dialog (edit);
        break;
#ifdef HAVE_CHARSET
    case CK_SelectCodepage:
        edit_select_codepage_cmd (edit);
        break;
#endif
    case CK_InsertLiteral:
        edit_insert_literal_cmd (edit);
        break;
    case CK_MacroStartStopRecord:
        edit_begin_end_macro_cmd (edit);
        break;
    case CK_RepeatStartStopRecord:
        edit_begin_end_repeat_cmd (edit);
        break;
    case CK_ExtendedKeyMap:
        edit->extmod = TRUE;
        break;
    default:
        break;
    }

    /* CK_PipeBlock */
    if ((command / CK_PipeBlock (0)) == 1)
        edit_block_process_cmd (edit, command - CK_PipeBlock (0));

    /* keys which must set the col position, and the search vars */
    switch (command)
    {
    case CK_Search:
    case CK_SearchContinue:
    case CK_Replace:
    case CK_ReplaceContinue:
    case CK_Complete:
        edit->prev_col = edit_get_col (edit);
        break;
    case CK_Up:
    case CK_MarkUp:
    case CK_MarkColumnUp:
    case CK_Down:
    case CK_MarkDown:
    case CK_MarkColumnDown:
    case CK_PageUp:
    case CK_MarkPageUp:
    case CK_MarkColumnPageUp:
    case CK_PageDown:
    case CK_MarkPageDown:
    case CK_MarkColumnPageDown:
    case CK_Top:
    case CK_MarkToFileBegin:
    case CK_Bottom:
    case CK_MarkToFileEnd:
    case CK_ParagraphUp:
    case CK_MarkParagraphUp:
    case CK_MarkColumnParagraphUp:
    case CK_ParagraphDown:
    case CK_MarkParagraphDown:
    case CK_MarkColumnParagraphDown:
    case CK_ScrollUp:
    case CK_MarkScrollUp:
    case CK_MarkColumnScrollUp:
    case CK_ScrollDown:
    case CK_MarkScrollDown:
    case CK_MarkColumnScrollDown:
        edit->search_start = edit->curs1;
        edit->found_len = 0;
        break;
    default:
        edit->found_len = 0;
        edit->prev_col = edit_get_col (edit);
        edit->search_start = edit->curs1;
    }
    edit_find_bracket (edit);

    if (option_auto_para_formatting)
    {
        switch (command)
        {
        case CK_BackSpace:
        case CK_Delete:
        case CK_DeleteToWordBegin:
        case CK_DeleteToWordEnd:
        case CK_DeleteToHome:
        case CK_DeleteToEnd:
            format_paragraph (edit, 0);
            edit->force |= REDRAW_PAGE;
        }
    }
}

/* --------------------------------------------------------------------------------------------- */

void
edit_stack_init (void)
{
    for (edit_stack_iterator = 0; edit_stack_iterator < MAX_HISTORY_MOVETO; edit_stack_iterator++)
    {
        edit_history_moveto[edit_stack_iterator].filename_vpath = NULL;
        edit_history_moveto[edit_stack_iterator].line = -1;
    }

    edit_stack_iterator = 0;
}

/* --------------------------------------------------------------------------------------------- */

void
edit_stack_free (void)
{
    for (edit_stack_iterator = 0; edit_stack_iterator < MAX_HISTORY_MOVETO; edit_stack_iterator++)
        vfs_path_free (edit_history_moveto[edit_stack_iterator].filename_vpath);
}

/* --------------------------------------------------------------------------------------------- */
/** move i lines */

void
edit_move_up (WEdit * edit, long i, gboolean do_scroll)
{
    edit_move_updown (edit, i, do_scroll, TRUE);
}

/* --------------------------------------------------------------------------------------------- */
/** move i lines */

void
edit_move_down (WEdit * edit, long i, gboolean do_scroll)
{
    edit_move_updown (edit, i, do_scroll, FALSE);
}

/* --------------------------------------------------------------------------------------------- */
