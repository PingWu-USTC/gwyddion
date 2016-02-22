/*
 *  @(#) $Id$
 *  Copyright (C) 2003-2004,2013,2016 David Necas (Yeti), Petr Klapetek.
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
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include <string.h>
#include <libgwyddion/gwymacros.h>
#include <libgwyddion/gwyutils.h>
#include <libgwyddion/gwycontainer.h>
#include <libgwymodule/gwymodule-xyz.h>
#include "gwymoduleinternal.h"

/* The surface function information */
typedef struct {
    const gchar *name;
    const gchar *menu_path;
    const gchar *stock_id;
    const gchar *tooltip;
    GwyRunType run;
    guint sens_mask;
    GwyXYZFunc func;
} GwyXYZFuncInfo;

/* Auxiliary structure to pass both user callback function and data to
 * g_hash_table_foreach() lambda argument in gwy_xyz_func_foreach() */
typedef struct {
    GFunc function;
    gpointer user_data;
} ProcFuncForeachData;

static GHashTable *surface_funcs = NULL;
static GPtrArray *call_stack = NULL;

/**
 * gwy_xyz_func_register:
 * @name: Name of function to register.  It should be a valid identifier and
 *        if a module registers only one function, module and function names
 *        should be the same.
 * @func: The function itself.
 * @menu_path: Menu path under XYZ Data menu.  The menu path should be
 *             marked translatabe, but passed untranslated (to allow merging
 *             of translated and untranslated submenus).
 * @stock_id: Stock icon id for toolbar.
 * @run: Supported run modes.  XYZ surface data processing functions can have
 *       two run modes: %GWY_RUN_IMMEDIATE (no questions asked) and
 *       %GWY_RUN_INTERACTIVE (a modal dialog with parameters).
 * @sens_mask: Sensitivity mask (a combination of #GwyMenuSensFlags flags).
 *             Usually it contains #GWY_MENU_FLAG_XYZ, possibly other
 *             requirements.
 * @tooltip: Tooltip for this function.
 *
 * Registers a surface data processing function.
 *
 * Note: the string arguments are not copied as modules are not expected to
 * vanish.  If they are constructed (non-constant) strings, do not free them.
 * Should modules ever become unloadable they will get a chance to clean-up.
 *
 * Returns: Normally %TRUE; %FALSE on failure.
 *
 * Since: 2.45
 **/
gboolean
gwy_xyz_func_register(const gchar *name,
                      GwyXYZFunc func,
                      const gchar *menu_path,
                      const gchar *stock_id,
                      GwyRunType run,
                      guint sens_mask,
                      const gchar *tooltip)
{
    GwyXYZFuncInfo *func_info;

    g_return_val_if_fail(name, FALSE);
    g_return_val_if_fail(func, FALSE);
    g_return_val_if_fail(menu_path, FALSE);
    g_return_val_if_fail(run & GWY_RUN_MASK, FALSE);
    gwy_debug("name = %s, menu path = %s, run = %d, func = %p",
              name, menu_path, run, func);

    if (!surface_funcs) {
        gwy_debug("Initializing...");
        surface_funcs = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              NULL, g_free);
        call_stack = g_ptr_array_new();
    }

    if (!gwy_strisident(name, "_-", NULL))
        g_warning("Function name `%s' is not a valid identifier. "
                  "It may be rejected in future.", name);
    if (g_hash_table_lookup(surface_funcs, name)) {
        g_warning("Duplicate function `%s', keeping only first", name);
        return FALSE;
    }

    func_info = g_new0(GwyXYZFuncInfo, 1);
    func_info->name = name;
    func_info->func = func;
    func_info->menu_path = menu_path;
    func_info->stock_id = stock_id;
    func_info->tooltip = tooltip;
    func_info->run = run;
    func_info->sens_mask = sens_mask;

    g_hash_table_insert(surface_funcs, (gpointer)func_info->name, func_info);
    if (!_gwy_module_add_registered_function(GWY_MODULE_PREFIX_XYZ, name)) {
        g_hash_table_remove(surface_funcs, func_info->name);
        return FALSE;
    }

    return TRUE;
}

/**
 * gwy_xyz_func_run:
 * @name: XYZ surface data processing function name.
 * @data: Data (a #GwyContainer).
 * @run: How the function should be run.
 *
 * Runs a surface data processing function identified by @name.
 *
 * Since: 2.45
 **/
void
gwy_xyz_func_run(const gchar *name,
                 GwyContainer *data,
                 GwyRunType run)
{
    GwyXYZFuncInfo *func_info;

    func_info = g_hash_table_lookup(surface_funcs, name);
    g_return_if_fail(run & func_info->run);
    g_ptr_array_add(call_stack, func_info);
    func_info->func(data, run, name);
    g_return_if_fail(call_stack->len);
    g_ptr_array_set_size(call_stack, call_stack->len-1);
}

static void
gwy_xyz_func_user_cb(gpointer key,
                     G_GNUC_UNUSED gpointer value,
                     gpointer user_data)
{
    ProcFuncForeachData *pffd = (ProcFuncForeachData*)user_data;

    pffd->function(key, pffd->user_data);
}

/**
 * gwy_xyz_func_foreach:
 * @function: Function to run for each surface function.  It will get function
 *            name (constant string owned by module system) as its first
 *            argument, @user_data as the second argument.
 * @user_data: Data to pass to @function.
 *
 * Calls a function for each surface function.
 *
 * Since: 2.45
 **/
void
gwy_xyz_func_foreach(GFunc function,
                     gpointer user_data)
{
    ProcFuncForeachData pffd;

    if (!surface_funcs)
        return;

    pffd.user_data = user_data;
    pffd.function = function;
    g_hash_table_foreach(surface_funcs, gwy_xyz_func_user_cb, &pffd);
}

/**
 * gwy_xyz_func_exists:
 * @name: XYZ surface data processing function name.
 *
 * Checks whether a surface data processing function exists.
 *
 * Returns: %TRUE if function @name exists, %FALSE otherwise.
 *
 * Since: 2.45
 **/
gboolean
gwy_xyz_func_exists(const gchar *name)
{
    return surface_funcs && g_hash_table_lookup(surface_funcs, name);
}

/**
 * gwy_xyz_func_get_run_types:
 * @name: XYZ surface data processing function name.
 *
 * Returns run modes supported by a surface data processing function.
 *
 * Returns: The run mode bit mask.
 *
 * Since: 2.45
 **/
GwyRunType
gwy_xyz_func_get_run_types(const gchar *name)
{
    GwyXYZFuncInfo *func_info;

    func_info = g_hash_table_lookup(surface_funcs, name);
    g_return_val_if_fail(func_info, 0);

    return func_info->run;
}

/**
 * gwy_xyz_func_get_menu_path:
 * @name: XYZ surface data processing function name.
 *
 * Returns the menu path of a surface data processing function.
 *
 * The returned menu path is only the tail part registered by the function,
 * i.e., without any leading "/XYZ Data".
 *
 * Returns: The menu path.  The returned string is owned by the module.
 *
 * Since: 2.45
 **/
const gchar*
gwy_xyz_func_get_menu_path(const gchar *name)
{
    GwyXYZFuncInfo *func_info;

    func_info = g_hash_table_lookup(surface_funcs, name);
    g_return_val_if_fail(func_info, 0);

    return func_info->menu_path;
}

/**
 * gwy_xyz_func_get_stock_id:
 * @name: XYZ surface data processing function name.
 *
 * Gets stock icon id of a surface data processing  function.
 *
 * Returns: The stock icon id.  The returned string is owned by the module.
 *
 * Since: 2.45
 **/
const gchar*
gwy_xyz_func_get_stock_id(const gchar *name)
{
    GwyXYZFuncInfo *func_info;

    g_return_val_if_fail(surface_funcs, NULL);
    func_info = g_hash_table_lookup(surface_funcs, name);
    g_return_val_if_fail(func_info, NULL);

    return func_info->stock_id;
}

/**
 * gwy_xyz_func_get_tooltip:
 * @name: XYZ surface data processing function name.
 *
 * Gets tooltip for a surface data processing function.
 *
 * Returns: The tooltip.  The returned string is owned by the module.
 *
 * Since: 2.45
 **/
const gchar*
gwy_xyz_func_get_tooltip(const gchar *name)
{
    GwyXYZFuncInfo *func_info;

    g_return_val_if_fail(surface_funcs, NULL);
    func_info = g_hash_table_lookup(surface_funcs, name);
    g_return_val_if_fail(func_info, NULL);

    return func_info->tooltip;
}

/**
 * gwy_xyz_func_get_sensitivity_mask:
 * @name: XYZ surface data processing function name.
 *
 * Gets menu sensititivy mask for a surface data processing function.
 *
 * Returns: The menu item sensitivity mask (a combination of #GwyMenuSensFlags
 *          flags).
 *
 * Since: 2.45
 **/
guint
gwy_xyz_func_get_sensitivity_mask(const gchar *name)
{
    GwyXYZFuncInfo *func_info;

    func_info = g_hash_table_lookup(surface_funcs, name);
    g_return_val_if_fail(func_info, 0);

    return func_info->sens_mask;
}

/**
 * gwy_xyz_func_current:
 *
 * Obtains the name of currently running surface data processing function.
 *
 * If no surface data processing function is currently running, %NULL is
 * returned.
 *
 * If multiple nested functions are running (which is not usual but technically
 * possible), the innermost function name is returned.
 *
 * Returns: The name of currently running surface data processing function or
 *          %NULL.
 *
 * Since: 2.38
 **/
const gchar*
gwy_xyz_func_current(void)
{
    GwyXYZFuncInfo *func_info;

    if (!call_stack || !call_stack->len)
        return NULL;

    func_info = (GwyXYZFuncInfo*)g_ptr_array_index(call_stack,
                                                      call_stack->len-1);
    return func_info->name;
}

gboolean
_gwy_xyz_func_remove(const gchar *name)
{
    gwy_debug("%s", name);
    if (!g_hash_table_remove(surface_funcs, name)) {
        g_warning("Cannot remove function %s", name);
        return FALSE;
    }
    return TRUE;
}

/************************** Documentation ****************************/

/**
 * SECTION:gwymodule-surface
 * @title: gwymodule-surface
 * @short_description: XYZ surface data processing modules
 *
 * XYZ surface data processing modules implement function processing surface
 * data represented with #GwySurface.  They reigster functions that get
 * a #GwyContainer with data and either modify it or create a new data from it.
 * In this regard, they are quite similar to regular (two-dimensional) data
 * processing functions but they live in separate menus, toolbars, etc.
 *
 * XYZ surface data processing functions were introduced in version 2.32.
 **/

/**
 * GwyXYZFuncInfo:
 * @name: An unique surface data processing function name.
 * @menu_path: A path under "/XYZ Data" where the function should appear.
 *             It must start with "/".
 * @surface: The function itself.
 * @run: Possible run-modes for this function.
 * @sens_flags: Sensitivity flags.  XYZ surface data processing function should
 *              include, in general, %GWY_MENU_FLAG_XYZ.  Functions
 *              constructing synthetic data from nothing do not have to specify
 *              even %GWY_MENU_FLAG_XYZ.
 *
 * Information about one surface data processing function.
 *
 * Since: 2.45
 **/

/**
 * GwyXYZFunc:
 * @data: The data container to operate on.
 * @run: Run mode.
 * @name: Function name from as registered with gwy_xyz_func_register()
 *        (single-function modules can safely ignore this argument).
 *
 * The type of surface data processing function.
 *
 * Since: 2.45
 **/

/* vim: set cin et ts=4 sw=4 cino=>1s,e0,n0,f0,{0,}0,^0,\:1s,=0,g1s,h0,t0,+1s,c3,(0,u0 : */
