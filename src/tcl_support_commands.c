/*
 * Copyright (c) Yiftach Tzori 2009-2012.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */
#include <config.h>
#include "globals.h"
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <string.h>
#include <ctype.h>
#include "gtk23compat.h"
#include "analyzer.h"
#include "tree.h"
#include "symbol.h"
#include "vcd.h"
#include "lx2.h"
#include "busy.h"
#include "debug.h"
#include "tcl_helper.h"
#include "tcl_support_commands.h"

/* **
 * Main function called by gtkwavetcl_forceOpenTreeNode
 * Inputs:
 *   char *name :: hierachical path to open
 * Output:
 *   One of:
 *     SST_NODE_FOUND - if path is in the dump file
 *     SST_NODE_NOT_EXIST - is path is not in the dump
 *     SST_TREE_NOT_EXIST - is Tree widget does not exist
 * Side effects:
 *    If  path is in the dump then its tree is opened and scrolled
 *    to be it to display. Node is selected and associated signals
 *    are displayed.
 *    No change in any other case
 */

int SST_open_node(char *name)
{
    int rv;

    if (name) {
        int name_len = strlen(name);
        char *name2 = g_alloca(name_len + 2);

        memcpy(name2, name, name_len);
        strcpy(name2 + name_len, ".");

        rv = force_open_tree_node(name2, 1, NULL);
        if (rv == SST_NODE_FOUND) {
            memcpy(name2, name, name_len);
            strcpy(name2 + name_len, ".");

            select_tree_node(name2);
        }
    } else {
        rv = SST_NODE_NOT_EXIST;
    }

    return rv;
}
/* ===== Double link lists */
llist_p *llist_new(llist_u v, ll_elem_type type, int arg)
{
    llist_p *p = (llist_p *)malloc_2(sizeof(llist_p));
    p->next = p->prev = NULL;
    switch (type) {
        case LL_INT:
            p->u.i = v.i;
            break;
        case LL_UINT:
            p->u.u = v.u;
            break;
        case LL_TIMETYPE:
            p->u.tt = v.tt;
            break;
        case LL_CHAR:
            p->u.c = v.c;
            break;
        case LL_SHORT:
            p->u.s = v.s;
            break;
        case LL_STR:
            if (arg == -1)
                p->u.str = strdup_2(v.str);
            else {
                p->u.str = (char *)malloc_2(arg);
                strncpy(p->u.str, v.str, arg);
                p->u.str[arg] = '\0';
            }
            break;
        case LL_VOID_P:
            p->u.p = v.p;
            break;
        case LL_NONE:
        default:
            fprintf(stderr, "Internal error in llist_new(), type: %d\n", type);
            exit(255);
    }
    return p;
}

/*
 * append llist_p element ELEM to the of the list whose first member is HEAD amd
 * last is TAIL. and return the head of the list.
 * if HEAD is NULL ELEM is returned.
 * if TAIL is defined then ELEM is chained to it and TAIL is set to point to
 * ELEM
 */

llist_p *llist_append(llist_p *head, llist_p *elem, llist_p **tail)
{
    llist_p *p;
    if (*tail) {
        p = tail[0];
        p->next = elem;
        elem->prev = p;
        tail[0] = elem;
    } else {
        if (head) {
            for (p = head; p->next; p = p->next)
                ;
            p->next = elem;
            elem->prev = p;
        } else {
            head = elem;
        }
    }
    return head;
}
/*
 * Remove the last element from list whose first member is HEAD
 * if TYPE is LL_STR the memory allocated for this string is freed.
 * if the TYPE is LL_VOID_P that the caller supplied function pointer F() is
 *  is executed (if not NULL)
 * HEAD and TAIL are updated.
 */

llist_p *llist_remove_last(llist_p *head, llist_p **tail, ll_elem_type type, void *f(void *))
{
    if (head) {
        llist_p *p = tail[0];
        switch (type) {
            case LL_STR:
                free_2(p->u.str);
                break;
            case LL_VOID_P:
                if (f)
                    f(p->u.p);
                break;
            case LL_INT:
            case LL_UINT:
            case LL_CHAR:
            case LL_SHORT:
            case LL_TIMETYPE:
            case LL_NONE:
            default:
                fprintf(stderr, "Internal error in llist_remove_last(), type: %d\n", type);
                exit(255);
        }
        if (p->prev) {
            tail[0] = p->prev;
        } else {
            head = tail[0] = NULL;
        }
        free_2(p);
    }
    return head;
}

/* Destroy the list whose first member is HEAD
 * function pointer F() is called in type is LL_VOID_P
 * if TYPE is LL_STR then string is freed
 */
void llist_free(llist_p *head, ll_elem_type type, void *f(void *))
{
    llist_p *p = head, *p1;
    while (p) {
        p1 = p->next;
        switch (type) {
            case LL_STR:
                free_2(p->u.str);
                break;
            case LL_VOID_P:
                if (f)
                    f(p->u.p);
                break;
            case LL_INT:
            case LL_UINT:
            case LL_CHAR:
            case LL_SHORT:
            case LL_TIMETYPE:
            case LL_NONE:
            default:
                fprintf(stderr, "Internal error in llist_free(), type: %d\n", type);
                exit(255);
        }
        free_2(p);
        p = p1;
    }
}
/* ===================================================== */
/* Create a GwTrace structure that contains the bit-vector VEC
 * This is based on the function AddVector()
 */
GwTrace *BitVector_to_Trptr(GwBitVector *vec)
{
    GwTrace *t;
    int n;

    GLOBALS->signalwindow_width_dirty = 1;

    n = vec->nbits;
    t = calloc_2(1, sizeof(GwTrace));
    if (t == NULL) {
        fprintf(stderr, "Out of memory, can't add %s to analyzer\n", vec->bvname);
        return (0);
    }

    t->name = vec->bvname;

    if (GLOBALS->hier_max_level)
        t->name = hier_extract(t->name, GLOBALS->hier_max_level);

    t->flags = (n > 3) ? TR_HEX | TR_RJUSTIFY : TR_BIN | TR_RJUSTIFY;
    t->vector = TRUE;
    t->n.vec = vec;
    /* AddTrace( t ); */
    return (t);
}

GwTrace *find_first_highlighted_trace(void)
{
    GwTrace *t = GLOBALS->traces.first;
    while (t) {
        if (t->flags & TR_HIGHLIGHT) {
            if (!(t->flags & (TR_BLANK | TR_ANALOG_BLANK_STRETCH))) {
                break;
            }
        }
        t = t->t_next;
    }
    return (t);
}

/* Find is signal named NAME is on display and return is GwTrace * value
 * or NULL
 * NAME is a full hierarchical name, but may not in include range '[..:..]'
 *  information.
 */
GwTrace *is_signal_displayed(char *name)
{
    GwTrace *t = GLOBALS->traces.first;
    char *p = strchr(name, '['), *p1;
    unsigned int len, len1;
    if (p)
        *p = '\0';
    len = strlen(name);
    while (t) {
        int cc;
        if (t->vector) {
            p = t->n.vec->bvname;
        } else {
            if (t->n.vec) {
                p = t->n.nd->nname;
            } else {
                p = NULL;
            }
        }

        if (p) {
            p1 = strchr(p, '[');
            len1 = (p1) ? (unsigned int)(p1 - p) : strlen(p);
            cc = ((len == len1) && !strncmp(name, p, len));
            if (cc)
                break;
        }
        t = t->t_next;
    }
    return t;
}

/* Create a GwTrace * structure for ND and return its value
 * This is based on the function AddNodeTraceReturn()
 */
GwTrace *Node_to_Trptr(GwNode *nd)
{
    GwTrace *t = NULL;
    GwHistEnt *histpnt;
    GwHistEnt **harray;
    int histcount;
    int i;

    if (nd->mv.mvlfac)
        import_trace(nd);

    GLOBALS->signalwindow_width_dirty = 1;

    if ((t = calloc_2(1, sizeof(GwTrace *))) == NULL) {
        fprintf(stderr, "Out of memory, can't add to analyzer\n");
        return (0);
    }

    if (!nd->harray) { /* make quick array lookup for aet display */
        histpnt = &(nd->head);
        histcount = 0;

        while (histpnt) {
            histcount++;
            histpnt = histpnt->next;
        }

        nd->numhist = histcount;

        if (!(nd->harray = harray = malloc_2(histcount * sizeof(GwHistEnt *)))) {
            fprintf(stderr, "Out of memory, can't add to analyzer\n");
            free_2(t);
            return (0);
        }

        histpnt = &(nd->head);
        for (i = 0; i < histcount; i++) {
            *harray = histpnt;
            harray++;
            histpnt = histpnt->next;
        }
    }

    if (!GLOBALS->hier_max_level) {
        t->name = nd->nname;
    } else {
        t->name = hier_extract(nd->nname, GLOBALS->hier_max_level);
    }

    if (nd->extvals) { /* expansion vectors */
        int n;

        n = nd->msi - nd->lsi;
        if (n < 0)
            n = -n;
        n++;

        t->flags = ((n > 3) || (n < -3)) ? TR_HEX | TR_RJUSTIFY : TR_BIN | TR_RJUSTIFY;
    } else {
        t->flags |= TR_BIN; /* binary */
    }
    t->vector = FALSE;
    t->n.nd = nd;
    /* if(tret) *tret = t;		... for expand */
    return t;
}
/*
 * Search for the signal named (full path) NAME in the signal data base and
 * create a GwTrace structure for it
 * NAME is a full hierarchy name, but may not include range information.
 * Return the structure created or NULL
 */
GwTrace *sig_name_to_Trptr(char *name)
{
    GwTrace *t = NULL;
    int i, name_len;
    char *hfacname = NULL;
    GwSymbol *s = NULL, *s2;
    int len = 0;
    GwBitVector *v = NULL;
    GwBits *b = NULL;
    int pre_import = 0;

    GwFacs *facs = gw_dump_file_get_facs(GLOBALS->dump_file);
    guint numfacs = gw_facs_get_length(facs);

    if (name) {
        name_len = strlen(name);
        for (i = 0; i < numfacs; i++) {
            GwSymbol *fac = gw_facs_get(facs, i);

            hfacname = fac->name;
            if (!strcmp(name, hfacname) ||
                ((!strncmp(name, hfacname, name_len) && hfacname[name_len] == '['))) {
                s = fac;
                if ((s2 = s->vec_root)) {
                    s = s2;
                } else {
                    s2 = s;
                }

                if (GLOBALS->is_lx2) {
                    while (s2) {
                        if (s2->n->mv.mvlfac) /* the node doesn't exist yet! */
                        {
                            lx2_set_fac_process_mask(s2->n);
                            pre_import++;
                        }

                        s2 = s2->vec_chain;
                        len++;
                    }
                } else {
                    while (s2) {
                        s2 = s2->vec_chain;
                        len++;
                    }
                }
                break;
            }
            s = NULL;
        }

        if (s) {
            if (pre_import) {
                lx2_import_masked(); /* import any missing nodes */
            }

            if (len > 1) {
                if ((b = makevec_chain(NULL, s, len))) {
                    if ((v = bits2vector(b))) {
                        t = BitVector_to_Trptr(v);
                    } else {
                        free_2(b->name);
                        if (b->attribs)
                            free_2(b->attribs);
                        free_2(b);
                    }
                }
            } else {
                GwNode *node = s->n;
                t = Node_to_Trptr(node);
            }
        }
    }

    return t;
}

/* Return the base prefix for the signal value */
const char *signal_value_prefix(TraceFlagsType flags)
{
    if (flags & TR_BIN)
        return "0b";
    if (flags & TR_HEX)
        return "0x";
    if (flags & TR_OCT)
        return "0";
    return "";
}

/* ===================================================== */

llist_p *signal_change_list(char *sig_name,
                            int dir,
                            GwTime start_time,
                            GwTime end_time,
                            int max_elements)
{
    llist_p *l0_head = NULL, *l0_tail = NULL, *l1_head = NULL, *l_elem, *lp;
    llist_p *l1_tail = NULL;
    char *s, s1[1024];
    GwHistEnt *h_ptr;
    GwTrace *t = NULL;
    GwTrace *t_created = NULL;
    if (!sig_name) {
        t = (GwTrace *)find_first_highlighted_trace();
    } else {
        /* case of sig name, find the representing GwTrace structure */
        if (!(t = is_signal_displayed(sig_name)))
            t = t_created = sig_name_to_Trptr(sig_name);
    }
    if (t) { /* we have a signal */
        /* create a list of value change structs (hptrs or vptrs */
        int nelem = 0 /* , bw = -1 */; /* scan-build */
        GwTime tstart = (dir == STRACE_FORWARD) ? start_time : end_time;
        GwTime tend = (dir == STRACE_FORWARD) ? end_time : start_time;
        if ((dir == STRACE_BACKWARD) && (max_elements == 1)) {
            max_elements++;
        }
        if (!t->vector) {
            GwHistEnt *h;
            GwHistEnt *h1;
            int len = 0;
            /* scan-build :
            if(t->n.nd->extvals) {
              bw = abs(t->n.nd->msi - t->n.nd->lsi) + 1 ;
            }
            */
            h = bsearch_node(t->n.nd, tstart - t->shift);
            for (h1 = h; h1; h1 = h1->next) {
                if (h1->time <= tend) {
                    if (len++ < max_elements) {
                        llist_u llp;
                        llp.p = h1;
                        l_elem = llist_new(llp, LL_VOID_P, -1);
                        l0_head = llist_append(l0_head, l_elem, &l0_tail);
                        if (!l0_tail)
                            l0_tail = l0_head;
                    } else {
                        if (dir == STRACE_FORWARD)
                            break;
                        else {
                            if (!l0_head) /* null pointer deref found by scan-build */
                            {
                                llist_u llp;
                                llp.p = h1;
                                l_elem = llist_new(llp, LL_VOID_P, -1);
                                l0_head = llist_append(l0_head, l_elem, &l0_tail);
                                if (!l0_tail)
                                    l0_tail = l0_head;
                            }
                            l_elem = l0_head;
                            l0_head = l0_head->next; /* what scan-build flagged as null */
                            l0_head->prev = NULL;
                            l_elem->u.p = (void *)h1;
                            l_elem->next = NULL;
                            l_elem->prev = l0_tail;
                            l0_tail->next = l_elem;
                            l0_tail = l_elem;
                        }
                    }
                }
            }
        } else {
            GwVectorEnt *v;
            GwVectorEnt *v1;
            v = bsearch_vector(t->n.vec, tstart - t->shift);
            for (v1 = v; v1; v1 = v1->next) {
                if (v1->time <= tend) {
                    llist_u llp;
                    llp.p = v1;
                    l_elem = llist_new(llp, LL_VOID_P, -1);
                    l0_head = llist_append(l0_head, l_elem, &l0_tail);
                    if (!l0_tail)
                        l0_tail = l0_head;
                }
            }
        }
        lp = (start_time < end_time) ? l0_head : l0_tail;
        /* now create a linked list of time,value.. */
        while (lp && (nelem++ < max_elements)) {
            llist_u llp;
            llp.tt = ((t->vector) ? ((GwVectorEnt *)lp->u.p)->time : ((GwHistEnt *)lp->u.p)->time);
            l_elem = llist_new(llp, LL_TIMETYPE, -1);
            l1_head = llist_append(l1_head, l_elem, &l1_tail);
            if (!l1_tail)
                l1_tail = l1_head;
            if (t->vector == 0) {
                if (!t->n.nd->extvals) { /* really single bit */
                    switch (((GwHistEnt *)lp->u.p)->v.h_val) {
                        case '0':
                        case GW_BIT_0:
                            llp.str = (char *)"0";
                            l_elem = llist_new(llp, LL_STR, -1);
                            break;

                        case '1':
                        case GW_BIT_1:
                            llp.str = (char *)"1";
                            l_elem = llist_new(llp, LL_STR, -1);
                            break;

                        case 'x':
                        case 'X':
                        case GW_BIT_X:
                            llp.str = (char *)"x";
                            l_elem = llist_new(llp, LL_STR, -1);
                            break;

                        case 'z':
                        case 'Z':
                        case GW_BIT_Z:
                            llp.str = (char *)"z";
                            l_elem = llist_new(llp, LL_STR, -1);
                            break;

                        case 'h':
                        case 'H':
                        case GW_BIT_H:
                            llp.str = (char *)"h";
                            l_elem = llist_new(llp, LL_STR, -1);
                            break; /* added for GHW... */

                        case 'u':
                        case 'U':
                        case GW_BIT_U:
                            llp.str = (char *)"u";
                            l_elem = llist_new(llp, LL_STR, -1);
                            break;

                        case 'w':
                        case 'W':
                        case GW_BIT_W:
                            llp.str = (char *)"w";
                            l_elem = llist_new(llp, LL_STR, -1);
                            break;

                        case 'l':
                        case 'L':
                        case GW_BIT_L:
                            llp.str = (char *)"l";
                            l_elem = llist_new(llp, LL_STR, -1);
                            break;

                        case '-':
                        case GW_BIT_DASH:
                            llp.str = (char *)"-";
                            l_elem = llist_new(llp, LL_STR, -1);
                            break;

                        default:
                            llp.str = (char *)"?";
                            l_elem = llist_new(llp, LL_STR, -1);
                            break; /* ...added for GHW */
                    }
                } else { /* this is still an array */
                    h_ptr = (GwHistEnt *)lp->u.p;
                    if (h_ptr->flags & GW_HIST_ENT_FLAG_REAL) {
                        if (!(h_ptr->flags & GW_HIST_ENT_FLAG_STRING)) {
                            s = convert_ascii_real(t, &h_ptr->v.h_double);
                        } else {
                            s = convert_ascii_string((char *)h_ptr->v.h_vector);
                        }
                    } else {
                        s = convert_ascii_vec(t, h_ptr->v.h_vector);
                    }
                    if (s) {
                        sprintf(s1, "%s%s", signal_value_prefix(t->flags), s);
                        llp.str = s1;
                        l_elem = llist_new(llp, LL_STR, -1);
                    } else {
                        l1_head = llist_remove_last(l1_head, &l1_tail, LL_INT, NULL);
                    }
                }
            } else {
                sprintf(s1,
                        "%s%s",
                        signal_value_prefix(t->flags),
                        convert_ascii(t, (GwVectorEnt *)lp->u.p));
                llp.str = s1;
                l_elem = llist_new(llp, LL_STR, -1);
            }
            l1_head = llist_append(l1_head, l_elem, &l1_tail);
            lp = (start_time < end_time) ? lp->next : lp->prev;
        }
        llist_free(l0_head, LL_VOID_P, NULL);
    }

    if (t_created) {
        FreeTrace(t_created);
    }

    return l1_head;
}
