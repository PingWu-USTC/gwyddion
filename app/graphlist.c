/*
 *  @(#) $Id$
 *  Copyright (C) 2004 David Necas (Yeti), Petr Klapetek.
 *  E-mail: yeti@gwyddion.net, klapetek@gwyddion.net.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111 USA
 */

#define DEBUG 1
#include <string.h>
#include <stdlib.h>

#include <libgwyddion/gwyddion.h>
#include <libgwydgets/gwydgets.h>
#include "gwyapp.h"
#include "gwyappinternal.h"

enum {
    GRAPHLIST_GMODEL,
    GRAPHLIST_VISIBLE,
    GRAPHLIST_TITLE,
    GRAPHLIST_NCURVES,
    GRAPHLIST_ID,
    GRAPHLIST_LAST
};

static GtkWidget* gwy_graph_list_construct(GwyContainer *data);
static void gwy_graph_list_cell_renderer(GtkTreeViewColumn *column,
                                         GtkCellRenderer *cell,
                                         GtkTreeModel *model,
                                         GtkTreeIter *piter,
                                         gpointer userdata);
static void gwy_graph_list_add_line(gpointer hkey,
                                    GValue *value,
                                    GtkListStore *store);
static gint gwy_graph_list_sort_func(GtkTreeModel *model,
                                     GtkTreeIter *a,
                                     GtkTreeIter *b,
                                     gpointer user_data);

void
gwy_app_graph_list_add(GwyDataWindow *data_window,
                       GwyGraph *graph)
{
    GwyContainer *data;
    GtkListStore *store;
    GtkTreeIter iter;
    GObject *gmodel;
    GtkWidget *graph_view, *list;
    gint32 lastid;
    gchar key[24];

    gwy_debug("");
    g_return_if_fail(GWY_IS_DATA_WINDOW(data_window));
    g_return_if_fail(GWY_IS_GRAPH(graph));

    data = gwy_data_window_get_data(data_window);
    gmodel = gwy_graph_model_new(graph);

    /* compute new id and new id list */
    if (gwy_container_gis_int32_by_name(data, "/0/graph/lastid", &lastid)) {
        if (lastid <= 0)
            g_warning("Broken last graph id");
        lastid = MAX(0, lastid) + 1;
    }
    else
        lastid = 1;

    g_snprintf(key, sizeof(key), "/0/graph/graph/%d", lastid);
    g_object_set_data(gmodel, "gwy-app-graph-list-id", GINT_TO_POINTER(lastid));
    gwy_container_set_int32_by_name(data, "/0/graph/lastid", lastid);
    gwy_container_set_object_by_name(data, key, gmodel);
    g_object_unref(gmodel);

    if (!(graph_view = g_object_get_data(G_OBJECT(data_window),
                                        "gwy-app-graph-list-window")))
        return;

    list = g_object_get_data(G_OBJECT(graph_view), "gwy-app-graph-list-view");
    g_assert(list);
    store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(list)));

    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter,
                       GRAPHLIST_GMODEL, gmodel,
                       -1);
}

GtkWidget*
gwy_app_graph_list(GwyDataWindow *data_window)
{
    GtkWidget *window, *vbox, *buttonbox, *button, *list;

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Graph list for FIXME");
    gtk_window_set_default_size(GTK_WINDOW(window), -1, 180);

    vbox = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    list = gwy_graph_list_construct(gwy_data_window_get_data(data_window));
    gtk_box_pack_start(GTK_BOX(vbox), list, TRUE, TRUE, 0);

    buttonbox = gtk_hbox_new(TRUE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(buttonbox), 2);
    gtk_box_pack_start(GTK_BOX(vbox), buttonbox, FALSE, FALSE, 0);

    button = gtk_button_new_with_mnemonic(_("_Delete"));
    gtk_box_pack_start(GTK_BOX(buttonbox), button, TRUE, TRUE, 0);
    button = gtk_button_new_with_mnemonic(_("Delete _All"));
    gtk_box_pack_start(GTK_BOX(buttonbox), button, TRUE, TRUE, 0);
    button = gtk_button_new_with_mnemonic(_("_Show All"));
    gtk_box_pack_start(GTK_BOX(buttonbox), button, TRUE, TRUE, 0);
    button = gtk_button_new_with_mnemonic(_("_Hide All"));
    gtk_box_pack_start(GTK_BOX(buttonbox), button, TRUE, TRUE, 0);

    g_object_set_data(G_OBJECT(data_window), "gwy-app-graph-list-window",
                      window);
    g_object_set_data(G_OBJECT(window), "gwy-app-graph-list-view", list);

    gtk_widget_show_all(vbox);

    return window;
}

static GtkWidget*
gwy_graph_list_construct(GwyContainer *data)
{
    static const struct {
        const gchar *title;
        const guint id;
    }
    columns[] = {
        { "Vis.", GRAPHLIST_VISIBLE },
        { "Title", GRAPHLIST_TITLE },
        { "Curves", GRAPHLIST_NCURVES },
        { "Id", GRAPHLIST_ID },   /* FIXME: debug only */
    };

    GtkWidget *tree;
    GtkListStore *store;
    GtkTreeSelection *selection;
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;
    gsize i;

    store = gtk_list_store_new(1, G_TYPE_POINTER);

    gwy_container_foreach(data, "/0/graph/graph",
                          (GHFunc)(gwy_graph_list_add_line), store);
    /*
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter)) {
        g_object_unref(store);
        return NULL;
    }
    */

    tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_tree_view_set_rules_hint(GTK_TREE_VIEW(tree), TRUE);
    g_object_unref(store);
    g_object_set_data(G_OBJECT(store), "container", data);

    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(store),
                                    0, gwy_graph_list_sort_func, NULL, NULL);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store), 0,
                                         GTK_SORT_ASCENDING);

    for (i = 0; i < G_N_ELEMENTS(columns); i++) {
        if (i == 0)
            renderer = gtk_cell_renderer_toggle_new();
        else
            renderer = gtk_cell_renderer_text_new();

        column = gtk_tree_view_column_new_with_attributes(columns[i].title,
                                                          renderer,
                                                          NULL);
        gtk_tree_view_column_set_cell_data_func(column, renderer,
                                                gwy_graph_list_cell_renderer,
                                                GUINT_TO_POINTER(columns[i].id),
                                                NULL);  /* destroy notify */
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
    }

    selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);

    return tree;
}

static void
gwy_graph_list_cell_renderer(G_GNUC_UNUSED GtkTreeViewColumn *column,
                             GtkCellRenderer *cell,
                             GtkTreeModel *model,
                             GtkTreeIter *piter,
                             gpointer userdata)
{
    GwyGraphModel *gmodel;
    gulong id;
    gchar s[16];

    id = GPOINTER_TO_UINT(userdata);
    g_assert(id > GRAPHLIST_GMODEL && id < GRAPHLIST_LAST);
    gtk_tree_model_get(model, piter, GRAPHLIST_GMODEL, &gmodel, -1);
    g_return_if_fail(gmodel);
    switch (id) {
        case GRAPHLIST_VISIBLE:
        g_object_set(cell, "active", gmodel->graph != NULL, NULL);
        break;

        case GRAPHLIST_TITLE:
        g_object_set(cell, "text", gmodel->title->str, NULL);
        break;

        case GRAPHLIST_NCURVES:
        g_snprintf(s, sizeof(s), "%d", gwy_graph_model_get_n_curves(gmodel));
        g_object_set(cell, "text", s, NULL);
        break;

        case GRAPHLIST_ID:
        g_snprintf(s, sizeof(s), "%d",
                   GPOINTER_TO_INT(g_object_get_data(G_OBJECT(gmodel),
                                                     "gwy-app-graph-list-id")));
        g_object_set(cell, "text", s, NULL);
        break;

        default:
        g_assert_not_reached();
        break;
    }
}

static void
gwy_graph_list_add_line(gpointer hkey,
                        GValue *value,
                        GtkListStore *store)
{
    GObject *gmodel;
    GtkTreeIter iter;

    g_return_if_fail(G_VALUE_HOLDS_OBJECT(value));
    gmodel = g_value_get_object(value);
    g_return_if_fail(GWY_IS_GRAPH_MODEL(gmodel));

    if (!g_object_get_data(gmodel, "gwy-app-graph-list-id")) {
        GQuark quark;
        const gchar *key;
        gint32 id;

        quark = GPOINTER_TO_INT(hkey);
        key = g_quark_to_string(quark);
        g_return_if_fail(key);
        key = strrchr(key, '/');
        g_return_if_fail(key);
        key++;
        id = atoi(key);
        g_return_if_fail(id);
        g_object_set_data(gmodel, "gwy-app-graph-list-id",
                          GINT_TO_POINTER(id));
    }

    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter,
                       GRAPHLIST_GMODEL, gmodel,
                       -1);
}

static gint
gwy_graph_list_sort_func(GtkTreeModel *model,
                         GtkTreeIter *a,
                         GtkTreeIter *b,
                         G_GNUC_UNUSED gpointer user_data)
{
    GObject *p, *q;
    guint x, y;

    gtk_tree_model_get(model, a, 0, &p, -1);
    gtk_tree_model_get(model, b, 0, &q, -1);

    x = GPOINTER_TO_INT(g_object_get_data(p, "gwy-app-graph-list-id"));
    y = GPOINTER_TO_INT(g_object_get_data(q, "gwy-app-graph-list-id"));

    gwy_debug("x = %d, y = %d", x, y);

    if (y > x)
        return -1;
    else if (x > y)
        return 1;
    else
        return 0;
}

/* vim: set cin et ts=4 sw=4 cino=>1s,e0,n0,f0,{0,}0,^0,\:1s,=0,g1s,h0,t0,+1s,c3,(0,u0 : */
