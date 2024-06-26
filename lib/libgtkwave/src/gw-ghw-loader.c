#include <stdint.h>
#include <glib.h>
#include <libghw.h>
#include <gtkwave.h>
#include "gw-ghw-loader.h"
#include "gw-ghw-file.h"
#include "gw-ghw-file-private.h"
#include "gw-time.h"

// TODO: remove!
#define WAVE_T_WHICH_UNDEFINED_COMPNAME (-1)

struct _GwGhwLoader
{
    GwLoader parent_instance;

    struct ghw_handler *h;
    GwNode **nxp;
    int sym_which;
    struct ghw_tree_node *gwt;
    struct ghw_tree_node *gwt_corr;
    int nbr_sig_ref;
    int num_glitches;
    int num_glitch_regions;
    char *asbuf;
    char *fac_name;
    int fac_name_len;
    int fac_name_max;
    gboolean warned;

    GSList *sym_chain;

    GwFacs *facs;
    GwTreeNode *treeroot;
    GwTime max_time;

    GwHistEntFactory *hist_ent_factory;
};

G_DEFINE_TYPE(GwGhwLoader, gw_ghw_loader, GW_TYPE_LOADER)

/************************ splay ************************/

/*
 * NOTE:
 * a GHW tree's "which" is not the same as a gtkwave "which"
 * in that gtkwave's points to the facs[] array and
 * GHW's functions as an alias handle.  The following
 * (now global) vars are used to resolve those differences...
 *
 * static GwNode **nxp;
 * static GwSymbol *sym_head = NULL, *sym_curr = NULL;
 * static int sym_which = 0;
 */

/*
 * pointer splay
 */
typedef struct ghw_tree_node ghw_Tree;
struct ghw_tree_node
{
    ghw_Tree *left, *right;
    void *item;
    int val_old;
    GwSymbol *sym;
};

long ghw_cmp_l(void *i, void *j)
{
    uintptr_t il = (uintptr_t)i, jl = (uintptr_t)j;
    return (il - jl);
}

static ghw_Tree *ghw_splay(void *i, ghw_Tree *t)
{
    /* Simple top down splay, not requiring i to be in the tree t.  */
    /* What it does is described above.                             */
    ghw_Tree N, *l, *r, *y;
    int dir;

    if (t == NULL)
        return t;
    N.left = N.right = NULL;
    l = r = &N;

    for (;;) {
        dir = ghw_cmp_l(i, t->item);
        if (dir < 0) {
            if (t->left == NULL)
                break;
            if (ghw_cmp_l(i, t->left->item) < 0) {
                y = t->left; /* rotate right */
                t->left = y->right;
                y->right = t;
                t = y;
                if (t->left == NULL)
                    break;
            }
            r->left = t; /* link right */
            r = t;
            t = t->left;
        } else if (dir > 0) {
            if (t->right == NULL)
                break;
            if (ghw_cmp_l(i, t->right->item) > 0) {
                y = t->right; /* rotate left */
                t->right = y->left;
                y->left = t;
                t = y;
                if (t->right == NULL)
                    break;
            }
            l->right = t; /* link left */
            l = t;
            t = t->right;
        } else {
            break;
        }
    }
    l->right = t->left; /* assemble */
    r->left = t->right;
    t->left = N.right;
    t->right = N.left;
    return t;
}

// Exit the program with return value 1 and print calling line
__attribute__((noreturn)) static void ghw_error_exit_line(char const *file, int line)
{
    fprintf(stderr, "Failed to load ghw file due to invalid data. Terminating.\n");
    fprintf(stderr, "Error raised at %s:%d.\n", file, line);
    exit(1);
}

#define ghw_error_exit() ghw_error_exit_line(__FILE__, __LINE__)

static ghw_Tree *ghw_insert(void *i, ghw_Tree *t, int val, GwSymbol *sym)
{
    /* Insert i into the tree t, unless it's already there.    */
    /* Return a pointer to the resulting tree.                 */

    ghw_Tree *n = g_new0(ghw_Tree, 1);

    if (n == NULL) {
        fprintf(stderr, "ghw_insert: ran out of memory, exiting.\n");
        exit(255);
    }
    n->item = i;
    n->val_old = val;
    n->sym = sym;
    if (t == NULL) {
        n->left = n->right = NULL;
        return n;
    }
    t = ghw_splay(i, t);
    int dir = ghw_cmp_l(i, t->item);
    if (dir < 0) {
        n->left = t->left;
        n->right = t;
        t->left = NULL;
        return n;
    } else if (dir > 0) {
        n->right = t->right;
        n->left = t;
        t->right = NULL;
        return n;
    } else { /* We get here if it's already in the tree */
        /* Don't add it again                      */
        g_free(n);
        return t;
    }
}

/*
 * chain together bits of the same fac
 */
int strand_pnt(char *s)
{
    int len = strlen(s) - 1;
    int i;
    int rc = -1;

    if (s[len] == ']') {
        for (i = len - 1; i > 0; i--) {
            if (((s[i] >= '0') && (s[i] <= '9')) || (s[i] == '-'))
                continue;
            if (s[i] != '[')
                break;
            return (i); /* position right before left bracket for strncmp */
        }
    }

    return (rc);
}

void rechain_facs(GwGhwLoader *self)
{
    GwSymbol *psr = NULL;
    GwSymbol *root = NULL;

    for (guint i = 0; i < gw_facs_get_length(self->facs); i++) {
        GwSymbol *fac = gw_facs_get(self->facs, i);

        if (psr != NULL) {
            GwSymbol *prev_fac = gw_facs_get(self->facs, i - 1);

            int ev1 = prev_fac->n->extvals;
            int ev2 = fac->n->extvals;

            if (!ev1 && !ev2) {
                char *fstr1 = prev_fac->name;
                char *fstr2 = fac->name;
                int p1 = strand_pnt(fstr1);
                int p2 = strand_pnt(fstr2);

                if (!root) {
                    if ((p1 >= 0) && (p1 == p2)) {
                        if (!strncmp(fstr1, fstr2, p1)) {
                            root = prev_fac;
                            root->vec_root = root;
                            root->vec_chain = fac;
                            fac->vec_root = root;
                        }
                    }
                } else {
                    if ((p1 >= 0) && (p1 == p2)) {
                        if (!strncmp(fstr1, fstr2, p1)) {
                            psr->vec_chain = fac;
                            fac->vec_root = root;
                        } else {
                            root = NULL;
                        }
                    } else {
                        root = NULL;
                    }
                }
            } else {
                root = NULL;
            }
        }

        psr = fac;
    }
}

/*
 * preserve tree->t_which ordering so hierarchy children index pointers don't get corrupted
 */

/* limited recursive version */

static void recurse_tree_build_whichcache(GwGhwLoader *self, GwTreeNode *t)
{
    if (t == NULL) {
        return;
    }

    GwTreeNode *t2 = t;
    int i;
    int cnt = 1;

    while ((t2 = t2->next)) {
        cnt++;
    }

    GwTreeNode **ar = g_new(GwTreeNode *, cnt);
    t2 = t;
    for (i = 0; i < cnt; i++) {
        ar[i] = t2;
        if (t2->child) {
            recurse_tree_build_whichcache(self, t2->child);
        }
        t2 = t2->next;
    }

    for (i = cnt - 1; i >= 0; i--) {
        t = ar[i];
        if (t->t_which >= 0) {
            GwSymbol *fac = gw_facs_get(self->facs, t->t_which);
            self->gwt = ghw_insert(t, self->gwt, t->t_which, fac);
        }
    }

    g_free(ar);
}

static void recurse_tree_fix_from_whichcache(GwGhwLoader *self, GwTreeNode *t)
{
    if (t == NULL) {
        return;
    }

    GwTreeNode *t2 = t;
    int i;
    int cnt = 1;

    while ((t2 = t2->next)) {
        cnt++;
    }

    GwTreeNode **ar = g_new(GwTreeNode *, cnt);
    t2 = t;
    for (i = 0; i < cnt; i++) {
        ar[i] = t2;
        if (t2->child) {
            recurse_tree_fix_from_whichcache(self, t2->child);
        }
        t2 = t2->next;
    }

    for (i = cnt - 1; i >= 0; i--) {
        t = ar[i];
        if (t->t_which >= 0) {
            self->gwt = ghw_splay(t, self->gwt);
            self->gwt_corr = ghw_splay(self->gwt->sym,
                                       self->gwt_corr); // all facs are in this tree so this is OK

            t->t_which = self->gwt_corr->val_old;
        }
    }

    g_free(ar);
}

static void incinerate_whichcache_tree(ghw_Tree *t)
{
    // Use a dynamic GPtrArray to store the pending nodes. In some cases, the
    // trees can be very deep, so we can't use explicit recursion here (because
    // we would run out of stack memory).
    GPtrArray *pending_nodes = g_ptr_array_new();

    g_ptr_array_add(pending_nodes, t);
    size_t n_pending = 1;

    while (n_pending > 0) {
        // Process the last entry in ptr_array to avoid shifts during removal
        ghw_Tree *p_current = g_ptr_array_index(pending_nodes, n_pending - 1);
        ghw_Tree current = *p_current; // store a temporary copy to use after free

        // Free the current node
        g_free(p_current);
        g_ptr_array_remove_index(pending_nodes, n_pending - 1);
        n_pending--;

        // Process the children of this node next
        if (current.left) {
            g_ptr_array_add(pending_nodes, current.left);
            n_pending++;
        }
        if (current.right) {
            g_ptr_array_add(pending_nodes, current.right);
            n_pending++;
        }
    }

    g_ptr_array_free(pending_nodes, TRUE);
}

/*
 * sort facs and also cache/reconGwTree->t_which pointers
 */
static void ghw_sortfacs(GwGhwLoader *self)
{
    recurse_tree_build_whichcache(self, self->treeroot);

    gw_facs_sort(self->facs);

    for (guint i = 0; i < gw_facs_get_length(self->facs); i++) {
        GwSymbol *fac = gw_facs_get(self->facs, i);
        self->gwt_corr = ghw_insert(fac, self->gwt_corr, i, NULL);
    }

    recurse_tree_fix_from_whichcache(self, self->treeroot);
    if (self->gwt != NULL) {
        incinerate_whichcache_tree(self->gwt);
        self->gwt = NULL;
    }
    if (self->gwt_corr) {
        incinerate_whichcache_tree(self->gwt_corr);
        self->gwt_corr = NULL;
    }
}

/*******************************************************************************/

static GwTreeNode *build_hierarchy_type(GwGhwLoader *self,
                                        union ghw_type *t,
                                        const char *pfx,
                                        unsigned int **sig);

static GwTreeNode *build_hierarchy_record(GwGhwLoader *self,
                                          const char *pfx,
                                          unsigned nbr_els,
                                          struct ghw_record_element *els,
                                          unsigned int **sig)
{
    GwTreeNode *last;
    GwTreeNode *c;
    unsigned int i;

    GwTreeNode *res = g_malloc0(sizeof(GwTreeNode) + strlen(pfx) + 1);
    strcpy(res->name, (char *)pfx);
    res->t_which = WAVE_T_WHICH_UNDEFINED_COMPNAME;

    last = NULL;
    for (i = 0; i < nbr_els; i++) {
        c = build_hierarchy_type(self, els[i].type, els[i].name, sig);
        if (last == NULL)
            res->child = c;
        else
            last->next = c;
        last = c;
    }
    return res;
}

static void build_hierarchy_array(GwGhwLoader *self,
                                  union ghw_type *arr,
                                  int dim,
                                  const char *pfx,
                                  GwTreeNode **res,
                                  unsigned int **sig)
{
    union ghw_type *idx;
    struct ghw_type_array *base = (struct ghw_type_array *)ghw_get_base_type(arr->sa.base);
    char *name = NULL;

    if ((unsigned int)dim == base->nbr_dim) {
        GwTreeNode *t;
        sprintf(self->asbuf, "%s]", pfx);
        name = g_strdup(self->asbuf);

        t = build_hierarchy_type(self, arr->sa.el, name, sig);

        g_free(name);

        if (*res != NULL)
            (*res)->next = t;
        *res = t;
        return;
    }

    idx = ghw_get_base_type(base->dims[dim]);
    switch (idx->kind) {
        case ghdl_rtik_type_i32: {
            int32_t v;
            char *nam;
            struct ghw_range_i32 *r;
            /* GwTreeNode *last; */
            int len;

            /* last = NULL; */
            if (arr->sa.rngs[dim]->kind != ghdl_rtik_type_i32)
                ghw_error_exit();
            r = &arr->sa.rngs[dim]->i32;
            len = ghw_get_range_length((union ghw_range *)r);
            if (len <= 0)
                break;
            v = r->left;
            while (1) {
                sprintf(self->asbuf, "%s%c" GHWPRI32, pfx, dim == 0 ? '[' : ',', v);
                nam = g_strdup(self->asbuf);
                build_hierarchy_array(self, arr, dim + 1, nam, res, sig);
                g_free(nam);
                if (v == r->right)
                    break;
                if (r->dir == 0)
                    v++;
                else
                    v--;
            }
        }
            return;

        case ghdl_rtik_type_e8: {
            int32_t v;
            char *nam;
            struct ghw_range_e8 *r;
            /* GwTreeNode *last; */
            int len;

            /* last = NULL; */
            if (arr->sa.rngs[dim]->kind != ghdl_rtik_type_e8)
                ghw_error_exit();
            r = &arr->sa.rngs[dim]->e8;
            len = ghw_get_range_length((union ghw_range *)r);
            if (len <= 0)
                break;
            v = r->left;
            while (1) {
                sprintf(self->asbuf, "%s%c" GHWPRI32, pfx, dim == 0 ? '[' : ',', v);
                nam = g_strdup(self->asbuf);
                build_hierarchy_array(self, arr, dim + 1, nam, res, sig);
                g_free(nam);
                if (v == r->right)
                    break;
                if (r->dir == 0)
                    v++;
                else
                    v--;
            }
        }
            return;
        /* PATCH-BEGIN: */
        case ghdl_rtik_type_b2: {
            int32_t v;
            char *nam;
            struct ghw_range_b2 *r;
            /* GwTreeNode *last; */
            int len;

            /* last = NULL; */
            if (arr->sa.rngs[dim]->kind != ghdl_rtik_type_b2)
                ghw_error_exit();
            r = &arr->sa.rngs[dim]->b2;
            len = ghw_get_range_length((union ghw_range *)r);
            if (len <= 0)
                break;
            v = r->left;
            while (1) {
                sprintf(self->asbuf, "%s%c" GHWPRI32, pfx, dim == 0 ? '[' : ',', v);
                nam = g_strdup(self->asbuf);
                build_hierarchy_array(self, arr, dim + 1, nam, res, sig);
                g_free(nam);
                if (v == r->right)
                    break;
                if (r->dir == 0)
                    v++;
                else
                    v--;
            }
        }
            return;
            /* PATCH-END: */
        default:
            fprintf(stderr, "build_hierarchy_array: unhandled type %d\n", idx->kind);
            abort();
    }
}

static GwTreeNode *build_hierarchy_type(GwGhwLoader *self,
                                        union ghw_type *t,
                                        const char *pfx,
                                        unsigned int **sig)
{
    switch (t->kind) {
        case ghdl_rtik_subtype_scalar:
            return build_hierarchy_type(self, t->ss.base, pfx, sig);

        case ghdl_rtik_type_b2:
        case ghdl_rtik_type_e8:
        case ghdl_rtik_type_f64:
        case ghdl_rtik_type_i32:
        case ghdl_rtik_type_i64:
        case ghdl_rtik_type_p32:
        case ghdl_rtik_type_p64: {
            GwSymbol *s = g_new0(GwSymbol, 1);
            self->sym_chain = g_slist_prepend(self->sym_chain, s);

            self->nbr_sig_ref++;
            GwTreeNode *res = g_malloc0(sizeof(GwTreeNode) + strlen(pfx) + 1);
            strcpy(res->name, (char *)pfx);
            // last element is GHW_NO_SIG, don't increment beyond it
            if (**sig == GHW_NO_SIG)
                ghw_error_exit();
            res->t_which = *(*sig)++;

            size_t nxp_idx = (size_t)res->t_which;
            if (nxp_idx >= self->h->nbr_sigs)
                ghw_error_exit();
            s->n = self->nxp[nxp_idx];
            return res;
        }

        case ghdl_rtik_subtype_array:
        case ghdl_rtik_subtype_array_ptr: {
            GwTreeNode *res = g_malloc0(sizeof(GwTreeNode) + strlen(pfx) + 1);
            strcpy(res->name, (char *)pfx);
            res->t_which = WAVE_T_WHICH_UNDEFINED_COMPNAME;
            GwTreeNode *r = res;
            build_hierarchy_array(self, t, 0, "", &res, sig);
            r->child = r->next;
            r->next = NULL;
            return r;
        }

        case ghdl_rtik_type_record:
            return build_hierarchy_record(self, pfx, t->rec.nbr_fields, t->rec.els, sig);

        case ghdl_rtik_subtype_record:
            return build_hierarchy_record(self, pfx, t->sr.base->nbr_fields, t->sr.els, sig);

        default:
            fprintf(stderr, "build_hierarchy_type: unhandled type %d\n", t->kind);
            abort();
    }
}

/* Create the gtkwave tree from the GHW hierarchy.  */

static GwTreeNode *build_hierarchy(GwGhwLoader *self, struct ghw_hie *hie)
{
    GwTreeNode *t;
    GwTreeNode *t_ch;
    GwTreeNode *prev;
    struct ghw_hie *ch;
    unsigned char ttype;

    switch (hie->kind) {
        case ghw_hie_design:
        case ghw_hie_block:
        case ghw_hie_instance:
        case ghw_hie_generate_for:
        case ghw_hie_generate_if:
        case ghw_hie_package:

            /* Convert kind.  */
            switch (hie->kind) {
                case ghw_hie_design:
                    ttype = GW_TREE_KIND_VHDL_ST_DESIGN;
                    break;
                case ghw_hie_block:
                    ttype = GW_TREE_KIND_VHDL_ST_BLOCK;
                    break;
                case ghw_hie_instance:
                    ttype = GW_TREE_KIND_VHDL_ST_INSTANCE;
                    break;
                case ghw_hie_generate_for:
                    ttype = GW_TREE_KIND_VHDL_ST_GENFOR;
                    break;
                case ghw_hie_generate_if:
                    ttype = GW_TREE_KIND_VHDL_ST_GENIF;
                    break;
                case ghw_hie_package:
                default:
                    ttype = GW_TREE_KIND_VHDL_ST_PACKAGE;
                    break;
            }

            /* For iterative generate, add the index.  */
            if (hie->kind == ghw_hie_generate_for) {
                char buf[128];
                int name_len, buf_len;
                char *n;

                ghw_get_value(buf, sizeof(buf), hie->u.blk.iter_value, hie->u.blk.iter_type);
                name_len = strlen(hie->name);
                buf_len = strlen(buf);

                t = g_malloc0(sizeof(GwTreeNode) + (2 + buf_len + name_len + 1));
                t->kind = ttype;
                n = t->name;

                memcpy(n, hie->name, name_len);
                n += name_len;
                *n++ = '[';
                memcpy(n, buf, buf_len);
                n += buf_len;
                *n++ = ']';
                *n = 0;
            } else {
                if (hie->name) {
                    t = g_malloc0(sizeof(GwTreeNode) + strlen(hie->name) + 1);
                    t->kind = ttype;
                    strcpy(t->name, (char *)hie->name);
                } else {
                    t = g_malloc0(sizeof(GwTreeNode) + 1);
                    t->kind = ttype;
                }
            }

            t->t_which = WAVE_T_WHICH_UNDEFINED_COMPNAME;

            /* Recurse.  */
            prev = NULL;
            for (ch = hie->u.blk.child; ch != NULL; ch = ch->brother) {
                t_ch = build_hierarchy(self, ch);
                if (t_ch != NULL) {
                    if (prev == NULL)
                        t->child = t_ch;
                    else
                        prev->next = t_ch;
                    prev = t_ch;
                }
            }
            return t;

        case ghw_hie_process:
            return NULL;

        case ghw_hie_signal:
        case ghw_hie_port_in:
        case ghw_hie_port_out:
        case ghw_hie_port_inout:
        case ghw_hie_port_buffer:
        case ghw_hie_port_linkage: {
            unsigned int *ptr = hie->u.sig.sigs;

            /* Convert kind.  */
            switch (hie->kind) {
                case ghw_hie_signal:
                    ttype = GW_TREE_KIND_VHDL_ST_SIGNAL;
                    break;
                case ghw_hie_port_in:
                    ttype = GW_TREE_KIND_VHDL_ST_PORTIN;
                    break;
                case ghw_hie_port_out:
                    ttype = GW_TREE_KIND_VHDL_ST_PORTOUT;
                    break;
                case ghw_hie_port_inout:
                    ttype = GW_TREE_KIND_VHDL_ST_PORTINOUT;
                    break;
                case ghw_hie_port_buffer:
                    ttype = GW_TREE_KIND_VHDL_ST_BUFFER;
                    break;
                case ghw_hie_port_linkage:
                default:
                    ttype = GW_TREE_KIND_VHDL_ST_LINKAGE;
                    break;
            }

            /* Convert type.  */
            t = build_hierarchy_type(self, hie->u.sig.type, hie->name, &ptr);
            if (*ptr != 0)
                abort();
            if (t) {
                t->kind = ttype;
            }
            return t;
        }
        default:
            fprintf(stderr, "ghw: build_hierarchy: cannot handle hie %d\n", hie->kind);
            abort();
    }
}

void facs_debug(GwGhwLoader *self)
{
    for (guint i = 0; i < gw_facs_get_length(self->facs); i++) {
        GwSymbol *fac = gw_facs_get(self->facs, i);
        GwNode *n = fac->n;
        printf("%d: %s\n", i, n->nname);
        if (n->extvals) {
            printf("  ext: %d - %d\n", n->msi, n->lsi);
        }
        for (GwHistEnt *h = &n->head; h; h = h->next) {
            printf("  time:%" GW_TIME_FORMAT " flags:%02x vect:%p\n",
                   h->time,
                   h->flags,
                   h->v.h_vector);
        }
    }
}

static void create_facs(GwGhwLoader *self)
{
    self->facs = gw_facs_new(self->nbr_sig_ref);

    guint i = 0;
    for (GSList *iter = self->sym_chain; iter != NULL; iter = iter->next, i++) {
        GwSymbol *symbol = iter->data;
        gw_facs_set(self->facs, i, symbol);
    }

    struct ghw_handler *h = self->h;

    for (i = 0; i < h->nbr_sigs; i++) {
        GwNode *n = self->nxp[i];

        if (h->sigs[i].type)
            switch (h->sigs[i].type->kind) {
                case ghdl_rtik_type_b2:
                    if (h->sigs[i].type->en.wkt == ghw_wkt_bit) {
                        n->extvals = 0;
                        break;
                    }
                    /* FALLTHROUGH */

                case ghdl_rtik_type_e8:
                    if (h->sigs[i].type->en.wkt == ghw_wkt_std_ulogic) {
                        n->extvals = 0;
                        break;
                    }
                    /* FALLTHROUGH */

                case ghdl_rtik_type_i32:
                case ghdl_rtik_type_p32:
                    n->extvals = 1;
                    n->msi = 31;
                    n->lsi = 0;
                    n->vartype = GW_VAR_TYPE_VCD_INTEGER;
                    break;

                case ghdl_rtik_type_i64:
                case ghdl_rtik_type_p64:
                    n->extvals = 1;
                    n->msi = 63;
                    n->lsi = 0;
                    n->vartype = GW_VAR_TYPE_VCD_INTEGER;
                    break;

                case ghdl_rtik_type_e32: /* ajb: what is e32? */
                case ghdl_rtik_type_f64:
                    n->extvals = 1;
                    n->msi = n->lsi = 0;
                    break;

                default:
                    fprintf(stderr, "ghw:create_facs: unhandled kind %d\n", h->sigs[i].type->kind);
                    n->extvals = 0;
            }
    }
}

static void set_fac_name_1(GwGhwLoader *self, GwTreeNode *t)
{
    for (; t != NULL; t = t->next) {
        int prev_len = self->fac_name_len;

        /* Complete the name.  */
        if (t->name[0]) /* originally (t->name != NULL) when using pointers */
        {
            int len;

            len = strlen(t->name) + 1;
            if (len + self->fac_name_len >= self->fac_name_max) {
                self->fac_name_max *= 2;
                if (self->fac_name_max <= len + self->fac_name_len)
                    self->fac_name_max = len + self->fac_name_len + 1;
                self->fac_name = g_realloc(self->fac_name, self->fac_name_max);
            }
            if (t->name[0] != '[') {
                self->fac_name[self->fac_name_len] = '.';
                /* The NUL is copied, since LEN is 1 + strlen.  */
                memcpy(self->fac_name + self->fac_name_len + 1, t->name, len);
                self->fac_name_len += len;
            } else {
                memcpy(self->fac_name + self->fac_name_len, t->name, len);
                self->fac_name_len += (len - 1);
            }
        }

        if (t->t_which >= 0) {
            GwSymbol *s = self->sym_chain->data;

            s->name = g_strdup(self->fac_name);
            size_t nxp_idx = (size_t)t->t_which;
            if (nxp_idx > self->h->nbr_sigs)
                ghw_error_exit();
            s->n = self->nxp[nxp_idx];
            if (!s->n->nname)
                s->n->nname = s->name;

            t->t_which = self->sym_which++; /* patch in gtkwave "which" as node is correct */

            self->sym_chain = g_slist_delete_link(self->sym_chain, self->sym_chain);
        }

        if (t->child != NULL) {
            set_fac_name_1(self, t->child);
        }

        /* Revert name.  */
        self->fac_name_len = prev_len;
        self->fac_name[self->fac_name_len] = 0;
    }
}

static void set_fac_name(GwGhwLoader *self)
{
    if (self->fac_name_max == 0) {
        self->fac_name_max = 1024;
        self->fac_name = g_malloc(self->fac_name_max);
    }

    self->fac_name_len = 3;
    memcpy(self->fac_name, "top", 4);
    set_fac_name_1(self, self->treeroot);
}

static void add_history(GwGhwLoader *self, GwNode *n, int sig_num)
{
    GwHistEnt *he;
    struct ghw_sig *sig = &self->h->sigs[sig_num];
    union ghw_type *sig_type = sig->type;
    int flags;
    int is_vector = 0;
    int is_double = 0;

    if (sig_type == NULL) {
        return;
    }

    switch (sig_type->kind) {
        case ghdl_rtik_type_i32:
        case ghdl_rtik_type_i64:
        case ghdl_rtik_type_p32:
        case ghdl_rtik_type_p64:
            flags = 0;
            break;

        case ghdl_rtik_type_b2:
            if (sig_type->en.wkt == ghw_wkt_bit) {
                flags = 0;
                break;
            }
            /* FALLTHROUGH */

        case ghdl_rtik_type_e8:
            if (sig_type->en.wkt == ghw_wkt_std_ulogic) {
                flags = 0;
                break;
            }
            /* FALLTHROUGH */

        case ghdl_rtik_type_e32:
            flags = GW_HIST_ENT_FLAG_STRING | GW_HIST_ENT_FLAG_REAL;
            if (GW_HIST_ENT_FLAG_STRING == 0) {
                if (!self->warned) {
                    fprintf(stderr, "warning: do not compile with STRICT_VCD\n");
                    self->warned = TRUE;
                }
                return;
            }
            break;

        case ghdl_rtik_type_f64:
            flags = GW_HIST_ENT_FLAG_REAL;
            break;

        default:
            fprintf(stderr, "ghw:add_history: unhandled kind %d\n", sig->type->kind);
            return;
    }

    if (!n->curr) {
        he = gw_hist_ent_factory_alloc(self->hist_ent_factory);
        he->flags = flags;
        he->time = -1;
        he->v.h_vector = NULL;

        n->head.next = he;
        n->curr = he;
        n->head.time = -2;
    }

    he = gw_hist_ent_factory_alloc(self->hist_ent_factory);
    he->flags = flags;
    he->time = self->h->snap_time;

    switch (sig_type->kind) {
        case ghdl_rtik_type_b2:
            if (sig_type->en.wkt == ghw_wkt_bit)
                he->v.h_val = sig->val->b2 == 0 ? GW_BIT_0 : GW_BIT_1;
            else {
                if (sig->val->b2 >= sig->type->en.nbr)
                    ghw_error_exit();
                he->v.h_vector = (char *)sig->type->en.lits[sig->val->b2];
                is_vector = 1;
            }
            break;

        case ghdl_rtik_type_e8: {
            unsigned char val_e8 = sig->val->e8;
            if (sig_type->en.wkt == ghw_wkt_std_ulogic) {
                /* Res: 0->0, 1->X, 2->Z, 3->1 */
                static const char map_su2vlg[9] = {/* U */ GW_BIT_U,
                                                   /* X */ GW_BIT_X,
                                                   /* 0 */ GW_BIT_0,
                                                   /* 1 */ GW_BIT_1,
                                                   /* Z */ GW_BIT_Z,
                                                   /* W */ GW_BIT_W,
                                                   /* L */ GW_BIT_L,
                                                   /* H */ GW_BIT_H,
                                                   /* - */ GW_BIT_DASH};
                if (val_e8 >= sizeof(map_su2vlg) / sizeof(map_su2vlg[0]))
                    ghw_error_exit();
                he->v.h_val = map_su2vlg[val_e8];
            } else {
                if (val_e8 >= sig_type->en.nbr)
                    ghw_error_exit();
                he->v.h_vector = (char *)sig_type->en.lits[val_e8];
                is_vector = 1;
            }
            break;
        }

        case ghdl_rtik_type_f64: {
            he->v.h_double = sig->val->f64;
            is_double = 1;
        } break;

        case ghdl_rtik_type_i32:
        case ghdl_rtik_type_p32: {
            he->v.h_vector = g_malloc(32);
            for (gint i = 0; i < 32; i++) {
                he->v.h_vector[31 - i] = ((sig->val->i32 >> i) & 1) ? GW_BIT_1 : GW_BIT_0;
            }

            is_vector = 1;
            break;
        }

        case ghdl_rtik_type_i64:
        case ghdl_rtik_type_p64: {
            he->v.h_vector = g_malloc(64);
            for (gint i = 0; i < 64; i++) {
                he->v.h_vector[63 - i] = ((sig->val->i64 >> i) & 1) ? GW_BIT_1 : GW_BIT_0;
            }

            is_vector = 1;
            break;
        }

        default:
            abort();
    }

    /* deglitch */
    if (n->curr->time == he->time) {
        int gl_add = 0;

        if (n->curr->time) /* filter out time zero glitches */
        {
            gl_add = 1;
        }

        self->num_glitches += gl_add;

        if (!(n->curr->flags & GW_HIST_ENT_FLAG_GLITCH)) {
            if (gl_add) {
                n->curr->flags |= GW_HIST_ENT_FLAG_GLITCH; /* set the glitch flag */
                self->num_glitch_regions++;
            }
        }

        if (is_double) {
            n->curr->v.h_double = he->v.h_double;
        } else if (is_vector) {
            if (n->curr->v.h_vector && sig_type->kind != ghdl_rtik_type_b2 &&
                sig_type->kind != ghdl_rtik_type_e8)
                g_free(n->curr->v.h_vector);
            n->curr->v.h_vector = he->v.h_vector;
            /* can't free up this "he" because of block allocation so assume it's dead */
        } else {
            n->curr->v.h_val = he->v.h_val;
        }
        return;
    } else /* look for duplicate dumps of same value at adjacent times */
    {
        if (!is_vector & !is_double) {
            if (n->curr->v.h_val == he->v.h_val) {
                return;
                /* can't free up this "he" because of block allocation so assume it's dead */
            }
        }
    }

    n->curr->next = he;
    n->curr = he;
}

static void add_tail(GwGhwLoader *self)
{
    unsigned int i;
    GwTime j;

    for (j = 1; j >= 0; j--) /* add two endcaps */
        for (i = 0; i < self->h->nbr_sigs; i++) {
            struct ghw_sig *sig = &self->h->sigs[i];
            GwNode *n = self->nxp[i];
            GwHistEnt *he;

            if (sig->type == NULL || n == NULL || !n->curr)
                continue;

            /* Copy the last one.  */
            he = gw_hist_ent_factory_alloc(self->hist_ent_factory);
            *he = *n->curr;
            he->time = GW_TIME_MAX - j;
            he->next = NULL;

            /* Append.  */
            n->curr->next = he;
            n->curr = he;
        }
}

static void read_traces(GwGhwLoader *self)
{
    int *list;
    unsigned int i;
    enum ghw_res res;

    list = g_malloc((gw_facs_get_length(self->facs) + 1) * sizeof(int));

    struct ghw_handler *h = self->h;

    while (1) {
        res = ghw_read_sm_hdr(h, list);
        switch (res) {
            case ghw_res_error:
            case ghw_res_eof:
                g_free(list);
                return;
            case ghw_res_ok:
            case ghw_res_other:
                break;
            case ghw_res_snapshot:
                if (h->snap_time > self->max_time) {
                    self->max_time = h->snap_time;
                }
                /* printf ("Time is "GHWPRI64"\n", h->snap_time); */

                for (i = 0; i < h->nbr_sigs; i++)
                    add_history(self, self->nxp[i], i);
                break;
            case ghw_res_cycle:
                while (1) {
                    int sig;

                    /* printf ("Time is "GHWPRI64"\n", h->snap_time); */
                    if (h->snap_time < GW_TIME_CONSTANT(9223372036854775807)) {
                        if (h->snap_time > self->max_time) {
                            self->max_time = h->snap_time;
                        }

                        for (i = 0; (sig = list[i]) != 0; i++) {
                            size_t nxp_idx = (size_t)sig;
                            if (nxp_idx > self->h->nbr_sigs)
                                ghw_error_exit();
                            add_history(self, self->nxp[nxp_idx], sig);
                        }
                    }
                    res = ghw_read_cycle_next(h);
                    if (res != 1)
                        break;
                    res = ghw_read_cycle_cont(h, list);
                    if (res < 0)
                        break;
                }
                if (res < 0)
                    break;
                res = ghw_read_cycle_end(h);
                if (res < 0)
                    break;
                break;
            default:
                break;
        }
    }
}

/*******************************************************************************/

GwDumpFile *gw_ghw_loader_load(GwLoader *loader, const gchar *fname, GError **error)
{
    GwGhwLoader *self = GW_GHW_LOADER(loader);

    struct ghw_handler handle = {0};
    unsigned int ui;
    int rc;

    // if (!GLOBALS->hier_was_explicitly_set) /* set default hierarchy split char */
    // {
    //     GLOBALS->hier_delimeter = '.';
    // }

    handle.flag_verbose = 0;
    if ((rc = ghw_open(&handle, fname)) < 0) {
        g_set_error(error,
                    GW_DUMP_FILE_ERROR,
                    GW_DUMP_FILE_ERROR_UNKNOWN,
                    "Failed to open GHW file (error code %d)",
                    rc);
        return NULL;
    }

    if (ghw_read_base(&handle) < 0) {
        fprintf(stderr, "Error in ghw file '%s'.\n", fname);
        return NULL; /* look at return code in caller for success status... */
    }

    if (handle.hie == NULL) {
        fprintf(stderr, "Error in ghw file '%s': No HIE.\n", fname);
        return NULL; /* look at return code in caller for success status... */
    }

    self->h = &handle;
    self->asbuf = g_malloc(4097);

    self->nxp = g_new0(GwNode *, handle.nbr_sigs);
    for (ui = 0; ui < handle.nbr_sigs; ui++) {
        self->nxp[ui] = g_new0(GwNode, 1);
    }

    self->treeroot = build_hierarchy(self, handle.hie);
    /* GHW does not contains a 'top' name.
       FIXME: should use basename of the file.  */

    create_facs(self);
    read_traces(self);
    add_tail(self);

    set_fac_name(self);

    g_clear_pointer(&self->nxp, g_free);

    /* fix up names on aliased nodes via cloning... */
    for (guint i = 0; i < gw_facs_get_length(self->facs); i++) {
        GwSymbol *fac = gw_facs_get(self->facs, i);

        if (strcmp(fac->name, fac->n->nname) != 0) {
            GwNode *n = g_new(GwNode, 1);
            memcpy(n, fac->n, sizeof(GwNode));
            fac->n = n;
            n->nname = fac->name;
        }
    }

    /* treeroot->name = "top"; */
    {
        const char *base_hier = "top";

        GwTreeNode *t = g_malloc0(sizeof(GwTreeNode) + strlen(base_hier) + 1);
        memcpy(t, self->treeroot, sizeof(GwTreeNode));
        strcpy(t->name,
               base_hier); /* scan-build false warning here, thinks name[1] is total length */
#ifndef WAVE_TALLOC_POOL_SIZE
        g_free(self->treeroot); /* if using tree alloc pool, can't deallocate this */
#endif
        self->treeroot = t;
    }

    ghw_close(&handle);

    rechain_facs(self); /* vectorize bitblasted nets */
    ghw_sortfacs(self); /* sort nets as ghw is unsorted ... also fix hier tree (it should really be
                       built *after* facs are sorted!) */

#if 0
 treedebug(GLOBALS->treeroot,"");
 facs_debug();
#endif

    fprintf(stderr,
            "[%" GW_TIME_FORMAT "] start time.\n[%" GW_TIME_FORMAT "] end time.\n",
            GW_TIME_CONSTANT(0),
            self->max_time);
    if (self->num_glitches)
        fprintf(stderr,
                "Warning: encountered %d glitch%s across %d glitch region%s.\n",
                self->num_glitches,
                (self->num_glitches != 1) ? "es" : "",
                self->num_glitch_regions,
                (self->num_glitch_regions != 1) ? "s" : "");

    GwTree *tree = gw_tree_new(g_steal_pointer(&self->treeroot));
    GwTimeRange *time_range = gw_time_range_new(0, self->max_time);

    // clang-format off
    GwGhwFile *dump_file = g_object_new(GW_TYPE_GHW_FILE,
                                        "tree", tree,
                                        "facs", g_steal_pointer(&self->facs),
                                        "time-dimension", GW_TIME_DIMENSION_FEMTO,
                                        "time-range", time_range,
                                        NULL);
    // clang-format on

    dump_file->hist_ent_factory = g_steal_pointer(&self->hist_ent_factory);

    g_object_unref(tree);
    g_object_unref(time_range);

    return GW_DUMP_FILE(dump_file);
}

static void gw_ghw_loader_class_init(GwGhwLoaderClass *klass)
{
    GwLoaderClass *loader_class = GW_LOADER_CLASS(klass);

    loader_class->load = gw_ghw_loader_load;
}

static void gw_ghw_loader_init(GwGhwLoader *self)
{
    self->hist_ent_factory = gw_hist_ent_factory_new();
}

GwLoader *gw_ghw_loader_new(void)
{
    return g_object_new(GW_TYPE_GHW_LOADER, NULL);
}
