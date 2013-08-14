/**
 * Copyright (C) 2012-2013 Daniel Carl
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */

#include <gdk/gdkkeysyms.h>
#include <gdk/gdkkeysyms-compat.h>
#include "config.h"
#include "main.h"
#include "map.h"
#include "mode.h"

extern VbCore vb;

typedef struct {
    char *in;         /* input keys */
    int  inlen;       /* length of the input keys */
    char *mapped;     /* mapped keys */
    int  mappedlen;   /* length of the mapped keys string */
    char mode;        /* mode for which the map is available */
} Map;

static struct {
    GSList *list;
    char   queue[MAP_QUEUE_SIZE];   /* queue holding typed keys */
    int    qlen;                    /* pointer to last char in queue */
    int    resolved;                /* number of resolved keys (no mapping required) */
    char   showbuf[12];             /* buffer used to show ambiguous keys to the user */
    int    slen;                    /* pointer to last char in showbuf */
    guint  timout_id;               /* source id of the timeout function */
} map;

static char *map_convert_keys(char *in, int inlen, int *len);
static char *map_convert_keylabel(char *in, int inlen, int *len);
static gboolean map_timeout(gpointer data);
static void showcmd(char *keys, int keylen, gboolean append);
static void free_map(Map *map);


void map_cleanup(void)
{
    if (map.list) {
        g_slist_free_full(map.list, (GDestroyNotify)free_map);
    }
}

/**
 * Handle all key events, convert the key event into the internal used ASCII
 * representation and put this into the key queue to be mapped.
 */
gboolean map_keypress(GtkWidget *widget, GdkEventKey* event, gpointer data)
{
    unsigned int key = 0;
    if ((event->state & GDK_CONTROL_MASK) == 0
        && event->keyval > 0
        && event->keyval < 0xff
    ) {
        key = event->keyval;
    } else {
        /* convert chars A-Za-z with ctrl flag <ctrl-a> or <ctrl-A> -> \001
         * and <ctrl-z> -> \032 like vi */
        if (event->keyval == GDK_Escape) {
            key = CTRL('[');
        } else if (event->keyval == GDK_Tab) {
            key = CTRL('I');
        } else if (event->keyval == GDK_ISO_Left_Tab) {
            key = CTRL('O');
        } else if (event->keyval == GDK_Return) {
            key = '\n';
        } else if (event->keyval == GDK_BackSpace) {
            /* FIXME how to handle <C-S-Del> to remove selected Numbers in
             * hint mode */
            key = CTRL('H'); /* 0x08 */
        } else if (event->keyval == GDK_Up) {
            key = CTRL('P'); /* or ^Ok instead? */
        } else if (event->keyval == GDK_Down) {
            key = CTRL('N');
        } else if (event->keyval >= 0x41 && event->keyval <= 0x5d) {/* chars A-] */
            key = event->keyval - 0x40;
        } else if (event->keyval >= 0x61 && event->keyval <= 0x7a) {/* chars a-z */
            key = event->keyval - 0x60;
        }
    }

    /* set initial value for the flag that should be changed in the modes key
     * handler functions */
    vb.state.processed_key = false;
    if (key) {
        map_handle_keys((char*)(&key), 1);
    }
    return vb.state.processed_key;
}


/**
 * Added the given key sequence ot the key queue and precesses the mapping of
 * chars. The key sequence do not need to be NUL terminated.
 * Keylen of 0 signalized a key timeout.
 */
MapState map_handle_keys(const char *keys, int keylen)
{
    int ambiguous;
    Map *match = NULL;
    gboolean timeout = (keylen == 0); /* keylen 0 signalized timeout */

    /* don't set the timeout function if a timeout is handled */
    if (!timeout) {
        /* if a previous timeout function was set remove this to start the
         * timeout new */
        if (map.timout_id) {
            g_source_remove(map.timout_id);
        }
        map.timout_id = g_timeout_add(1000, (GSourceFunc)map_timeout, NULL);
    }

    /* copy the keys onto the end of queue */
    while (map.qlen < LENGTH(map.queue) && keylen > 0) {
        map.queue[map.qlen++] = *keys++;
        keylen--;
    }

    /* try to resolve keys against the map */
    while (true) {
        /* send any resolved key to the parser */
        while (map.resolved > 0) {
            /* pop the next char from queue */
            map.resolved--;
            map.qlen--;

            /* get first char of queue */
            char qk = map.queue[0];
            /* move all other queue entries one step to the left */
            for (int i = 0; i < map.qlen; i++) {
                map.queue[i] = map.queue[i + 1];
            }

            /* remove the nomap flag */
            vb.mode->flags &= ~FLAG_NOMAP;

            /* send the key to the parser */
            if (RESULT_MORE != mode_handle_key((unsigned int)qk)) {
                showcmd(NULL, 0, false);
            }
        }

        /* if all keys where processed return MAP_DONE */
        if (map.qlen == 0) {
            map.resolved = 0;
            return match ? MAP_DONE : MAP_NOMATCH;
        }

        /* try to find matching maps */
        match     = NULL;
        ambiguous = 0;
        if (!(vb.mode->flags & FLAG_NOMAP)) {
            for (GSList *l = map.list; l != NULL; l = l->next) {
                Map *m = (Map*)l->data;
                /* ignore maps for other modes */
                if (m->mode != vb.mode->id) {
                    continue;
                }

                /* find ambiguous matches */
                if (!timeout && m->inlen > map.qlen && !strncmp(m->in, map.queue, map.qlen)) {
                    ambiguous++;
                    showcmd(map.queue, map.qlen, false);
                }
                /* complete match or better/longer match than previous found */
                if (m->inlen <= map.qlen
                    && !strncmp(m->in, map.queue, m->inlen)
                    && (!match || match->inlen < m->inlen)
                ) {
                    /* backup this found possible match */
                    match = m;
                }
            }
        }

        /* if there are ambiguous matches return MAP_KEY and flush queue
         * after a timeout if the user do not type more keys */
        if (ambiguous) {
            return MAP_AMBIGUOUS;
        }

        /* replace the matched chars from queue by the cooked string that
         * is the result of the mapping */
        if (match) {
            int i, j;
            if (match->inlen < match->mappedlen) {
                /* make some space within the queue */
                for (i = map.qlen + match->mappedlen - match->inlen, j = map.qlen; j > match->inlen; ) {
                    map.queue[--i] = map.queue[--j];
                }
            } else if (match->inlen > match->mappedlen) {
                /* delete some keys */
                for (i = match->mappedlen, j = match->inlen; i < map.qlen; ) {
                    map.queue[i++] = map.queue[j++];
                }
            }

            /* copy the mapped string into the queue */
            strncpy(map.queue, match->mapped, match->mappedlen);
            map.qlen += match->mappedlen - match->inlen;
            if (match->inlen <= match->mappedlen) {
                map.resolved = match->inlen;
            } else {
                map.resolved = match->mappedlen;
            }
        } else {
            /* first char is not mapped but resolved */
            map.resolved = 1;
            showcmd(map.queue, map.resolved, true);
        }
    }

    /* should never be reached */
    return MAP_DONE;
}

void map_insert(char *in, char *mapped, char mode)
{
    int inlen, mappedlen;
    char *lhs = map_convert_keys(in, strlen(in), &inlen);
    char *rhs = map_convert_keys(mapped, strlen(mapped), &mappedlen);

    /* TODO replace keysymbols in 'in' and 'mapped' string */
    Map *new = g_new(Map, 1);
    new->in        = lhs;
    new->inlen     = inlen;
    new->mapped    = rhs;
    new->mappedlen = mappedlen;
    new->mode      = mode;

    map.list = g_slist_prepend(map.list, new);
}

gboolean map_delete(char *in, char mode)
{
    int len;
    char *lhs = map_convert_keys(in, strlen(in), &len);

    for (GSList *l = map.list; l != NULL; l = l->next) {
        Map *m = (Map*)l->data;

        /* remove only if the map's lhs matches the given key sequence */
        if (m->mode == mode && m->inlen == len && !strcmp(m->in, lhs)) {
            /* remove the found list item */
            map.list = g_slist_delete_link(map.list, l);

            return true;
        }
    }

    return false;
}

/**
 * Converts a keysequence into a internal raw keysequence.
 * Returned keyseqence must be freed if not used anymore.
 */
static char *map_convert_keys(char *in, int inlen, int *len)
{
    int symlen, rawlen;
    char *p, *dest, *raw;
    char ch[1];
    GString *str = g_string_new("");

    *len = 0;
    for (p = in; p < &in[inlen]; p++) {
        /* if it starts not with < we can add it literally */
        if (*p != '<') {
            g_string_append_len(str, p, 1);
            *len += 1;
            continue;
        }

        /* search matching > of symbolic name */
        symlen = 1;
        do {
            if (&p[symlen] == &in[inlen]
                || p[symlen] == '<'
                || p[symlen] == ' '
            ) {
                break;
            }
        } while (p[symlen++] != '>');

        raw    = NULL;
        rawlen = 0;
        /* check if we found a real keylabel */
        if (p[symlen - 1] == '>') {
            if (symlen == 5 && p[2] == '-') {
                /* is it a <C-X> */
                if (p[1] == 'C') {
                    /* TODO add macro to check if the char is a upper or lower
                     * with these ranges */
                    if (p[3] >= 0x41 && p[3] <= 0x5d) {
                        ch[0]  = p[3] - 0x40;
                        raw    = ch;
                        rawlen = 1;
                    } else if (p[3] >= 0x61 && p[3] <= 0x7a) {
                        ch[0]  = p[3] - 0x60;
                        raw    = ch;
                        rawlen = 1;
                    }
                }
            }

            if (!raw) {
                raw = map_convert_keylabel(p, symlen, &rawlen);
            }
        }

        /* we found no known keylabel - so use the chars literally */
        if (!rawlen) {
            rawlen = symlen;
            raw    = p;
        }

        /* write the converted keylabel into the buffer */
        g_string_append_len(str, raw, rawlen);

        /* move p after the keylabel */
        p += symlen - 1;

        *len += rawlen;
    }
    dest = str->str;

    /* don't free the character data of the GString */
    g_string_free(str, false);

    return dest;
}

/**
 * Translate given key string into a internal representation <cr> -> \n.
 * The len of the translated key sequence is put into given *len pointer.
 */
static char *map_convert_keylabel(char *in, int inlen, int *len)
{
    static struct {
        char *label;
        int  len;
        char *ch;
        int  chlen;
    } keys[] = {
        {"<CR>",  4, "\n",   1},
        {"<Tab>", 5, "\t",   1},
        {"<Esc>", 5, "\x1b", 1}
    };

    for (int i = 0; i < LENGTH(keys); i++) {
        if (inlen == keys[i].len && !strncmp(keys[i].label, in, inlen)) {
            *len = keys[i].chlen;
            return keys[i].ch;
        }
    }
    *len = 0;

    return NULL;
}

/**
 * Timeout function to signalize a key timeout to the map.
 */
static gboolean map_timeout(gpointer data)
{
    /* signalize the timeout to the key handler */
    map_handle_keys("", 0);

    /* call only once */
    return false;
}

/**
 * Add given keys to the show command queue to show them to the user.
 * If the keylen of 0 is given, the show command queue is cleared.
 */
static void showcmd(char *keys, int keylen, gboolean append)
{
    /* make sure we keep in buffer range also for ^X chars */
    int max = LENGTH(map.showbuf) - 2;

    if (!append) {
        map.slen = 0;
    }

    /* truncate the buffer */
    if (!keylen) {
        map.showbuf[(map.slen = 0)] = '\0';
    } else {
        /* TODO if not all keys would fit into the buffer use the last significat
         * chars instead */
        while (keylen-- && map.slen < max) {
            char key = *keys++;
            if (IS_CTRL(key)) {
                map.showbuf[map.slen++] = '^';
                map.showbuf[map.slen++] = CTRL(key);
            } else {
                map.showbuf[map.slen++] = key;
            }
        }
        map.showbuf[map.slen] = '\0';
    }

    /* show the typed keys */
    gtk_label_set_text(GTK_LABEL(vb.gui.statusbar.cmd), map.showbuf);
}

static void free_map(Map *map)
{
    g_free(map->in);
    g_free(map->mapped);
    g_free(map);
}
