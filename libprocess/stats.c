/*
 *  @(#) $Id$
 *  Copyright (C) 2003-2009 David Necas (Yeti), Petr Klapetek.
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

#ifdef HAVE_FFTW3
#include <fftw3.h>
#endif

#include <libgwyddion/gwymacros.h>
#include <libgwyddion/gwymath.h>
#include <libprocess/datafield.h>
#include <libprocess/level.h>
#include <libprocess/stats.h>
#include <libprocess/linestats.h>
#include <libprocess/grains.h>
#include <libprocess/inttrans.h>
#include <libprocess/simplefft.h>
#include "gwyprocessinternal.h"
#include "wrappers.h"

typedef gdouble (*LineStatFunc)(GwyDataLine *dline);

typedef struct _BinTreeNode BinTreeNode;
typedef struct _QuadTreeNode QuadTreeNode;

struct _BinTreeNode {
    /* This optimally uses memory on 64bit architectures where pt and
     * children have the same size (16 bytes). */
    union {
        /* The at most two points inside for non-max-depth leaves. */
        struct {
            gdouble a;
            gdouble b;
        } pt;
        /* Children for non-max-depth non-leaves. */
        BinTreeNode *children[2];
    } u;
    /* Always set; for max-depth leaves it is the only meaningful field. */
    guint count;
};

typedef struct {
    gdouble min;
    gdouble max;
    BinTreeNode *root;
    guint maxdepth;
    gboolean degenerate;
    gdouble degenerateS;
} BinTree;

struct _QuadTreeNode {
    /* This optimally uses memory on 64bit architectures where pt and
     * children have the same size (32 bytes). */
    union {
        /* The at most two points inside for non-max-depth leaves. */
        struct {
            GwyXY a;
            GwyXY b;
        } pt;
        /* Children for non-max-depth non-leaves. */
        QuadTreeNode *children[4];
    } u;
    /* Always set; for max-depth leaves it is the only meaningful field. */
    guint count;
};

typedef struct {
    GwyXY min;
    GwyXY max;
    QuadTreeNode *root;
    guint maxdepth;
    gboolean degenerate;
    gdouble degenerateS;
} QuadTree;

/**
 * gwy_data_field_get_max:
 * @data_field: A data field.
 *
 * Finds the maximum value of a data field.
 *
 * This quantity is cached.
 *
 * Returns: The maximum value.
 **/
gdouble
gwy_data_field_get_max(GwyDataField *data_field)
{
    g_return_val_if_fail(GWY_IS_DATA_FIELD(data_field), -G_MAXDOUBLE);
    gwy_debug("%s", CTEST(data_field, MAX) ? "cache" : "lame");

    if (!CTEST(data_field, MAX)) {
        const gdouble *p;
        gdouble max;
        gint i;

        max = data_field->data[0];
        p = data_field->data;
        for (i = data_field->xres * data_field->yres; i; i--, p++) {
            if (G_UNLIKELY(max < *p))
                max = *p;
        }
        CVAL(data_field, MAX) = max;
        data_field->cached |= CBIT(MAX);
    }

    return CVAL(data_field, MAX);
}


/**
 * gwy_data_field_area_get_max:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Finds the maximum value in a rectangular part of a data field.
 *
 * Returns: The maximum value.  When the number of samples to calculate
 *          maximum of is zero, -%G_MAXDOUBLE is returned.
 **/
gdouble
gwy_data_field_area_get_max(GwyDataField *dfield,
                            GwyDataField *mask,
                            gint col, gint row,
                            gint width, gint height)
{
    gint i, j;
    gdouble max = -G_MAXDOUBLE;
    const gdouble *datapos, *mpos;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(dfield), max);
    g_return_val_if_fail(!mask || (GWY_IS_DATA_FIELD(mask)
                                   && mask->xres == dfield->xres
                                   && mask->yres == dfield->yres),
                         max);
    g_return_val_if_fail(col >= 0 && row >= 0
                         && width >= 0 && height >= 0
                         && col + width <= dfield->xres
                         && row + height <= dfield->yres,
                         max);
    if (!width || !height)
        return max;

    if (mask) {
        datapos = dfield->data + row*dfield->xres + col;
        mpos = mask->data + row*mask->xres + col;
        for (i = 0; i < height; i++) {
            const gdouble *drow = datapos + i*dfield->xres;
            const gdouble *mrow = mpos + i*mask->xres;

            for (j = 0; j < width; j++) {
                if (G_UNLIKELY(max < *drow) && *mrow > 0.0)
                    max = *drow;
                drow++;
                mrow++;
            }
        }

        return max;
    }

    if (col == 0 && width == dfield->xres
        && row == 0 && height == dfield->yres)
        return gwy_data_field_get_max(dfield);

    datapos = dfield->data + row*dfield->xres + col;
    for (i = 0; i < height; i++) {
        const gdouble *drow = datapos + i*dfield->xres;

        for (j = 0; j < width; j++) {
            if (G_UNLIKELY(max < *drow))
                max = *drow;
            drow++;
        }
    }

    return max;
}

/**
 * gwy_data_field_get_min:
 * @data_field: A data field.
 *
 * Finds the minimum value of a data field.
 *
 * This quantity is cached.
 *
 * Returns: The minimum value.
 **/
gdouble
gwy_data_field_get_min(GwyDataField *data_field)
{
    g_return_val_if_fail(GWY_IS_DATA_FIELD(data_field), -G_MAXDOUBLE);
    gwy_debug("%s", CTEST(data_field, MIN) ? "cache" : "lame");

    if (!CTEST(data_field, MIN)) {
        gdouble min;
        const gdouble *p;
        gint i;

        min = data_field->data[0];
        p = data_field->data;
        for (i = data_field->xres * data_field->yres; i; i--, p++) {
            if (G_UNLIKELY(min > *p))
                min = *p;
        }
        CVAL(data_field, MIN) = min;
        data_field->cached |= CBIT(MIN);
    }

    return CVAL(data_field, MIN);
}


/**
 * gwy_data_field_area_get_min:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Finds the minimum value in a rectangular part of a data field.
 *
 * Returns: The minimum value.  When the number of samples to calculate
 *          minimum of is zero, -%G_MAXDOUBLE is returned.
 **/
gdouble
gwy_data_field_area_get_min(GwyDataField *dfield,
                            GwyDataField *mask,
                            gint col, gint row,
                            gint width, gint height)
{
    gint i, j;
    gdouble min = G_MAXDOUBLE;
    const gdouble *datapos, *mpos;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(dfield), min);
    g_return_val_if_fail(!mask || (GWY_IS_DATA_FIELD(mask)
                                   && mask->xres == dfield->xres
                                   && mask->yres == dfield->yres),
                         min);
    g_return_val_if_fail(col >= 0 && row >= 0
                         && width >= 0 && height >= 0
                         && col + width <= dfield->xres
                         && row + height <= dfield->yres,
                         min);
    if (!width || !height)
        return min;

    if (mask) {
        datapos = dfield->data + row*dfield->xres + col;
        mpos = mask->data + row*mask->xres + col;
        for (i = 0; i < height; i++) {
            const gdouble *drow = datapos + i*dfield->xres;
            const gdouble *mrow = mpos + i*mask->xres;

            for (j = 0; j < width; j++) {
                if (G_UNLIKELY(min > *drow) && *mrow > 0.0)
                    min = *drow;
                drow++;
                mrow++;
            }
        }

        return min;
    }

    if (col == 0 && width == dfield->xres
        && row == 0 && height == dfield->yres)
        return gwy_data_field_get_min(dfield);

    datapos = dfield->data + row*dfield->xres + col;
    for (i = 0; i < height; i++) {
        const gdouble *drow = datapos + i*dfield->xres;

        for (j = 0; j < width; j++) {
            if (G_UNLIKELY(min > *drow))
                min = *drow;
            drow++;
        }
    }

    return min;
}

/**
 * gwy_data_field_get_min_max:
 * @data_field: A data field.
 * @min: Location to store minimum to.
 * @max: Location to store maximum to.
 *
 * Finds minimum and maximum values of a data field.
 **/
void
gwy_data_field_get_min_max(GwyDataField *data_field,
                           gdouble *min,
                           gdouble *max)
{
    gboolean need_min = FALSE, need_max = FALSE;
    gdouble min1, max1;
    const gdouble *p;
    gint i;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));

    if (min) {
        if (CTEST(data_field, MIN))
            *min = CVAL(data_field, MIN);
        else
            need_min = TRUE;
    }
    if (max) {
        if (CTEST(data_field, MAX))
            *max = CVAL(data_field, MAX);
        else
            need_max = TRUE;
    }

    if (!need_min && !need_max)
        return;
    else if (!need_min) {
        *max = gwy_data_field_get_max(data_field);
        return;
    }
    else if (!need_max) {
        *min = gwy_data_field_get_min(data_field);
        return;
    }

    min1 = data_field->data[0];
    max1 = data_field->data[0];
    p = data_field->data;
    for (i = data_field->xres * data_field->yres; i; i--, p++) {
        if (G_UNLIKELY(min1 > *p))
            min1 = *p;
        if (G_UNLIKELY(max1 < *p))
            max1 = *p;
    }

    *min = min1;
    *max = max1;
    CVAL(data_field, MIN) = min1;
    CVAL(data_field, MAX) = max1;
    data_field->cached |= CBIT(MIN) | CBIT(MAX);
}

/**
 * gwy_data_field_area_get_min_max:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @min: Location to store minimum to.
 * @max: Location to store maximum to.
 *
 * Finds minimum and maximum values in a rectangular part of a data field.
 *
 * This function is equivalent to calling
 * @gwy_data_field_area_get_min_max_mask()
 * with masking mode %GWY_MASK_INCLUDE.
 **/
void
gwy_data_field_area_get_min_max(GwyDataField *data_field,
                                GwyDataField *mask,
                                gint col, gint row,
                                gint width, gint height,
                                gdouble *min,
                                gdouble *max)
{
    gwy_data_field_area_get_min_max_mask(data_field, mask, GWY_MASK_INCLUDE,
                                         col, row, width, height,
                                         min, max);
}

/**
 * gwy_data_field_area_get_min_max_mask:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @mode: Masking mode to use.  See the introduction for description of
 *        masking modes.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @min: Location to store minimum to.
 * @max: Location to store maximum to.
 *
 * Finds minimum and maximum values in a rectangular part of a data field.
 *
 * Since: 2.18
 **/
void
gwy_data_field_area_get_min_max_mask(GwyDataField *data_field,
                                     GwyDataField *mask,
                                     GwyMaskingType mode,
                                     gint col, gint row,
                                     gint width, gint height,
                                     gdouble *min,
                                     gdouble *max)
{
    gdouble min1 = G_MAXDOUBLE, max1 = -G_MAXDOUBLE;
    const gdouble *datapos, *mpos;
    gint i, j;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    g_return_if_fail(!mask || (GWY_IS_DATA_FIELD(mask)
                               && mask->xres == data_field->xres
                               && mask->yres == data_field->yres));
    g_return_if_fail(col >= 0 && row >= 0
                     && width >= 0 && height >= 0
                     && col + width <= data_field->xres
                     && row + height <= data_field->yres);
    if (!width || !height) {
        if (min)
            *min = min1;
        if (max)
            *max = max1;
        return;
    }

    if (!min && !max)
        return;

    if (mask && mode != GWY_MASK_IGNORE) {
        datapos = data_field->data + row*data_field->xres + col;
        mpos = mask->data + row*mask->xres + col;
        for (i = 0; i < height; i++) {
            const gdouble *drow = datapos + i*data_field->xres;
            const gdouble *mrow = mpos + i*mask->xres;

            if (mode == GWY_MASK_INCLUDE) {
                for (j = 0; j < width; j++) {
                    if (G_UNLIKELY(min1 > *drow) && *mrow > 0.0)
                        min1 = *drow;
                    if (G_UNLIKELY(max1 < *drow) && *mrow > 0.0)
                        max1 = *drow;
                    drow++;
                    mrow++;
                }
            }
            else {
                for (j = 0; j < width; j++) {
                    if (G_UNLIKELY(min1 > *drow) && *mrow < 1.0)
                        min1 = *drow;
                    if (G_UNLIKELY(max1 < *drow) && *mrow < 1.0)
                        max1 = *drow;
                    drow++;
                    mrow++;
                }
            }
        }

        if (min)
            *min = min1;
        if (max)
            *max = max1;

        return;
    }

    if (col == 0 && width == data_field->xres
        && row == 0 && height == data_field->yres) {
        gwy_data_field_get_min_max(data_field, min, max);
        return;
    }

    if (!min) {
        *max = gwy_data_field_area_get_max(data_field, NULL,
                                           col, row, width, height);
        return;
    }
    if (!max) {
        *min = gwy_data_field_area_get_min(data_field, NULL,
                                           col, row, width, height);
        return;
    }

    datapos = data_field->data + row*data_field->xres + col;
    for (i = 0; i < height; i++) {
        const gdouble *drow = datapos + i*data_field->xres;

        for (j = 0; j < width; j++) {
            if (G_UNLIKELY(min1 > *drow))
                min1 = *drow;
            if (G_UNLIKELY(max1 < *drow))
                max1 = *drow;
            drow++;
        }
    }

    *min = min1;
    *max = max1;
}

void
_gwy_data_field_get_min_max(GwyDataField *field,
                            GwyDataField *mask,
                            GwyMaskingType mode,
                            const GwyFieldPart *fpart,
                            G_GNUC_UNUSED gpointer params,
                            gdouble *results)
{
    gwy_data_field_area_get_min_max_mask(field, mask, mode,
                                         fpart ? fpart->col : 0,
                                         fpart ? fpart->row : 0,
                                         fpart ? fpart->width : field->xres,
                                         fpart ? fpart->height : field->yres,
                                         results, results + 1);
}

/**
 * gwy_data_field_get_sum:
 * @data_field: A data field.
 *
 * Sums all values in a data field.
 *
 * This quantity is cached.
 *
 * Returns: The sum of all values.
 **/
gdouble
gwy_data_field_get_sum(GwyDataField *data_field)
{
    gint i;
    gdouble sum = 0;
    const gdouble *p;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(data_field), sum);

    gwy_debug("%s", CTEST(data_field, SUM) ? "cache" : "lame");
    if (CTEST(data_field, SUM))
        return CVAL(data_field, SUM);

    p = data_field->data;
    for (i = data_field->xres * data_field->yres; i; i--, p++)
        sum += *p;

    CVAL(data_field, SUM) = sum;
    data_field->cached |= CBIT(SUM);

    return sum;
}
/**
 * gwy_data_field_area_get_sum:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Sums values of a rectangular part of a data field.
 *
 * This function is equivalent to calling @gwy_data_field_area_get_sum_mask()
 * with masking mode %GWY_MASK_INCLUDE.
 *
 * Returns: The sum of all values inside area.
 **/
gdouble
gwy_data_field_area_get_sum(GwyDataField *dfield,
                            GwyDataField *mask,
                            gint col, gint row,
                            gint width, gint height)
{
    return gwy_data_field_area_get_sum_mask(dfield, mask, GWY_MASK_INCLUDE,
                                            col, row, width, height);
}

/**
 * gwy_data_field_area_get_sum_mask:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @mode: Masking mode to use.  See the introduction for description of
 *        masking modes.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Sums values of a rectangular part of a data field.
 *
 * Returns: The sum of all values inside area.
 *
 * Since: 2.18
 **/
gdouble
gwy_data_field_area_get_sum_mask(GwyDataField *dfield,
                                 GwyDataField *mask,
                                 GwyMaskingType mode,
                                 gint col, gint row,
                                 gint width, gint height)
{
    gint i, j;
    gdouble sum = 0;
    const gdouble *datapos, *mpos;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(dfield), sum);
    g_return_val_if_fail(!mask || (GWY_IS_DATA_FIELD(mask)
                                   && mask->xres == dfield->xres
                                   && mask->yres == dfield->yres),
                         sum);
    g_return_val_if_fail(col >= 0 && row >= 0
                         && width >= 0 && height >= 0
                         && col + width <= dfield->xres
                         && row + height <= dfield->yres,
                         sum);

    if (mask && mode != GWY_MASK_IGNORE) {
        datapos = dfield->data + row*dfield->xres + col;
        mpos = mask->data + row*mask->xres + col;
        for (i = 0; i < height; i++) {
            const gdouble *drow = datapos + i*dfield->xres;
            const gdouble *mrow = mpos + i*mask->xres;

            if (mode == GWY_MASK_INCLUDE) {
                for (j = 0; j < width; j++) {
                    if (*mrow > 0.0)
                        sum += *drow;
                    drow++;
                    mrow++;
                }
            }
            else {
                for (j = 0; j < width; j++) {
                    if (*mrow < 1.0)
                        sum += *drow;
                    drow++;
                    mrow++;
                }
            }
        }

        return sum;
    }

    if (col == 0 && width == dfield->xres
        && row == 0 && height == dfield->yres)
        return gwy_data_field_get_sum(dfield);

    datapos = dfield->data + row*dfield->xres + col;
    for (i = 0; i < height; i++) {
        const gdouble *drow = datapos + i*dfield->xres;

        for (j = 0; j < width; j++)
            sum += *(drow++);
    }

    return sum;
}

/**
 * gwy_data_field_get_avg:
 * @data_field: A data field
 *
 * Computes average value of a data field.
 *
 * This quantity is cached.
 *
 * Returns: The average value.
 **/
gdouble
gwy_data_field_get_avg(GwyDataField *data_field)
{
    return gwy_data_field_get_sum(data_field)/((data_field->xres
                                                * data_field->yres));
}
/**
 * gwy_data_field_area_get_avg:
 * @data_field: A data field
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Computes average value of a rectangular part of a data field.
 *
 * This function is equivalent to calling @gwy_data_field_area_get_avg_mask()
 * with masking mode %GWY_MASK_INCLUDE.
 *
 * Returns: The average value.
 **/
gdouble
gwy_data_field_area_get_avg(GwyDataField *dfield,
                            GwyDataField *mask,
                            gint col, gint row,
                            gint width, gint height)
{
    return gwy_data_field_area_get_avg_mask(dfield, mask, GWY_MASK_INCLUDE,
                                            col, row, width, height);
}

/**
 * gwy_data_field_area_get_avg_mask:
 * @data_field: A data field
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @mode: Masking mode to use.  See the introduction for description of
 *        masking modes.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Computes average value of a rectangular part of a data field.
 *
 * Returns: The average value.
 *
 * Since: 2.18
 **/
gdouble
gwy_data_field_area_get_avg_mask(GwyDataField *dfield,
                                 GwyDataField *mask,
                                 GwyMaskingType mode,
                                 gint col, gint row,
                                 gint width, gint height)
{
    const gdouble *datapos, *mpos;
    gdouble sum = 0;
    gint i, j;
    guint nn;

    if (!mask || mode == GWY_MASK_IGNORE) {
        return gwy_data_field_area_get_sum_mask(dfield, NULL, GWY_MASK_IGNORE,
                                                col, row,
                                                width, height)/(width*height);
    }

    g_return_val_if_fail(GWY_IS_DATA_FIELD(dfield), sum);
    g_return_val_if_fail(GWY_IS_DATA_FIELD(mask)
                         && mask->xres == dfield->xres
                         && mask->yres == dfield->yres,
                         sum);
    g_return_val_if_fail(col >= 0 && row >= 0
                         && width >= 0 && height >= 0
                         && col + width <= dfield->xres
                         && row + height <= dfield->yres,
                         sum);

    datapos = dfield->data + row*dfield->xres + col;
    mpos = mask->data + row*mask->xres + col;
    nn = 0;
    for (i = 0; i < height; i++) {
        const gdouble *drow = datapos + i*dfield->xres;
        const gdouble *mrow = mpos + i*mask->xres;

        if (mode == GWY_MASK_INCLUDE) {
            for (j = 0; j < width; j++) {
                if (*mrow > 0.0) {
                    sum += *drow;
                    nn++;
                }
                drow++;
                mrow++;
            }
        }
        else {
            for (j = 0; j < width; j++) {
                if (*mrow < 1.0) {
                    sum += *drow;
                    nn++;
                }
                drow++;
                mrow++;
            }
        }
    }

    return sum/nn;
}

/**
 * gwy_data_field_get_rms:
 * @data_field: A data field.
 *
 * Computes root mean square value of a data field.
 *
 * This quantity is cached.
 *
 * Returns: The root mean square value.
 **/
gdouble
gwy_data_field_get_rms(GwyDataField *data_field)
{
    gint i, n;
    gdouble rms = 0.0, sum, sum2;
    const gdouble *p;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(data_field), rms);

    gwy_debug("%s", CTEST(data_field, RMS) ? "cache" : "lame");
    if (CTEST(data_field, RMS))
        return CVAL(data_field, RMS);

    sum = gwy_data_field_get_sum(data_field);
    sum2 = 0.0;
    p = data_field->data;
    for (i = data_field->xres * data_field->yres; i; i--, p++)
        sum2 += (*p)*(*p);

    n = data_field->xres * data_field->yres;
    rms = sqrt(fabs(sum2 - sum*sum/n)/n);

    CVAL(data_field, RMS) = rms;
    data_field->cached |= CBIT(RMS);

    return rms;
}

/**
 * gwy_data_field_area_get_rms:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Computes root mean square value of a rectangular part of a data field.
 *
 * Returns: The root mean square value.
 *
 * This function is equivalent to calling @gwy_data_field_area_get_rms_mask()
 * with masking mode %GWY_MASK_INCLUDE.
 **/

gdouble
gwy_data_field_area_get_rms(GwyDataField *dfield,
                            GwyDataField *mask,
                            gint col, gint row,
                            gint width, gint height)
{
    return gwy_data_field_area_get_rms_mask(dfield, mask, GWY_MASK_INCLUDE,
                                            col, row, width, height);
}

/**
 * gwy_data_field_area_get_rms_mask:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @mode: Masking mode to use.  See the introduction for description of
 *        masking modes.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Computes root mean square value of deviations of a rectangular part of a
 * data field.
 *
 * Returns: The root mean square value of deviations from the mean value.
 *
 * Since: 2.18
 **/
gdouble
gwy_data_field_area_get_rms_mask(GwyDataField *dfield,
                                 GwyDataField *mask,
                                 GwyMaskingType mode,
                                 gint col, gint row,
                                 gint width, gint height)
{
    gint i, j;
    gdouble rms = 0.0, sum2 = 0.0;
    gdouble sum;
    const gdouble *datapos, *mpos;
    guint nn;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(dfield), rms);
    g_return_val_if_fail(!mask || (GWY_IS_DATA_FIELD(mask)
                                   && mask->xres == dfield->xres
                                   && mask->yres == dfield->yres),
                         rms);
    g_return_val_if_fail(col >= 0 && row >= 0
                         && width >= 0 && height >= 0
                         && col + width <= dfield->xres
                         && row + height <= dfield->yres,
                         rms);
    if (!width || !height)
        return rms;

    if (mask && mode != GWY_MASK_INCLUDE) {
        sum = gwy_data_field_area_get_sum_mask(dfield, mask, mode,
                                               col, row, width, height);
        datapos = dfield->data + row*dfield->xres + col;
        mpos = mask->data + row*mask->xres + col;
        nn = 0;
        for (i = 0; i < height; i++) {
            const gdouble *drow = datapos + i*dfield->xres;
            const gdouble *mrow = mpos + i*mask->xres;

            if (mode == GWY_MASK_INCLUDE) {
                for (j = 0; j < width; j++) {
                    if (*mrow > 0.0) {
                        sum2 += (*drow) * (*drow);
                        nn++;
                    }
                    drow++;
                    mrow++;
                }
            }
            else {
                for (j = 0; j < width; j++) {
                    if (*mrow < 1.0) {
                        sum2 += (*drow) * (*drow);
                        nn++;
                    }
                    drow++;
                    mrow++;
                }
            }
        }
        rms = sqrt(fabs(sum2 - sum*sum/nn)/nn);

        return rms;
    }

    if (col == 0 && width == dfield->xres
        && row == 0 && height == dfield->yres)
        return gwy_data_field_get_rms(dfield);

    sum = gwy_data_field_area_get_sum(dfield, NULL, col, row, width, height);
    datapos = dfield->data + row*dfield->xres + col;
    for (i = 0; i < height; i++) {
        const gdouble *drow = datapos + i*dfield->xres;

        for (j = 0; j < width; j++) {
            sum2 += (*drow) * (*drow);
            drow++;
        }
    }

    nn = width*height;
    rms = sqrt(fabs(sum2 - sum*sum/nn)/nn);

    return rms;
}

/**
 * gwy_data_field_area_get_grainwise_rms:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @mode: Masking mode to use.  See the introduction for description of
 *        masking modes.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Computes grain-wise root mean square value of deviations of a rectangular
 * part of a data field.
 *
 * Grain-wise means that the mean value is determined for each grain (i.e.
 * cotinguous part of the mask or inverted mask) separately and the deviations
 * are calculated from these mean values.
 *
 * Returns: The root mean square value of deviations from the mean value.
 *
 * Since: 2.29
 **/
gdouble
gwy_data_field_area_get_grainwise_rms(GwyDataField *dfield,
                                      GwyDataField *mask,
                                      GwyMaskingType mode,
                                      gint col,
                                      gint row,
                                      gint width,
                                      gint height)
{
    GwyDataField *grainmask;
    gint *grains, *size, *g;
    gint i, j, n;
    gint xres, yres, ngrains;
    gdouble *m;
    const gdouble *datapos;
    gdouble rms = 0.0;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(dfield), rms);
    xres = dfield->xres;
    yres = dfield->yres;
    g_return_val_if_fail(!mask || (GWY_IS_DATA_FIELD(mask)
                                   && mask->xres == xres
                                   && mask->yres == yres),
                         rms);
    g_return_val_if_fail(col >= 0 && row >= 0
                         && width >= 0 && height >= 0
                         && col + width <= xres
                         && row + height <= yres,
                         rms);
    if (!width || !height)
        return rms;

    if (!mask || mode == GWY_MASK_IGNORE)
        return gwy_data_field_area_get_rms_mask(dfield, NULL,
                                                GWY_MASK_IGNORE,
                                                col, row, width, height);

    if (mode == GWY_MASK_INCLUDE) {
        if (col == 0 && row == 0 && width == xres && height == yres)
            grainmask = (GwyDataField*)g_object_ref(mask);
        else
            grainmask = gwy_data_field_area_extract(mask,
                                                    col, row, width, height);
    }
    else {
        grainmask = gwy_data_field_area_extract(mask, col, row, width, height);
        gwy_data_field_grains_invert(grainmask);
    }

    grains = g_new0(gint, width*height);
    ngrains = gwy_data_field_number_grains(grainmask, grains);
    if (!ngrains) {
        g_free(grains);
        g_object_unref(grainmask);
        return rms;
    }

    m = g_new0(gdouble, ngrains+1);
    size = g_new0(gint, ngrains+1);
    datapos = dfield->data + row*xres + col;
    g = grains;
    for (i = 0; i < height; i++) {
        for (j = 0; j < width; j++, g++) {
            m[*g] += datapos[i*xres + j];
            size[*g]++;
        }
    }

    n = 0;
    for (i = 1; i <= ngrains; i++) {
        m[i] /= size[i];
        n += size[i];
    }

    g = grains;
    rms = 0.0;
    for (i = 0; i < height; i++) {
        for (j = 0; j < width; j++, g++) {
            if (*g) {
                gdouble d = datapos[i*xres + j] - m[*g];
                rms += d*d;
            }
        }
    }
    rms = sqrt(rms/n);

    g_free(size);
    g_free(m);
    g_free(grains);
    g_object_unref(grainmask);

    return rms;
}

/**
 * gwy_data_field_get_autorange:
 * @data_field: A data field.
 * @from: Location to store range start.
 * @to: Location to store range end.
 *
 * Computes value range with outliers cut-off.
 *
 * The purpose of this function is to find a range is suitable for false color
 * mapping.  The precise method how it is calculated is unspecified and may be
 * subject to changes.
 *
 * However, it is guaranteed minimum <= @from <= @to <= maximum.
 *
 * This quantity is cached.
 **/
void
gwy_data_field_get_autorange(GwyDataField *data_field,
                             gdouble *from,
                             gdouble *to)
{
    enum { AR_NDH = 512 };
    guint dh[AR_NDH];
    gdouble min, max, rmin, rmax, q;
    gdouble *p;
    guint i, n, j;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));

    gwy_debug("%s", CTEST(data_field, ARF) ? "cache" : "lame");
    if ((!from || CTEST(data_field, ARF))
        && (!to || CTEST(data_field, ART))) {
        if (from)
            *from = CVAL(data_field, ARF);
        if (to)
            *to = CVAL(data_field, ART);
        return;
    }

    gwy_data_field_get_min_max(data_field, &min, &max);
    if (min == max) {
        rmin = min;
        rmax = max;
    }
    else {
        max += 1e-6*(max - min);
        q = AR_NDH/(max - min);

        n = data_field->xres*data_field->yres;
        gwy_clear(dh, AR_NDH);
        for (i = n, p = data_field->data; i; i--, p++) {
            j = (*p - min)*q;
            dh[MIN(j, AR_NDH-1)]++;
        }

        j = 0;
        for (i = j = 0; dh[i] < 5e-2*n/AR_NDH && j < 2e-2*n; i++)
            j += dh[i];
        rmin = min + i/q;

        j = 0;
        for (i = AR_NDH-1, j = 0; dh[i] < 5e-2*n/AR_NDH && j < 2e-2*n; i--)
            j += dh[i];
        rmax = min + (i + 1)/q;
    }

    if (from)
        *from = rmin;
    if (to)
        *to = rmax;

    CVAL(data_field, ARF) = rmin;
    CVAL(data_field, ART) = rmax;
    data_field->cached |= CBIT(ARF) | CBIT(ART);
}

/**
 * gwy_data_field_get_stats:
 * @data_field: A data field.
 * @avg: Where average height value of the surface should be stored, or %NULL.
 * @ra: Where average value of irregularities should be stored, or %NULL.
 * @rms: Where root mean square value of irregularities (Rq) should be stored,
 *       or %NULL.
 * @skew: Where skew (symmetry of height distribution) should be stored, or
 *        %NULL.
 * @kurtosis: Where kurtosis (peakedness of height ditribution) should be
 *            stored, or %NULL.
 *
 * Computes basic statistical quantities of a data field.
 **/
void
gwy_data_field_get_stats(GwyDataField *data_field,
                         gdouble *avg,
                         gdouble *ra,
                         gdouble *rms,
                         gdouble *skew,
                         gdouble *kurtosis)
{
    gint i;
    gdouble c_sz2, c_sz3, c_sz4, c_abs1;
    const gdouble *p = data_field->data;
    guint nn = data_field->xres * data_field->yres;
    gdouble dif, myavg, myrms;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));

    c_sz2 = c_sz3 = c_sz4 = c_abs1 = 0;

    myavg = gwy_data_field_get_avg(data_field);
    if (avg)
        *avg = myavg;

    for (i = nn; i; i--, p++) {
        dif = (*p - myavg);
        c_abs1 += fabs(dif);
        c_sz2 += dif*dif;
        c_sz3 += dif*dif*dif;
        c_sz4 += dif*dif*dif*dif;

    }

    myrms = c_sz2/nn;
    if (ra)
        *ra = c_abs1/nn;
    if (skew)
        *skew = c_sz3/pow(myrms, 1.5)/nn;
    if (kurtosis)
        *kurtosis = c_sz4/(myrms)/(myrms)/nn - 3;
    if (rms)
        *rms = sqrt(myrms);

    if (!CTEST(data_field, RMS)) {
        CVAL(data_field, RMS) = sqrt(myrms);
        data_field->cached |= CBIT(RMS);
    }
}

/**
 * gwy_data_field_area_get_stats:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @avg: Where average height value of the surface should be stored, or %NULL.
 * @ra: Where average value of irregularities should be stored, or %NULL.
 * @rms: Where root mean square value of irregularities (Rq) should be stored,
 *       or %NULL.
 * @skew: Where skew (symmetry of height distribution) should be stored, or
 *        %NULL.
 * @kurtosis: Where kurtosis (peakedness of height ditribution) should be
 *            stored, or %NULL.
 *
 * Computes basic statistical quantities of a rectangular part of a data field.
 *
 * This function is equivalent to calling @gwy_data_field_area_get_stats_mask()
 * with masking mode %GWY_MASK_INCLUDE.
 **/
void
gwy_data_field_area_get_stats(GwyDataField *dfield,
                              GwyDataField *mask,
                              gint col, gint row,
                              gint width, gint height,
                              gdouble *avg,
                              gdouble *ra,
                              gdouble *rms,
                              gdouble *skew,
                              gdouble *kurtosis)
{
    gwy_data_field_area_get_stats_mask(dfield, mask, GWY_MASK_INCLUDE,
                                       col, row, width, height,
                                       avg, ra, rms, skew, kurtosis);
}

/**
 * gwy_data_field_area_get_stats_mask:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @mode: Masking mode to use.  See the introduction for description of
 *        masking modes.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @avg: Where average height value of the surface should be stored, or %NULL.
 * @ra: Where average value of irregularities should be stored, or %NULL.
 * @rms: Where root mean square value of irregularities (Rq) should be stored,
 *       or %NULL.
 * @skew: Where skew (symmetry of height distribution) should be stored, or
 *        %NULL.
 * @kurtosis: Where kurtosis (peakedness of height ditribution) should be
 *            stored, or %NULL.
 *
 * Computes basic statistical quantities of a rectangular part of a data field.
 *
 * Since: 2.18
 **/
void
gwy_data_field_area_get_stats_mask(GwyDataField *dfield,
                                   GwyDataField *mask,
                                   GwyMaskingType mode,
                                   gint col, gint row,
                                   gint width, gint height,
                                   gdouble *avg,
                                   gdouble *ra,
                                   gdouble *rms,
                                   gdouble *skew,
                                   gdouble *kurtosis)
{
    gdouble c_sz2, c_sz3, c_sz4, c_abs1;
    gdouble dif, myavg, myrms;
    const gdouble *datapos, *mpos;
    gint i, j;
    guint nn;

    g_return_if_fail(GWY_IS_DATA_FIELD(dfield));
    g_return_if_fail(!mask || (GWY_IS_DATA_FIELD(mask)
                               && mask->xres == dfield->xres
                               && mask->yres == dfield->yres));
    g_return_if_fail(col >= 0 && row >= 0
                     && width > 0 && height > 0
                     && col + width <= dfield->xres
                     && row + height <= dfield->yres);

    c_sz2 = c_sz3 = c_sz4 = c_abs1 = 0;

    myavg = gwy_data_field_area_get_avg_mask(dfield, mask, mode,
                                             col, row, width, height);
    if (mask && mode != GWY_MASK_IGNORE) {
        datapos = dfield->data + row*dfield->xres + col;
        mpos = mask->data + row*mask->xres + col;
        nn = 0;
        for (i = 0; i < height; i++) {
            const gdouble *drow = datapos + i*dfield->xres;
            const gdouble *mrow = mpos + i*mask->xres;

            if (mode == GWY_MASK_INCLUDE) {
                for (j = 0; j < width; j++) {
                    if (*mrow > 0.0) {
                        dif = *drow - myavg;
                        c_abs1 += fabs(dif);
                        c_sz2 += dif*dif;
                        c_sz3 += dif*dif*dif;
                        c_sz4 += dif*dif*dif*dif;
                        nn++;
                    }
                    drow++;
                    mrow++;
                }
            }
            else {
                for (j = 0; j < width; j++) {
                    if (*mrow < 1.0) {
                        dif = *drow - myavg;
                        c_abs1 += fabs(dif);
                        c_sz2 += dif*dif;
                        c_sz3 += dif*dif*dif;
                        c_sz4 += dif*dif*dif*dif;
                        nn++;
                    }
                    drow++;
                    mrow++;
                }
            }
        }
    }
    else {
        nn = width*height;
        datapos = dfield->data + row*dfield->xres + col;
        for (i = 0; i < height; i++) {
            const gdouble *drow = datapos + i*dfield->xres;

            for (j = 0; j < width; j++) {
                dif = *(drow++) - myavg;
                c_abs1 += fabs(dif);
                c_sz2 += dif*dif;
                c_sz3 += dif*dif*dif;
                c_sz4 += dif*dif*dif*dif;
            }
        }
    }

    myrms = c_sz2/nn;
    if (avg)
        *avg = myavg;
    if (ra)
        *ra = c_abs1/nn;
    if (skew)
        *skew = c_sz3/pow(myrms, 1.5)/nn;
    if (kurtosis)
        *kurtosis = c_sz4/(myrms)/(myrms)/nn - 3;
    if (rms)
        *rms = sqrt(myrms);
}

/**
 * gwy_data_field_area_count_in_range:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @below: Upper bound to compare data to.  The number of samples less
 *         than or equal to @below is stored in @nbelow.
 * @above: Lower bound to compare data to.  The number of samples greater
 *         than or equal to @above is stored in @nabove.
 * @nbelow: Location to store the number of samples less than or equal
 *          to @below, or %NULL.
 * @nabove: Location to store the number of samples greater than or equal
 *          to @above, or %NULL.
 *
 * Counts data samples in given range.
 *
 * No assertion is made about the values of @above and @below, in other words
 * @above may be larger than @below.  To count samples in an open interval
 * instead of a closed interval, exchange @below and @above and then subtract
 * the @nabove and @nbelow from @width*@height to get the complementary counts.
 *
 * With this trick the common task of counting positive values can be
 * realized:
 * <informalexample><programlisting>
 * gwy_data_field_area_count_in_range(data_field, NULL,
 *                                    col, row, width, height,
 *                                    0.0, 0.0, &amp;count, NULL);
 * count = width*height - count;
 * </programlisting></informalexample>
 **/
void
gwy_data_field_area_count_in_range(GwyDataField *data_field,
                                   GwyDataField *mask,
                                   gint col, gint row,
                                   gint width, gint height,
                                   gdouble below,
                                   gdouble above,
                                   gint *nbelow,
                                   gint *nabove)
{
    const gdouble *datapos, *mpos;
    gint i, j, na, nb;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    g_return_if_fail(!mask || (GWY_IS_DATA_FIELD(mask)
                               && mask->xres == data_field->xres
                               && mask->yres == data_field->yres));
    g_return_if_fail(col >= 0 && row >= 0
                     && width > 0 && height > 0
                     && col + width <= data_field->xres
                     && row + height <= data_field->yres);

    if (!nabove && !nbelow)
        return;

    na = nb = 0;
    if (mask) {
        datapos = data_field->data + row*data_field->xres + col;
        mpos = mask->data + row*mask->xres + col;
        for (i = 0; i < height; i++) {
            const gdouble *drow = datapos + i*data_field->xres;
            const gdouble *mrow = mpos + i*mask->xres;

            for (j = 0; j < width; j++) {
                if (*mrow > 0.0) {
                    if (*drow >= above)
                        na++;
                    if (*drow <= below)
                        nb++;
                }
                drow++;
                mrow++;
            }
        }
    }
    else {
        datapos = data_field->data + row*data_field->xres + col;
        for (i = 0; i < height; i++) {
            const gdouble *drow = datapos + i*data_field->xres;

            for (j = 0; j < width; j++) {
                if (*drow >= above)
                    na++;
                if (*drow <= below)
                    nb++;
                drow++;
            }
        }
    }

    if (nabove)
        *nabove = na;
    if (nbelow)
        *nbelow = nb;
}

/**
 * gwy_data_field_area_dh:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, a suitable resolution is determined automatically.
 *
 * Calculates distribution of heights in a rectangular part of data field.
 **/
void
gwy_data_field_area_dh(GwyDataField *data_field,
                       GwyDataField *mask,
                       GwyDataLine *target_line,
                       gint col, gint row,
                       gint width, gint height,
                       gint nstats)
{
    GwySIUnit *fieldunit, *lineunit, *rhounit;
    gdouble min, max;
    const gdouble *drow, *mrow;
    gint i, j, k;
    guint nn;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    g_return_if_fail(!mask || (GWY_IS_DATA_FIELD(mask)
                               && mask->xres == data_field->xres
                               && mask->yres == data_field->yres));
    g_return_if_fail(GWY_IS_DATA_LINE(target_line));
    g_return_if_fail(col >= 0 && row >= 0
                     && width >= 1 && height >= 1
                     && col + width <= data_field->xres
                     && row + height <= data_field->yres);

    if (mask) {
        nn = 0;
        for (i = 0; i < height; i++) {
            mrow = mask->data + (i + row)*mask->xres + col;
            for (j = 0; j < width; j++) {
                if (mrow[j])
                    nn++;
            }
        }
    }
    else
        nn = width*height;

    if (nstats < 1) {
        nstats = floor(3.49*cbrt(nn) + 0.5);
        nstats = MAX(nstats, 2);
    }

    gwy_data_line_resample(target_line, nstats, GWY_INTERPOLATION_NONE);
    gwy_data_line_clear(target_line);
    gwy_data_field_area_get_min_max(data_field, nn ? mask : NULL,
                                    col, row, width, height,
                                    &min, &max);

    /* Set proper units */
    fieldunit = gwy_data_field_get_si_unit_z(data_field);
    lineunit = gwy_data_line_get_si_unit_x(target_line);
    gwy_serializable_clone(G_OBJECT(fieldunit), G_OBJECT(lineunit));
    rhounit = gwy_data_line_get_si_unit_y(target_line);
    gwy_si_unit_power(lineunit, -1, rhounit);

    /* Handle border cases */
    if (min == max) {
        gwy_data_line_set_real(target_line, min ? max : 1.0);
        target_line->data[0] = nstats/gwy_data_line_get_real(target_line);
        return;
    }

    /* Calculate height distribution */
    gwy_data_line_set_real(target_line, max - min);
    gwy_data_line_set_offset(target_line, min);
    if (mask) {
        for (i = 0; i < height; i++) {
            drow = data_field->data + (i + row)*data_field->xres + col;
            mrow = mask->data + (i + row)*mask->xres + col;

            for (j = 0; j < width; j++) {
                if (mrow[j]) {
                    k = (gint)((drow[j] - min)/(max - min)*nstats);
                    /* Fix rounding errors */
                    if (G_UNLIKELY(k >= nstats))
                        k = nstats-1;
                    else if (G_UNLIKELY(k < 0))
                        k = 0;

                    target_line->data[k] += 1;
                }
            }
        }
    }
    else {
        for (i = 0; i < height; i++) {
            drow = data_field->data + (i + row)*data_field->xres + col;

            for (j = 0; j < width; j++) {
                k = (gint)((drow[j] - min)/(max - min)*nstats);
                /* Fix rounding errors */
                if (G_UNLIKELY(k >= nstats))
                    k = nstats-1;
                else if (G_UNLIKELY(k < 0))
                    k = 0;

                target_line->data[k] += 1;
            }
        }
    }

    /* Normalize integral to 1 */
    gwy_data_line_multiply(target_line, nstats/(max - min)/MAX(nn, 1));
}

/**
 * gwy_data_field_dh:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, a suitable resolution is determined automatically.
 *
 * Calculates distribution of heights in a data field.
 **/
void
gwy_data_field_dh(GwyDataField *data_field,
                  GwyDataLine *target_line,
                  gint nstats)
{
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    gwy_data_field_area_dh(data_field, NULL, target_line,
                           0, 0, data_field->xres, data_field->yres,
                           nstats);
}

/**
 * gwy_data_field_area_cdh:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, a suitable resolution is determined automatically.
 *
 * Calculates cumulative distribution of heights in a rectangular part of data
 * field.
 **/
void
gwy_data_field_area_cdh(GwyDataField *data_field,
                        GwyDataField *mask,
                        GwyDataLine *target_line,
                        gint col, gint row,
                        gint width, gint height,
                        gint nstats)
{
    GwySIUnit *rhounit, *lineunit;

    gwy_data_field_area_dh(data_field, mask, target_line,
                           col, row, width, height,
                           nstats);
    gwy_data_line_cumulate(target_line);
    gwy_data_line_multiply(target_line, gwy_data_line_itor(target_line, 1));

    /* Update units after integration */
    lineunit = gwy_data_line_get_si_unit_x(target_line);
    rhounit = gwy_data_line_get_si_unit_y(target_line);
    gwy_si_unit_multiply(rhounit, lineunit, rhounit);
}

/**
 * gwy_data_field_cdh:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, a suitable resolution is determined automatically.
 *
 * Calculates cumulative distribution of heights in a data field.
 **/
void
gwy_data_field_cdh(GwyDataField *data_field,
                   GwyDataLine *target_line,
                   gint nstats)
{
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    gwy_data_field_area_cdh(data_field, NULL, target_line,
                            0, 0, data_field->xres, data_field->yres,
                            nstats);
}

/**
 * gwy_data_field_area_da:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @orientation: Orientation to compute the slope distribution in.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, a suitable resolution is determined automatically.
 *
 * Calculates distribution of slopes in a rectangular part of data field.
 **/
void
gwy_data_field_area_da(GwyDataField *data_field,
                       GwyDataLine *target_line,
                       gint col, gint row,
                       gint width, gint height,
                       GwyOrientation orientation,
                       gint nstats)
{
    GwySIUnit *lineunit, *rhounit;
    GwyDataField *der;
    const gdouble *drow;
    gdouble *derrow;
    gdouble q;
    gint xres, yres, i, j, size;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    xres = data_field->xres;
    yres = data_field->yres;
    size = (orientation == GWY_ORIENTATION_HORIZONTAL) ? width : height;
    g_return_if_fail(col >= 0 && row >= 0
                     && width >= 1 && height >= 1
                     && size >= 2
                     && col + width <= xres
                     && row + height <= yres);

    /* Create a temporary data field from horizontal/vertical derivations
     * and then simply use gwy_data_field_dh().
     * XXX: Should not such a thing exist as a public method? */
    der = gwy_data_field_new(width, height,
                             data_field->xreal*width/xres,
                             data_field->yreal*height/yres,
                             FALSE);

    switch (orientation) {
        case GWY_ORIENTATION_HORIZONTAL:
        q = xres/data_field->xreal;
        /* Instead of testing border columns in each gwy_data_field_get_xder()
         * call, special-case them explicitely */
        for (i = 0; i < height; i++) {
            drow = data_field->data + (i + row)*xres + col;
            derrow = der->data + i*width;

            derrow[0] = drow[1] - drow[0];
            for (j = 1; j < width-1; j++)
                derrow[j] = (drow[j+1] - drow[j-1])/2.0;
            if (width > 1)
                derrow[j] = drow[width-1] - drow[width-2];
        }
        break;

        case GWY_ORIENTATION_VERTICAL:
        q = yres/data_field->yreal;
        /* Instead of testing border rows in each gwy_data_field_get_yder()
         * call, special-case them explicitely */
        drow = data_field->data + row*xres + col;
        derrow = der->data;
        for (j = 0; j < width; j++)
            derrow[j] = *(drow + j+xres) - *(drow + j);

        for (i = 1; i < height-1; i++) {
            drow = data_field->data + (i + row)*xres + col;
            derrow = der->data + i*width;

            for (j = 0; j < width; j++)
                derrow[j] = (*(drow + j+xres) - *(drow + j-xres))/2.0;
        }

        if (height > 1) {
            drow = data_field->data + (row + height-1)*xres + col;
            derrow = der->data + (height-1)*width;
            for (j = 0; j < width; j++)
                derrow[j] = *(drow + j) - *(drow + j-xres);
        }
        break;

        default:
        g_assert_not_reached();
        break;
    }

    gwy_data_field_dh(der, target_line, nstats);
    /* Fix derivation normalization.  At the same time we have to multiply
     * target_line values with inverse factor to keep integral intact */
    gwy_data_line_set_real(target_line, q*gwy_data_line_get_real(target_line));
    gwy_data_line_set_offset(target_line,
                             q*gwy_data_line_get_offset(target_line));
    gwy_data_line_multiply(target_line, 1.0/q);
    g_object_unref(der);

    /* Set proper units */
    lineunit = gwy_data_line_get_si_unit_x(target_line);
    gwy_si_unit_divide(gwy_data_field_get_si_unit_z(data_field),
                       gwy_data_field_get_si_unit_xy(data_field),
                       lineunit);
    rhounit = gwy_data_line_get_si_unit_y(target_line);
    gwy_si_unit_power(lineunit, -1, rhounit);
}

/**
 * gwy_data_field_da:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @orientation: Orientation to compute the slope distribution in.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, a suitable resolution is determined automatically.
 *
 * Calculates distribution of slopes in a data field.
 **/
void
gwy_data_field_da(GwyDataField *data_field,
                  GwyDataLine *target_line,
                  GwyOrientation orientation,
                  gint nstats)
{
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    gwy_data_field_area_da(data_field, target_line,
                           0, 0, data_field->xres, data_field->yres,
                           orientation, nstats);
}

/**
 * gwy_data_field_area_cda:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @orientation: Orientation to compute the slope distribution in.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, a suitable resolution is determined automatically.
 *
 * Calculates cumulative distribution of slopes in a rectangular part of data
 * field.
 **/
void
gwy_data_field_area_cda(GwyDataField *data_field,
                        GwyDataLine *target_line,
                        gint col, gint row,
                        gint width, gint height,
                        GwyOrientation orientation,
                        gint nstats)
{
    GwySIUnit *lineunit, *rhounit;

    gwy_data_field_area_da(data_field, target_line,
                           col, row, width, height,
                           orientation, nstats);
    gwy_data_line_cumulate(target_line);
    gwy_data_line_multiply(target_line, gwy_data_line_itor(target_line, 1));

    /* Update units after integration */
    lineunit = gwy_data_line_get_si_unit_x(target_line);
    rhounit = gwy_data_line_get_si_unit_y(target_line);
    gwy_si_unit_multiply(rhounit, lineunit, rhounit);
}

/**
 * gwy_data_field_cda:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @orientation: Orientation to compute the slope distribution in.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, a suitable resolution is determined automatically.
 *
 * Calculates cumulative distribution of slopes in a data field.
 **/
void
gwy_data_field_cda(GwyDataField *data_field,
                   GwyDataLine *target_line,
                   GwyOrientation orientation,
                   gint nstats)
{
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    gwy_data_field_area_cda(data_field, target_line,
                            0, 0, data_field->xres, data_field->yres,
                            orientation, nstats);
}

#ifdef HAVE_FFTW3
typedef void (*GwyFFTAreaFunc)(fftw_plan plan,
                               GwyDataLine *din,
                               GwyDataLine *dout,
                               GwyDataLine *target_line);

static inline void
do_fft_acf(fftw_plan plan,
           GwyDataLine *din,
           GwyDataLine *dout,
           GwyDataLine *target_line)
{
    gdouble *in, *out;
    gint j, width, res;

    width = target_line->res;
    res = din->res;
    in = din->data;
    out = dout->data;

    gwy_clear(in + width, res - width);

    fftw_execute(plan);
    in[0] = out[0]*out[0];
    for (j = 1; j < (res + 1)/2; j++)
        in[j] = in[res-j] = out[j]*out[j] + out[res-j]*out[res-j];
    if (!(res % 2))
        in[res/2] = out[res/2]*out[res/2];

    fftw_execute(plan);
    for (j = 0; j < width; j++)
        target_line->data[j] += out[j]/(width - j);
}

static inline void
do_fft_hhcf(fftw_plan plan,
            GwyDataLine *din,
            GwyDataLine *dout,
            GwyDataLine *target_line)
{
    gdouble *in, *out;
    gdouble sum;
    gint j, width, res;

    width = target_line->res;
    res = din->res;
    in = din->data;
    out = dout->data;

    sum = 0.0;
    for (j = 0; j < width; j++) {
        sum += in[j]*in[j] + in[width-1-j]*in[width-1-j];
        target_line->data[width-1-j] += sum*res/(j+1);
    }

    gwy_clear(in + width, res - width);

    fftw_execute(plan);
    in[0] = out[0]*out[0];
    for (j = 1; j < (res + 1)/2; j++)
        in[j] = in[res-j] = out[j]*out[j] + out[res-j]*out[res-j];
    if (!(res % 2))
        in[res/2] = out[res/2]*out[res/2];

    fftw_execute(plan);
    for (j = 0; j < width; j++)
        target_line->data[j] -= 2*out[j]/(width - j);
}

static void
gwy_data_field_area_func_fft(GwyDataField *data_field,
                             GwyDataLine *target_line,
                             GwyFFTAreaFunc func,
                             gint col, gint row,
                             gint width, gint height,
                             GwyOrientation orientation,
                             GwyInterpolationType interpolation,
                             gint nstats)
{
    GwyDataLine *din, *dout;
    fftw_plan plan;
    gdouble *in, *out, *drow, *dcol;
    gint i, j, xres, yres, res = 0;
    gdouble avg;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    g_return_if_fail(GWY_IS_DATA_LINE(target_line));
    xres = data_field->xres;
    yres = data_field->yres;
    g_return_if_fail(col >= 0 && row >= 0
                     && width >= 1 && height >= 1
                     && col + width <= xres
                     && row + height <= yres);
    g_return_if_fail(orientation == GWY_ORIENTATION_HORIZONTAL
                     || orientation == GWY_ORIENTATION_VERTICAL);

    switch (orientation) {
        case GWY_ORIENTATION_HORIZONTAL:
        res = gwy_fft_find_nice_size(2*xres);
        gwy_data_line_resample(target_line, width, GWY_INTERPOLATION_NONE);
        break;

        case GWY_ORIENTATION_VERTICAL:
        res = gwy_fft_find_nice_size(2*yres);
        gwy_data_line_resample(target_line, height, GWY_INTERPOLATION_NONE);
        break;
    }
    gwy_data_line_clear(target_line);
    gwy_data_line_set_offset(target_line, 0.0);

    din = gwy_data_line_new(res, 1.0, FALSE);
    dout = gwy_data_line_new(res, 1.0, FALSE);
    in = gwy_data_line_get_data(din);
    out = gwy_data_line_get_data(dout);
    plan = fftw_plan_r2r_1d(res, in, out, FFTW_R2HC, _GWY_FFTW_PATIENCE);
    g_return_if_fail(plan);

    switch (orientation) {
        case GWY_ORIENTATION_HORIZONTAL:
        for (i = 0; i < height; i++) {
            drow = data_field->data + (i + row)*xres + col;
            avg = gwy_data_field_area_get_avg(data_field, NULL,
                                              col, row+i, width, 1);
            for (j = 0; j < width; j++)
                in[j] = drow[j] - avg;
            func(plan, din, dout, target_line);
        }
        gwy_data_line_set_real(target_line,
                               gwy_data_field_jtor(data_field, width));
        gwy_data_line_multiply(target_line, 1.0/(res*height));
        break;

        case GWY_ORIENTATION_VERTICAL:
        for (i = 0; i < width; i++) {
            dcol = data_field->data + row*xres + (i + col);
            avg = gwy_data_field_area_get_avg(data_field, NULL,
                                              col+i, row, 1, height);
            for (j = 0; j < height; j++)
                in[j] = dcol[j*xres] - avg;
            func(plan, din, dout, target_line);
        }
        gwy_data_line_set_real(target_line,
                               gwy_data_field_itor(data_field, height));
        gwy_data_line_multiply(target_line, 1.0/(res*width));
        break;
    }

    fftw_destroy_plan(plan);
    g_object_unref(din);
    g_object_unref(dout);

    if (nstats > 1)
        gwy_data_line_resample(target_line, nstats, interpolation);
}
#else  /* HAVE_FFTW3 */
typedef void (*GwyLameAreaFunc)(GwyDataLine *source,
                                GwyDataLine *target);

static void
gwy_data_field_area_func_lame(GwyDataField *data_field,
                              GwyDataLine *target_line,
                              GwyLameAreaFunc func,
                              gint col, gint row,
                              gint width, gint height,
                              GwyOrientation orientation,
                              GwyInterpolationType interpolation,
                              gint nstats)
{
    GwyDataLine *data_line, *tmp_line;
    gint i, j, xres, yres, size;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    g_return_if_fail(GWY_IS_DATA_LINE(target_line));
    xres = data_field->xres;
    yres = data_field->yres;
    g_return_if_fail(col >= 0 && row >= 0
                     && width >= 1 && height >= 1
                     && col + width <= xres
                     && row + height <= yres);
    g_return_if_fail(orientation == GWY_ORIENTATION_HORIZONTAL
                     || orientation == GWY_ORIENTATION_VERTICAL);

    size = (orientation == GWY_ORIENTATION_HORIZONTAL) ? width : height;
    data_line = gwy_data_line_new(size, 1.0, FALSE);
    tmp_line = gwy_data_line_new(size, 1.0, FALSE);
    gwy_data_line_resample(target_line, size, GWY_INTERPOLATION_NONE);
    gwy_data_line_clear(target_line);
    gwy_data_line_set_offset(target_line, 0.0);

    switch (orientation) {
        case GWY_ORIENTATION_HORIZONTAL:
        for (i = 0; i < height; i++) {
            gwy_data_field_get_row_part(data_field, data_line, row+i,
                                        col, col+width);
            func(data_line, tmp_line);
            for (j = 0; j < width; j++)
                target_line->data[j] += tmp_line->data[j];
        }
        gwy_data_line_set_real(target_line,
                               gwy_data_field_jtor(data_field, width));
        gwy_data_line_multiply(target_line, 1.0/height);
        break;

        case GWY_ORIENTATION_VERTICAL:
        for (i = 0; i < width; i++) {
            gwy_data_field_get_column_part(data_field, data_line, col+i,
                                           row, row+height);
            func(data_line, tmp_line);
            for (j = 0; j < height; j++)
                target_line->data[j] += tmp_line->data[j];
        }
        gwy_data_line_set_real(target_line,
                               gwy_data_field_itor(data_field, height));
        gwy_data_line_multiply(target_line, 1.0/width);
        break;
    }

    g_object_unref(data_line);
    g_object_unref(tmp_line);

    if (nstats > 1)
        gwy_data_line_resample(target_line, nstats, interpolation);
}
#endif  /* HAVE_FFTW3 */

/**
 * gwy_data_field_area_acf:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @orientation: Orientation of lines (ACF is simply averaged over the
 *               other orientation).
 * @interpolation: Interpolation to use when @nstats is given and requires
 *                 resampling.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, @width (@height) is used.
 *
 * Calculates one-dimensional autocorrelation function of a rectangular part of
 * a data field.
 **/
void
gwy_data_field_area_acf(GwyDataField *data_field,
                        GwyDataLine *target_line,
                        gint col, gint row,
                        gint width, gint height,
                        GwyOrientation orientation,
                        GwyInterpolationType interpolation,
                        gint nstats)
{
    GwySIUnit *fieldunit, *lineunit;

#ifdef HAVE_FFTW3
    gwy_data_field_area_func_fft(data_field, target_line,
                                 &do_fft_acf,
                                 col, row, width, height,
                                 orientation, interpolation, nstats);
#else
    gwy_data_field_area_func_lame(data_field, target_line,
                                  &gwy_data_line_acf,
                                  col, row, width, height,
                                  orientation, interpolation, nstats);
#endif  /* HAVE_FFTW3 */

    /* Set proper units */
    fieldunit = gwy_data_field_get_si_unit_xy(data_field);
    lineunit = gwy_data_line_get_si_unit_x(target_line);
    gwy_serializable_clone(G_OBJECT(fieldunit), G_OBJECT(lineunit));
    lineunit = gwy_data_line_get_si_unit_y(target_line);
    gwy_si_unit_power(gwy_data_field_get_si_unit_z(data_field), 2, lineunit);
}

/**
 * gwy_data_field_acf:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @orientation: Orientation of lines (ACF is simply averaged over the
 *               other orientation).
 * @interpolation: Interpolation to use when @nstats is given and requires
 *                 resampling.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, data field width (height) is used.
 *
 * Calculates one-dimensional autocorrelation function of a data field.
 **/
void
gwy_data_field_acf(GwyDataField *data_field,
                   GwyDataLine *target_line,
                   GwyOrientation orientation,
                   GwyInterpolationType interpolation,
                   gint nstats)
{
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    gwy_data_field_area_acf(data_field, target_line,
                            0, 0, data_field->xres, data_field->yres,
                            orientation, interpolation, nstats);
}

/**
 * gwy_data_field_area_hhcf:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @orientation: Orientation of lines (HHCF is simply averaged over the
 *               other orientation).
 * @interpolation: Interpolation to use when @nstats is given and requires
 *                 resampling.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, @width (@height) is used.
 *
 * Calculates one-dimensional autocorrelation function of a rectangular part of
 * a data field.
 **/
void
gwy_data_field_area_hhcf(GwyDataField *data_field,
                         GwyDataLine *target_line,
                         gint col, gint row,
                         gint width, gint height,
                         GwyOrientation orientation,
                         GwyInterpolationType interpolation,
                         gint nstats)
{
    GwySIUnit *fieldunit, *lineunit;

#ifdef HAVE_FFTW3
    gwy_data_field_area_func_fft(data_field, target_line, &do_fft_hhcf,
                                 col, row, width, height,
                                 orientation, interpolation, nstats);
#else
    gwy_data_field_area_func_lame(data_field, target_line,
                                  &gwy_data_line_hhcf,
                                  col, row, width, height,
                                  orientation, interpolation, nstats);
#endif  /* HAVE_FFTW3 */

    /* Set proper units */
    fieldunit = gwy_data_field_get_si_unit_xy(data_field);
    lineunit = gwy_data_line_get_si_unit_x(target_line);
    gwy_serializable_clone(G_OBJECT(fieldunit), G_OBJECT(lineunit));
    lineunit = gwy_data_line_get_si_unit_y(target_line);
    gwy_si_unit_power(gwy_data_field_get_si_unit_z(data_field), 2, lineunit);
}

/**
 * gwy_data_field_hhcf:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @orientation: Orientation of lines (HHCF is simply averaged over the
 *               other orientation).
 * @interpolation: Interpolation to use when @nstats is given and requires
 *                 resampling.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, data field width (height) is used.
 *
 * Calculates one-dimensional autocorrelation function of a data field.
 **/
void
gwy_data_field_hhcf(GwyDataField *data_field,
                    GwyDataLine *target_line,
                    GwyOrientation orientation,
                    GwyInterpolationType interpolation,
                    gint nstats)
{
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    gwy_data_field_area_hhcf(data_field, target_line,
                             0, 0, data_field->xres, data_field->yres,
                             orientation, interpolation, nstats);
}

/**
 * gwy_data_field_area_psdf:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @orientation: Orientation of lines (PSDF is simply averaged over the
 *               other orientation).
 * @interpolation: Interpolation to use when @nstats is given and requires
 *                 resampling.
 * @windowing: Windowing type to use.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, data field width (height) is used.
 *
 * Calculates one-dimensional power spectrum density function of a rectangular
 * part of a data field.
 **/
void
gwy_data_field_area_psdf(GwyDataField *data_field,
                         GwyDataLine *target_line,
                         gint col, gint row,
                         gint width, gint height,
                         GwyOrientation orientation,
                         GwyInterpolationType interpolation,
                         GwyWindowingType windowing,
                         gint nstats)
{
    GwyDataField *re_field, *im_field;
    GwySIUnit *xyunit, *zunit, *lineunit;
    gdouble *re, *im, *target;
    gint i, j, xres, yres, size;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    g_return_if_fail(GWY_IS_DATA_LINE(target_line));
    xres = data_field->xres;
    yres = data_field->yres;
    size = (orientation == GWY_ORIENTATION_HORIZONTAL) ? width : height;
    g_return_if_fail(col >= 0 && row >= 0
                     && width >= 1 && height >= 1
                     && size >= 4
                     && col + width <= xres
                     && row + height <= yres);
    g_return_if_fail(orientation == GWY_ORIENTATION_HORIZONTAL
                     || orientation == GWY_ORIENTATION_VERTICAL);

    if (nstats < 1)
        nstats = size/2 - 1;
    gwy_data_line_resample(target_line, size/2, GWY_INTERPOLATION_NONE);
    gwy_data_line_clear(target_line);
    gwy_data_line_set_offset(target_line, 0.0);

    re_field = gwy_data_field_new(width, height, 1.0, 1.0, FALSE);
    im_field = gwy_data_field_new(width, height, 1.0, 1.0, FALSE);
    target = target_line->data;
    switch (orientation) {
        case GWY_ORIENTATION_HORIZONTAL:
        gwy_data_field_area_1dfft(data_field, NULL, re_field, im_field,
                                  col, row, width, height,
                                  orientation,
                                  windowing,
                                  GWY_TRANSFORM_DIRECTION_FORWARD,
                                  interpolation,
                                  TRUE, 2);
        re = re_field->data;
        im = im_field->data;
        for (i = 0; i < height; i++) {
            for (j = 0; j < size/2; j++)
                target[j] += re[i*width + j]*re[i*width + j]
                             + im[i*width + j]*im[i*width + j];
        }
        gwy_data_line_multiply(target_line,
                               data_field->xreal/xres/(2*G_PI*height));
        gwy_data_line_set_real(target_line, G_PI*xres/data_field->xreal);
        break;

        case GWY_ORIENTATION_VERTICAL:
        gwy_data_field_area_1dfft(data_field, NULL, re_field, im_field,
                                  col, row, width, height,
                                  orientation,
                                  windowing,
                                  GWY_TRANSFORM_DIRECTION_FORWARD,
                                  interpolation,
                                  TRUE, 2);
        re = re_field->data;
        im = im_field->data;
        for (i = 0; i < width; i++) {
            for (j = 0; j < size/2; j++)
                target[j] += re[j*width + i]*re[j*width + i]
                             + im[j*width + i]*im[j*width + i];
        }
        gwy_data_line_multiply(target_line,
                               data_field->yreal/yres/(2*G_PI*width));
        gwy_data_line_set_real(target_line, G_PI*yres/data_field->yreal);
        break;
    }

    gwy_data_line_set_offset(target_line,
                             target_line->real/target_line->res);
    gwy_data_line_resize(target_line, 1, target_line->res);
    gwy_data_line_resample(target_line, nstats, interpolation);

    g_object_unref(re_field);
    g_object_unref(im_field);

    /* Set proper units */
    xyunit = gwy_data_field_get_si_unit_xy(data_field);
    zunit = gwy_data_field_get_si_unit_z(data_field);
    lineunit = gwy_data_line_get_si_unit_x(target_line);
    gwy_si_unit_power(xyunit, -1, lineunit);
    lineunit = gwy_data_line_get_si_unit_y(target_line);
    gwy_si_unit_power(zunit, 2, lineunit);
    gwy_si_unit_multiply(lineunit, xyunit, lineunit);
}

/**
 * gwy_data_field_psdf:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @orientation: Orientation of lines (PSDF is simply averaged over the
 *               other orientation).
 * @interpolation: Interpolation to use when @nstats is given and requires
 *                 resampling.
 * @windowing: Windowing type to use.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, data field width (height) is used.
 *
 * Calculates one-dimensional power spectrum density function of a data field.
 **/
void
gwy_data_field_psdf(GwyDataField *data_field,
                    GwyDataLine *target_line,
                    GwyOrientation orientation,
                    GwyInterpolationType interpolation,
                    GwyWindowingType windowing,
                    gint nstats)
{
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    gwy_data_field_area_psdf(data_field, target_line,
                             0, 0, data_field->xres, data_field->yres,
                             orientation, interpolation, windowing, nstats);
}

/**
 * gwy_data_field_area_rpsdf:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @interpolation: Interpolation to use when @nstats is given and requires
 *                 resampling.
 * @windowing: Windowing type to use.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, data field width (height) is used.
 *
 * Calculates radial power spectrum density function of a rectangular
 * part of a data field.
 *
 * Since: 2.7
 **/
void
gwy_data_field_area_rpsdf(GwyDataField *data_field,
                          GwyDataLine *target_line,
                          gint col, gint row,
                          gint width, gint height,
                          GwyInterpolationType interpolation,
                          GwyWindowingType windowing,
                          gint nstats)
{
    GwyDataField *re_field, *im_field;
    GwySIUnit *xyunit, *zunit, *lineunit;
    gdouble *re, *im;
    gint i, j, k, xres, yres;
    gdouble xreal, yreal, r;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    g_return_if_fail(GWY_IS_DATA_LINE(target_line));
    xres = data_field->xres;
    yres = data_field->yres;
    g_return_if_fail(col >= 0 && row >= 0
                     && width >= 4 && height >= 4
                     && col + width <= xres
                     && row + height <= yres);
    xreal = data_field->xreal;
    yreal = data_field->yreal;

    re_field = gwy_data_field_new(width, height,
                                  width*xreal/xres, height*yreal/yres,
                                  FALSE);
    im_field = gwy_data_field_new_alike(re_field, FALSE);
    gwy_data_field_area_2dfft(data_field, NULL, re_field, im_field,
                              col, row, width, height,
                              windowing,
                              GWY_TRANSFORM_DIRECTION_FORWARD,
                              interpolation,
                              TRUE, 2);
    re = re_field->data;
    im = im_field->data;
    for (i = 0; i < height; i++) {
        for (j = 0; j < width; j++) {
            k = i*width + j;
            re[k] = re[k]*re[k] + im[k]*im[k];
        }
    }
    g_object_unref(im_field);

    gwy_data_field_fft_postprocess(re_field, TRUE);
    r = 0.5*MAX(re_field->xreal, re_field->yreal);
    gwy_data_field_angular_average(re_field, target_line,
                                   NULL, GWY_MASK_IGNORE,
                                   0.0, 0.0, r, nstats ? nstats+1 : 0);
    g_object_unref(re_field);
    /* Get rid of the zero first element which is bad for logscale. */
    nstats = target_line->res-1;
    gwy_data_line_resize(target_line, 1, nstats+1);
    target_line->off += target_line->real/nstats;

    /* Postprocess does not use angular coordinates, fix that. */
    target_line->real *= 2.0*G_PI;
    target_line->off *= 2.0*G_PI;
    r = xreal*yreal/(2.0*G_PI*width*height) * target_line->real/nstats;
    for (k = 0; k < nstats; k++)
        target_line->data[k] *= r*(k + 1);

    /* Set proper value units */
    xyunit = gwy_data_field_get_si_unit_xy(data_field);
    zunit = gwy_data_field_get_si_unit_z(data_field);
    lineunit = gwy_data_line_get_si_unit_x(target_line);
    gwy_si_unit_power(xyunit, -1, lineunit);
    lineunit = gwy_data_line_get_si_unit_y(target_line);
    gwy_si_unit_power(zunit, 2, lineunit);
    gwy_si_unit_multiply(lineunit, xyunit, lineunit);
}

/**
 * gwy_data_field_rpsdf:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @interpolation: Interpolation to use when @nstats is given and requires
 *                 resampling.
 * @windowing: Windowing type to use.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, data field width (height) is used.
 *
 * Calculates radial power spectrum density function of a data field.
 *
 * Since: 2.7
 **/
void
gwy_data_field_rpsdf(GwyDataField *data_field,
                     GwyDataLine *target_line,
                     GwyInterpolationType interpolation,
                     GwyWindowingType windowing,
                     gint nstats)
{
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    gwy_data_field_area_rpsdf(data_field, target_line,
                              0, 0, data_field->xres, data_field->yres,
                              interpolation, windowing, nstats);
}

/**
 * gwy_data_field_area_racf:
 * @data_field: A data field.
 * @target_line: A data line to store the autocorrelation function to.  It
 *               will be resampled to requested width.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @nstats: The number of samples to take on the autocorrelation function.  If
 *          nonpositive, a suitable resolution is chosen automatically.
 *
 * Calculates radially averaged autocorrelation function of a rectangular part
 * of a data field.
 *
 * Since: 2.22
 **/
void
gwy_data_field_area_racf(GwyDataField *data_field,
                         GwyDataLine *target_line,
                         gint col, gint row,
                         gint width, gint height,
                         gint nstats)
{
    GwyDataField *acf_field;
    gint xres, yres, size;
    gdouble r;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    g_return_if_fail(GWY_IS_DATA_LINE(target_line));
    xres = data_field->xres;
    yres = data_field->yres;
    g_return_if_fail(col >= 0 && row >= 0
                     && width >= 4 && height >= 4
                     && col + width <= xres
                     && row + height <= yres);

    size = MIN(width, height)/2;
    if (nstats < 1)
        nstats = size;

    acf_field = gwy_data_field_new(2*size - 1, 2*size - 1, 1.0, 1.0, FALSE);
    gwy_data_field_area_2dacf(data_field, acf_field,
                              col, row, width, height, size, size);
    r = 0.5*MAX(acf_field->xreal, acf_field->yreal);
    gwy_data_field_angular_average(acf_field, target_line,
                                   NULL, GWY_MASK_IGNORE,
                                   0.0, 0.0, r, nstats);
    g_object_unref(acf_field);
}

/**
 * gwy_data_field_racf:
 * @data_field: A data field.
 * @target_line: A data line to store the autocorrelation function to.  It
 *               will be resampled to requested width.
 * @nstats: The number of samples to take on the autocorrelation function.  If
 *          nonpositive, a suitable resolution is chosen automatically.
 *
 * Calculates radially averaged autocorrelation function of a data field.
 *
 * Since: 2.22
 **/
void
gwy_data_field_racf(GwyDataField *data_field,
                    GwyDataLine *target_line,
                    gint nstats)
{
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    gwy_data_field_area_racf(data_field, target_line,
                             0, 0, data_field->xres, data_field->yres,
                             nstats);
}

/**
 * gwy_data_field_area_2dacf:
 * @data_field: A data field.
 * @target_field: A data field to store the result to.  It will be resampled
 *                to (2@xrange-1)x(2@yrange-1).
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @xrange: Horizontal correlation range.  Non-positive value means
 *          the default range of half of @data_field width will be used.
 * @yrange: Vertical correlation range.  Non-positive value means
 *          the default range of half of @data_field height will be used.
 *
 * Calculates two-dimensional autocorrelation function of a data field area.
 *
 * The resulting data field has the correlation corresponding to (0,0) in the
 * centre.
 *
 * The maximum possible values of @xrange and @yrange are @data_field
 * width and height, respectively.  However, as the values for longer
 * distances are calculated from smaller number of data points they become
 * increasingly bogus, therefore the default range is half of the size.
 *
 * Since: 2.7
 **/
void
gwy_data_field_area_2dacf(GwyDataField *data_field,
                          GwyDataField *target_field,
                          gint col, gint row,
                          gint width, gint height,
                          gint xrange, gint yrange)
{
#ifdef HAVE_FFTW3
    fftw_plan plan;
#endif
    GwyDataField *re_in, *re_out, *im_out, *ibuf;
    GwySIUnit *xyunit, *zunit, *unit;
    gdouble *src, *dst, *dstm;
    gint i, j, xres, yres, xsize, ysize;
    gdouble xreal, yreal, v, q;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    g_return_if_fail(GWY_IS_DATA_FIELD(target_field));
    xres = data_field->xres;
    yres = data_field->yres;
    g_return_if_fail(col >= 0 && row >= 0
                     && width >= 4 && height >= 4
                     && col + width <= xres
                     && row + height <= yres);
    if (xrange <= 0)
        xrange = width/2;
    if (yrange <= 0)
        yrange = height/2;
    g_return_if_fail(xrange <= width && yrange <= height);
    xreal = data_field->xreal;
    yreal = data_field->yreal;

    xsize = gwy_fft_find_nice_size(width + xrange);
    ysize = gwy_fft_find_nice_size(height + yrange);

    re_in = gwy_data_field_new(xsize, height, 1.0, 1.0, TRUE);
    re_out = gwy_data_field_new_alike(re_in, FALSE);
    im_out = gwy_data_field_new_alike(re_in, FALSE);
    ibuf = gwy_data_field_new_alike(re_in, FALSE);

    /* Stage 1: Row-wise FFT, with zero-padded columns.
     * No need to transform the padding rows as zero rises from zeroes. */
    gwy_data_field_area_copy(data_field, re_in, col, row, width, height, 0, 0);
    gwy_data_field_1dfft_raw(re_in, NULL, re_out, im_out,
                             GWY_ORIENTATION_HORIZONTAL,
                             GWY_TRANSFORM_DIRECTION_FORWARD);

    /* Stage 2: Column-wise FFT, taking the norm and another column-wise FTT.
     * We take the advantage of the fact that the order of the row- and
     * column-wise transforms is arbitrary and that taking the norm is a
     * local operation. */
    /* Use interleaved arrays, this enables us to foist them as `complex'
     * to FFTW. */
    src = g_new(gdouble, 4*ysize);
    dst = src + 2*ysize;
#ifdef HAVE_FFTW3
    q = sqrt(xsize)/ysize;
    plan = fftw_plan_dft_1d(ysize, (fftw_complex*)src, (fftw_complex*)dst,
                            FFTW_FORWARD, _GWY_FFTW_PATIENCE);
#else
    q = sqrt(xsize*ysize);
#endif
    for (j = 0; j < xsize; j++) {
        for (i = 0; i < height; i++) {
            src[2*i + 0] = re_out->data[i*xsize + j];
            src[2*i + 1] = im_out->data[i*xsize + j];
        }
        gwy_clear(src + 2*height, 2*(ysize - height));
#ifdef HAVE_FFTW3
        fftw_execute(plan);
#else
        gwy_fft_simple(GWY_TRANSFORM_DIRECTION_FORWARD, ysize,
                       2, src, src + 1,
                       2, dst, dst + 1);
#endif
        for (i = 0; i < ysize; i++) {
            src[2*i] = dst[2*i]*dst[2*i] + dst[2*i + 1]*dst[2*i + 1];
            src[2*i + 1] = 0.0;
        }
#ifdef HAVE_FFTW3
        fftw_execute(plan);
#else
        gwy_fft_simple(GWY_TRANSFORM_DIRECTION_FORWARD, ysize,
                       2, src, src + 1,
                       2, dst, dst + 1);
#endif
        for (i = 0; i < height; i++) {
            re_in->data[i*xsize + j] = dst[2*i + 0];
            ibuf->data[i*xsize + j]  = dst[2*i + 1];
        }
    }
#ifdef HAVE_FFTW3
    fftw_destroy_plan(plan);
#endif
    g_free(src);

    /* Stage 3: The final row-wise FFT. */
    gwy_data_field_1dfft_raw(re_in, ibuf, re_out, im_out,
                             GWY_ORIENTATION_HORIZONTAL,
                             GWY_TRANSFORM_DIRECTION_FORWARD);

    g_object_unref(ibuf);
    g_object_unref(re_in);
    g_object_unref(im_out);

    gwy_data_field_resample(target_field, 2*xrange - 1, 2*yrange - 1,
                            GWY_INTERPOLATION_NONE);
    /* Extract the correlation data and reshuflle it to human-undestandable
     * positions with 0.0 at the centre. */
    for (i = 0; i < yrange; i++) {
        src = re_out->data + i*xsize;
        dst = target_field->data + (yrange-1 + i)*target_field->xres;
        dstm = target_field->data + (yrange-1 - i)*target_field->xres;
        for (j = 0; j < xrange; j++) {
            if (j > 0) {
                v = q*src[xsize - j]/(height - i)/(width - j);
                if (i > 0)
                    dstm[xrange-1 + j] = v;
                dst[xrange-1 - j] = v;
            }
            v = q*src[j]/(height - i)/(width - j);
            if (i > 0)
                dstm[xrange-1 - j] = v;
            dst[xrange-1 + j] = v;
        }
    }
    g_object_unref(re_out);

    target_field->xreal = xreal*target_field->xres/xres;
    target_field->yreal = yreal*target_field->yres/yres;
    target_field->xoff = -0.5*target_field->xreal;
    target_field->yoff = -0.5*target_field->yreal;

    xyunit = gwy_data_field_get_si_unit_xy(data_field);
    zunit = gwy_data_field_get_si_unit_z(data_field);
    unit = gwy_data_field_get_si_unit_xy(target_field);
    gwy_serializable_clone(G_OBJECT(xyunit), G_OBJECT(unit));
    unit = gwy_data_field_get_si_unit_z(target_field);
    gwy_si_unit_power(zunit, 2, unit);

    gwy_data_field_invalidate(target_field);
}

/**
 * gwy_data_field_2dacf:
 * @data_field: A data field.
 * @target_field: A data field to store the result to.
 *
 * Calculates two-dimensional autocorrelation function of a data field.
 *
 * See gwy_data_field_area_2dacf() for details.  Parameters missing (not
 * adjustable) in this function are set to their default values.
 *
 * Since: 2.7
 **/
void
gwy_data_field_2dacf(GwyDataField *data_field,
                     GwyDataField *target_field)
{
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));

    gwy_data_field_area_2dacf(data_field, target_field,
                              0, 0, data_field->xres, data_field->yres, 0, 0);
}

/**
 * gwy_data_field_area_minkowski_volume:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, a suitable resolution is determined automatically.
 *
 * Calculates Minkowski volume functional of a rectangular part of a data
 * field.
 *
 * Volume functional is calculated as the number of values above each
 * threshold value (,white pixels`) divided by the total number of samples
 * in the area.  Is it's equivalent to 1-CDH.
 **/
void
gwy_data_field_area_minkowski_volume(GwyDataField *data_field,
                                     GwyDataLine *target_line,
                                     gint col, gint row,
                                     gint width, gint height,
                                     gint nstats)
{
    gwy_data_field_area_cdh(data_field, NULL, target_line,
                            col, row, width, height,
                            nstats);
    gwy_data_line_multiply(target_line, -1.0);
    gwy_data_line_add(target_line, 1.0);
}

/**
 * gwy_data_field_minkowski_volume:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, a suitable resolution is determined automatically.
 *
 * Calculates Minkowski volume functional of a data field.
 *
 * See gwy_data_field_area_minkowski_volume() for details.
 **/
void
gwy_data_field_minkowski_volume(GwyDataField *data_field,
                                GwyDataLine *target_line,
                                gint nstats)
{
    gwy_data_field_cdh(data_field, target_line, nstats);
    gwy_data_line_multiply(target_line, -1.0);
    gwy_data_line_add(target_line, 1.0);
}

/**
 * gwy_data_field_area_minkowski_boundary:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, a suitable resolution is determined automatically.
 *
 * Calculates Minkowski boundary functional of a rectangular part of a data
 * field.
 *
 * Boundary functional is calculated as the number of boundaries for each
 * threshold value (the number of pixel sides where of neighouring pixels is
 * ,white` and the other ,black`) divided by the total number of samples
 * in the area.
 **/
void
gwy_data_field_area_minkowski_boundary(GwyDataField *data_field,
                                       GwyDataLine *target_line,
                                       gint col, gint row,
                                       gint width, gint height,
                                       gint nstats)
{
    GwySIUnit *fieldunit, *lineunit;
    const gdouble *data;
    gdouble *line;
    gdouble min, max, q;
    gint xres, i, j, k, k0, kr, kd;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    g_return_if_fail(GWY_IS_DATA_LINE(target_line));
    g_return_if_fail(col >= 0 && row >= 0
                     && width >= 0 && height >= 0
                     && col + width <= data_field->xres
                     && row + height <= data_field->yres);

    if (nstats < 1) {
        nstats = floor(3.49*cbrt(width*height) + 0.5);
        nstats = MAX(nstats, 2);
    }

    gwy_data_line_resample(target_line, nstats, GWY_INTERPOLATION_NONE);
    gwy_data_line_clear(target_line);
    gwy_data_field_area_get_min_max(data_field, NULL,
                                    col, row, width, height,
                                    &min, &max);
    /* There are no boundaries on a totally flat sufrace */
    if (min == max || width == 0 || height == 0)
        return;

    xres = data_field->xres;
    q = nstats/(max - min);
    line = target_line->data;

    for (i = 0; i < height-1; i++) {
        kr = (gint)((data_field->data[i*xres + col] - min)*q);
        for (j = 0; j < width-1; j++) {
            data = data_field->data + (i + row)*xres + (col + j);

            k0 = kr;

            kr = (gint)((data[1] - min)*q);
            for (k = MAX(MIN(k0, kr), 0); k < MIN(MAX(k0, kr), nstats); k++)
                line[k] += 1;

            kd = (gint)((data[xres] - min)*q);
            for (k = MAX(MIN(k0, kd), 0); k < MIN(MAX(k0, kd), nstats); k++)
                line[k] += 1;
        }
    }

    gwy_data_line_multiply(target_line, 1.0/(width*height));
    gwy_data_line_set_real(target_line, max - min);
    gwy_data_line_set_offset(target_line, min);

    /* Set proper units */
    fieldunit = gwy_data_field_get_si_unit_z(data_field);
    lineunit = gwy_data_line_get_si_unit_x(target_line);
    gwy_serializable_clone(G_OBJECT(fieldunit), G_OBJECT(lineunit));
    lineunit = gwy_data_line_get_si_unit_y(target_line);
    gwy_si_unit_set_from_string(lineunit, NULL);
}

/**
 * gwy_data_field_minkowski_boundary:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, a suitable resolution is determined automatically.
 *
 * Calculates Minkowski boundary functional of a data field.
 *
 * See gwy_data_field_area_minkowski_boundary() for details.
 **/
void
gwy_data_field_minkowski_boundary(GwyDataField *data_field,
                                  GwyDataLine *target_line,
                                  gint nstats)
{
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    gwy_data_field_area_minkowski_boundary(data_field, target_line,
                                           0, 0,
                                           data_field->xres, data_field->yres,
                                           nstats);
}

/**
 * gwy_data_field_area_minkowski_euler:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, a suitable resolution is determined automatically.
 *
 * Calculates Minkowski connectivity functional (Euler characteristics) of
 * a rectangular part of a data field.
 *
 * Connectivity functional is calculated as the number connected areas of
 * pixels above threhsold (,white`) minus the number of connected areas of
 * pixels below threhsold (,black`) for each threshold value, divided by the
 * total number of samples in the area.
 **/
void
gwy_data_field_area_minkowski_euler(GwyDataField *data_field,
                                    GwyDataLine *target_line,
                                    gint col, gint row,
                                    gint width, gint height,
                                    gint nstats)
{
    GwySIUnit *fieldunit, *lineunit;
    GwyDataLine *tmp_line;
    gint i;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    g_return_if_fail(GWY_IS_DATA_LINE(target_line));
    g_return_if_fail(col >= 0 && row >= 0
                     && width >= 0 && height >= 0
                     && col + width <= data_field->xres
                     && row + height <= data_field->yres);

    if (nstats < 1) {
        nstats = floor(3.49*cbrt(width*height) + 0.5);
        nstats = MAX(nstats, 2);
    }

    gwy_data_line_resample(target_line, nstats, GWY_INTERPOLATION_NONE);
    tmp_line = gwy_data_line_new_alike(target_line, FALSE);

    gwy_data_field_area_grains_tgnd(data_field, target_line,
                                    col, row, width, height,
                                    FALSE, nstats);
    gwy_data_field_area_grains_tgnd(data_field, tmp_line,
                                    col, row, width, height,
                                    TRUE, nstats);

    for (i = 0; i < nstats; i++)
        target_line->data[i] -= tmp_line->data[nstats-1 - i];
    g_object_unref(tmp_line);

    gwy_data_line_multiply(target_line, 1.0/(width*height));
    gwy_data_line_invert(target_line, TRUE, FALSE);

    /* Set proper units */
    fieldunit = gwy_data_field_get_si_unit_z(data_field);
    lineunit = gwy_data_line_get_si_unit_x(target_line);
    gwy_serializable_clone(G_OBJECT(fieldunit), G_OBJECT(lineunit));
    lineunit = gwy_data_line_get_si_unit_y(target_line);
    gwy_si_unit_set_from_string(lineunit, NULL);
}

/**
 * gwy_data_field_minkowski_euler:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to requested width.
 * @nstats: The number of samples to take on the distribution function.  If
 *          nonpositive, a suitable resolution is determined automatically.
 *
 * Calculates Minkowski connectivity functional (Euler characteristics) of
 * a data field.
 *
 * See gwy_data_field_area_minkowski_euler() for details.
 **/
void
gwy_data_field_minkowski_euler(GwyDataField *data_field,
                               GwyDataLine *target_line,
                               gint nstats)
{
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    gwy_data_field_area_minkowski_euler(data_field, target_line,
                                        0, 0,
                                        data_field->xres, data_field->yres,
                                        nstats);
}

/**
 * square_area1:
 * @z1: Z-value in first corner.
 * @z2: Z-value in second corner.
 * @z3: Z-value in third corner.
 * @z4: Z-value in fourth corner.
 * @q: One fourth of rectangle projected area (x-size * ysize).
 *
 * Calculates approximate area of a one square pixel.
 *
 * Returns: The area.
 **/
static inline gdouble
square_area1(gdouble z1, gdouble z2, gdouble z3, gdouble z4,
             gdouble q)
{
    gdouble c;

    c = (z1 + z2 + z3 + z4)/4.0;
    z1 -= c;
    z2 -= c;
    z3 -= c;
    z4 -= c;

    return (sqrt(1.0 + 2.0*(z1*z1 + z2*z2)/q)
            + sqrt(1.0 + 2.0*(z2*z2 + z3*z3)/q)
            + sqrt(1.0 + 2.0*(z3*z3 + z4*z4)/q)
            + sqrt(1.0 + 2.0*(z4*z4 + z1*z1)/q));
}

/**
 * square_area1w:
 * @z1: Z-value in first corner.
 * @z2: Z-value in second corner.
 * @z3: Z-value in third corner.
 * @z4: Z-value in fourth corner.
 * @w1: Weight of first corner (0 or 1).
 * @w2: Weight of second corner (0 or 1).
 * @w3: Weight of third corner (0 or 1).
 * @w4: Weight of fourth corner (0 or 1).
 * @q: One fourth of rectangle projected area (x-size * ysize).
 *
 * Calculates approximate area of a one square pixel with some corners possibly
 * missing.
 *
 * Returns: The area.
 **/
static inline gdouble
square_area1w(gdouble z1, gdouble z2, gdouble z3, gdouble z4,
              gint w1, gint w2, gint w3, gint w4,
              gdouble q)
{
    gdouble c;

    c = (z1 + z2 + z3 + z4)/4.0;
    z1 -= c;
    z2 -= c;
    z3 -= c;
    z4 -= c;

    return ((w1 + w2)*sqrt(1.0 + 2.0*(z1*z1 + z2*z2)/q)
            + (w2 + w3)*sqrt(1.0 + 2.0*(z2*z2 + z3*z3)/q)
            + (w3 + w4)*sqrt(1.0 + 2.0*(z3*z3 + z4*z4)/q)
            + (w4 + w1)*sqrt(1.0 + 2.0*(z4*z4 + z1*z1)/q))/2.0;
}

/**
 * square_area2:
 * @z1: Z-value in first corner.
 * @z2: Z-value in second corner.
 * @z3: Z-value in third corner.
 * @z4: Z-value in fourth corner.
 * @x: One fourth of square of rectangle width (x-size).
 * @y: One fourth of square of rectangle height (y-size).
 *
 * Calculates approximate area of a one general rectangular pixel.
 *
 * Returns: The area.
 **/
static inline gdouble
square_area2(gdouble z1, gdouble z2, gdouble z3, gdouble z4,
             gdouble x, gdouble y)
{
    gdouble c;

    c = (z1 + z2 + z3 + z4)/2.0;

    return (sqrt(1.0 + (z1 - z2)*(z1 - z2)/x
                 + (z1 + z2 - c)*(z1 + z2 - c)/y)
            + sqrt(1.0 + (z2 - z3)*(z2 - z3)/y
                   + (z2 + z3 - c)*(z2 + z3 - c)/x)
            + sqrt(1.0 + (z3 - z4)*(z3 - z4)/x
                   + (z3 + z4 - c)*(z3 + z4 - c)/y)
            + sqrt(1.0 + (z1 - z4)*(z1 - z4)/y
                   + (z1 + z4 - c)*(z1 + z4 - c)/x));
}

/**
 * square_area2w:
 * @z1: Z-value in first corner.
 * @z2: Z-value in second corner.
 * @z3: Z-value in third corner.
 * @z4: Z-value in fourth corner.
 * @w1: Weight of first corner (0 or 1).
 * @w2: Weight of second corner (0 or 1).
 * @w3: Weight of third corner (0 or 1).
 * @w4: Weight of fourth corner (0 or 1).
 * @x: One fourth of square of rectangle width (x-size).
 * @y: One fourth of square of rectangle height (y-size).
 *
 * Calculates approximate area of a one general rectangular pixel with some
 * corners possibly missing.
 *
 * Returns: The area.
 **/
static inline gdouble
square_area2w(gdouble z1, gdouble z2, gdouble z3, gdouble z4,
              gint w1, gint w2, gint w3, gint w4,
              gdouble x, gdouble y)
{
    gdouble c;

    c = (z1 + z2 + z3 + z4)/2.0;

    return ((w1 + w2)*sqrt(1.0 + (z1 - z2)*(z1 - z2)/x
                           + (z1 + z2 - c)*(z1 + z2 - c)/y)
            + (w2 + w3)*sqrt(1.0 + (z2 - z3)*(z2 - z3)/y
                             + (z2 + z3 - c)*(z2 + z3 - c)/x)
            + (w3 + w4)*sqrt(1.0 + (z3 - z4)*(z3 - z4)/x
                             + (z3 + z4 - c)*(z3 + z4 - c)/y)
            + (w4 + w1)*sqrt(1.0 + (z1 - z4)*(z1 - z4)/y
                             + (z1 + z4 - c)*(z1 + z4 - c)/x))/2.0;
}

/**
 * stripe_area1:
 * @n: The number of values in @r, @rr, @m.
 * @stride: Stride in @r, @rr, @m.
 * @r: Array of @n z-values of vertices, this row of vertices is considered
 *     inside.
 * @rr: Array of @n z-values of vertices, this row of vertices is considered
 *      outside.
 * @m: Mask for @r (@rr does not need mask since it has zero weight by
 *     definition), or %NULL to sum over all @r vertices.
 * @mode: Masking mode.
 * @q: One fourth of rectangle projected area (x-size * ysize).
 *
 * Calculates approximate area of a half-pixel stripe.
 *
 * Returns: The area.
 **/
static gdouble
stripe_area1(gint n,
             gint stride,
             const gdouble *r,
             const gdouble *rr,
             const gdouble *m,
             GwyMaskingType mode,
             gdouble q)
{
    gdouble sum = 0.0;
    gint j;

    if (m && mode != GWY_MASK_IGNORE) {
        if (mode == GWY_MASK_INCLUDE) {
            for (j = 0; j < n-1; j++)
                sum += square_area1w(r[j*stride], r[(j + 1)*stride],
                                     rr[(j + 1)*stride], rr[j*stride],
                                     m[j*stride] > 0.0, m[(j + 1)*stride] > 0.0,
                                     0, 0,
                                     q);
        }
        else {
            for (j = 0; j < n-1; j++)
                sum += square_area1w(r[j*stride], r[(j + 1)*stride],
                                     rr[(j + 1)*stride], rr[j*stride],
                                     m[j*stride] < 1.0, m[(j + 1)*stride] < 1.0,
                                     0, 0,
                                     q);
        }
    }
    else {
        for (j = 0; j < n-1; j++)
            sum += square_area1w(r[j*stride], r[(j + 1)*stride],
                                 rr[(j + 1)*stride], rr[j*stride],
                                 1, 1, 0, 0,
                                 q);
    }

    return sum;
}

/**
 * stripe_area2:
 * @n: The number of values in @r, @rr, @m.
 * @stride: Stride in @r, @rr, @m.
 * @r: Array of @n z-values of vertices, this row of vertices is considered
 *     inside.
 * @rr: Array of @n z-values of vertices, this row of vertices is considered
 *      outside.
 * @m: Mask for @r (@rr does not need mask since it has zero weight by
 *     definition), or %NULL to sum over all @r vertices.
 * @x: One fourth of square of rectangle width (x-size).
 * @y: One fourth of square of rectangle height (y-size).
 *
 * Calculates approximate area of a half-pixel stripe.
 *
 * Returns: The area.
 **/
static gdouble
stripe_area2(gint n,
             gint stride,
             const gdouble *r,
             const gdouble *rr,
             const gdouble *m,
             GwyMaskingType mode,
             gdouble x,
             gdouble y)
{
    gdouble sum = 0.0;
    gint j;

    if (m && mode == GWY_MASK_INCLUDE) {
        for (j = 0; j < n-1; j++)
            sum += square_area2w(r[j*stride], r[(j + 1)*stride],
                                 rr[(j + 1)*stride], rr[j*stride],
                                 m[j*stride] > 0.0, m[(j + 1)*stride] > 0.0,
                                 0, 0,
                                 x, y);
    }
    else if (m && mode == GWY_MASK_EXCLUDE) {
        for (j = 0; j < n-1; j++)
            sum += square_area2w(r[j*stride], r[(j + 1)*stride],
                                 rr[(j + 1)*stride], rr[j*stride],
                                 m[j*stride] < 1.0, m[(j + 1)*stride] < 1.0,
                                 0, 0,
                                 x, y);
    }
    else {
        for (j = 0; j < n-1; j++)
            sum += square_area2w(r[j*stride], r[(j + 1)*stride],
                                 rr[(j + 1)*stride], rr[j*stride],
                                 1, 1, 0, 0,
                                 x, y);
    }

    return sum;
}

static gdouble
calculate_surface_area(GwyDataField *dfield,
                       GwyDataField *mask,
                       GwyMaskingType mode,
                       gint col, gint row,
                       gint width, gint height)
{
    const gdouble *r, *m, *dataul, *maskul;
    gint i, j, xres, yres, s;
    gdouble x, y, q, sum = 0.0;

    /* special cases */
    if (!width || !height)
        return sum;

    xres = dfield->xres;
    yres = dfield->yres;
    x = dfield->xreal/dfield->xres;
    y = dfield->yreal/dfield->yres;
    q = x*y;
    x = x*x;
    y = y*y;
    dataul = dfield->data + xres*row + col;

    if (mask && mode != GWY_MASK_IGNORE) {
        maskul = mask->data + xres*row + col;
        if (fabs(log(x/y)) < 1e-7) {
            /* Inside */
            for (i = 0; i < height-1; i++) {
                r = dataul + xres*i;
                m = maskul + xres*i;
                if (mode == GWY_MASK_INCLUDE) {
                    for (j = 0; j < width-1; j++)
                        sum += square_area1w(r[j], r[j+1],
                                             r[j+xres+1], r[j+xres],
                                             m[j] > 0.0, m[j+1] > 0.0,
                                             m[j+xres+1] > 0.0, m[j+xres] > 0.0,
                                             q);
                }
                else {
                    for (j = 0; j < width-1; j++)
                        sum += square_area1w(r[j], r[j+1],
                                             r[j+xres+1], r[j+xres],
                                             m[j] < 1.0, m[j+1] < 1.0,
                                             m[j+xres+1] < 1.0, m[j+xres] < 1.0,
                                             q);
                }
            }

            /* Top row */
            s = !(row == 0);
            sum += stripe_area1(width, 1, dataul, dataul - s*xres,
                                maskul, mode, q);

            /* Bottom row */
            s = !(row + height == yres);
            sum += stripe_area1(width, 1,
                                dataul + xres*(height-1),
                                dataul + xres*(height-1 + s),
                                maskul + xres*(height-1), mode, q);

            /* Left column */
            s = !(col == 0);
            sum += stripe_area1(height, xres, dataul, dataul - s,
                                maskul, mode, q);
            /* Right column */
            s = !(col + width == xres);
            sum += stripe_area1(height, xres,
                                dataul + width-1, dataul + width-1 + s,
                                maskul + width-1, mode, q);
        }
        else {
            /* Inside */
            for (i = 0; i < height-1; i++) {
                r = dataul + xres*i;
                m = maskul + xres*i;
                if (mode == GWY_MASK_INCLUDE) {
                    for (j = 0; j < width-1; j++)
                        sum += square_area2w(r[j], r[j+1],
                                             r[j+xres+1], r[j+xres],
                                             m[j] > 0.0, m[j+1] > 0.0,
                                             m[j+xres+1] > 0.0, m[j+xres] > 0.0,
                                             x, y);
                }
                else {
                    for (j = 0; j < width-1; j++)
                        sum += square_area2w(r[j], r[j+1],
                                             r[j+xres+1], r[j+xres],
                                             m[j] < 1.0, m[j+1] < 1.0,
                                             m[j+xres+1] < 1.0, m[j+xres] < 1.0,
                                             x, y);
                }
            }

            /* Top row */
            s = !(row == 0);
            sum += stripe_area2(width, 1, dataul, dataul - s*xres, maskul,
                                mode, x, y);

            /* Bottom row */
            s = !(row + height == yres);
            sum += stripe_area2(width, 1,
                                dataul + xres*(height-1),
                                dataul + xres*(height-1 + s),
                                maskul + xres*(height-1),
                                mode, x, y);

            /* Left column */
            s = !(col == 0);
            sum += stripe_area2(height, xres, dataul, dataul - s, maskul,
                                mode, y, x);

            /* Right column */
            s = !(col + width == xres);
            sum += stripe_area2(height, xres,
                                dataul + width-1, dataul + width-1 + s,
                                maskul + width-1,
                                mode, y, x);
        }

        /* Just take the four corner quater-pixels as flat.  */
        if (mode == GWY_MASK_INCLUDE) {
            if (maskul[0] > 0.0)
                sum += 1.0;
            if (maskul[width-1] > 0.0)
                sum += 1.0;
            if (maskul[xres*(height-1)] > 0.0)
                sum += 1.0;
            if (maskul[xres*(height-1) + width-1] > 0.0)
                sum += 1.0;
        }
        else {
            if (maskul[0] < 1.0)
                sum += 1.0;
            if (maskul[width-1] < 1.0)
                sum += 1.0;
            if (maskul[xres*(height-1)] < 1.0)
                sum += 1.0;
            if (maskul[xres*(height-1) + width-1] < 1.0)
                sum += 1.0;
        }
    }
    else {
        if (fabs(log(x/y)) < 1e-7) {
            /* Inside */
            for (i = 0; i < height-1; i++) {
                r = dataul + xres*i;
                for (j = 0; j < width-1; j++)
                    sum += square_area1(r[j], r[j+1], r[j+xres+1], r[j+xres],
                                        q);
            }

            /* Top row */
            s = !(row == 0);
            sum += stripe_area1(width, 1, dataul, dataul - s*xres,
                                NULL, GWY_MASK_IGNORE, q);

            /* Bottom row */
            s = !(row + height == yres);
            sum += stripe_area1(width, 1,
                                dataul + xres*(height-1),
                                dataul + xres*(height-1 + s),
                                NULL, GWY_MASK_IGNORE, q);

            /* Left column */
            s = !(col == 0);
            sum += stripe_area1(height, xres, dataul, dataul - s,
                                NULL, GWY_MASK_IGNORE, q);

            /* Right column */
            s = !(col + width == xres);
            sum += stripe_area1(height, xres,
                                dataul + width-1, dataul + width-1 + s,
                                NULL, GWY_MASK_IGNORE, q);
        }
        else {
            for (i = 0; i < height-1; i++) {
                r = dataul + xres*i;
                for (j = 0; j < width-1; j++)
                    sum += square_area2(r[j], r[j+1], r[j+xres+1], r[j+xres],
                                        x, y);
            }

            /* Top row */
            s = !(row == 0);
            sum += stripe_area2(width, 1, dataul, dataul - s*xres, NULL,
                                GWY_MASK_IGNORE, x, y);

            /* Bottom row */
            s = !(row + height == yres);
            sum += stripe_area2(width, 1,
                                dataul + xres*(height-1),
                                dataul + xres*(height-1 + s),
                                NULL,
                                GWY_MASK_IGNORE, x, y);

            /* Left column */
            s = !(col == 0);
            sum += stripe_area2(height, xres, dataul, dataul - s, NULL,
                                GWY_MASK_IGNORE, y, x);

            /* Right column */
            s = !(col + width == xres);
            sum += stripe_area2(height, xres,
                                dataul + width-1, dataul + width-1 + s, NULL,
                                GWY_MASK_IGNORE, y, x);
        }

        /* Just take the four corner quater-pixels as flat.  */
        sum += 4.0;
    }

    return sum*q/4;
}

/**
 * gwy_data_field_get_surface_area:
 * @data_field: A data field.
 *
 * Computes surface area of a data field.
 *
 * This quantity is cached.
 *
 * Returns: The surface area.
 **/
gdouble
gwy_data_field_get_surface_area(GwyDataField *data_field)
{
    gdouble area = 0.0;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(data_field), area);

    gwy_debug("%s", CTEST(data_field, ARE) ? "cache" : "lame");
    if (CTEST(data_field, ARE))
        return CVAL(data_field, ARE);

    area = calculate_surface_area(data_field, NULL, GWY_MASK_IGNORE,
                                  0, 0, data_field->xres, data_field->yres);

    CVAL(data_field, ARE) = area;
    data_field->cached |= CBIT(ARE);

    return area;
}

/**
 * gwy_data_field_area_get_surface_area_mask:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Computes surface area of a rectangular part of a data field.
 *
 * This function is equivalent to calling
 * @gwy_data_field_area_get_surface_area_mask()
 * with masking mode %GWY_MASK_INCLUDE.
 *
 * Returns: The surface area.
 **/
gdouble
gwy_data_field_area_get_surface_area(GwyDataField *data_field,
                                     GwyDataField *mask,
                                     gint col, gint row,
                                     gint width, gint height)
{
    return gwy_data_field_area_get_surface_area_mask(data_field, mask,
                                                     GWY_MASK_INCLUDE,
                                                     col, row, width, height);
}

/**
 * gwy_data_field_area_get_surface_area_mask:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @mode: Masking mode to use.  See the introduction for description of
 *        masking modes.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Computes surface area of a rectangular part of a data field.
 *
 * This quantity makes sense only if the lateral dimensions and values of
 * @data_field are the same physical quantities.
 *
 * Returns: The surface area.
 *
 * Since: 2.18
 **/
gdouble
gwy_data_field_area_get_surface_area_mask(GwyDataField *data_field,
                                          GwyDataField *mask,
                                          GwyMaskingType mode,
                                          gint col, gint row,
                                          gint width, gint height)
{
    gdouble area = 0.0;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(data_field), area);
    g_return_val_if_fail(!mask || (GWY_IS_DATA_FIELD(mask)
                                   && mask->xres == data_field->xres
                                   && mask->yres == data_field->yres), area);
    g_return_val_if_fail(col >= 0 && row >= 0
                         && width >= 0 && height >= 0
                         && col + width <= data_field->xres
                         && row + height <= data_field->yres,
                         area);

    /* The result is the same, but it can be cached. */
    if ((!mask || mode == GWY_MASK_IGNORE)
        && row == 0 && col == 0
        && width == data_field->xres && height == data_field->yres)
        return gwy_data_field_get_surface_area(data_field);

    return calculate_surface_area(data_field, mask, mode,
                                  col, row, width, height);
}

/**
 * square_var1:
 * @z1: Z-value in first corner.
 * @z2: Z-value in second corner.
 * @z3: Z-value in third corner.
 * @z4: Z-value in fourth corner.
 * @q: One fourth of rectangle projected var (x-size * ysize).
 *
 * Calculates approximate variation of a one square pixel.
 *
 * Returns: The variation.
 **/
static inline gdouble
square_var1(gdouble z1, gdouble z2, gdouble z3, gdouble z4,
            gdouble q)
{
    gdouble z12 = z1 - z2, z23 = z2 - z3, z34 = z3 - z4, z41 = z4 - z1;

    return (sqrt((z12*z12 + z41*z41)/q)
            + sqrt((z23*z23 + z12*z12)/q)
            + sqrt((z34*z34 + z23*z23)/q)
            + sqrt((z41*z41 + z34*z34)/q));
}

/**
 * square_var1w:
 * @z1: Z-value in first corner.
 * @z2: Z-value in second corner.
 * @z3: Z-value in third corner.
 * @z4: Z-value in fourth corner.
 * @w1: Weight of first corner (0 or 1).
 * @w2: Weight of second corner (0 or 1).
 * @w3: Weight of third corner (0 or 1).
 * @w4: Weight of fourth corner (0 or 1).
 * @q: One fourth of rectangle projected var (x-size * ysize).
 *
 * Calculates approximate variation of a one square pixel with some corners
 * possibly missing.
 *
 * Returns: The variation.
 **/
static inline gdouble
square_var1w(gdouble z1, gdouble z2, gdouble z3, gdouble z4,
             gint w1, gint w2, gint w3, gint w4,
             gdouble q)
{
    gdouble z12 = z1 - z2, z23 = z2 - z3, z34 = z3 - z4, z41 = z4 - z1;

    return (w1*sqrt((z12*z12 + z41*z41)/q)
            + w2*sqrt((z23*z23 + z12*z12)/q)
            + w3*sqrt((z34*z34 + z23*z23)/q)
            + w4*sqrt((z41*z41 + z34*z34)/q));
}

/**
 * square_var2:
 * @z1: Z-value in first corner.
 * @z2: Z-value in second corner.
 * @z3: Z-value in third corner.
 * @z4: Z-value in fourth corner.
 * @x: One fourth of square of rectangle width (x-size).
 * @y: One fourth of square of rectangle height (y-size).
 *
 * Calculates approximate variation of a one general rectangular pixel.
 *
 * Returns: The variation.
 **/
static inline gdouble
square_var2(gdouble z1, gdouble z2, gdouble z3, gdouble z4,
            gdouble x, gdouble y)
{
    gdouble z12 = z1 - z2, z23 = z2 - z3, z34 = z3 - z4, z41 = z4 - z1;

    return (sqrt(z12*z12/x + z41*z41/y)
            + sqrt(z23*z23/y + z12*z12/x)
            + sqrt(z34*z34/x + z23*z23/y)
            + sqrt(z41*z41/y + z34*z34/x));
}

/**
 * square_var2w:
 * @z1: Z-value in first corner.
 * @z2: Z-value in second corner.
 * @z3: Z-value in third corner.
 * @z4: Z-value in fourth corner.
 * @w1: Weight of first corner (0 or 1).
 * @w2: Weight of second corner (0 or 1).
 * @w3: Weight of third corner (0 or 1).
 * @w4: Weight of fourth corner (0 or 1).
 * @x: One fourth of square of rectangle width (x-size).
 * @y: One fourth of square of rectangle height (y-size).
 *
 * Calculates approximate variation of a one general rectangular pixel with
 * some corners possibly missing.
 *
 * Returns: The variation.
 **/
static inline gdouble
square_var2w(gdouble z1, gdouble z2, gdouble z3, gdouble z4,
             gint w1, gint w2, gint w3, gint w4,
             gdouble x, gdouble y)
{
    gdouble z12 = z1 - z2, z23 = z2 - z3, z34 = z3 - z4, z41 = z4 - z1;

    return (w1*sqrt(z12*z12/x + z41*z41/y)
            + w2*sqrt(z23*z23/y + z12*z12/x)
            + w3*sqrt(z34*z34/x + z23*z23/y)
            + w4*sqrt(z41*z41/y + z34*z34/x));
}

/**
 * stripe_var1:
 * @n: The number of values in @r, @rr, @m.
 * @stride: Stride in @r, @rr, @m.
 * @r: Array of @n z-values of vertices, this row of vertices is considered
 *     inside.
 * @rr: Array of @n z-values of vertices, this row of vertices is considered
 *      outside.
 * @m: Mask for @r (@rr does not need mask since it has zero weight by
 *     definition), or %NULL to sum over all @r vertices.
 * @mode: Masking mode.
 * @q: One fourth of rectangle projected var (x-size * ysize).
 *
 * Calculates approximate variation of a half-pixel stripe.
 *
 * Returns: The variation.
 **/
static gdouble
stripe_var1(gint n,
            gint stride,
            const gdouble *r,
            const gdouble *rr,
            const gdouble *m,
            GwyMaskingType mode,
            gdouble q)
{
    gdouble sum = 0.0;
    gint j;

    if (m && mode != GWY_MASK_IGNORE) {
        if (mode == GWY_MASK_INCLUDE) {
            for (j = 0; j < n-1; j++)
                sum += square_var1w(r[j*stride], r[(j + 1)*stride],
                                    rr[(j + 1)*stride], rr[j*stride],
                                    m[j*stride] > 0.0, m[(j + 1)*stride] > 0.0,
                                    0, 0,
                                    q);
        }
        else {
            for (j = 0; j < n-1; j++)
                sum += square_var1w(r[j*stride], r[(j + 1)*stride],
                                    rr[(j + 1)*stride], rr[j*stride],
                                    m[j*stride] < 1.0, m[(j + 1)*stride] < 1.0,
                                    0, 0,
                                    q);
        }
    }
    else {
        for (j = 0; j < n-1; j++)
            sum += square_var1w(r[j*stride], r[(j + 1)*stride],
                                rr[(j + 1)*stride], rr[j*stride],
                                1, 1, 0, 0,
                                q);
    }

    return sum;
}

/**
 * stripe_var2:
 * @n: The number of values in @r, @rr, @m.
 * @stride: Stride in @r, @rr, @m.
 * @r: Array of @n z-values of vertices, this row of vertices is considered
 *     inside.
 * @rr: Array of @n z-values of vertices, this row of vertices is considered
 *      outside.
 * @m: Mask for @r (@rr does not need mask since it has zero weight by
 *     definition), or %NULL to sum over all @r vertices.
 * @x: One fourth of square of rectangle width (x-size).
 * @y: One fourth of square of rectangle height (y-size).
 *
 * Calculates approximate variation of a half-pixel stripe.
 *
 * Returns: The variation.
 **/
static gdouble
stripe_var2(gint n,
            gint stride,
            const gdouble *r,
            const gdouble *rr,
            const gdouble *m,
            GwyMaskingType mode,
            gdouble x,
            gdouble y)
{
    gdouble sum = 0.0;
    gint j;

    if (m && mode == GWY_MASK_INCLUDE) {
        for (j = 0; j < n-1; j++)
            sum += square_var2w(r[j*stride], r[(j + 1)*stride],
                                rr[(j + 1)*stride], rr[j*stride],
                                m[j*stride] > 0.0, m[(j + 1)*stride] > 0.0,
                                0, 0,
                                x, y);
    }
    else if (m && mode == GWY_MASK_EXCLUDE) {
        for (j = 0; j < n-1; j++)
            sum += square_var2w(r[j*stride], r[(j + 1)*stride],
                                rr[(j + 1)*stride], rr[j*stride],
                                m[j*stride] < 1.0, m[(j + 1)*stride] < 1.0,
                                0, 0,
                                x, y);
    }
    else {
        for (j = 0; j < n-1; j++)
            sum += square_var2w(r[j*stride], r[(j + 1)*stride],
                                rr[(j + 1)*stride], rr[j*stride],
                                1, 1, 0, 0,
                                x, y);
    }

    return sum;
}

static gdouble
calculate_variation(GwyDataField *dfield,
                    GwyDataField *mask,
                    GwyMaskingType mode,
                    gint col, gint row,
                    gint width, gint height)
{
    const gdouble *r, *m, *dataul, *maskul;
    gint i, j, xres, yres, s;
    gdouble x, y, q, sum = 0.0;

    /* special cases */
    if (!width || !height)
        return sum;

    xres = dfield->xres;
    yres = dfield->yres;
    x = dfield->xreal/dfield->xres;
    y = dfield->yreal/dfield->yres;
    q = x*y;
    x = x*x;
    y = y*y;
    dataul = dfield->data + xres*row + col;

    if (mask && mode != GWY_MASK_IGNORE) {
        maskul = mask->data + xres*row + col;
        if (fabs(log(x/y)) < 1e-7) {
            /* Inside */
            for (i = 0; i < height-1; i++) {
                r = dataul + xres*i;
                m = maskul + xres*i;
                if (mode == GWY_MASK_INCLUDE) {
                    for (j = 0; j < width-1; j++)
                        sum += square_var1w(r[j], r[j+1],
                                            r[j+xres+1], r[j+xres],
                                            m[j] > 0.0, m[j+1] > 0.0,
                                            m[j+xres+1] > 0.0, m[j+xres] > 0.0,
                                            q);
                }
                else {
                    for (j = 0; j < width-1; j++)
                        sum += square_var1w(r[j], r[j+1],
                                            r[j+xres+1], r[j+xres],
                                            m[j] < 1.0, m[j+1] < 1.0,
                                            m[j+xres+1] < 1.0, m[j+xres] < 1.0,
                                            q);
                }
            }

            /* Top row */
            s = !(row == 0);
            sum += stripe_var1(width, 1, dataul, dataul - s*xres,
                               maskul, mode, q);

            /* Bottom row */
            s = !(row + height == yres);
            sum += stripe_var1(width, 1,
                               dataul + xres*(height-1),
                               dataul + xres*(height-1 + s),
                               maskul + xres*(height-1), mode, q);

            /* Left column */
            s = !(col == 0);
            sum += stripe_var1(height, xres, dataul, dataul - s,
                               maskul, mode, q);
            /* Right column */
            s = !(col + width == xres);
            sum += stripe_var1(height, xres,
                               dataul + width-1, dataul + width-1 + s,
                               maskul + width-1, mode, q);
        }
        else {
            /* Inside */
            for (i = 0; i < height-1; i++) {
                r = dataul + xres*i;
                m = maskul + xres*i;
                if (mode == GWY_MASK_INCLUDE) {
                    for (j = 0; j < width-1; j++)
                        sum += square_var2w(r[j], r[j+1],
                                            r[j+xres+1], r[j+xres],
                                            m[j] > 0.0, m[j+1] > 0.0,
                                            m[j+xres+1] > 0.0, m[j+xres] > 0.0,
                                            x, y);
                }
                else {
                    for (j = 0; j < width-1; j++)
                        sum += square_var2w(r[j], r[j+1],
                                            r[j+xres+1], r[j+xres],
                                            m[j] < 1.0, m[j+1] < 1.0,
                                            m[j+xres+1] < 1.0, m[j+xres] < 1.0,
                                            x, y);
                }
            }

            /* Top row */
            s = !(row == 0);
            sum += stripe_var2(width, 1, dataul, dataul - s*xres, maskul,
                               mode, x, y);

            /* Bottom row */
            s = !(row + height == yres);
            sum += stripe_var2(width, 1,
                               dataul + xres*(height-1),
                               dataul + xres*(height-1 + s),
                               maskul + xres*(height-1),
                               mode, x, y);

            /* Left column */
            s = !(col == 0);
            sum += stripe_var2(height, xres, dataul, dataul - s, maskul,
                               mode, y, x);

            /* Right column */
            s = !(col + width == xres);
            sum += stripe_var2(height, xres,
                               dataul + width-1, dataul + width-1 + s,
                               maskul + width-1,
                               mode, y, x);
        }
    }
    else {
        if (fabs(log(x/y)) < 1e-7) {
            /* Inside */
            for (i = 0; i < height-1; i++) {
                r = dataul + xres*i;
                for (j = 0; j < width-1; j++)
                    sum += square_var1(r[j], r[j+1], r[j+xres+1], r[j+xres],
                                       q);
            }

            /* Top row */
            s = !(row == 0);
            sum += stripe_var1(width, 1, dataul, dataul - s*xres,
                               NULL, GWY_MASK_IGNORE, q);

            /* Bottom row */
            s = !(row + height == yres);
            sum += stripe_var1(width, 1,
                               dataul + xres*(height-1),
                               dataul + xres*(height-1 + s),
                               NULL, GWY_MASK_IGNORE, q);

            /* Left column */
            s = !(col == 0);
            sum += stripe_var1(height, xres, dataul, dataul - s,
                               NULL, GWY_MASK_IGNORE, q);

            /* Right column */
            s = !(col + width == xres);
            sum += stripe_var1(height, xres,
                               dataul + width-1, dataul + width-1 + s,
                               NULL, GWY_MASK_IGNORE, q);
        }
        else {
            for (i = 0; i < height-1; i++) {
                r = dataul + xres*i;
                for (j = 0; j < width-1; j++)
                    sum += square_var2(r[j], r[j+1], r[j+xres+1], r[j+xres],
                                       x, y);
            }

            /* Top row */
            s = !(row == 0);
            sum += stripe_var2(width, 1, dataul, dataul - s*xres, NULL,
                               GWY_MASK_IGNORE, x, y);

            /* Bottom row */
            s = !(row + height == yres);
            sum += stripe_var2(width, 1,
                               dataul + xres*(height-1),
                               dataul + xres*(height-1 + s),
                               NULL,
                               GWY_MASK_IGNORE, x, y);

            /* Left column */
            s = !(col == 0);
            sum += stripe_var2(height, xres, dataul, dataul - s, NULL,
                               GWY_MASK_IGNORE, y, x);

            /* Right column */
            s = !(col + width == xres);
            sum += stripe_var2(height, xres,
                               dataul + width-1, dataul + width-1 + s, NULL,
                               GWY_MASK_IGNORE, y, x);
        }
    }

    return sum*q/4;
}

/**
 * gwy_data_field_get_variation:
 * @data_field: A data field.
 *
 * Computes the total variation of a data field.
 *
 * See gwy_data_field_area_get_variation() for the definition.
 *
 * This quantity is cached.
 *
 * Returns: The variation.
 *
 * Since: 2.38
 **/
gdouble
gwy_data_field_get_variation(GwyDataField *data_field)
{
    gdouble var = 0.0;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(data_field), var);

    gwy_debug("%s", CTEST(data_field, VAR) ? "cache" : "lame");
    if (CTEST(data_field, VAR))
        return CVAL(data_field, VAR);

    var = calculate_variation(data_field, NULL, GWY_MASK_IGNORE,
                              0, 0, data_field->xres, data_field->yres);

    CVAL(data_field, VAR) = var;
    data_field->cached |= CBIT(VAR);

    return var;
}

/**
 * gwy_data_field_area_get_variation:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @mode: Masking mode to use.  See the introduction for description of
 *        masking modes.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Computes the total variation of a rectangular part of a data field.
 *
 * The total variation is estimated as the integral of the absolute value of
 * local gradient.
 *
 * This quantity has the somewhat odd units of value unit times lateral unit.
 * It can be envisioned as follows.  If the surface has just two height levels
 * (upper and lower planes) then the quantity is the length of the boundary
 * between the upper and lower part, multiplied by the step height.  If the
 * surface is piece-wise constant, then the variation is the step height
 * integrated along the boundaries between the constant parts.  Therefore, for
 * non-fractal surfaces it scales with the linear dimension of the image, not
 * with its area, despite being an area integral.
 *
 * Returns: The variation.
 *
 * Since: 2.38
 **/
gdouble
gwy_data_field_area_get_variation(GwyDataField *data_field,
                                  GwyDataField *mask,
                                  GwyMaskingType mode,
                                  gint col, gint row,
                                  gint width, gint height)
{
    gdouble var = 0.0;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(data_field), var);
    g_return_val_if_fail(!mask || (GWY_IS_DATA_FIELD(mask)
                                   && mask->xres == data_field->xres
                                   && mask->yres == data_field->yres), var);
    g_return_val_if_fail(col >= 0 && row >= 0
                         && width >= 0 && height >= 0
                         && col + width <= data_field->xres
                         && row + height <= data_field->yres,
                         var);

    /* The result is the same, but it can be cached. */
    if ((!mask || mode == GWY_MASK_IGNORE)
        && row == 0 && col == 0
        && width == data_field->xres && height == data_field->yres)
        return gwy_data_field_get_variation(data_field);

    return calculate_variation(data_field, mask, mode,
                               col, row, width, height);
}

/**
 * square_volume:
 * @z1: Z-value in first corner.
 * @z2: Z-value in second corner.
 * @z3: Z-value in third corner.
 * @z4: Z-value in fourth corner.
 *
 * Calculates approximate volume of a one square pixel.
 *
 * Returns: The volume.
 **/
static inline gdouble
square_volume(gdouble z1, gdouble z2, gdouble z3, gdouble z4)
{
    gdouble c;

    c = (z1 + z2 + z3 + z4)/4.0;

    return c;
}

/**
 * square_volumew:
 * @z1: Z-value in first corner.
 * @z2: Z-value in second corner.
 * @z3: Z-value in third corner.
 * @z4: Z-value in fourth corner.
 * @w1: Weight of first corner (0 or 1).
 * @w2: Weight of second corner (0 or 1).
 * @w3: Weight of third corner (0 or 1).
 * @w4: Weight of fourth corner (0 or 1).
 *
 * Calculates approximate volume of a one square pixel with some corners
 * possibly missing.
 *
 * Returns: The volume.
 **/
static inline gdouble
square_volumew(gdouble z1, gdouble z2, gdouble z3, gdouble z4,
               gint w1, gint w2, gint w3, gint w4)
{
    gdouble c;

    c = (z1 + z2 + z3 + z4)/4.0;

    return (w1*(3.0*z1 + z2 + z4 + c)
            + w2*(3.0*z2 + z1 + z3 + c)
            + w3*(3.0*z3 + z2 + z4 + c)
            + w4*(3.0*z4 + z3 + z1 + c))/24.0;
}

/**
 * stripe_volume:
 * @n: The number of values in @r, @rr, @m.
 * @stride: Stride in @r, @rr, @m.
 * @r: Array of @n z-values of vertices, this row of vertices is considered
 *     inside.
 * @rr: Array of @n z-values of vertices, this row of vertices is considered
 *      outside.
 * @m: Mask for @r (@rr does not need mask since it has zero weight by
 *     definition), or %NULL to sum over all @r vertices.
 *
 * Calculates approximate volume of a half-pixel stripe.
 *
 * Returns: The volume.
 **/
static gdouble
stripe_volume(gint n,
              gint stride,
              const gdouble *r,
              const gdouble *rr,
              const gdouble *m)
{
    gdouble sum = 0.0;
    gint j;

    if (m) {
        for (j = 0; j < n-1; j++)
            sum += square_volumew(r[j*stride], r[(j + 1)*stride],
                                  rr[(j + 1)*stride], rr[j*stride],
                                  m[j*stride] > 0.0, m[(j + 1)*stride] > 0.0,
                                  0, 0);
    }
    else {
        for (j = 0; j < n-1; j++)
            sum += square_volumew(r[j*stride], r[(j + 1)*stride],
                                  rr[(j + 1)*stride], rr[j*stride],
                                  1, 1, 0, 0);
    }

    return sum;
}

/**
 * stripe_volumeb:
 * @n: The number of values in @r, @rr, @m.
 * @stride: Stride in @r, @rr, @m.
 * @r: Array of @n z-values of vertices, this row of vertices is considered
 *     inside.
 * @rr: Array of @n z-values of vertices, this row of vertices is considered
 *      outside.
 * @b: Array of @n z-values of basis, this row of vertices is considered
 *     inside.
 * @br: Array of @n z-values of basis, this row of vertices is considered
 *      outside.
 * @m: Mask for @r (@rr does not need mask since it has zero weight by
 *     definition), or %NULL to sum over all @r vertices.
 *
 * Calculates approximate volume of a half-pixel stripe, taken from basis.
 *
 * Returns: The volume.
 **/
static gdouble
stripe_volumeb(gint n,
               gint stride,
               const gdouble *r,
               const gdouble *rr,
               const gdouble *b,
               const gdouble *br,
               const gdouble *m)
{
    gdouble sum = 0.0;
    gint j;

    if (m) {
        for (j = 0; j < n-1; j++)
            sum += square_volumew(r[j*stride] - b[j*stride],
                                  r[(j + 1)*stride] - b[(j + 1)*stride],
                                  rr[(j + 1)*stride] - br[(j + 1)*stride],
                                  rr[j*stride] - br[j*stride],
                                  m[j*stride] > 0.0,
                                  m[(j + 1)*stride] > 0.0,
                                  0, 0);
    }
    else {
        for (j = 0; j < n-1; j++)
            sum += square_volumew(r[j*stride] - b[j*stride],
                                  r[(j + 1)*stride] - b[(j + 1)*stride],
                                  rr[(j + 1)*stride] - br[(j + 1)*stride],
                                  rr[j*stride] - br[j*stride],
                                  1, 1, 0, 0);
    }

    return sum;
}

static gdouble
calculate_volume(GwyDataField *dfield,
                 GwyDataField *basis,
                 GwyDataField *mask,
                 gint col, gint row,
                 gint width, gint height)
{
    const gdouble *r, *m, *b, *dataul, *maskul, *basisul;
    gint i, j, xres, yres, s;
    gdouble sum = 0.0;

    /* special cases */
    if (!width || !height)
        return sum;

    xres = dfield->xres;
    yres = dfield->yres;
    dataul = dfield->data + xres*row + col;

    if (mask) {
        maskul = mask->data + xres*row + col;
        if (!basis) {
            /* Inside */
            for (i = 0; i < height-1; i++) {
                r = dataul + xres*i;
                m = maskul + xres*i;
                for (j = 0; j < width-1; j++)
                    sum += square_volumew(r[j], r[j+1],
                                          r[j+xres+1], r[j+xres],
                                          m[j] > 0.0, m[j+1] > 0.0,
                                          m[j+xres+1] > 0.0, m[j+xres] > 0.0);
            }

            /* Top row */
            s = !(row == 0);
            sum += stripe_volume(width, 1, dataul, dataul - s*xres, maskul);

            /* Bottom row */
            s = !(row + height == yres);
            sum += stripe_volume(width, 1,
                                 dataul + xres*(height-1),
                                 dataul + xres*(height-1 + s),
                                 maskul + xres*(height-1));

            /* Left column */
            s = !(col == 0);
            sum += stripe_volume(height, xres, dataul, dataul - s, maskul);

            /* Right column */
            s = !(col + width == xres);
            sum += stripe_volume(height, xres,
                                 dataul + width-1, dataul + width-1 + s,
                                 maskul + width-1);

            /* Just take the four corner quater-pixels as flat.  */
            if (maskul[0])
                sum += dataul[0]/4.0;
            if (maskul[width-1])
                sum += dataul[width-1]/4.0;
            if (maskul[xres*(height-1)])
                sum += dataul[xres*(height-1)]/4.0;
            if (maskul[xres*(height-1) + width-1])
                sum += dataul[xres*(height-1) + width-1]/4.0;
        }
        else {
            basisul = basis->data + xres*row + col;

            /* Inside */
            for (i = 0; i < height-1; i++) {
                r = dataul + xres*i;
                m = maskul + xres*i;
                b = basisul + xres*i;
                for (j = 0; j < width-1; j++)
                    sum += square_volumew(r[j] - b[j],
                                          r[j+1] - b[j+1],
                                          r[j+xres+1] - b[j+xres+1],
                                          r[j+xres] - b[j+xres],
                                          m[j] > 0.0, m[j+1] > 0.0,
                                          m[j+xres+1] > 0.0, m[j+xres] > 0.0);
            }

            /* Top row */
            s = !(row == 0);
            sum += stripe_volumeb(width, 1,
                                  dataul, dataul - s*xres,
                                  basisul, basisul - s*xres,
                                  maskul);

            /* Bottom row */
            s = !(row + height == yres);
            sum += stripe_volumeb(width, 1,
                                  dataul + xres*(height-1),
                                  dataul + xres*(height-1 + s),
                                  basisul + xres*(height-1),
                                  basisul + xres*(height-1 + s),
                                  maskul + xres*(height-1));

            /* Left column */
            s = !(col == 0);
            sum += stripe_volumeb(height, xres,
                                  dataul, dataul - s,
                                  basisul, basisul - s,
                                  maskul);

            /* Right column */
            s = !(col + width == xres);
            sum += stripe_volumeb(height, xres,
                                  dataul + width-1, dataul + width-1 + s,
                                  basisul + width-1, basisul + width-1 + s,
                                  maskul + width-1);

            /* Just take the four corner quater-pixels as flat.  */
            if (maskul[0])
                sum += (dataul[0] - basisul[0])/4.0;
            if (maskul[width-1])
                sum += (dataul[width-1] - basisul[width-1])/4.0;
            if (maskul[xres*(height-1)])
                sum += (dataul[xres*(height-1)] - basisul[xres*(height-1)])/4.0;
            if (maskul[xres*(height-1) + width-1])
                sum += (dataul[xres*(height-1) + width-1]
                        - basisul[xres*(height-1) + width-1])/4.0;
        }
    }
    else {
        if (!basis) {
            /* Inside */
            for (i = 0; i < height-1; i++) {
                r = dataul + xres*i;
                for (j = 0; j < width-1; j++)
                    sum += square_volume(r[j], r[j+1], r[j+xres+1], r[j+xres]);
            }

            /* Top row */
            s = !(row == 0);
            sum += stripe_volume(width, 1, dataul, dataul - s*xres, NULL);

            /* Bottom row */
            s = !(row + height == yres);
            sum += stripe_volume(width, 1,
                                 dataul + xres*(height-1),
                                 dataul + xres*(height-1 + s),
                                 NULL);

            /* Left column */
            s = !(col == 0);
            sum += stripe_volume(height, xres, dataul, dataul - s, NULL);

            /* Right column */
            s = !(col + width == xres);
            sum += stripe_volume(height, xres,
                                 dataul + width-1, dataul + width-1 + s,
                                 NULL);

            /* Just take the four corner quater-pixels as flat.  */
            sum += dataul[0]/4.0;
            sum += dataul[width-1]/4.0;
            sum += dataul[xres*(height-1)]/4.0;
            sum += dataul[xres*(height-1) + width-1]/4.0;
        }
        else {
            basisul = basis->data + xres*row + col;

            /* Inside */
            for (i = 0; i < height-1; i++) {
                r = dataul + xres*i;
                b = basisul + xres*i;
                for (j = 0; j < width-1; j++)
                    sum += square_volume(r[j] - b[j],
                                         r[j+1] - b[j+1],
                                         r[j+xres+1] - b[j+xres+1],
                                         r[j+xres] - b[j+xres]);
            }

            /* Top row */
            s = !(row == 0);
            sum += stripe_volumeb(width, 1,
                                  dataul, dataul - s*xres,
                                  basisul, basisul - s*xres,
                                  NULL);

            /* Bottom row */
            s = !(row + height == yres);
            sum += stripe_volumeb(width, 1,
                                  dataul + xres*(height-1),
                                  dataul + xres*(height-1 + s),
                                  basisul + xres*(height-1),
                                  basisul + xres*(height-1 + s),
                                  NULL);

            /* Left column */
            s = !(col == 0);
            sum += stripe_volumeb(height, xres,
                                  dataul, dataul - s,
                                  basisul, basisul - s,
                                  NULL);

            /* Right column */
            s = !(col + width == xres);
            sum += stripe_volumeb(height, xres,
                                  dataul + width-1, dataul + width-1 + s,
                                  basisul + width-1, basisul + width-1 + s,
                                  NULL);

            /* Just take the four corner quater-pixels as flat.  */
            sum += (dataul[0] - basisul[0])/4.0;
            sum += (dataul[width-1] - basisul[width-1])/4.0;
            sum += (dataul[xres*(height-1)] - basisul[xres*(height-1)])/4.0;
            sum += (dataul[xres*(height-1) + width-1]
                    - basisul[xres*(height-1) + width-1])/4.0;
        }
    }

    return sum* dfield->xreal/dfield->xres * dfield->yreal/dfield->yres;
}

/* Don't define gwy_data_field_get_volume() without mask and basis, it would
 * just be a complicate way to calculate gwy_data_field_get_sum() */

/**
 * gwy_data_field_area_get_volume:
 * @data_field: A data field.
 * @basis: The basis or background for volume calculation if not %NULL.
 *         The height of each vertex is then the difference between
 *         @data_field value and @basis value.  Value %NULL is the same
 *         as passing all zeroes for the basis.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Computes volume of a rectangular part of a data field.
 *
 * Returns: The volume.
 *
 * Since: 2.3
 **/
gdouble
gwy_data_field_area_get_volume(GwyDataField *data_field,
                               GwyDataField *basis,
                               GwyDataField *mask,
                               gint col, gint row,
                               gint width, gint height)
{
    gdouble vol = 0.0;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(data_field), vol);
    g_return_val_if_fail(!basis || (GWY_IS_DATA_FIELD(basis)
                                    && basis->xres == data_field->xres
                                    && basis->yres == data_field->yres), vol);
    g_return_val_if_fail(!mask || (GWY_IS_DATA_FIELD(mask)
                                   && mask->xres == data_field->xres
                                   && mask->yres == data_field->yres), vol);
    g_return_val_if_fail(col >= 0 && row >= 0
                         && width >= 0 && height >= 0
                         && col + width <= data_field->xres
                         && row + height <= data_field->yres,
                         vol);

    return calculate_volume(data_field, basis, mask, col, row, width, height);
}

/* Find the flattest part of the curve representing scaling histogram-based
 * entropy on scale and use the value there as the entropy estimate.  Handle
 * the too-few-pixels cases gracefully.
 *
 * NB: We assume
 * (1) ecurve beings from large scales.  This is important only when it has
 * lots of points because we may skip a few at the beginning then to avoid
 * mistaking the flat part of the curve there for the inflexion point.
 * (2) ecurve goes by powers of 2 scales, this is for the mindiff filtering.
 */
static gdouble
calculate_entropy_from_scaling(const gdouble *ecurve, guint maxdiv)
{
    /* Initialise S to the δ-function entropy and mindiff to the half of the
     * asymptotic value for distribution that is sum of δ-functions.
     * This means only if the differences drops substantially from this
     * asymptotic value we will consider is as potential inflexion point.
     * If we get ecurve[] essentially corresponding to a set of δ-functions
     * then we return -G_MAXDOUBLE. */
    gdouble S = -G_MAXDOUBLE, mindiff = 0.6*G_LN2;
    guint i, from = (maxdiv >= 12) + (maxdiv >= 36);

    if (maxdiv < 1)
        return ecurve[0];

    if (maxdiv < 5) {
        for (i = from; i <= maxdiv-2; i++) {
            gdouble diff = 0.5*(fabs(ecurve[i+1] - ecurve[i])
                                + fabs(ecurve[i+2] - ecurve[i+1]))/G_LN2;
            gdouble diff2 = 0.5*(fabs(ecurve[i] + ecurve[i+2]
                                      - 2.0*ecurve[i+1]))/(G_LN2*G_LN2);
            if (diff + diff2 < mindiff) {
                S = ecurve[i+1];
                mindiff = diff + diff2;
            }
        }
    }
    else {
        for (i = from; i <= maxdiv-4; i++) {
            gdouble diff = 0.25*(fabs(ecurve[i+1] - ecurve[i])
                                 + fabs(ecurve[i+2] - ecurve[i+1])
                                 + fabs(ecurve[i+3] - ecurve[i+2])
                                 + fabs(ecurve[i+4] - ecurve[i+3]));
            gdouble diff2 = 0.5*(fabs(ecurve[i+1] + ecurve[i+4]
                                      - 2.0*ecurve[i+2]))/(G_LN2*G_LN2);
            if (diff + diff2 < mindiff) {
                S = (ecurve[i+1] + ecurve[i+2] + ecurve[i+3])/3.0;
                mindiff = diff + diff2;
            }
        }
    }

    return S;
}

/* This is what we get on average from all possible two-point configurations if
 * they are randomly distributed.  A fairly good estimate that in practice
 * seems to result in some deviation on the 5th significant digit, which is
 * hardly significant at all.  The contribution is the same in 1D and 2D. */
static void
add_estimated_unsplit_node_entropy(gdouble *S, guint maxdepth, gdouble w)
{
    gdouble q = 2.0*G_LN2*w;
    guint i;

    for (i = 0; i <= maxdepth; i++, S++) {
        S[0] += q;
        q *= 0.5;
    }
}

static BinTreeNode*
bin_tree_node_new(const gdouble pt)
{
    BinTreeNode *btnode = g_slice_new(BinTreeNode);
    btnode->u.pt.a = pt;
    btnode->count = 1;
    return btnode;
}

static void
bin_tree_add_node(BinTreeNode *btnode, const gdouble pt,
                  gdouble min, gdouble max, guint maxdepth)
{
    BinTreeNode *child;
    gdouble centre;
    guint i;

    /* We reached maximum allowed subdivision.  Just increase the count. */
    if (!maxdepth) {
        if (btnode->count <= 2)
            gwy_clear(&btnode->u, 1);
        btnode->count++;
        return;
    }

    /* We will descend into subtrees. */
    centre = 0.5*(min + max);

    /* If this node has just one point add the other there and we are done. */
    if (btnode->count == 1) {
        btnode->u.pt.b = pt;
        btnode->count++;
        return;
    }

    /* We will be recursing.  So if this node is a leaf start by making it
     * non-leaf. */
    if (btnode->count == 2) {
        gdouble pta = btnode->u.pt.a;
        gdouble ptb = btnode->u.pt.b;
        guint ia = (pta > centre);
        guint ib = (ptb > centre);

        gwy_clear(&btnode->u, 1);
        child = btnode->u.children[ia] = bin_tree_node_new(pta);
        /* Must distinguish between creating two child nodes and creating one
         * two-point child node. */
        if (ia == ib) {
            child->u.pt.b = ptb;
            child->count = 2;
        }
        else
            btnode->u.children[ib] = bin_tree_node_new(ptb);
    }

    /* Add the new point to the appropriate child. */
    i = (pt > centre);
    maxdepth--;
    btnode->count++;

    if ((child = btnode->u.children[i])) {
        /* Recurse.  This will end either by reaching maxdepth=0 or by
         * successful separation in the other branch of this conditon. */
        if (i == 0)
            bin_tree_add_node(child, pt, min, centre, maxdepth);
        else
            bin_tree_add_node(child, pt, centre, max, maxdepth);
    }
    else {
        /* There is nothing here yet.  Add the point as a new leaf. */
        btnode->u.children[i] = bin_tree_node_new(pt);
    }
}

static void
bin_tree_add(BinTree *btree, const gdouble pt)
{
    if (G_LIKELY(btree->root)) {
        bin_tree_add_node(btree->root, pt,
                          btree->min, btree->max, btree->maxdepth);
    }
    else
        btree->root = bin_tree_node_new(pt);
}

static void
bin_tree_find_range(BinTree *btree, const gdouble *xdata, guint n)
{
    gdouble min = G_MAXDOUBLE;
    gdouble max = -G_MAXDOUBLE;
    guint i;

    for (i = 0; i < n; i++) {
        gdouble x = xdata[i];

        if (x < min)
            min = x;
        if (x > max)
            max = x;
    }

    btree->min = min;
    btree->max = max;
}

static void
bin_tree_node_free(BinTreeNode *btnode)
{
    guint i;

    if (btnode->count > 2) {
        for (i = 0; i < G_N_ELEMENTS(btnode->u.children); i++) {
            if (btnode->u.children[i])
                bin_tree_node_free(btnode->u.children[i]);
        }
    }
    g_slice_free(BinTreeNode, btnode);
}

static void
bin_tree_free(BinTree *btree)
{
    if (!btree->degenerate)
        bin_tree_node_free(btree->root);
    g_free(btree);
}

static BinTree*
bin_tree_new(const gdouble *xdata, guint n, guint maxdepth)
{
    BinTree *btree;
    guint i;

    btree = g_new0(BinTree, 1);

    if (!maxdepth)
        maxdepth = 24;
    btree->maxdepth = maxdepth;

    bin_tree_find_range(btree, xdata, n);
    if (!(btree->min < btree->max)) {
        btree->degenerate = TRUE;
        btree->degenerateS = G_MAXDOUBLE;
        return btree;
    }

    /* Return explicit estimates for n < 4, making maxdiv at least 1 (with
     * half-scales included, ecurve will have at least 3 points then). */
    if (n == 2) {
        btree->degenerate = TRUE;
        btree->degenerateS = log(btree->max - btree->min);
        return btree;
    }
    if (n == 3) {
        btree->degenerate = TRUE;
        btree->degenerateS = (log(btree->max - btree->min)
                              + 0.5*log(1.5) - G_LN2/3.0);
        return btree;
    }

    for (i = 0; i < n; i++) {
        gdouble pt = xdata[i];
        bin_tree_add(btree, pt);
    }

    return btree;
}

static void
bin_tree_node_entropies_at_scales(BinTreeNode *btnode, guint maxdepth,
                                  gdouble *S, guint *unsplit)
{
    BinTreeNode *child;
    guint i;

    /* Singular points contribute to p*ln(p) always with zero.  So we can stop
     * recursion to finer subdivisions when count == 1. */
    if (btnode->count <= 1)
        return;

    if (!maxdepth) {
        S[0] += gwy_xlnx_int(btnode->count);
        return;
    }

    if (btnode->count == 2) {
        unsplit[0]++;
        return;
    }

    S[0] += gwy_xlnx_int(btnode->count);
    S++;

    maxdepth--;
    unsplit++;
    for (i = 0; i < G_N_ELEMENTS(btnode->u.children); i++) {
        if ((child = btnode->u.children[i]))
            bin_tree_node_entropies_at_scales(child, maxdepth, S, unsplit);
    }
}

static gdouble*
bin_tree_entropies_at_scales(BinTree *btree, guint maxdepth)
{
    gdouble *S;
    guint *unsplit;
    guint i, n, npts;
    gdouble Sscale;

    if (!maxdepth)
        maxdepth = btree->maxdepth;

    n = maxdepth + 1;
    S = g_new0(gdouble, n);

    if (btree->degenerate) {
        S[0] = btree->degenerateS;
        for (i = 1; i < n; i++)
            S[i] = S[i-1] - G_LN2;
        return S;
    }

    unsplit = g_new0(guint, maxdepth);
    bin_tree_node_entropies_at_scales(btree->root,
                                      MIN(maxdepth, btree->maxdepth),
                                      S, unsplit);

    for (i = 0; i < maxdepth; i++) {
        if (unsplit[i])
            add_estimated_unsplit_node_entropy(S + i, maxdepth - i, unsplit[i]);
    }
    g_free(unsplit);

    npts = btree->root->count;
    Sscale = log(npts*(btree->max - btree->min));
    for (i = 0; i < n; i++)
        S[i] = Sscale - i*G_LN2 - S[i]/npts;

    return S;
}

static gdouble*
calculate_entropy_at_scales(GwyDataField *dfield,
                            GwyDataField *mask,
                            GwyMaskingType mode,
                            gint col, gint row,
                            gint width, gint height,
                            guint *maxdiv,
                            gdouble *S)
{
    gint xres;
    guint i, j, n;
    gdouble *xdata;
    const gdouble *base;
    gboolean must_free_xdata = TRUE;
    gdouble *ecurve;
    BinTree *btree;

    if (mask) {
        gwy_data_field_area_count_in_range(mask, NULL, col, row, width, height,
                                           G_MAXDOUBLE, 1.0, NULL, &n);
        if (mode == GWY_MASK_EXCLUDE)
            n = width*height - n;
    }
    else
        n = width*height;

    if (!*maxdiv) {
        if (n >= 2)
            *maxdiv = (guint)floor(3.0*log(n)/G_LN2 + 1e-12);
        else
            *maxdiv = 2;

        /* We will run out of significant digits in coordinates after that. */
        *maxdiv = MIN(*maxdiv, 50);
    }

    if (n < 2) {
        ecurve = g_new(gdouble, *maxdiv+1);
        for (i = 0; i <= *maxdiv; i++)
            ecurve[i] = -G_MAXDOUBLE;
        if (S)
            *S = -G_MAXDOUBLE;
        return ecurve;
    }

    xres = dfield->xres;
    base = dfield->data + row*xres + col;
    if (n == xres*dfield->yres) {
        /* Handle the full-field case without allocating anything. */
        xdata = dfield->data;
        must_free_xdata = FALSE;
    }
    else {
        xdata = g_new(gdouble, n);
        if (mask) {
            const gdouble *mbase = mask->data + row*xres + col;
            const gboolean invert = (mode == GWY_MASK_EXCLUDE);
            guint k = 0;

            for (i = 0; i < height; i++) {
                const gdouble *d = base + i*xres;
                const gdouble *m = mbase + i*xres;
                for (j = width; j; j--, d++, m++) {
                    if ((*m < 1.0) == invert)
                        xdata[k++] = *d;
                }
            }
            g_assert(k == n);
        }
        else {
            for (i = 0; i < height; i++)
                gwy_assign(xdata + i*width, base + i*xres, width);
        }
    }

    btree = bin_tree_new(xdata, n, *maxdiv);
    if (must_free_xdata)
        g_free(xdata);

    ecurve = bin_tree_entropies_at_scales(btree, *maxdiv);
    if (S) {
        if (btree->degenerate)
            *S = btree->degenerateS;
        else
            *S = calculate_entropy_from_scaling(ecurve, *maxdiv);
    }
    bin_tree_free(btree);

    return ecurve;
}

/**
 * gwy_data_field_area_get_entropy_at_scales:
 * @data_field: A data field.
 * @target_line: A data line to store the result to.  It will be resampled to
 *               @maxdiv+1 items.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @mode: Masking mode to use.  See the introduction for description of
 *        masking modes.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @maxdiv: Maximum number of divisions of the value range.  Pass zero to
 *          choose it automatically.
 *
 * Calculates estimates of value distribution entropy at various scales.
 *
 * Returns: The best estimate, as gwy_data_field_area_get_entropy().
 *
 * Since: 2.44
 **/
gdouble
gwy_data_field_area_get_entropy_at_scales(GwyDataField *data_field,
                                          GwyDataLine *target_line,
                                          GwyDataField *mask,
                                          GwyMaskingType mode,
                                          gint col, gint row,
                                          gint width, gint height,
                                          gint maxdiv)
{
    GwySIUnit *lineunit;
    guint umaxdiv = (maxdiv > 0 ? maxdiv : 0);
    gdouble *ecurve;
    gdouble min, max, S = -G_MAXDOUBLE;
    gint i;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(data_field), S);
    g_return_val_if_fail(GWY_IS_DATA_LINE(target_line), S);
    g_return_val_if_fail(!mask || (GWY_IS_DATA_FIELD(mask)
                                   && mask->xres == data_field->xres
                                   && mask->yres == data_field->yres), S);
    g_return_val_if_fail(col >= 0 && row >= 0
                         && width >= 0 && height >= 0
                         && col + width <= data_field->xres
                         && row + height <= data_field->yres, S);

    ecurve = calculate_entropy_at_scales(data_field, mask, mode,
                                         col, row, width, height,
                                         &umaxdiv, &S);
    maxdiv = maxdiv ? maxdiv : umaxdiv + 1;
    gwy_data_line_resample(target_line, maxdiv, GWY_INTERPOLATION_NONE);
    target_line->real = maxdiv*G_LN2;
    for (i = 0; i < maxdiv; i++)
        target_line->data[maxdiv-1 - i] = ecurve[i];
    g_free(ecurve);

    gwy_data_field_area_get_min_max_mask(data_field, mask, mode,
                                         col, row, width, height,
                                         &min, &max);
    if (max > min)
        target_line->off = log(max - min) - (maxdiv - 0.5)*G_LN2;

    lineunit = gwy_data_line_get_si_unit_x(target_line);
    gwy_si_unit_set_from_string(lineunit, NULL);
    lineunit = gwy_data_line_get_si_unit_y(target_line);
    gwy_si_unit_set_from_string(lineunit, NULL);

    return S;
}

/**
 * gwy_data_field_get_entropy:
 * @data_field: A data field.
 *
 * Computes the entropy of a data field.
 *
 * See gwy_data_field_area_get_entropy() for the definition.
 *
 * This quantity is cached.
 *
 * Returns: The value distribution entropy.
 *
 * Since: 2.42
 **/
gdouble
gwy_data_field_get_entropy(GwyDataField *data_field)
{
    gdouble S = -G_MAXDOUBLE;
    gdouble *ecurve;
    guint maxdiv = 0;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(data_field), S);

    gwy_debug("%s", CTEST(data_field, ENT) ? "cache" : "lame");
    if (CTEST(data_field, ENT))
        return CVAL(data_field, ENT);

    ecurve = calculate_entropy_at_scales(data_field, NULL, GWY_MASK_IGNORE,
                                         0, 0,
                                         data_field->xres, data_field->yres,
                                         &maxdiv, &S);
    g_free(ecurve);

    CVAL(data_field, ENT) = S;
    data_field->cached |= CBIT(ENT);

    return S;
}

/**
 * gwy_data_field_area_get_entropy:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @mode: Masking mode to use.  See the introduction for description of
 *        masking modes.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Estimates the entropy of field data distribution.
 *
 * The estimate is calculated as @S = ln(@n Δ) − 1/@n ∑ @n_i ln(@n_i), where
 * @n is the number of pixels considered, Δ the bin size and @n_i the count in
 * the @i-th bin.  If @S is plotted as a function of the bin size Δ, it is,
 * generally, a growing function with a plateau for ‘reasonable’ bin sizes.
 * The estimate is taken at the plateau.  If no plateau is found, which means
 * the distribution is effectively a sum of δ-functions, -%G_MAXDOUBLE is
 * returned.
 *
 * It should be noted that this estimate may be biased.
 *
 * Returns: The estimated entropy of the data values.  The entropy of no data
 *          or a single single is returned as -%G_MAXDOUBLE.
 *
 * Since: 2.42
 **/
gdouble
gwy_data_field_area_get_entropy(GwyDataField *data_field,
                                GwyDataField *mask,
                                GwyMaskingType mode,
                                gint col, gint row,
                                gint width, gint height)
{
    gdouble S = -G_MAXDOUBLE;
    gdouble *ecurve;
    guint maxdiv = 0;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(data_field), S);
    g_return_val_if_fail(!mask || (GWY_IS_DATA_FIELD(mask)
                                   && mask->xres == data_field->xres
                                   && mask->yres == data_field->yres), S);
    g_return_val_if_fail(col >= 0 && row >= 0
                         && width >= 0 && height >= 0
                         && col + width <= data_field->xres
                         && row + height <= data_field->yres,
                         S);

    /* The result is the same, but it can be cached. */
    if ((!mask || mode == GWY_MASK_IGNORE)
        && row == 0 && col == 0
        && width == data_field->xres && height == data_field->yres)
        return gwy_data_field_get_entropy(data_field);

    ecurve = calculate_entropy_at_scales(data_field, mask, mode,
                                         col, row, width, height,
                                         &maxdiv, &S);
    g_free(ecurve);
    return S;
}

static QuadTreeNode*
quad_tree_node_new(const GwyXY *pt)
{
    QuadTreeNode *qtnode = g_slice_new(QuadTreeNode);
    qtnode->u.pt.a = *pt;
    qtnode->count = 1;
    return qtnode;
}

static void
quad_tree_add_node(QuadTreeNode *qtnode, const GwyXY *pt,
                   GwyXY min, GwyXY max, guint maxdepth)
{
    QuadTreeNode *child;
    GwyXY centre;
    guint i;

    /* We reached maximum allowed subdivision.  Just increase the count. */
    if (!maxdepth) {
        if (qtnode->count <= 2)
            gwy_clear(&qtnode->u, 1);
        qtnode->count++;
        return;
    }

    /* We will descend into subtrees. */
    centre.x = 0.5*(min.x + max.x);
    centre.y = 0.5*(min.y + max.y);

    /* If this node has just one point add the other there and we are done. */
    if (qtnode->count == 1) {
        qtnode->u.pt.b = *pt;
        qtnode->count++;
        return;
    }

    /* We will be recursing.  So if this node is a leaf start by making it
     * non-leaf. */
    if (qtnode->count == 2) {
        GwyXY pta = qtnode->u.pt.a;
        GwyXY ptb = qtnode->u.pt.b;
        guint ia = (pta.x > centre.x) + 2*(pta.y > centre.y);
        guint ib = (ptb.x > centre.x) + 2*(ptb.y > centre.y);

        gwy_clear(&qtnode->u, 1);
        child = qtnode->u.children[ia] = quad_tree_node_new(&pta);
        /* Must distinguish between creating two child nodes and creating one
         * two-point child node. */
        if (ia == ib) {
            child->u.pt.b = ptb;
            child->count = 2;
        }
        else
            qtnode->u.children[ib] = quad_tree_node_new(&ptb);
    }

    /* Add the new point to the appropriate child. */
    i = (pt->x > centre.x) + 2*(pt->y > centre.y);
    maxdepth--;
    qtnode->count++;

    if ((child = qtnode->u.children[i])) {
        /* Recurse.  This will end either by reaching maxdepth=0 or by
         * successful separation in the other branch of this conditon. */
        if (i == 0)
            quad_tree_add_node(child, pt, min, centre, maxdepth);
        else if (i == 1) {
            min.x = centre.x;
            max.y = centre.y;
            quad_tree_add_node(child, pt, min, max, maxdepth);
        }
        else if (i == 2) {
            max.x = centre.x;
            min.y = centre.y;
            quad_tree_add_node(child, pt, min, max, maxdepth);
        }
        else
            quad_tree_add_node(child, pt, centre, max, maxdepth);
    }
    else {
        /* There is nothing here yet.  Add the point as a new leaf. */
        qtnode->u.children[i] = quad_tree_node_new(pt);
    }
}

static void
quad_tree_add(QuadTree *qtree, const GwyXY *pt)
{
    if (G_LIKELY(qtree->root)) {
        quad_tree_add_node(qtree->root, pt,
                           qtree->min, qtree->max, qtree->maxdepth);
    }
    else
        qtree->root = quad_tree_node_new(pt);
}

static void
quad_tree_find_range(QuadTree *qtree,
                     const gdouble *xdata, const gdouble *ydata, guint n)
{
    GwyXY min = { G_MAXDOUBLE, G_MAXDOUBLE };
    GwyXY max = { -G_MAXDOUBLE, -G_MAXDOUBLE };
    guint i;

    for (i = 0; i < n; i++) {
        gdouble x = xdata[i];
        gdouble y = ydata[i];

        if (x < min.x)
            min.x = x;
        if (x > max.x)
            max.x = x;
        if (y < min.y)
            min.y = y;
        if (y > max.y)
            max.y = y;
    }

    qtree->min = min;
    qtree->max = max;
}

static void
quad_tree_node_free(QuadTreeNode *qtnode)
{
    guint i;

    if (qtnode->count > 2) {
        for (i = 0; i < G_N_ELEMENTS(qtnode->u.children); i++) {
            if (qtnode->u.children[i])
                quad_tree_node_free(qtnode->u.children[i]);
        }
    }
    g_slice_free(QuadTreeNode, qtnode);
}

static void
quad_tree_free(QuadTree *qtree)
{
    quad_tree_node_free(qtree->root);
    g_free(qtree);
}

static QuadTree*
quad_tree_new(const gdouble *xdata, const gdouble *ydata, guint n,
              guint maxdepth)
{
    QuadTree *qtree;
    guint i;

    qtree = g_new0(QuadTree, 1);

    if (!maxdepth)
        maxdepth = 16;
    qtree->maxdepth = maxdepth;

    quad_tree_find_range(qtree, xdata, ydata, n);
    if (!(qtree->min.x < qtree->max.x) || !(qtree->min.y < qtree->max.y)) {
        qtree->degenerate = TRUE;
        qtree->degenerateS = G_MAXDOUBLE;
        return qtree;
    }

    /* Return explicit estimates for n < 4, making maxdiv at least 1 (with
     * half-scales included, ecurve will have at least 3 points then). */
    if (n == 2) {
        qtree->degenerate = TRUE;
        qtree->degenerateS = (log(qtree->max.x - qtree->min.x)
                               + log(qtree->max.y - qtree->min.y));
        return qtree;
    }
    if (n == 3) {
        qtree->degenerate = TRUE;
        qtree->degenerateS = (log(qtree->max.x - qtree->min.x)
                               + log(qtree->max.y - qtree->min.y)
                               + 0.5*log(1.5) - 2.0*G_LN2/3.0);
        return qtree;
    }

    for (i = 0; i < n; i++) {
        GwyXY pt = { xdata[i], ydata[i] };
        quad_tree_add(qtree, &pt);
    }

    return qtree;
}

static gdouble
quad_tree_node_half_scale_entropy(QuadTreeNode *qtnode)
{
    QuadTreeNode *child;
    guint cnt[G_N_ELEMENTS(qtnode->u.children)] = { 0, 0, 0, 0 };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(qtnode->u.children); i++) {
        if ((child = qtnode->u.children[i]))
            cnt[i] = child->count;
    }
    return 0.5*(gwy_xlnx_int(cnt[0] + cnt[1])
                + gwy_xlnx_int(cnt[2] + cnt[3])
                + gwy_xlnx_int(cnt[0] + cnt[2])
                + gwy_xlnx_int(cnt[1] + cnt[3]));
}

static void
quad_tree_node_entropies_at_scales(QuadTreeNode *qtnode, guint maxdepth,
                                   gdouble *S, guint *unsplit)
{
    QuadTreeNode *child;
    guint i;

    /* Singular points contribute to p*ln(p) always with zero.  So we can stop
     * recursion to finer subdivisions when count == 1. */
    if (qtnode->count <= 1)
        return;

    if (!maxdepth) {
        S[0] += gwy_xlnx_int(qtnode->count);
        return;
    }

    if (qtnode->count == 2) {
        unsplit[0]++;
        return;
    }

    S[0] += gwy_xlnx_int(qtnode->count);
    S++;

    // Half-scale entropies we estimate as averages of horizontal and vertical
    // binning.
    S[0] += quad_tree_node_half_scale_entropy(qtnode);
    S++;

    maxdepth--;
    unsplit++;
    for (i = 0; i < G_N_ELEMENTS(qtnode->u.children); i++) {
        if ((child = qtnode->u.children[i]))
            quad_tree_node_entropies_at_scales(child, maxdepth, S, unsplit);
    }
}

static gdouble*
quad_tree_entropies_at_scales(QuadTree *qtree, guint maxdepth)
{
    gdouble *S;
    guint *unsplit;
    guint i, n, npts;
    gdouble Sscale;

    if (!maxdepth)
        maxdepth = qtree->maxdepth;

    n = 2*maxdepth + 1;
    S = g_new0(gdouble, n);
    unsplit = g_new0(guint, maxdepth);
    quad_tree_node_entropies_at_scales(qtree->root,
                                       MIN(maxdepth, qtree->maxdepth),
                                       S, unsplit);

    for (i = 0; i < maxdepth; i++) {
        if (unsplit[i])
            add_estimated_unsplit_node_entropy(S + 2*i, 2*(maxdepth - i),
                                               unsplit[i]);
    }
    g_free(unsplit);

    npts = qtree->root->count;
    Sscale = log(npts
                 *(qtree->max.x - qtree->min.x)*(qtree->max.y - qtree->min.y));
    for (i = 0; i < n; i++)
        S[i] = Sscale - i*G_LN2 - S[i]/npts;

    return S;
}

static gdouble*
calculate_entropy_2d_at_scales(GwyDataField *xfield,
                               GwyDataField *yfield,
                               guint *maxdiv,
                               gdouble *S)
{
    guint xres, yres, n, i;
    gdouble *ecurve;
    QuadTree *qtree;

    xres = xfield->xres;
    yres = xfield->yres;
    n = xres*yres;

    if (!*maxdiv) {
        if (n >= 2)
            *maxdiv = (guint)floor(1.5*log(n)/G_LN2 + 1e-12);
        else
            *maxdiv = 1;

        /* We will run out of significant digits in coordinates after that. */
        *maxdiv = MIN(*maxdiv, 50);
    }

    if (n < 2) {
        ecurve = g_new(gdouble, *maxdiv+1);
        for (i = 0; i <= *maxdiv; i++)
            ecurve[i] = -G_MAXDOUBLE;
        if (S)
            *S = -G_MAXDOUBLE;
        return ecurve;
    }

    qtree = quad_tree_new(xfield->data, yfield->data, n, *maxdiv);
    ecurve = quad_tree_entropies_at_scales(qtree, *maxdiv);
    if (S) {
        if (qtree->degenerate)
            *S = qtree->degenerateS;
        else
            *S = calculate_entropy_from_scaling(ecurve, 2*(*maxdiv));
    }
    quad_tree_free(qtree);

    return ecurve;
}

/**
 * gwy_data_field_get_entropy_2d_at_scales:
 * @xfield: A data field containing the @x-coordinates.
 * @yfield: A data field containing the @y-coordinates.
 * @target_line: A data line to store the result to.  It will be resampled to
 *               @maxdiv+1 items.
 * @maxdiv: Maximum number of divisions of the value range.  Pass zero to
 *          choose it automatically.
 *
 * Calculates estimates of entropy of two-dimensional point cloud at various
 * scales.
 *
 * Returns: The best estimate, as gwy_data_field_get_entropy_2d().
 *
 * Since: 2.44
 **/
gdouble
gwy_data_field_get_entropy_2d_at_scales(GwyDataField *xfield,
                                        GwyDataField *yfield,
                                        GwyDataLine *target_line,
                                        gint maxdiv)
{
    GwySIUnit *lineunit;
    guint umaxdiv = (maxdiv > 0 ? maxdiv/2 : 0);
    gdouble *ecurve;
    gdouble xmin, xmax, ymin, ymax, S = -G_MAXDOUBLE;
    gint i;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(xfield), S);
    g_return_val_if_fail(GWY_IS_DATA_FIELD(yfield), S);
    g_return_val_if_fail(GWY_IS_DATA_LINE(target_line), S);
    g_return_val_if_fail(xfield->xres == yfield->xres, S);
    g_return_val_if_fail(xfield->yres == yfield->yres, S);

    ecurve = calculate_entropy_2d_at_scales(xfield, yfield, &umaxdiv, &S);
    maxdiv = maxdiv ? maxdiv : 2*umaxdiv + 1;
    gwy_data_line_resample(target_line, maxdiv, GWY_INTERPOLATION_NONE);
    target_line->real = maxdiv*G_LN2;
    for (i = 0; i < maxdiv; i++)
        target_line->data[maxdiv-1 - i] = ecurve[i];
    g_free(ecurve);

    gwy_data_field_get_min_max(xfield, &xmin, &xmax);
    gwy_data_field_get_min_max(xfield, &ymin, &ymax);
    if ((xmax > xmin) && (ymax > ymin))
        target_line->off = (log((xmax - xmin)*(ymax - ymin))
                            - (maxdiv - 0.5)*G_LN2);

    lineunit = gwy_data_line_get_si_unit_x(target_line);
    gwy_si_unit_set_from_string(lineunit, NULL);
    lineunit = gwy_data_line_get_si_unit_y(target_line);
    gwy_si_unit_set_from_string(lineunit, NULL);

    return S;
}

/**
 * gwy_data_field_get_entropy_2d:
 * @xfield: A data field containing the @x-coordinates.
 * @yfield: A data field containing the @y-coordinates.
 *
 * Computes the entropy of a two-dimensional point cloud.
 *
 * Each pair of corresponding @xfield and @yfield pixels is assumed to
 * represent the coordinates (@x,@y) of a point in plane.  Hence they must have
 * the same dimensions.
 *
 * Returns: The two-dimensional distribution entropy.
 *
 * Since: 2.44
 **/
gdouble
gwy_data_field_get_entropy_2d(GwyDataField *xfield,
                              GwyDataField *yfield)
{
    gdouble *ecurve;
    guint maxdiv = 0;
    gdouble S = -G_MAXDOUBLE;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(xfield), S);
    g_return_val_if_fail(GWY_IS_DATA_FIELD(yfield), S);
    g_return_val_if_fail(xfield->xres == yfield->xres, S);
    g_return_val_if_fail(xfield->yres == yfield->yres, S);

    ecurve = calculate_entropy_2d_at_scales(xfield, yfield, &maxdiv, &S);
    g_free(ecurve);

    return S;
}

/**
 * gwy_data_field_slope_distribution:
 * @data_field: A data field.
 * @derdist: A data line to fill with angular slope distribution. Its
 *           resolution determines resolution of the distribution.
 * @kernel_size: If positive, local plane fitting will be used for slope
 *               computation; if nonpositive, plain central derivations
 *               will be used.
 *
 * Computes angular slope distribution.
 **/
void
gwy_data_field_slope_distribution(GwyDataField *dfield,
                                  GwyDataLine *derdist,
                                  gint kernel_size)
{
    GwySIUnit *lineunit;
    gdouble *data, *der;
    gdouble bx, by, phi;
    gint xres, yres, nder;
    gint col, row, iphi;

    g_return_if_fail(GWY_IS_DATA_FIELD(dfield));
    g_return_if_fail(GWY_IS_DATA_LINE(derdist));

    nder = gwy_data_line_get_res(derdist);
    der = gwy_data_line_get_data(derdist);
    data = dfield->data;
    xres = dfield->xres;
    yres = dfield->yres;
    gwy_clear(der, nder);
    if (kernel_size > 0) {
        for (row = 0; row + kernel_size < yres; row++) {
            for (col = 0; col + kernel_size < xres; col++) {
                gwy_data_field_area_fit_plane(dfield, NULL, col, row,
                                              kernel_size, kernel_size,
                                              NULL, &bx, &by);
                phi = atan2(by, bx);
                iphi = (gint)floor(nder*(phi + G_PI)/(2.0*G_PI));
                iphi = CLAMP(iphi, 0, nder-1);
                der[iphi] += hypot(bx, by);
            }
        }
    }
    else {
        gdouble qx = xres/gwy_data_field_get_xreal(dfield);
        gdouble qy = yres/gwy_data_field_get_yreal(dfield);

        for (row = 1; row + 1 < yres; row++) {
            for (col = 1; col + 1 < xres; col++) {
                bx = data[row*xres + col + 1] - data[row*xres + col - 1];
                by = data[row*xres + xres + col] - data[row*xres - xres + col];
                phi = atan2(by*qy, bx*qx);
                iphi = (gint)floor(nder*(phi + G_PI)/(2.0*G_PI));
                iphi = CLAMP(iphi, 0, nder-1);
                der[iphi] += hypot(bx, by);
            }
        }
    }

    /* Set proper units */
    lineunit = gwy_data_line_get_si_unit_x(derdist);
    gwy_si_unit_set_from_string(lineunit, NULL);
    lineunit = gwy_data_line_get_si_unit_y(derdist);
    gwy_si_unit_divide(gwy_data_field_get_si_unit_z(dfield),
                       gwy_data_field_get_si_unit_xy(dfield),
                       lineunit);
}

/**
 * gwy_data_field_area_get_median:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Computes median value of a data field area.
 *
 * This function is equivalent to calling
 * @gwy_data_field_area_get_median_mask()
 * with masking mode %GWY_MASK_INCLUDE.
 *
 * Returns: The median value.
 **/
gdouble
gwy_data_field_area_get_median(GwyDataField *dfield,
                               GwyDataField *mask,
                               gint col, gint row,
                               gint width, gint height)
{
    return gwy_data_field_area_get_median_mask(dfield, mask, GWY_MASK_INCLUDE,
                                               col, row, width, height);
}

/**
 * gwy_data_field_area_get_median_mask:
 * @data_field: A data field.
 * @mask: Mask specifying which values to take into account/exclude, or %NULL.
 * @mode: Masking mode to use.  See the introduction for description of
 *        masking modes.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 *
 * Computes median value of a data field area.
 *
 * Returns: The median value.
 *
 * Since: 2.18
 **/
gdouble
gwy_data_field_area_get_median_mask(GwyDataField *dfield,
                                    GwyDataField *mask,
                                    GwyMaskingType mode,
                                    gint col, gint row,
                                    gint width, gint height)
{
    gdouble median = 0.0;
    const gdouble *datapos, *mpos;
    gdouble *buffer;
    gint i, j;
    guint nn;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(dfield), median);
    g_return_val_if_fail(!mask || (GWY_IS_DATA_FIELD(mask)
                                   && mask->xres == dfield->xres
                                   && mask->yres == dfield->yres),
                         median);
    g_return_val_if_fail(col >= 0 && row >= 0
                         && width >= 0 && height >= 0
                         && col + width <= dfield->xres
                         && row + height <= dfield->yres,
                         median);
    if (!width || !height)
        return median;

    if (mask && mode != GWY_MASK_IGNORE) {
        buffer = g_new(gdouble, width*height);
        datapos = dfield->data + row*dfield->xres + col;
        mpos = mask->data + row*mask->xres + col;
        nn = 0;
        for (i = 0; i < height; i++) {
            const gdouble *drow = datapos + i*dfield->xres;
            const gdouble *mrow = mpos + i*mask->xres;

            if (mode == GWY_MASK_INCLUDE) {
                for (j = 0; j < width; j++) {
                    if (*mrow > 0.0) {
                        buffer[nn] = *drow;
                        nn++;
                    }
                    drow++;
                    mrow++;
                }
            }
            else {
                for (j = 0; j < width; j++) {
                    if (*mrow < 1.0) {
                        buffer[nn] = *drow;
                        nn++;
                    }
                    drow++;
                    mrow++;
                }
            }
        }

        if (nn)
            median = gwy_math_median(nn, buffer);

        g_free(buffer);

        return median;
    }

    if (col == 0 && width == dfield->xres
        && row == 0 && height == dfield->yres)
        return gwy_data_field_get_median(dfield);

    buffer = g_new(gdouble, width*height);
    datapos = dfield->data + row*dfield->xres + col;
    if (height == 1 || (col == 0 && width == dfield->xres))
        gwy_assign(buffer, datapos, width*height);
    else {
        for (i = 0; i < height; i++)
            gwy_assign(buffer + i*width, datapos + i*dfield->xres, width);
    }
    median = gwy_math_median(width*height, buffer);
    g_free(buffer);

    return median;
}

/**
 * gwy_data_field_get_median:
 * @data_field: A data field.
 *
 * Computes median value of a data field.
 *
 * This quantity is cached.
 *
 * Returns: The median value.
 **/
gdouble
gwy_data_field_get_median(GwyDataField *data_field)
{
    gint xres, yres;
    gdouble *buffer;
    gdouble med;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(data_field), 0.0);

    gwy_debug("%s", CTEST(data_field, MED) ? "cache" : "lame");
    if (CTEST(data_field, MED))
        return CVAL(data_field, MED);

    xres = data_field->xres;
    yres = data_field->yres;
    buffer = g_memdup(data_field->data, xres*yres*sizeof(gdouble));
    med = gwy_math_median(xres*yres, buffer);
    g_free(buffer);

    CVAL(data_field, MED) = med;
    data_field->cached |= CBIT(MED);

    return med;
}

/**
 * gwy_data_field_area_get_normal_coeffs:
 * @data_field: A data field.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @nx: Where x-component of average normal vector should be stored, or %NULL.
 * @ny: Where y-component of average normal vector should be stored, or %NULL.
 * @nz: Where z-component of average normal vector should be stored, or %NULL.
 * @normalize1: true to normalize the normal vector to 1, false to normalize
 *              the vector so that z-component is 1.
 *
 * Computes average normal vector of an area of a data field.
 **/
void
gwy_data_field_area_get_normal_coeffs(GwyDataField *data_field,
                                      gint col, gint row,
                                      gint width, gint height,
                                      gdouble *nx, gdouble *ny, gdouble *nz,
                                      gboolean normalize1)
{
    gint i, j;
    int ctr = 0;
    gdouble d1x, d1y, d1z, d2x, d2y, d2z, dcx, dcy, dcz, dd;
    gdouble sumdx = 0.0, sumdy = 0.0, sumdz = 0.0, sumw = 0.0;
    gdouble avgdx, avgdy, avgdz;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    g_return_if_fail(col >= 0 && row >= 0
                     && width > 0 && height > 0
                     && col + width <= data_field->xres
                     && row + height <= data_field->yres);
    /* This probably should not be enforced */
    /*
    g_return_if_fail(gwy_si_unit_equal(gwy_data_field_get_si_unit_xy(a),
                                       gwy_data_field_get_si_unit_z(a)),
                     FALSE);
                     */

    for (i = col; i < col + width; i++) {
        for (j = row; j < row + height; j++) {
            d1x = 1.0;
            d1y = 0.0;
            d1z = gwy_data_field_get_xder(data_field, i, j);
            d2x = 0.0;
            d2y = 1.0;
            d2z = gwy_data_field_get_yder(data_field, i, j);
            /* Cross product = normal vector */
            dcx = d1y*d2z - d1z*d2y;
            dcy = d1z*d2x - d1x*d2z;
            dcz = d1x*d2y - d1y*d2x; /* Always 1 */
            /* Normalize and add */
            dd = sqrt(dcx*dcx + dcy*dcy + dcz*dcz);
            dcx /= dd;
            sumdx += dcx;
            dcy /= dd;
            sumdy += dcy;
            dcz /= dd;
            sumdz += dcz;
            sumw += 1.0/dd;
            ctr++;
        }
    }
    /* average dimensionless normal vector */
    if (normalize1) {
        /* normalize to 1 */
        avgdx = sumdx/ctr;
        avgdy = sumdy/ctr;
        avgdz = sumdz/ctr;
    }
    else {
        /* normalize for gwy_data_field_plane_level */
        avgdx = sumdx/sumw;
        avgdy = sumdy/sumw;
        avgdz = sumdz/sumw;
    }

    if (nx)
        *nx = avgdx;
    if (ny)
        *ny = avgdy;
    if (nz)
        *nz = avgdz;
}


/**
 * gwy_data_field_get_normal_coeffs:
 * @data_field: A data field.
 * @nx: Where x-component of average normal vector should be stored, or %NULL.
 * @ny: Where y-component of average normal vector should be stored, or %NULL.
 * @nz: Where z-component of average normal vector should be stored, or %NULL.
 * @normalize1: true to normalize the normal vector to 1, false to normalize
 *              the vector so that z-component is 1.
 *
 * Computes average normal vector of a data field.
 **/
void
gwy_data_field_get_normal_coeffs(GwyDataField *data_field,
                                 gdouble *nx, gdouble *ny, gdouble *nz,
                                 gboolean normalize1)
{
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    gwy_data_field_area_get_normal_coeffs(data_field,
                                          0, 0,
                                          data_field->xres, data_field->yres,
                                          nx, ny, nz, normalize1);
}


/**
 * gwy_data_field_area_get_inclination:
 * @data_field: A data field.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @theta: Where theta angle (in radians) should be stored, or %NULL.
 * @phi: Where phi angle (in radians) should be stored, or %NULL.
 *
 * Calculates the inclination of the image (polar and azimuth angle).
 **/
void
gwy_data_field_area_get_inclination(GwyDataField *data_field,
                                    gint col, gint row,
                                    gint width, gint height,
                                    gdouble *theta,
                                    gdouble *phi)
{
    gdouble nx, ny, nz, nr;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    g_return_if_fail(col >= 0 && row >= 0
                     && width > 0 && height > 0
                     && col + width <= data_field->xres
                     && row + height <= data_field->yres);
    gwy_data_field_area_get_normal_coeffs(data_field,
                                          col, row, width, height,
                                          &nx, &ny, &nz, TRUE);

    nr = hypot(nx, ny);
    if (theta)
        *theta = atan2(nr, nz);
    if (phi)
        *phi = atan2(ny, nx);
}


/**
 * gwy_data_field_get_inclination:
 * @data_field: A data field.
 * @theta: Where theta angle (in radians) should be stored, or %NULL.
 * @phi: Where phi angle (in radians) should be stored, or %NULL.
 *
 * Calculates the inclination of the image (polar and azimuth angle).
 **/
void
gwy_data_field_get_inclination(GwyDataField *data_field,
                               gdouble *theta,
                               gdouble *phi)
{
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    gwy_data_field_area_get_inclination(data_field,
                                        0, 0,
                                        data_field->xres, data_field->yres,
                                        theta,
                                        phi);
}

static gint
extract_field_row_masked(GwyDataField *dfield,
                         GwyDataField *mask,
                         GwyMaskingType masking,
                         gdouble *values,
                         gint col, gint row, gint width)
{
    gint xres = dfield->xres;
    const gdouble *d = dfield->data + row*xres + col, *m;
    gint i, n;

    if (!mask)
        masking = GWY_MASK_IGNORE;

    if (masking == GWY_MASK_INCLUDE) {
        m = mask->data + row*xres + col;
        for (i = n = 0; i < width; i++) {
            if (m[i] > 0.0)
                values[n++] = d[i];
        }
    }
    else if (masking == GWY_MASK_EXCLUDE) {
        m = mask->data + row*xres + col;
        for (i = n = 0; i < width; i++) {
            if (m[i] <= 0.0)
                values[n++] = d[i];
        }
    }
    else {
        n = width;
        gwy_assign(values, d, n);
    }

    return n;
}

static gint
extract_field_column_masked(GwyDataField *dfield,
                            GwyDataField *mask,
                            GwyMaskingType masking,
                            gdouble *values,
                            gint col, gint row, gint height)
{
    gint xres = dfield->xres;
    const gdouble *d = dfield->data + row*xres + col, *m;
    gint i, n;

    if (!mask)
        masking = GWY_MASK_IGNORE;

    if (masking == GWY_MASK_INCLUDE) {
        m = mask->data + row*xres + col;
        for (i = n = 0; i < height; i++) {
            if (m[xres*i] > 0.0)
                values[n++] = d[xres*i];
        }
    }
    else if (masking == GWY_MASK_EXCLUDE) {
        m = mask->data + row*xres + col;
        for (i = n = 0; i < height; i++) {
            if (m[xres*i] <= 0.0)
                values[n++] = d[xres*i];
        }
    }
    else {
        n = height;
        for (i = 0; i < height; i++)
            values[i] = d[xres*i];
    }

    return n;
}

static void
calc_field_row_linestat_masked(GwyDataField *dfield,
                               GwyDataField *mask,
                               GwyMaskingType masking,
                               GwyDataLine *dline,
                               GwyDataLine *weights,
                               LineStatFunc func,
                               gdouble filler_value,
                               gint col, gint row,
                               gint width, gint height)
{
    GwyDataLine *buf;
    gint i, n;
    gdouble *ldata, *wdata, *bufdata;
    gdouble dx = gwy_data_field_get_xmeasure(dfield);

    gwy_data_line_resample(dline, height, GWY_INTERPOLATION_NONE);
    gwy_data_line_set_real(dline, gwy_data_field_itor(dfield, height));
    gwy_data_line_set_offset(dline, gwy_data_field_itor(dfield, row));
    ldata = dline->data;

    if (weights) {
        gwy_data_line_resample(weights, height, GWY_INTERPOLATION_NONE);
        gwy_data_line_set_real(weights, gwy_data_field_itor(dfield, height));
        gwy_data_line_set_offset(weights, gwy_data_field_itor(dfield, row));
        gwy_data_line_clear(weights);
        wdata = weights->data;
    }
    else
        wdata = NULL;

    buf = gwy_data_line_new(width, width*dx, FALSE);
    bufdata = buf->data;

    for (i = 0; i < height; i++) {
        n = extract_field_row_masked(dfield, mask, masking, bufdata,
                                     col, row + i, width);
        if (n) {
            /* Temporarily shorten the dataline to avoid reallocations. */
            buf->res = n;
            buf->real = n*dx;
            ldata[i] = func(buf);
            buf->res = width;
            buf->real = width*dx;
            if (wdata)
                wdata[i] = n;
        }
        else
            ldata[i] = filler_value;
    }

    g_object_unref(buf);
}

static void
calc_field_column_linestat_masked(GwyDataField *dfield,
                                  GwyDataField *mask,
                                  GwyMaskingType masking,
                                  GwyDataLine *dline,
                                  GwyDataLine *weights,
                                  LineStatFunc func,
                                  gdouble filler_value,
                                  gint col, gint row,
                                  gint width, gint height)
{
    GwyDataLine *buf;
    gint i, n;
    gdouble *ldata, *wdata, *bufdata;
    gdouble dy = gwy_data_field_get_ymeasure(dfield);

    gwy_data_line_resample(dline, width, GWY_INTERPOLATION_NONE);
    gwy_data_line_set_real(dline, gwy_data_field_jtor(dfield, width));
    gwy_data_line_set_offset(dline, gwy_data_field_jtor(dfield, col));
    ldata = dline->data;

    if (weights) {
        gwy_data_line_resample(weights, width, GWY_INTERPOLATION_NONE);
        gwy_data_line_set_real(weights, gwy_data_field_jtor(dfield, width));
        gwy_data_line_set_offset(weights, gwy_data_field_jtor(dfield, col));
        gwy_data_line_clear(weights);
        wdata = weights->data;
    }
    else
        wdata = NULL;

    buf = gwy_data_line_new(height, height*dy, FALSE);
    bufdata = buf->data;

    for (i = 0; i < width; i++) {
        n = extract_field_column_masked(dfield, mask, masking, bufdata,
                                        col + i, row, height);
        if (n) {
            /* Temporarily shorten the dataline to avoid reallocations. */
            buf->res = n;
            buf->real = n*dy;
            ldata[i] = func(buf);
            buf->res = height;
            buf->real = height*dy;
            if (wdata)
                wdata[i] = n;
        }
        else
            ldata[i] = filler_value;
    }

    g_object_unref(buf);
}

static gdouble
gwy_data_line_get_slope(GwyDataLine *dline)
{
    gdouble v;

    gwy_data_line_get_line_coeffs(dline, NULL, &v);
    return v*dline->res/dline->real;
}

static gdouble
gwy_data_line_get_range(GwyDataLine *dline)
{
    gdouble min, max;

    gwy_data_line_get_min_max(dline, &min, &max);
    return max - min;
}

static gdouble
gwy_data_line_get_median_destructive(GwyDataLine *dline)
{
    return gwy_math_median(dline->res, dline->data);
}

static gdouble
gwy_data_line_get_Rt_destructive(GwyDataLine *dline)
{
    gwy_data_line_add(dline, -gwy_data_line_get_avg(dline));
    return gwy_data_line_get_xtm(dline, 1, 1);
}

static gdouble
gwy_data_line_get_Rz_destructive(GwyDataLine *dline)
{
    gwy_data_line_add(dline, -gwy_data_line_get_avg(dline));
    return gwy_data_line_get_xtm(dline, 5, 1);
}

/**
 * gwy_data_field_get_line_stats_mask:
 * @data_field: A data field.
 * @mask: Mask of values to take values into account, or %NULL for full
 *        @data_field.
 * @masking: Masking mode to use.  See the introduction for description of
 *           masking modes.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to the number of rows (columns).
 * @weights: A data line to store number of data points contributing to each
 *           value in @target_line, or %NULL.  It is useful when masking is
 *           used to possibly exclude values calculated from too few data
 *           points.
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @quantity: The line quantity to calulate for each row (column).
 * @orientation: Line orientation.  For %GWY_ORIENTATION_HORIZONTAL each
 *               @target_line point corresponds to a row of the area,
 *               for %GWY_ORIENTATION_VERTICAL each @target_line point
 *               corresponds to a column of the area.
 *
 * Calculates a line quantity for each row or column in a data field area.
 *
 * Since: 2.46
 **/
void
gwy_data_field_get_line_stats_mask(GwyDataField *data_field,
                                   GwyDataField *mask,
                                   GwyMaskingType masking,
                                   GwyDataLine *target_line,
                                   GwyDataLine *weights,
                                   gint col, gint row,
                                   gint width, gint height,
                                   GwyLineStatQuantity quantity,
                                   GwyOrientation orientation)
{
    static const LineStatFunc funcs[] = {
        gwy_data_line_get_avg,
        gwy_data_line_get_median_destructive,
        gwy_data_line_get_min,
        gwy_data_line_get_max,
        gwy_data_line_get_rms,
        gwy_data_line_get_length,
        gwy_data_line_get_slope,
        gwy_data_line_get_tan_beta0,
        gwy_data_line_get_ra,
        gwy_data_line_get_Rz_destructive,
        gwy_data_line_get_Rt_destructive,
        gwy_data_line_get_skew,
        gwy_data_line_get_kurtosis,
        gwy_data_line_get_range,
        gwy_data_line_get_variation,
    };

    LineStatFunc func;
    GwySIUnit *zunit, *xyunit, *lunit;
    gint xres, yres;

    g_return_if_fail(quantity < G_N_ELEMENTS(funcs));
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    g_return_if_fail(GWY_IS_DATA_LINE(target_line));
    g_return_if_fail(!mask || GWY_IS_DATA_FIELD(mask));
    g_return_if_fail(col >= 0 && row >= 0
                     && width > 0 && height > 0
                     && col + width <= data_field->xres
                     && row + height <= data_field->yres);

    func = funcs[quantity];
    xres = data_field->xres;
    yres = data_field->yres;

    if (mask) {
        g_return_if_fail(mask->xres == xres);
        g_return_if_fail(mask->yres == yres);
    }

    if (orientation == GWY_ORIENTATION_VERTICAL) {
        calc_field_column_linestat_masked(data_field, mask, masking,
                                          target_line, weights, func, 0.0,
                                          col, row, width, height);
    }
    else {
        calc_field_row_linestat_masked(data_field, mask, masking,
                                       target_line, weights, func, 0.0,
                                       col, row, width, height);
    }

    xyunit = gwy_data_field_get_si_unit_xy(data_field);
    zunit = gwy_data_field_get_si_unit_z(data_field);

    lunit = gwy_data_line_get_si_unit_x(target_line);
    gwy_serializable_clone(G_OBJECT(xyunit), G_OBJECT(lunit));

    lunit = gwy_data_line_get_si_unit_y(target_line);
    switch (quantity) {
        case GWY_LINE_STAT_LENGTH:
        if (!gwy_si_unit_equal(xyunit, zunit))
            g_warning("Length makes no sense when lateral and value units "
                      "differ");
        case GWY_LINE_STAT_MEAN:
        case GWY_LINE_STAT_MEDIAN:
        case GWY_LINE_STAT_MINIMUM:
        case GWY_LINE_STAT_MAXIMUM:
        case GWY_LINE_STAT_RMS:
        case GWY_LINE_STAT_RA:
        case GWY_LINE_STAT_RT:
        case GWY_LINE_STAT_RZ:
        case GWY_LINE_STAT_RANGE:
        case GWY_LINE_STAT_VARIATION:
        gwy_serializable_clone(G_OBJECT(zunit), G_OBJECT(lunit));
        break;

        case GWY_LINE_STAT_SLOPE:
        case GWY_LINE_STAT_TAN_BETA0:
        case GWY_LINE_STAT_SKEW:
        case GWY_LINE_STAT_KURTOSIS:
        gwy_si_unit_divide(zunit, xyunit, lunit);
        break;

        default:
        g_assert_not_reached();
        break;
    }

    if (weights) {
        lunit = gwy_data_line_get_si_unit_x(weights);
        gwy_serializable_clone(G_OBJECT(xyunit), G_OBJECT(lunit));
        lunit = gwy_data_line_get_si_unit_y(weights);
        gwy_si_unit_set_from_string(lunit, NULL);
    }
}

/**
 * gwy_data_field_area_get_line_stats:
 * @data_field: A data field.
 * @mask: Mask of values to take values into account, or %NULL for full
 *        @data_field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to the number of rows (columns).
 * @col: Upper-left column coordinate.
 * @row: Upper-left row coordinate.
 * @width: Area width (number of columns).
 * @height: Area height (number of rows).
 * @quantity: The line quantity to calulate for each row (column).
 * @orientation: Line orientation.  For %GWY_ORIENTATION_HORIZONTAL each
 *               @target_line point corresponds to a row of the area,
 *               for %GWY_ORIENTATION_VERTICAL each @target_line point
 *               corresponds to a column of the area.
 *
 * Calculates a line quantity for each row or column in a data field area.
 *
 * Use gwy_data_field_get_line_stats_mask() for full masking type options.
 *
 * Since: 2.2
 **/
void
gwy_data_field_area_get_line_stats(GwyDataField *data_field,
                                   GwyDataField *mask,
                                   GwyDataLine *target_line,
                                   gint col, gint row,
                                   gint width, gint height,
                                   GwyLineStatQuantity quantity,
                                   GwyOrientation orientation)
{
    gwy_data_field_get_line_stats_mask(data_field, mask, GWY_MASK_INCLUDE,
                                       target_line, NULL,
                                       col, row, width, height,
                                       quantity, orientation);
}

/**
 * gwy_data_field_get_line_stats:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to @data_field height (width).
 * @quantity: The line quantity to calulate for each row (column).
 * @orientation: Line orientation.  See gwy_data_field_area_get_line_stats().
 *
 * Calculates a line quantity for each row or column of a data field.
 *
 * Since: 2.2
 **/
void
gwy_data_field_get_line_stats(GwyDataField *data_field,
                              GwyDataLine *target_line,
                              GwyLineStatQuantity quantity,
                              GwyOrientation orientation)
{
    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    gwy_data_field_area_get_line_stats(data_field, NULL, target_line,
                                       0, 0,
                                       data_field->xres, data_field->yres,
                                       quantity, orientation);
}

/**
 * gwy_data_field_count_maxima:
 * @data_field: A data field.
 *
 * Counts the number of regional maxima in a data field.
 *
 * See gwy_data_field_mark_extrema() for the definition of a regional maximum.
 *
 * Returns: The number of regional maxima.
 *
 * Since: 2.38
 **/
guint
gwy_data_field_count_maxima(GwyDataField *data_field)
{
    GwyDataField *mask;
    gint *grains;
    guint ngrains;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(data_field), 0);
    mask = gwy_data_field_new_alike(data_field, FALSE);
    gwy_data_field_mark_extrema(data_field, mask, TRUE);
    grains = g_new0(gint, data_field->xres*data_field->yres);
    ngrains = gwy_data_field_number_grains(mask, grains);
    g_free(grains);
    g_object_unref(mask);
    return ngrains;
}

/**
 * gwy_data_field_count_minima:
 * @data_field: A data field
 *
 * Counts the number of regional minima in a data field.
 *
 * See gwy_data_field_mark_extrema() for the definition of a regional minimum.
 *
 * Returns: The number of regional minima.
 *
 * Since: 2.38
 **/
guint
gwy_data_field_count_minima(GwyDataField *data_field)
{
    GwyDataField *mask;
    gint *grains;
    guint ngrains;

    g_return_val_if_fail(GWY_IS_DATA_FIELD(data_field), 0);
    mask = gwy_data_field_new_alike(data_field, FALSE);
    gwy_data_field_mark_extrema(data_field, mask, FALSE);
    grains = g_new0(gint, data_field->xres*data_field->yres);
    ngrains = gwy_data_field_number_grains(mask, grains);
    g_free(grains);
    g_object_unref(mask);
    return ngrains;
}

/**
 * gwy_data_field_angular_average:
 * @data_field: A data field.
 * @target_line: A data line to store the distribution to.  It will be
 *               resampled to @nstats size.
 * @mask: Mask of pixels to include from/exclude in the averaging, or %NULL
 *        for full @data_field.
 * @masking: Masking mode to use.  See the introduction for description of
 *           masking modes.
 * @x: X-coordinate of the averaging disc origin, in real coordinates
 *     including offsets.
 * @y: Y-coordinate of the averaging disc origin, in real coordinates
 *     including offsets.
 * @r: Radius, in real coordinates.  It determines the real length of the
 *     resulting line.
 * @nstats: The number of samples the resulting line should have.  A
 *          non-positive value means the sampling will be determined
 *          automatically.
 *
 * Performs angular averaging of a part of a data field.
 *
 * The result of such averaging is an radial profile, starting from the disc
 * centre.
 *
 * The function does not guarantee that @target_line will have exactly @nstats
 * samples upon return.  A cmaller number of samples than requested may be
 * calculated for instance if either central or outer part of the disc is
 * excluded by masking.
 *
 * Since: 2.42
 **/
void
gwy_data_field_angular_average(GwyDataField *data_field,
                               GwyDataLine *target_line,
                               GwyDataField *mask,
                               GwyMaskingType masking,
                               gdouble x,
                               gdouble y,
                               gdouble r,
                               gint nstats)
{
    gint ifrom, ito, jfrom, jto, i, j, k, kfrom, kto, xres, yres;
    gdouble xreal, yreal, dx, dy, xoff, yoff, h, rr;
    const gdouble *d, *m;
    gdouble *target, *weight;

    g_return_if_fail(GWY_IS_DATA_FIELD(data_field));
    g_return_if_fail(GWY_IS_DATA_LINE(target_line));
    g_return_if_fail(r >= 0.0);
    xres = data_field->xres;
    yres = data_field->yres;
    if (masking == GWY_MASK_IGNORE)
        mask = NULL;
    else if (!mask)
        masking = GWY_MASK_IGNORE;

    if (mask) {
        g_return_if_fail(GWY_IS_DATA_FIELD(mask));
        g_return_if_fail(mask->xres == xres);
        g_return_if_fail(mask->yres == yres);
    }

    xreal = data_field->xreal;
    yreal = data_field->yreal;
    xoff = data_field->xoff;
    yoff = data_field->yoff;
    g_return_if_fail(x >= xoff && x <= xoff + xreal);
    g_return_if_fail(y >= yoff && y <= yoff + yreal);
    /* Just for integer overflow; we limit i and j ranges explicitly later. */
    r = MIN(r, hypot(xreal, yreal));
    x -= xoff;
    y -= yoff;

    dx = xreal/xres;
    dy = yreal/yres;

    /* Prefer sampling close to the shorter step. */
    if (nstats < 1) {
        h = 2.0*dx*dy/(dx + dy);
        nstats = GWY_ROUND(r/h);
        nstats = MAX(nstats, 1);
    }
    h = r/nstats;

    d = data_field->data;
    m = mask ? mask->data : NULL;

    gwy_data_line_resample(target_line, nstats, GWY_INTERPOLATION_NONE);
    gwy_data_line_clear(target_line);
    gwy_data_field_copy_units_to_data_line(data_field, target_line);
    target_line->real = h*nstats;
    target_line->off = 0.0;
    target = target_line->data;
    /* Just return something for single-point lines. */
    if (nstats < 2 || r == 0.0) {
        /* NB: gwy_data_field_get_dval_real() does not use offsets. */
        target[0] = gwy_data_field_get_dval_real(data_field, x, y,
                                                 GWY_INTERPOLATION_ROUND);
        return;
    }

    ifrom = (gint)floor(gwy_data_field_rtoi(data_field, y - r));
    ifrom = MAX(ifrom, 0);
    ito = (gint)ceil(gwy_data_field_rtoi(data_field, y + r));
    ito = MIN(ito, yres-1);

    jfrom = (gint)floor(gwy_data_field_rtoj(data_field, x - r));
    jfrom = MAX(jfrom, 0);
    jto = (gint)ceil(gwy_data_field_rtoj(data_field, x + r));
    jto = MIN(jto, xres-1);

    weight = g_new0(gdouble, nstats);
    for (i = ifrom; i <= ito; i++) {
        gdouble yy = (i + 0.5)*dy - y;
        for (j = jfrom; j <= jto; j++) {
            gdouble xx = (j + 0.5)*dx - x;
            gdouble v = d[i*xres + j];

            if ((masking == GWY_MASK_INCLUDE && m[i*xres + j] <= 0.0)
                || (masking == GWY_MASK_EXCLUDE && m[i*xres + j] >= 1.0))
                continue;

            rr = sqrt(xx*xx + yy*yy)/h;
            k = floor(rr);
            if (k+1 >= nstats) {
                if (k+1 == nstats) {
                    target[k] += v;
                    weight[k] += 1.0;
                }
                continue;
            }

            rr -= k;
            if (rr <= 0.5)
                rr = 2.0*rr*rr;
            else
                rr = 1.0 - 2.0*(1.0 - rr)*(1.0 - rr);

            target[k] += (1.0 - rr)*v;
            target[k+1] += rr*v;
            weight[k] += 1.0 - rr;
            weight[k+1] += rr;
        }
    }

    /* Get rid of initial and trailing no-data segment. */
    for (kfrom = 0; kfrom < nstats; kfrom++) {
        if (weight[kfrom])
            break;
    }
    for (kto = nstats-1; kto > kfrom; kto--) {
        if (weight[kto])
            break;
    }
    if (kto - kfrom < 2) {
        /* XXX: This is not correct.  We do not care. */
        target_line->real = h;
        target[0] = gwy_data_field_get_dval_real(data_field, x, y,
                                                 GWY_INTERPOLATION_ROUND);
        return;
    }

    if (kfrom != 0 || kto != nstats-1) {
        nstats = kto+1 - kfrom;
        gwy_data_line_resize(target_line, kfrom, kto+1);
        target = target_line->data;
        target_line->off = kfrom*h;
        memmove(weight, weight + kfrom, nstats*sizeof(gdouble));
    }
    g_assert(weight[0]);
    g_assert(weight[nstats-1]);

    /* Fill holes where we have no weight, this can occur near the start if
     * large nstats is requested. */
    kfrom = -1;
    for (k = 0; k < nstats; k++) {
        if (weight[k]) {
            target[k] /= weight[k];
            if (kfrom+1 != k) {
                gdouble first = target[kfrom];
                gdouble last = target[k];
                for (j = kfrom+1; j < k; j++) {
                    gdouble w = (j - kfrom)/(gdouble)(k - kfrom);
                    target[j] = w*last + (1.0 - w)*first;
                }
            }
            kfrom = k;
        }
    }

    g_free(weight);
}

/************************** Documentation ****************************/

/**
 * SECTION:stats
 * @title: stats
 * @short_description: Two-dimensional statistical functions
 *
 * Many statistical functions permit to pass masks that determine which values
 * in the data field to take into account or ignore when calculating the
 * statistical characteristics.  Masking mode %GWY_MASK_INCLUDE means that
 * maks values equal to 0.0 and below cause corresponding data field samples
 * to be ignored, values equal to 1.0 and above cause inclusion of
 * corresponding data field samples.  The behaviour for values inside interval
 * (0.0, 1.0) is undefined.  In mode @GWY_MASK_EXCLUDE, the meaning of mask is
 * inverted, as if all mask values x were replaced with 1-x.  The
 * mask field is ignored in mode @GWY_MASK_IGNORE, i.e. the same behaviour
 * occurs as with %NULL mask argument.
 **/

/* vim: set cin et ts=4 sw=4 cino=>1s,e0,n0,f0,{0,}0,^0,\:1s,=0,g1s,h0,t0,+1s,c3,(0,u0 : */
