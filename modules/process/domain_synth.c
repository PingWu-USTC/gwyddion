/*
 *  @(#) $Id$
 *  Copyright (C) 2014 David Necas (Yeti).
 *  E-mail: yeti@gwyddion.net.
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
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include <string.h>
#include <gtk/gtk.h>
#include <libgwyddion/gwymacros.h>
#include <libgwyddion/gwymath.h>
#include <libprocess/stats.h>
#include <libprocess/filters.h>
#include <libgwydgets/gwydataview.h>
#include <libgwydgets/gwylayer-basic.h>
#include <libgwydgets/gwydgetutils.h>
#include <libgwymodule/gwymodule-process.h>
#include <app/gwyapp.h>

#include "dimensions.h"

#define DOMAIN_SYNTH_RUN_MODES (GWY_RUN_IMMEDIATE | GWY_RUN_INTERACTIVE)

enum {
    PREVIEW_SIZE = 320,
};

enum {
    RESPONSE_RESET = 1,
};

enum {
    PAGE_DIMENSIONS = 0,
    PAGE_GENERATOR  = 1,
    PAGE_NPAGES
};

enum {
    QUANTITY_U = 0,
    QUANTITY_V = 1,
    QUANTITY_MASK = (1 << QUANTITY_U) | (1 << QUANTITY_V),
};

typedef struct _ObjSynthControls DomainSynthControls;

typedef struct {
    gint active_page;
    gint seed;
    gboolean randomize;
    gboolean update;   /* Always false */
    gboolean animated;
    guint quantity;
    guint preview_quantity;
    guint niters;
    gdouble height;
    gdouble T;
    gdouble J;
    gdouble mu;
    gdouble nu;
    gdouble dt;
} DomainSynthArgs;

struct _ObjSynthControls {
    DomainSynthArgs *args;
    GwyDimensions *dims;
    GtkWidget *dialog;
    GtkWidget *view;
    GtkWidget *update;
    GtkWidget *update_now;
    GtkWidget *animated;
    GtkObject *seed;
    GtkWidget *randomize;
    GtkTable *table;
    GtkObject *niters;
    GtkObject *T;
    GtkObject *J;
    GtkObject *mu;
    GtkObject *nu;
    GtkObject *dt;
    GtkObject *height;
    GtkWidget *height_units;
    GtkWidget *quantity;
    GtkWidget *preview_quantity;
    GwyContainer *mydata;
    GwyDataField *surface;
    gdouble pxsize;
    gdouble zscale;
    gboolean in_init;
};

static gboolean   module_register        (void);
static void       domain_synth           (GwyContainer *data,
                                          GwyRunType run);
static void       run_noninteractive     (DomainSynthArgs *args,
                                          const GwyDimensionArgs *dimsargs,
                                          GwyContainer *data,
                                          GwyDataField *dfield,
                                          gint oldid,
                                          GQuark quark);
static gboolean   domain_synth_dialog    (DomainSynthArgs *args,
                                          GwyDimensionArgs *dimsargs,
                                          GwyContainer *data,
                                          GwyDataField *dfield,
                                          gint id);
static GtkWidget* quantity_selector_new(DomainSynthControls *controls);
static void       update_controls        (DomainSynthControls *controls,
                                          DomainSynthArgs *args);
static void       page_switched          (DomainSynthControls *controls,
                                          GtkNotebookPage *page,
                                          gint pagenum);
static void       update_values          (DomainSynthControls *controls);
static void       quantity_selected      (GtkComboBox *combo,
                                          DomainSynthControls *controls);
static void       domain_synth_invalidate(DomainSynthControls *controls);
static void       preview                (DomainSynthControls *controls);
static gboolean   domain_synth_do        (const DomainSynthArgs *args,
                                          GwyDataField *ufield,
                                          GwyDataField *vfield,
                                          gdouble preview_time);
static void       domain_synth_load_args (GwyContainer *container,
                                          DomainSynthArgs *args,
                                          GwyDimensionArgs *dimsargs);
static void       domain_synth_save_args (GwyContainer *container,
                                          const DomainSynthArgs *args,
                                          const GwyDimensionArgs *dimsargs);

#define GWY_SYNTH_CONTROLS DomainSynthControls
#define GWY_SYNTH_INVALIDATE(controls) domain_synth_invalidate(controls)

#include "synth.h"

static const DomainSynthArgs domain_synth_defaults = {
    PAGE_DIMENSIONS,
    42, TRUE, FALSE, TRUE,
    QUANTITY_U, 1 << QUANTITY_U,
    200, 1.0,
    0.8, 1.5, 0.2, 0.0, 0.005,
};

static const GwyDimensionArgs dims_defaults = GWY_DIMENSION_ARGS_INIT;

static const GwyEnum quantity_types[] = {
    { N_("Discrete state"),       QUANTITY_U, },
    { N_("Continuous inhibitor"), QUANTITY_V, },
};

static GwyModuleInfo module_info = {
    GWY_MODULE_ABI_VERSION,
    &module_register,
    N_("Generates domain images using a hybrid Ising model."),
    "Yeti <yeti@gwyddion.net>",
    "1.0",
    "David Nečas (Yeti)",
    "2014",
};

GWY_MODULE_QUERY(module_info)

static gboolean
module_register(void)
{
    gwy_process_func_register("domain_synth",
                              (GwyProcessFunc)&domain_synth,
                              N_("/S_ynthetic/_Domains..."),
                              NULL,
                              DOMAIN_SYNTH_RUN_MODES,
                              0,
                              N_("Generate image with domains"));

    return TRUE;
}

static void
domain_synth(GwyContainer *data, GwyRunType run)
{
    DomainSynthArgs args;
    GwyDimensionArgs dimsargs;
    GwyDataField *dfield;
    GQuark quark;
    gint id;

    g_return_if_fail(run & DOMAIN_SYNTH_RUN_MODES);
    domain_synth_load_args(gwy_app_settings_get(), &args, &dimsargs);
    gwy_app_data_browser_get_current(GWY_APP_DATA_FIELD, &dfield,
                                     GWY_APP_DATA_FIELD_ID, &id,
                                     GWY_APP_DATA_FIELD_KEY, &quark,
                                     0);

    if (run == GWY_RUN_IMMEDIATE
        || domain_synth_dialog(&args, &dimsargs, data, dfield, id)) {
        run_noninteractive(&args, &dimsargs, data, dfield, id, quark);
    }

    gwy_dimensions_free_args(&dimsargs);
}

static void
run_noninteractive(DomainSynthArgs *args,
                   const GwyDimensionArgs *dimsargs,
                   GwyContainer *data,
                   GwyDataField *dfield,
                   gint oldid,
                   GQuark quark)
{
    GwyDataField *newfield, *vfield;
    GwySIUnit *siunit;
    gboolean replace = dimsargs->replace && dfield;
    gboolean add = dimsargs->add && dfield;
    gint newid;
    gboolean ok;

    if (args->randomize)
        args->seed = g_random_int() & 0x7fffffff;

    if (add || replace) {
        if (add)
            newfield = gwy_data_field_duplicate(dfield);
        else
            newfield = gwy_data_field_new_alike(dfield, TRUE);
    }
    else {
        gdouble mag = pow10(dimsargs->xypow10) * dimsargs->measure;
        newfield = gwy_data_field_new(dimsargs->xres, dimsargs->yres,
                                      mag*dimsargs->xres, mag*dimsargs->yres,
                                      TRUE);

        siunit = gwy_data_field_get_si_unit_xy(newfield);
        gwy_si_unit_set_from_string(siunit, dimsargs->xyunits);

        siunit = gwy_data_field_get_si_unit_z(newfield);
        gwy_si_unit_set_from_string(siunit, dimsargs->zunits);
    }

    gwy_app_wait_start(gwy_app_find_window_for_channel(data, oldid),
                       _("Starting..."));
    vfield = gwy_data_field_new_alike(newfield, FALSE);
    //scale_to_unit_cubes(newfield, &rx, &ry);
    ok = domain_synth_do(args, newfield, vfield, HUGE_VAL);
    g_object_unref(vfield);
    //scale_from_unit_cubes(newfield, rx, ry);
    gwy_app_wait_finish();

    if (!ok) {
        g_object_ref(newfield);
        return;
    }

    if (replace) {
        gwy_app_undo_qcheckpointv(data, 1, &quark);
        gwy_container_set_object(data, gwy_app_get_data_key_for_id(oldid),
                                 newfield);
        gwy_app_channel_log_add(data, oldid, oldid, "proc::domain_synth", NULL);
        g_object_unref(newfield);
        return;
    }

    if (data) {
        newid = gwy_app_data_browser_add_data_field(newfield, data, TRUE);
        if (oldid != -1)
            gwy_app_sync_data_items(data, data, oldid, newid, FALSE,
                                    GWY_DATA_ITEM_GRADIENT,
                                    0);
    }
    else {
        newid = 0;
        data = gwy_container_new();
        gwy_container_set_object(data, gwy_app_get_data_key_for_id(newid),
                                 newfield);
        gwy_app_data_browser_add(data);
        gwy_app_data_browser_reset_visibility(data,
                                              GWY_VISIBILITY_RESET_SHOW_ALL);
        g_object_unref(data);
    }

    gwy_app_set_data_field_title(data, newid, _("Generated"));
    gwy_app_channel_log_add(data, add ? oldid : -1, newid,
                            "proc::domain_synth", NULL);
    g_object_unref(newfield);
}

static gboolean
domain_synth_dialog(DomainSynthArgs *args,
                    GwyDimensionArgs *dimsargs,
                    GwyContainer *data,
                    GwyDataField *dfield_template,
                    gint id)
{
    GtkWidget *dialog, *table, *vbox, *hbox, *notebook, *hbox2, *check;
    DomainSynthControls controls;
    GwyDataField *dfield;
    GwyPixmapLayer *layer;
    gboolean finished;
    gint response;
    gint row;

    gwy_clear(&controls, 1);
    controls.in_init = TRUE;
    controls.args = args;
    controls.pxsize = 1.0;
    dialog = gtk_dialog_new_with_buttons(_("Domains"),
                                         NULL, 0,
                                         _("_Reset"), RESPONSE_RESET,
                                         GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                         GTK_STOCK_OK, GTK_RESPONSE_OK,
                                         NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    controls.dialog = dialog;

    hbox = gtk_hbox_new(FALSE, 2);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), hbox,
                       FALSE, FALSE, 4);

    vbox = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, FALSE, FALSE, 4);

    controls.mydata = gwy_container_new();
    dfield = gwy_data_field_new(PREVIEW_SIZE, PREVIEW_SIZE,
                                dimsargs->measure*PREVIEW_SIZE,
                                dimsargs->measure*PREVIEW_SIZE,
                                TRUE);
    gwy_container_set_object_by_name(controls.mydata, "/0/data", dfield);
    if (dfield_template) {
        gwy_app_sync_data_items(data, controls.mydata, id, 0, FALSE,
                                GWY_DATA_ITEM_PALETTE,
                                0);
        controls.surface = gwy_synth_surface_for_preview(dfield_template,
                                                         PREVIEW_SIZE);
        controls.zscale = 3.0*gwy_data_field_get_rms(dfield_template);
    }
    controls.view = gwy_data_view_new(controls.mydata);
    layer = gwy_layer_basic_new();
    g_object_set(layer,
                 "data-key", "/0/data",
                 "gradient-key", "/0/base/palette",
                 NULL);
    gwy_data_view_set_base_layer(GWY_DATA_VIEW(controls.view), layer);

    gtk_box_pack_start(GTK_BOX(vbox), controls.view, FALSE, FALSE, 0);

    hbox2 = gwy_synth_instant_updates_new(&controls,
                                          &controls.update_now,
                                          &controls.update,
                                          &args->update);
    gtk_box_pack_start(GTK_BOX(vbox), hbox2, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(controls.update, TRUE);
    g_signal_connect_swapped(controls.update_now, "clicked",
                             G_CALLBACK(preview), &controls);

    controls.animated = check
        = gtk_check_button_new_with_mnemonic(_("Progressive preview"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), args->animated);
    gtk_box_pack_start(GTK_BOX(hbox2), check, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(check), "target", &args->animated);
    g_signal_connect_swapped(check, "toggled",
                             G_CALLBACK(gwy_synth_boolean_changed), &controls);

    gtk_box_pack_start(GTK_BOX(vbox),
                       gwy_synth_random_seed_new(&controls,
                                                 &controls.seed, &args->seed),
                       FALSE, FALSE, 0);

    controls.randomize = gwy_synth_randomize_new(&args->randomize);
    gtk_box_pack_start(GTK_BOX(vbox), controls.randomize, FALSE, FALSE, 0);

    notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(hbox), notebook, TRUE, TRUE, 4);
    g_signal_connect_swapped(notebook, "switch-page",
                             G_CALLBACK(page_switched), &controls);

    controls.dims = gwy_dimensions_new(dimsargs, dfield_template);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             gwy_dimensions_get_widget(controls.dims),
                             gtk_label_new(_("Dimensions")));

    table = gtk_table_new(19 + (dfield_template ? 1 : 0), 4, FALSE);
    controls.table = GTK_TABLE(table);
    gtk_table_set_row_spacings(GTK_TABLE(table), 2);
    gtk_table_set_col_spacings(GTK_TABLE(table), 6);
    gtk_container_set_border_width(GTK_CONTAINER(table), 4);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), table,
                             gtk_label_new(_("Generator")));
    row = 0;

    controls.niters = gtk_adjustment_new(args->niters, 1, 10000, 1, 10, 0);
    g_object_set_data(G_OBJECT(controls.niters), "target", &args->niters);
    gwy_table_attach_hscale(table, row, _("_Number of iterations:"), NULL,
                            GTK_OBJECT(controls.niters), GWY_HSCALE_SQRT);
    g_signal_connect_swapped(controls.niters, "value-changed",
                             G_CALLBACK(gwy_synth_int_changed), &controls);
    row++;

#if 0
    controls.coverage = gtk_adjustment_new(args->coverage,
                                           0.1, 1000.0, 0.001, 1.0, 0);
    g_object_set_data(G_OBJECT(controls.coverage), "target", &args->coverage);
    gwy_table_attach_hscale(table, row, _("Co_verage:"), NULL,
                            controls.coverage, GWY_HSCALE_SQRT);
    g_signal_connect_swapped(controls.coverage, "value-changed",
                             G_CALLBACK(gwy_synth_double_changed), &controls);
    row++;

    gtk_table_set_row_spacing(GTK_TABLE(table), row-1, 8);
    gtk_table_attach(GTK_TABLE(table), gwy_label_new_header(_("Particle Size")),
                     0, 3, row, row+1, GTK_FILL, 0, 0, 0);
    row++;

    controls.height = gtk_adjustment_new(args->height, 0.1, 10.0, 0.1, 1.0, 0);
    g_object_set_data(G_OBJECT(controls.height), "target", &args->height);
    gwy_table_attach_hscale(table, row, _("_Height:"), "px",
                            controls.height, GWY_HSCALE_SQRT);
    g_signal_connect_swapped(controls.height, "value-changed",
                             G_CALLBACK(gwy_synth_double_changed), &controls);
    row++;

    row = gwy_synth_attach_variance(&controls, row,
                                    &controls.height_noise,
                                    &args->height_noise);

    gtk_table_set_row_spacing(GTK_TABLE(table), row-1, 8);
    gtk_table_attach(GTK_TABLE(table),
                     gwy_label_new_header(_("Incidence")),
                     0, 3, row, row+1, GTK_FILL, 0, 0, 0);
    row++;

    row = gwy_synth_attach_angle(&controls, row, &controls.theta, &args->theta,
                                 0.0, 0.99*G_PI/2.0, _("Inclination"));
    row = gwy_synth_attach_variance(&controls, row,
                                    &controls.theta_spread,
                                    &args->theta_spread);

    row = gwy_synth_attach_angle(&controls, row, &controls.phi, &args->phi,
                                 -G_PI, G_PI, _("Direction"));
    row = gwy_synth_attach_variance(&controls, row,
                                    &controls.phi_spread,
                                    &args->phi_spread);

    gtk_table_set_row_spacing(GTK_TABLE(table), row-1, 8);
    gtk_table_attach(GTK_TABLE(table),
                     gwy_label_new_header(_("Options")),
                     0, 3, row, row+1, GTK_FILL, 0, 0, 0);
    row++;

    controls.relaxation = relaxation_selector_new(&controls);
    gwy_table_attach_hscale(table, row, _("Relaxation type:"), NULL,
                            GTK_OBJECT(controls.relaxation), GWY_HSCALE_WIDGET);
#endif

    gtk_widget_show_all(dialog);
    controls.in_init = FALSE;
    /* Must be done when widgets are shown, see GtkNotebook docs */
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), args->active_page);
    update_values(&controls);

    finished = FALSE;
    while (!finished) {
        response = gtk_dialog_run(GTK_DIALOG(dialog));
        switch (response) {
            case GTK_RESPONSE_CANCEL:
            case GTK_RESPONSE_DELETE_EVENT:
            case GTK_RESPONSE_OK:
            gtk_widget_destroy(dialog);
            case GTK_RESPONSE_NONE:
            finished = TRUE;
            break;

            case RESPONSE_RESET:
            {
                gint temp2 = args->active_page;
                *args = domain_synth_defaults;
                args->active_page = temp2;
            }
            controls.in_init = TRUE;
            update_controls(&controls, args);
            controls.in_init = FALSE;
            break;

            default:
            g_assert_not_reached();
            break;
        }
    }

    domain_synth_save_args(gwy_app_settings_get(), args, dimsargs);

    g_object_unref(controls.mydata);
    gwy_object_unref(controls.surface);
    gwy_dimensions_free(controls.dims);

    return response == GTK_RESPONSE_OK;
}

static GtkWidget*
quantity_selector_new(DomainSynthControls *controls)
{
    GtkWidget *combo;

    combo = gwy_enum_combo_box_new(quantity_types,
                                   G_N_ELEMENTS(quantity_types),
                                   G_CALLBACK(quantity_selected),
                                   controls, controls->args->quantity, TRUE);
    return combo;
}

static void
update_controls(DomainSynthControls *controls,
                DomainSynthArgs *args)
{
    gtk_adjustment_set_value(GTK_ADJUSTMENT(controls->seed), args->seed);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(controls->randomize),
                                 args->randomize);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(controls->animated),
                                 args->animated);
    gwy_enum_combo_box_set_active(GTK_COMBO_BOX(controls->quantity),
                                  args->quantity);
}

static void
page_switched(DomainSynthControls *controls,
              G_GNUC_UNUSED GtkNotebookPage *page,
              gint pagenum)
{
    if (controls->in_init)
        return;

    controls->args->active_page = pagenum;
    if (pagenum == PAGE_GENERATOR)
        update_values(controls);
}

static void
update_values(DomainSynthControls *controls)
{
    GwyDimensions *dims = controls->dims;

    controls->pxsize = dims->args->measure * pow10(dims->args->xypow10);
    if (controls->height_units)
        gtk_label_set_markup(GTK_LABEL(controls->height_units),
                             dims->zvf->units);
}

static void
quantity_selected(GtkComboBox *combo,
                  DomainSynthControls *controls)
{
    controls->args->quantity = gwy_enum_combo_box_get_active(combo);
}

static void
domain_synth_invalidate(G_GNUC_UNUSED DomainSynthControls *controls)
{
}

static void
preview(DomainSynthControls *controls)
{
    DomainSynthArgs *args = controls->args;
    GwyDataField *dfield, *vfield;
    //gdouble rx, ry;

    dfield = GWY_DATA_FIELD(gwy_container_get_object_by_name(controls->mydata,
                                                             "/0/data"));

    if (controls->dims->args->add && controls->surface)
        gwy_data_field_copy(controls->surface, dfield, TRUE);
    else
        gwy_data_field_clear(dfield);

    //scale_to_unit_cubes(dfield, &rx, &ry);

    gwy_app_wait_start(GTK_WINDOW(controls->dialog), _("Starting..."));
    vfield = gwy_data_field_new_alike(dfield, FALSE);
    domain_synth_do(args, dfield, vfield, 1.25);
    g_object_unref(vfield);
    gwy_app_wait_finish();

    //scale_from_unit_cubes(dfield,rx, ry);
    gwy_data_field_data_changed(dfield);
}

static inline gint
mc_step8(gint u,
         gint u1, gint u2, gint u3, gint u4,
         gint u5, gint u6, gint u7, gint u8,
         GRand *rng, gdouble T, gdouble J, gdouble v)
{
    gint s1 = (u == u1) + (u == u2) + (u == u3) + (u == u4);
    gint s2 = (u == u5) + (u == u6) + (u == u7) + (u == u8);
    gdouble E = 6.0 - s1 - 0.5*s2 + J*u*v;
    gdouble Enew = s1 + 0.5*s2 - J*u*v;
    if (Enew < E || g_rand_double(rng) < exp((E - Enew)/T))
        return -u;
    return u;
}

static void
field_mc_step8(const GwyDataField *vfield, const gint *u, gint *unew,
               const DomainSynthArgs *args,
               GRand *rng)
{
    gdouble T = args->T, J = args->J;
    guint xres = vfield->xres, yres = vfield->yres, n = xres*yres;
    const gdouble *v = vfield->data;
    guint i, j;

    /* Top row. */
    unew[0] = mc_step8(u[0],
                       u[1], u[xres-1], u[xres], u[n-xres],
                       u[xres+1], u[2*xres-1], u[n-xres+1], u[n-1],
                       rng, T, J, v[0]);

    for (j = 1; j < xres-1; j++) {
        unew[j] = mc_step8(u[j],
                           u[j-1], u[j+1], u[j+xres], u[j + n-xres],
                           u[j+xres-1], u[j+xres+1], u[j-1 + n-xres], u[j+1 + n-xres],
                           rng, T, J, v[j]);
    }

    j = xres-1;
    unew[j] = mc_step8(u[j],
                       u[0], u[j+xres], u[j-1], u[n-1],
                       u[2*xres-2],  u[xres], u[n-2], u[n-xres],
                       rng, T, J, v[j]);

    /* Inner rows. */
    for (i = 1; i < yres-1; i++) {
        gint *unewrow = unew + i*xres;
        const gint *urow = u + i*xres;
        const gint *uprevrow = u + (i - 1)*xres;
        const gint *unextrow = u + (i + 1)*xres;
        const gdouble *vrow = v + i*xres;

        unewrow[0] = mc_step8(urow[0],
                              uprevrow[0], urow[1], unextrow[0], urow[xres-1],
                              uprevrow[1], uprevrow[xres-1], unextrow[1], unextrow[xres-1],
                              rng, T, J, vrow[0]);

        for (j = 1; j < xres-1; j++) {
            unewrow[j] = mc_step8(urow[j],
                                  uprevrow[j], urow[j-1], urow[j+1], unextrow[j],
                                  uprevrow[j-1], uprevrow[j+1], unextrow[j-1], unextrow[j+1],
                                  rng, T, J, vrow[j]);
        }

        j = xres-1;
        unewrow[j] = mc_step8(urow[j],
                              uprevrow[j], urow[0], urow[xres-2], unextrow[j],
                              uprevrow[0], uprevrow[xres-2], unextrow[0], unextrow[xres-2],
                              rng, T, J, vrow[j]);
    }

    /* Bottom row. */
    j = i = n-xres;
    unew[j] = mc_step8(u[j],
                       u[j+1], u[0], u[n-1], u[j-xres],
                       u[j - xres-1], u[j - xres+1], u[1], u[xres-1],
                       rng, T, J, v[j]);

    for (j = 1; j < xres-1; j++) {
        unew[i + j] = mc_step8(u[i + j],
                               u[i + j-1], u[i + j+1], u[i + j-xres], u[j],
                               u[i + j-xres-1], u[i + j-xres+1], u[j-1], u[j+1],
                               rng, T, J, v[i + j]);
    }

    j = n-1;
    unew[j] = mc_step8(u[j],
                       u[i], u[j-xres], u[xres-1], u[j-1],
                       u[0], u[xres-2], u[i-2], u[i-xres],
                       rng, T, J, v[j]);
}

static inline gdouble
v_rk4_step(gdouble v, gint u, gdouble mu, gdouble nu, gdouble dt)
{
    gdouble p = (mu*u - v - nu)*dt;
    return v + p*(1.0 - p*(0.5 - p*(1.0/6.0 - p/24.0)));
}

static void
field_rk4_step(GwyDataField *vfield, const gint *u,
               const DomainSynthArgs *args)
{
    gdouble mu = args->mu, nu = args->nu, dt = args->dt;
    guint xres = vfield->xres, yres = vfield->yres, n = xres*yres;
    gdouble *v = vfield->data;
    guint k;

    for (k = 0; k < n; k++)
        v[k] = v_rk4_step(v[k], u[k], mu, nu, dt);
}

static gint*
create_random_ufield(guint xres, guint yres, GRand *rng)
{
    gint *ufield = g_new(gint, xres*yres);
    guint k;

    for (k = 0; k < xres*yres; k++)
        ufield[k] = g_rand_boolean(rng) ? 1 : -1;

    return ufield;
}

static gboolean
domain_synth_do(const DomainSynthArgs *args,
                GwyDataField *ufield,
                GwyDataField *vfield,
                gdouble preview_time)
{
    gint xres, yres;
    gulong i;
    gdouble lasttime = 0.0, lastpreviewtime = 0.0, currtime;
    GTimer *timer;
    GRand *rng;
    gint *u, *ubuf;
    guint k;
    gboolean finished = FALSE;

    timer = g_timer_new();

    xres = gwy_data_field_get_xres(ufield);
    yres = gwy_data_field_get_yres(ufield);

    gwy_app_wait_set_message(_("Running computation..."));
    gwy_app_wait_set_fraction(0.0);

    rng = g_rand_new();
    g_rand_set_seed(rng, args->seed);

    gwy_data_field_clear(vfield);
    u = create_random_ufield(xres, yres, rng);
    ubuf = g_new(gint, xres*yres);

    for (i = 0; i < args->niters; i++) {
        field_mc_step8(vfield, u, ubuf, args, rng);
        field_rk4_step(vfield, ubuf, args);
        field_mc_step8(vfield, ubuf, u, args, rng);
        field_rk4_step(vfield, u, args);

        if (i % 20 == 0) {
            currtime = g_timer_elapsed(timer, NULL);
            if (currtime - lasttime >= 0.25) {
                if (!gwy_app_wait_set_fraction((gdouble)i/args->niters))
                    goto fail;
                lasttime = currtime;

                if (args->animated
                    && currtime - lastpreviewtime >= preview_time) {
                    for (k = 0; k < xres*yres; k++)
                        ufield->data[k] = 0.5*(u[k] + ubuf[k]);
                    gwy_data_field_invalidate(ufield);
                    gwy_data_field_data_changed(ufield);
                    gwy_data_field_data_changed(vfield);
                    lastpreviewtime = lasttime;
                }
            }
        }
    }

    for (k = 0; k < xres*yres; k++)
        ufield->data[k] = 0.5*(u[k] + ubuf[k]);
    gwy_data_field_invalidate(ufield);

    finished = TRUE;

fail:
    g_timer_destroy(timer);
    g_rand_free(rng);
    g_free(u);
    g_free(ubuf);

    return finished;
}

static const gchar prefix[]               = "/module/domain_synth";
static const gchar active_page_key[]      = "/module/domain_synth/active_page";
static const gchar randomize_key[]        = "/module/domain_synth/randomize";
static const gchar seed_key[]             = "/module/domain_synth/seed";
static const gchar animated_key[]         = "/module/domain_synth/animated";
static const gchar T_key[]                = "/module/domain_synth/T";
static const gchar J_key[]                = "/module/domain_synth/J";
static const gchar mu_key[]               = "/module/domain_synth/mu";
static const gchar nu_key[]               = "/module/domain_synth/nu";
static const gchar dt_key[]               = "/module/domain_synth/dt";
static const gchar quantity_key[]         = "/module/domain_synth/quantity";
static const gchar preview_quantity_key[] = "/module/domain_synth/preview_quantity";
static const gchar niters_key[]           = "/module/domain_synth/niters";
static const gchar height_key[]           = "/module/domain_synth/height";

static void
domain_synth_sanitize_args(DomainSynthArgs *args)
{
    args->active_page = CLAMP(args->active_page,
                              PAGE_DIMENSIONS, PAGE_NPAGES-1);
    args->seed = MAX(0, args->seed);
    args->randomize = !!args->randomize;
    args->animated = !!args->animated;
    args->niters = MIN(args->niters, 10000);
    args->T = CLAMP(args->T, 0.001, 100.0);
    args->J = CLAMP(args->J, 0.001, 100.0);
    args->mu = CLAMP(args->mu, 0.001, 100.0);
    args->nu = CLAMP(args->nu, -1.0, 1.0);
    args->dt = CLAMP(args->dt, 0.001, 100.0);
    args->height = CLAMP(args->height, 0.001, 10000.0);
    args->quantity &= QUANTITY_MASK;
    args->preview_quantity = MIN(args->preview_quantity, QUANTITY_V);
}

static void
domain_synth_load_args(GwyContainer *container,
                    DomainSynthArgs *args,
                    GwyDimensionArgs *dimsargs)
{
    *args = domain_synth_defaults;

    gwy_container_gis_int32_by_name(container, active_page_key,
                                    &args->active_page);
    gwy_container_gis_int32_by_name(container, seed_key, &args->seed);
    gwy_container_gis_boolean_by_name(container, randomize_key,
                                      &args->randomize);
    gwy_container_gis_boolean_by_name(container, animated_key,
                                      &args->animated);
    gwy_container_gis_int32_by_name(container, niters_key, &args->niters);
    gwy_container_gis_double_by_name(container, T_key, &args->T);
    gwy_container_gis_double_by_name(container, J_key, &args->J);
    gwy_container_gis_double_by_name(container, mu_key, &args->mu);
    gwy_container_gis_double_by_name(container, nu_key, &args->nu);
    gwy_container_gis_double_by_name(container, dt_key, &args->dt);
    gwy_container_gis_enum_by_name(container, quantity_key, &args->quantity);
    gwy_container_gis_enum_by_name(container, preview_quantity_key,
                                   &args->preview_quantity);
    gwy_container_gis_double_by_name(container, height_key, &args->height);
    domain_synth_sanitize_args(args);

    gwy_clear(dimsargs, 1);
    gwy_dimensions_copy_args(&dims_defaults, dimsargs);
    gwy_dimensions_load_args(dimsargs, container, prefix);
}

static void
domain_synth_save_args(GwyContainer *container,
                    const DomainSynthArgs *args,
                    const GwyDimensionArgs *dimsargs)
{
    gwy_container_set_int32_by_name(container, active_page_key,
                                    args->active_page);
    gwy_container_set_int32_by_name(container, seed_key, args->seed);
    gwy_container_set_boolean_by_name(container, randomize_key,
                                      args->randomize);
    gwy_container_set_boolean_by_name(container, animated_key,
                                      args->animated);
    gwy_container_set_int32_by_name(container, niters_key, args->niters);
    gwy_container_set_double_by_name(container, T_key, args->T);
    gwy_container_set_double_by_name(container, J_key, args->J);
    gwy_container_set_double_by_name(container, mu_key, args->mu);
    gwy_container_set_double_by_name(container, nu_key, args->nu);
    gwy_container_set_double_by_name(container, dt_key, args->dt);
    gwy_container_set_enum_by_name(container, quantity_key, args->quantity);
    gwy_container_set_enum_by_name(container, preview_quantity_key,
                                   args->preview_quantity);
    gwy_container_set_double_by_name(container, height_key, args->height);

    gwy_dimensions_save_args(dimsargs, container, prefix);
}

/* vim: set cin et ts=4 sw=4 cino=>1s,e0,n0,f0,{0,}0,^0,\:1s,=0,g1s,h0,t0,+1s,c3,(0,u0 : */
