/*
 *  @(#) $Id$
 *  Copyright (C) 2003 David Necas (Yeti), Petr Klapetek.
 *  E-mail: yeti@physics.muni.cz, klapetek@physics.muni.cz.
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

#include <math.h>
#include <glib.h>
#include <libgwyddion/gwymacros.h>
#include <libgwyddion/gwymath.h>
#include <libgwyddion/gwycontainer.h>
#include <libprocess/datafield.h>
#include <libgwydgets/gwydgets.h>
#include <app/file.h>
#include <app/app.h>
#include "tools.h"

typedef struct {
    gboolean is_visible; 
    GPtrArray *positions;
    GtkWidget *graph;
    GtkWidget *interpolation;
    GtkWidget *separation;
    gint interp;
    gboolean separate;
} ProfileControls;

static GtkWidget* profile_dialog_create            (GwyDataView *data_view);
static void       profile_do                       (void);
static void       profile_selection_updated_cb    (void);
static void       profile_dialog_response_cb       (gpointer unused,
                                                 gint response);
static void       profile_dialog_abandon           (void);
static void       profile_dialog_set_visible       (gboolean visible);

static GtkWidget *dialog = NULL;
static ProfileControls controls;
static gulong updated_id = 0;
static gulong response_id = 0;
static GwyDataViewLayer *select_layer = NULL;
static GwyDataField *datafield = NULL;
static GPtrArray *dtl = NULL;
static GPtrArray *str = NULL;


void
gwy_tool_profile_use(GwyDataWindow *data_window)
{
    GwyDataViewLayer *layer;
    GwyDataView *data_view;
    gint i;

    gwy_debug("%s: %p", __FUNCTION__, data_window);

    if (!data_window) {
        profile_dialog_abandon();
        return;
    }
    g_return_if_fail(GWY_IS_DATA_WINDOW(data_window));
    data_view = (GwyDataView*)gwy_data_window_get_data_view(data_window);
    layer = gwy_data_view_get_top_layer(data_view);
    if (layer && layer == select_layer)
        return;
    if (select_layer && updated_id)
        g_signal_handler_disconnect(select_layer, updated_id);

    if (layer && GWY_IS_LAYER_SELECT(layer))
        select_layer = layer;
    else {
        select_layer = (GwyDataViewLayer*)gwy_layer_lines_new();
        gwy_data_view_set_top_layer(data_view, select_layer);
    }
    if (!dialog)
        dialog = profile_dialog_create(data_view);

    
    if (!dtl)
    {
        dtl = g_ptr_array_new(); 
        for (i=0; i<5; i++) g_ptr_array_add(dtl, (gpointer)gwy_data_line_new(100, 100, 0));
    }
    if (!str)
    {
        str = g_ptr_array_new();
        g_ptr_array_add(str, (gpointer)g_string_new("Profile 1"));
        g_ptr_array_add(str, (gpointer)g_string_new("Profile 2"));
        g_ptr_array_add(str, (gpointer)g_string_new("Profile 3"));
        g_ptr_array_add(str, (gpointer)g_string_new("Profile 4"));
        g_ptr_array_add(str, (gpointer)g_string_new("Profile 5"));

    }

    updated_id = g_signal_connect(select_layer, "updated",
                                   G_CALLBACK(profile_selection_updated_cb),
                                   NULL);
    profile_selection_updated_cb();
}

static void
profile_do(void)
{
    GtkWidget *data_window;
    GwyContainer *data;
    GwyDataField *dfield;
    gdouble mylines[12];

    if (!gwy_layer_lines_get_lines(select_layer, mylines))
        return;

    data = gwy_data_view_get_data(GWY_DATA_VIEW(select_layer->parent));
    data = GWY_CONTAINER(gwy_serializable_duplicate(G_OBJECT(data)));
    gwy_app_clean_up_data(data);
    dfield = GWY_DATA_FIELD(gwy_container_get_object_by_name(data, "/0/data"));

    gwy_data_view_update(GWY_DATA_VIEW(select_layer->parent));
    gwy_debug("%s: %d %d", __FUNCTION__,
              gwy_data_field_get_xres(dfield), gwy_data_field_get_yres(dfield));
}

static void
profile_dialog_abandon(void)
{
    if (select_layer && updated_id)
        g_signal_handler_disconnect(select_layer, updated_id);
    updated_id = 0;
    select_layer = NULL;
    if (dialog) {
        g_signal_handler_disconnect(dialog, response_id);
        gtk_widget_destroy(dialog);
        dialog = NULL;
        response_id = 0;
        controls.is_visible = FALSE;
    }
}

static GtkWidget*
profile_dialog_create(GwyDataView *data_view)
{
    GwyContainer *data;
    GwyDataField *dfield;
    GtkWidget *dialog, *table, *label, *vbox;
    gdouble xreal, yreal, max, unit;
    gint i;

    gwy_debug("%s", __FUNCTION__);
    data = gwy_data_view_get_data(data_view);
    datafield = GWY_DATA_FIELD(gwy_container_get_object_by_name(data, "/0/data"));
    xreal = gwy_data_field_get_xreal(datafield);
    yreal = gwy_data_field_get_yreal(datafield);

    dialog = gtk_dialog_new_with_buttons(_("Extract profile"),
                                         NULL,
                                         GTK_DIALOG_DESTROY_WITH_PARENT,
                                         GTK_STOCK_APPLY, GTK_RESPONSE_APPLY,
                                         GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE,
                                         NULL);
    g_signal_connect(dialog, "delete_event",
                     G_CALLBACK(gwy_dialog_prevent_delete_cb), NULL);
    response_id = g_signal_connect(dialog, "response",
                                   G_CALLBACK(profile_dialog_response_cb), NULL);
    table = gtk_table_new(2, 2, FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(table), 4);
    gtk_container_add(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), table);

    vbox = gtk_vbox_new(0,0);
    
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), _("<b>Profile positions</b>"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_box_pack_start(vbox, label, 0, 0, 0);
   
    
    controls.positions = g_ptr_array_new();
    g_ptr_array_add(controls.positions, (gpointer)gtk_label_new("x1 = 0, y1 = 0"));
    g_ptr_array_add(controls.positions, (gpointer)gtk_label_new("x2 = 0, y2 = 0"));
    g_ptr_array_add(controls.positions, (gpointer)gtk_label_new("x1 = 0, y1 = 0"));
    g_ptr_array_add(controls.positions, (gpointer)gtk_label_new("x2 = 0, y2 = 0"));
    g_ptr_array_add(controls.positions, (gpointer)gtk_label_new("x1 = 0, y1 = 0"));
    g_ptr_array_add(controls.positions, (gpointer)gtk_label_new("x2 = 0, y2 = 0"));

    gtk_misc_set_alignment(GTK_MISC(controls.positions->pdata[0]), 0.0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(controls.positions->pdata[1]), 0.0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(controls.positions->pdata[2]), 0.0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(controls.positions->pdata[3]), 0.0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(controls.positions->pdata[4]), 0.0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(controls.positions->pdata[5]), 0.0, 0.5);
    
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), _("Profile 1:"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_box_pack_start(vbox, label, 0, 0, 5);
    gtk_box_pack_start(vbox, GTK_WIDGET(controls.positions->pdata[0]), 0, 0, 0);
    gtk_box_pack_start(vbox, GTK_WIDGET(controls.positions->pdata[1]), 0, 0, 0);
    
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), _("Profile 2:"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_box_pack_start(vbox, label, 0, 0, 5);     
    gtk_box_pack_start(vbox, GTK_WIDGET(controls.positions->pdata[2]), 0, 0, 0);
    gtk_box_pack_start(vbox, GTK_WIDGET(controls.positions->pdata[3]), 0, 0, 0);
    
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), _("Profile 3:"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_box_pack_start(vbox, label, 0, 0, 5);     
    gtk_box_pack_start(vbox, GTK_WIDGET(controls.positions->pdata[4]), 0, 0, 0);
    gtk_box_pack_start(vbox, GTK_WIDGET(controls.positions->pdata[5]), 0, 0, 0);
    
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), _("<b>Module parameters</b>"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_box_pack_start(vbox, label, 0, 0, 10);

    controls.separation = gtk_check_button_new_with_label("separate profiles");
    controls.interpolation;
    
  
    gtk_table_attach(GTK_TABLE(table), vbox, 0, 1, 0, 1, GTK_FILL, 0, 2, 2);
    
    controls.graph = gwy_graph_new();
    gtk_table_attach(GTK_TABLE(table), controls.graph, 1, 2, 0, 1, GTK_FILL, 0, 2, 2);
    
    
    /*
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, 0, 1, GTK_FILL, 0, 2, 2);
    label = gtk_label_new(_("X"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 1, 2, 1, 2, GTK_FILL, 0, 2, 2);
    label = gtk_label_new(_("Y"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 1, 2, 2, 3, GTK_FILL, 0, 2, 2);
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), _("<b>Size</b>"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, 3, 4, GTK_FILL, 0, 2, 2);
    label = gtk_label_new(_("Width"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 1, 2, 4, 5, GTK_FILL, 0, 2, 2);
    label = gtk_label_new(_("Height"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 1, 2, 5, 6, GTK_FILL, 0, 2, 2);

    controls.x = gtk_label_new("");
    controls.y = gtk_label_new("");
    controls.w = gtk_label_new("");
    controls.h = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(controls.x), 1.0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(controls.y), 1.0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(controls.w), 1.0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(controls.h), 1.0, 0.5);
    gtk_table_attach_defaults(GTK_TABLE(table), controls.x, 2, 3, 1, 2);
    gtk_table_attach_defaults(GTK_TABLE(table), controls.y, 2, 3, 2, 3);
    gtk_table_attach_defaults(GTK_TABLE(table), controls.w, 2, 3, 4, 5);
    gtk_table_attach_defaults(GTK_TABLE(table), controls.h, 2, 3, 5, 6);*/
    gtk_widget_show_all(table);
    controls.is_visible = FALSE;

    return dialog;
}

static void
update_label(GtkWidget *label, gdouble value)
{
    /*
    gchar buffer[16];

    g_snprintf(buffer, sizeof(buffer), "%.*f %s",
               controls.precision, value/controls.mag, controls.units);
    gtk_label_set_text(GTK_LABEL(label), buffer);
    */
}

static void
profile_selection_updated_cb(void)
{
    gdouble lines[12];
    gboolean is_visible, is_selected;
    gint ilines[12];
    GString *lab1, *lab2, *lab3;
    gint i, xres, j;
    GwyGraphAutoProperties prop;
    GwyGraph *gr;

    gwy_debug("%s", __FUNCTION__);
    /*XXX: seems broken
     * is_visible = GTK_WIDGET_VISIBLE(dialog);*/
    
    is_visible = controls.is_visible;
    is_selected = gwy_layer_lines_get_lines(select_layer, lines);
                                                 
    if (!is_visible && !is_selected)
        return;

/*    for (i=0; i<300; i++) dtl->data[i]=3.0*sin(i/100);*/
    gwy_graph_get_autoproperties(controls.graph, &prop);
    prop.is_point = 0;
    prop.is_line = 1;
    gwy_graph_set_autoproperties(controls.graph, &prop);
   
    if (is_selected) {

        gwy_graph_clear(controls.graph);

        j = 0;
        for (i=0; i<is_selected; i++)
        {
            gwy_data_field_get_data_line(datafield, dtl->pdata[i], 
                                     (gint)(gwy_data_field_rtoj(datafield, lines[j++])),
                                     (gint)(gwy_data_field_rtoj(datafield, lines[j++])),
                                     (gint)(gwy_data_field_rtoj(datafield, lines[j++])),
                                     (gint)(gwy_data_field_rtoj(datafield, lines[j++])),
                                     100,
                                     GWY_INTERPOLATION_BILINEAR
                                      );
            gwy_graph_add_dataline(controls.graph, dtl->pdata[i],
                               0, str->pdata[i], NULL);
        }


        
        gtk_widget_queue_draw(GTK_WIDGET(controls.graph));
        
/*        update_label(controls.x, MIN(x0, x1));
        update_label(controls.y, MIN(y0, y1));
        update_label(controls.w, fabs(x1 - x0));
        update_label(controls.h, fabs(y1 - y0));*/
    }
    
    if (!is_visible)
        profile_dialog_set_visible(TRUE);
    
}

static void
profile_dialog_response_cb(gpointer unused, gint response)
{
    gwy_debug("%s: response %d", __FUNCTION__, response);
    switch (response) {
        case GTK_RESPONSE_CLOSE:
        case GTK_RESPONSE_DELETE_EVENT:
        profile_dialog_set_visible(FALSE);
        break;

        case GTK_RESPONSE_NONE:
        gwy_tool_profile_use(NULL);
        break;

        case GTK_RESPONSE_APPLY:
        profile_do();
        break;

        default:
        g_assert_not_reached();
        break;
    }
}

static void
profile_dialog_set_visible(gboolean visible)
{
    gwy_debug("%s: now %d, setting to %d",
              __FUNCTION__, controls.is_visible, visible);
    if (controls.is_visible == visible)
        return;

    controls.is_visible = visible;
    if (visible)
        gtk_window_present(GTK_WINDOW(dialog));
    else
        gtk_widget_hide(dialog);
}

/* vim: set cin et ts=4 sw=4 cino=>1s,e0,n0,f0,{0,}0,^0,\:1s,=0,g1s,h0,t0,+1s,c3,(0,u0 : */

