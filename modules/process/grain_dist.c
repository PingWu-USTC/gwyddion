/*
 *  @(#) $Id$
 *  Copyright (C) 2003-2006 David Necas (Yeti), Petr Klapetek.
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

#include "config.h"
#include <math.h>
#include <gtk/gtk.h>
#include <libgwyddion/gwymacros.h>
#include <libgwymodule/gwymodule.h>
#include <libprocess/grains.h>
#include <libgwydgets/gwydgets.h>
#include <app/gwyapp.h>

#define STAT_RUN_MODES (GWY_RUN_IMMEDIATE | GWY_RUN_INTERACTIVE)

typedef enum {
    MODE_GRAPH,
    MODE_RAW
} GrainDistMode;

typedef struct {
    GrainDistMode mode;
    guint selected;
    gboolean fixres;
    gint resolution;
} GrainDistArgs;

typedef struct {
    GSList *qlist;
} GrainDistControls;

static gboolean module_register                (void);
static void     grain_dist                     (GwyContainer *data,
                                                GwyRunType run);
static void     grain_stat                     (GwyContainer *data,
                                                GwyRunType run);
static void     grain_dist_dialog              (GrainDistArgs *args,
                                                GwyContainer *data,
                                                GwyDataField *dfield,
                                                GwyDataField *mfield);
static void     grain_dist_dialog_update_values(GrainDistControls *controls,
                                                GrainDistArgs *args);
static void     grain_dist_run                 (GrainDistArgs *args,
                                                GwyContainer *data,
                                                GwyDataField *dfield,
                                                GwyDataField *mfield);
static void     grain_dist_load_args           (GwyContainer *container,
                                                GrainDistArgs *args);
static void     grain_dist_save_args           (GwyContainer *container,
                                                GrainDistArgs *args);

static const GrainDistArgs grain_dist_defaults = {
    MODE_GRAPH,
    1 << GWY_GRAIN_VALUE_EQUIV_DISC_RADIUS,
    FALSE,
    120,
};

static GwyModuleInfo module_info = {
    GWY_MODULE_ABI_VERSION,
    &module_register,
    N_("Evaluates distribution of grains (continuous parts of mask)."),
    "Petr Klapetek <petr@klapetek.cz>, Sven Neumann <neumann@jpk.com>, "
        "Yeti <yeti@gwyddion.net>",
    "2.0",
    "David Nečas (Yeti) & Petr Klapetek & Sven Neumann",
    "2003-2006",
};

GWY_MODULE_QUERY(module_info)

static gboolean
module_register(void)
{
    gwy_process_func_register("grain_dist",
                              (GwyProcessFunc)&grain_dist,
                              N_("/_Grains/_Distributions..."),
                              GWY_STOCK_GRAINS_GRAPH,
                              STAT_RUN_MODES,
                              GWY_MENU_FLAG_DATA | GWY_MENU_FLAG_DATA_MASK,
                              N_("Distributions of various grain "
                                 "characteristics"));
    gwy_process_func_register("grain_stat",
                              (GwyProcessFunc)&grain_stat,
                              N_("/_Grains/S_tatistics..."),
                              NULL,
                              STAT_RUN_MODES,
                              GWY_MENU_FLAG_DATA | GWY_MENU_FLAG_DATA_MASK,
                              N_("Simple grain statistics"));

    return TRUE;
}

static void
grain_dist(GwyContainer *data, GwyRunType run)
{
    GrainDistArgs args;
    GwyDataField *dfield;
    GwyDataField *mfield;

    g_return_if_fail(run & STAT_RUN_MODES);
    grain_dist_load_args(gwy_app_settings_get(), &args);
    gwy_app_data_browser_get_current(GWY_APP_DATA_FIELD, &dfield,
                                     GWY_APP_MASK_FIELD, &mfield,
                                     0);
    g_return_if_fail(dfield && mfield);

    if (run == GWY_RUN_IMMEDIATE)
        grain_dist_run(&args, data, dfield, mfield);
    else {
        grain_dist_dialog(&args, data, dfield, mfield);
        grain_dist_save_args(gwy_app_settings_get(), &args);
    }
}

static GSList*
append_checkbox_list(GtkTable *table,
                     gint *row,
                     const gchar *title,
                     GSList *list,
                     guint nchoices,
                     const GwyEnum *choices,
                     guint state)
{
    GtkWidget *label, *check;
    gchar *s;
    guint i, bit;

    if (*row > 0)
        gtk_table_set_row_spacing(table, *row - 1, 8);

    label = gtk_label_new(NULL);
    s = g_strconcat("<b>", title, "</b>", NULL);
    gtk_label_set_markup(GTK_LABEL(label), s);
    g_free(s);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_table_attach(table, label,
                     0, 3, *row, *row + 1, GTK_EXPAND | GTK_FILL, 0, 0, 0);
    (*row)++;

    for (i = 0; i < nchoices; i++) {
        bit = 1 << choices[i].value;
        check = gtk_check_button_new_with_mnemonic(_(choices[i].name));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), state & bit);
        g_object_set_data(G_OBJECT(check), "bit", GUINT_TO_POINTER(bit));
        gtk_table_attach(table, check,
                         0, 4, *row, *row + 1, GTK_EXPAND | GTK_FILL, 0, 0, 0);
        list = g_slist_prepend(list, check);
        (*row)++;
    }

    return list;
}

static void
grain_dist_dialog(GrainDistArgs *args,
                  GwyContainer *data,
                  GwyDataField *dfield,
                  GwyDataField *mfield)
{
    static const GwyEnum quantities_area[] = {
        { N_("_Projected area"),         GWY_GRAIN_VALUE_PROJECTED_AREA,    },
        { N_("Equivalent _square side"), GWY_GRAIN_VALUE_EQUIV_SQUARE_SIDE, },
        { N_("Equivalent disc _radius"), GWY_GRAIN_VALUE_EQUIV_DISC_RADIUS, },
        { N_("S_urface area"),           GWY_GRAIN_VALUE_SURFACE_AREA,      },
    };
    static const GwyEnum quantities_value[] = {
        { N_("Ma_ximum"), GWY_GRAIN_VALUE_MAXIMUM, },
        { N_("M_inimum"), GWY_GRAIN_VALUE_MINIMUM, },
        { N_("_Mean"),    GWY_GRAIN_VALUE_MEAN,    },
        { N_("Me_dian"),  GWY_GRAIN_VALUE_MEDIAN,  },
    };
    static const GwyEnum quantities_boundary[] = {
        { N_("Projected _boundary length"),
            GWY_GRAIN_VALUE_FLAT_BOUNDARY_LENGTH, },
    };

    GrainDistControls controls;
    GtkWidget *dialog, *table;
    gint row, response;

    dialog = gtk_dialog_new_with_buttons(_("Grain Distributions"), NULL, 0,
                                         GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                         GTK_STOCK_OK, GTK_RESPONSE_OK,
                                         NULL);
    gtk_dialog_set_has_separator(GTK_DIALOG(dialog), FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

    table = gtk_table_new(15, 4, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table), 2);
    gtk_table_set_col_spacings(GTK_TABLE(table), 6);
    gtk_container_set_border_width(GTK_CONTAINER(table), 4);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), table, TRUE, TRUE, 0);
    row = 0;

    controls.qlist = append_checkbox_list(GTK_TABLE(table), &row, _("Value"),
                                          NULL,
                                          G_N_ELEMENTS(quantities_value),
                                          quantities_value,
                                          args->selected);
    controls.qlist = append_checkbox_list(GTK_TABLE(table), &row, _("Area"),
                                          controls.qlist,
                                          G_N_ELEMENTS(quantities_area),
                                          quantities_area,
                                          args->selected);
    controls.qlist = append_checkbox_list(GTK_TABLE(table), &row, _("Boundary"),
                                          controls.qlist,
                                          G_N_ELEMENTS(quantities_boundary),
                                          quantities_boundary,
                                          args->selected);

    gtk_widget_show_all(dialog);
    do {
        response = gtk_dialog_run(GTK_DIALOG(dialog));
        switch (response) {
            case GTK_RESPONSE_CANCEL:
            case GTK_RESPONSE_DELETE_EVENT:
            grain_dist_dialog_update_values(&controls, args);
            gtk_widget_destroy(dialog);
            case GTK_RESPONSE_NONE:
            return;
            break;

            case GTK_RESPONSE_OK:
            break;

            default:
            g_assert_not_reached();
            break;
        }
    } while (response != GTK_RESPONSE_OK);

    grain_dist_dialog_update_values(&controls, args);
    gtk_widget_destroy(dialog);

    grain_dist_run(args, data, dfield, mfield);
}

static void
grain_dist_dialog_update_values(GrainDistControls *controls,
                                GrainDistArgs *args)
{
    GSList *l;
    guint bit;

    args->selected = 0;
    for (l = controls->qlist; l; l = g_slist_next(l)) {
        bit = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(l->data), "bit"));
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(l->data)))
            args->selected |= bit;
    }
}

static void
grain_dist_run(GrainDistArgs *args,
               GwyContainer *data,
               GwyDataField *dfield,
               GwyDataField *mfield)
{
    GwyGraphCurveModel *cmodel;
    GwyGraphModel *gmodel;
    GwyDataLine *dataline;

    /* TODO */

    dataline = gwy_data_field_grains_get_distribution
                                        (dfield, mfield, NULL, 0, NULL,
                                         GWY_GRAIN_VALUE_EQUIV_SQUARE_SIDE, 0);

    gmodel = gwy_graph_model_new();
    cmodel = gwy_graph_curve_model_new();
    gwy_graph_model_add_curve(gmodel, cmodel);
    g_object_unref(cmodel);

    gwy_graph_model_set_title(gmodel, _("Grain Size Histogram"));
    gwy_graph_model_set_units_from_data_line(gmodel, dataline);
    gwy_graph_curve_model_set_description(cmodel, "Grain sizes");
    gwy_graph_curve_model_set_data_from_dataline(cmodel, dataline, 0, 0);
    g_object_unref(dataline);

    gwy_app_data_browser_add_graph_model(gmodel, data, TRUE);
    g_object_unref(gmodel);
}

static void
grain_stat(G_GNUC_UNUSED GwyContainer *data, GwyRunType run)
{
    GtkWidget *dialog, *table, *label;
    GwyDataField *dfield, *mfield;
    GwySIUnit *siunit, *siunit2;
    GwySIValueFormat *vf;
    gint i, xres, yres, ngrains;
    gdouble total_area, area, v, size;
    gdouble *sizes;
    gint *grains;
    GString *str;
    gint row;

    g_return_if_fail(run & STAT_RUN_MODES);
    gwy_app_data_browser_get_current(GWY_APP_DATA_FIELD, &dfield,
                                     GWY_APP_MASK_FIELD, &mfield,
                                     0);
    g_return_if_fail(dfield);
    g_return_if_fail(mfield);

    xres = gwy_data_field_get_xres(mfield);
    yres = gwy_data_field_get_yres(mfield);
    total_area = gwy_data_field_get_xreal(dfield)
                 *gwy_data_field_get_yreal(dfield);

    grains = g_new0(gint, xres*yres);
    ngrains = gwy_data_field_number_grains(mfield, grains);
    sizes = gwy_data_field_grains_get_values(dfield, NULL, ngrains, grains,
                                             GWY_GRAIN_VALUE_PROJECTED_AREA);
    g_free(grains);
    size = area = 0.0;
    for (i = 1; i <= ngrains; i++) {
        area += sizes[i];
        size += sqrt(sizes[i]);
    }
    g_free(sizes);

    dialog = gtk_dialog_new_with_buttons(_("Grain Statistics"), NULL, 0,
                                         GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE,
                                         NULL);
    gtk_dialog_set_has_separator(GTK_DIALOG(dialog), FALSE);

    table = gtk_table_new(4, 2, FALSE);
    gtk_container_add(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), table);
    gtk_container_set_border_width(GTK_CONTAINER(table), 4);
    row = 0;
    str = g_string_new("");

    label = gtk_label_new(_("Number of grains:"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, row, row+1,
                     GTK_FILL, 0, 2, 2);
    g_string_printf(str, "%d", ngrains);
    label = gtk_label_new(str->str);
    gtk_misc_set_alignment(GTK_MISC(label), 1.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 1, 2, row, row+1,
                     GTK_FILL, 0, 2, 2);
    row++;

    siunit = gwy_data_field_get_si_unit_xy(dfield);
    siunit2 = gwy_si_unit_power(siunit, 2, NULL);

    label = gtk_label_new(_("Total projected area (abs.):"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, row, row+1,
                     GTK_FILL, 0, 2, 2);
    v = area;
    vf = gwy_si_unit_get_format(siunit2, GWY_SI_UNIT_FORMAT_VFMARKUP, v, NULL);
    g_string_printf(str, "%.*f %s",
                    vf->precision, v/vf->magnitude, vf->units);
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), str->str);
    gtk_misc_set_alignment(GTK_MISC(label), 1.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 1, 2, row, row+1,
                     GTK_FILL, 0, 2, 2);
    row++;

    label = gtk_label_new(_("Total projected area (rel.):"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.0);
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, row, row+1,
                     GTK_FILL, 0, 2, 2);
    g_string_printf(str, "%.2f %%", 100.0*area/total_area);
    label = gtk_label_new(str->str);
    gtk_misc_set_alignment(GTK_MISC(label), 1.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 1, 2, row, row+1,
                     GTK_FILL, 0, 2, 2);
    row++;

    label = gtk_label_new(_("Mean grain area:"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, row, row+1,
                     GTK_FILL, 0, 2, 2);
    v = area/ngrains;
    gwy_si_unit_get_format(siunit2, GWY_SI_UNIT_FORMAT_VFMARKUP, v, vf);
    g_string_printf(str, "%.*f %s",
                    vf->precision, v/vf->magnitude, vf->units);
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), str->str);
    gtk_misc_set_alignment(GTK_MISC(label), 1.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 1, 2, row, row+1,
                     GTK_FILL, 0, 2, 2);
    row++;

    label = gtk_label_new(_("Mean grain size:"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, row, row+1,
                     GTK_FILL, 0, 2, 2);
    v = size/ngrains;
    gwy_si_unit_get_format(siunit, GWY_SI_UNIT_FORMAT_VFMARKUP, v, vf);
    g_string_printf(str, "%.*f %s",
                    vf->precision, v/vf->magnitude, vf->units);
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), str->str);
    gtk_misc_set_alignment(GTK_MISC(label), 1.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 1, 2, row, row+1,
                     GTK_FILL, 0, 2, 2);
    row++;

    gwy_si_unit_value_format_free(vf);
    g_string_free(str, TRUE);
    g_object_unref(siunit2);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static const gchar fixres_key[]     = "/module/grain_dist/fixres";
static const gchar mode_key[]       = "/module/grain_dist/mode";
static const gchar resolution_key[] = "/module/grain_dist/resolution";
static const gchar selected_key[]   = "/module/grain_dist/selected";

static void
grain_dist_sanitize_args(GrainDistArgs *args)
{
    args->fixres = !!args->fixres;
    args->mode = MIN(args->mode, MODE_RAW);
    /* TODO */
}

static void
grain_dist_load_args(GwyContainer *container,
                     GrainDistArgs *args)
{
    *args = grain_dist_defaults;

    gwy_container_gis_boolean_by_name(container, fixres_key, &args->fixres);
    gwy_container_gis_int32_by_name(container, selected_key, &args->selected);
    gwy_container_gis_int32_by_name(container, resolution_key,
                                    &args->resolution);
    gwy_container_gis_enum_by_name(container, mode_key, &args->mode);
    grain_dist_sanitize_args(args);
}

static void
grain_dist_save_args(GwyContainer *container,
                     GrainDistArgs *args)
{
    gwy_container_set_boolean_by_name(container, fixres_key, args->fixres);
    gwy_container_set_int32_by_name(container, selected_key, args->selected);
    gwy_container_set_int32_by_name(container, resolution_key,
                                    args->resolution);
    gwy_container_set_enum_by_name(container, mode_key, args->mode);
}
/* vim: set cin et ts=4 sw=4 cino=>1s,e0,n0,f0,{0,}0,^0,\:1s,=0,g1s,h0,t0,+1s,c3,(0,u0 : */
