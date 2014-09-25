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

/* FIXME: Is it possible that the BMP resolution does not match the data
 * resolution? */

/**
 * [FILE-MAGIC-USERGUIDE]
 * EM4SYS NX II
 * .bmp
 * Read
 **/
#define DEBUG 1
#include "config.h"
#include <string.h>
#include <libgwyddion/gwymacros.h>
#include <libgwyddion/gwyutils.h>
#include <libgwyddion/gwymath.h>
#include <libprocess/datafield.h>
#include <libgwymodule/gwymodule-file.h>
#include <app/gwyapp.h>
#include <app/gwymoduleutils-file.h>

#include "err.h"
#include "get.h"

#define EXTENSION ".bmp"

#define Micron (1e-6)

enum {
    BMP_HEADER_SIZE = 54,
    HEADER_SIZE = 243
};

/* I have got some header field documentation but their physical order in the
 * file is not clear. */
typedef struct {
    gchar file_version[10];
    guint year, month, day, hour, minute;
    gchar head_mode[6];   /* enumerated string */
    gchar unknown_2[55];  /* only zeroes ever seen */
    guint xres;
    guint yres;
    gdouble xreal;
    gdouble yreal;
    gdouble zreal;
    gdouble xoff;
    gdouble yoff;
    /* We are now at offset 0x78 in the header.  There are a few non-zero bytes
     * just after that and then at the very end of the header.
     *
     * Hence, somewhere, there should be also:
     * LowPassFilter, ScanRate, CruiseTime, SetPoint, some PAL RGB info
     * and 100-byte comment and some 30-bytes FlattenType info. */
    guint scan_rate;
    guint cruise_time;
    gchar unknown_3[119];
} NXIIFile;

static gboolean      module_register (void);
static gint          nxii_detect     (const GwyFileDetectInfo *fileinfo,
                                      gboolean only_name);
static GwyContainer* nxii_load       (const gchar *filename,
                                      GwyRunType mode,
                                      GError **error);
static gboolean      read_nxii_header(const guchar *p,
                                      NXIIFile *nxiifile,
                                      GError **error);
static gboolean      read_bmp_header (const guchar *p,
                                      guint *xres,
                                      guint *yres,
                                      guint *size);

static GwyModuleInfo module_info = {
    GWY_MODULE_ABI_VERSION,
    &module_register,
    N_("Imports EM4SYS data files."),
    "Yeti <yeti@gwyddion.net>",
    "1.0",
    "David Nečas (Yeti)",
    "2014",
};

GWY_MODULE_QUERY(module_info)

static gboolean
module_register(void)
{
    gwy_file_func_register("nxiifile",
                           N_("EM4SYS NX II files (.bmp)"),
                           (GwyFileDetectFunc)&nxii_detect,
                           (GwyFileLoadFunc)&nxii_load,
                           NULL,
                           NULL);

    return TRUE;
}

static gint
nxii_detect(const GwyFileDetectInfo *fileinfo,
           gboolean only_name)
{
    guint xres, yres, size;

    if (only_name)
        return 0;

    if (fileinfo->file_size < HEADER_SIZE + BMP_HEADER_SIZE
        || fileinfo->buffer_len < BMP_HEADER_SIZE)
        return 0;

    if (!read_bmp_header(fileinfo->head, &xres, &yres, &size))
        return 0;

    /* The AFM header and data are piggybacked to a Windows BMP file.
     * Their sizes are *NOT* included in the file size field of BMP header.
     * Hence the image is reported as invalid by some programs.  But we base
     * the detection on that. */
    gwy_debug("specified BMP file size %u, actual size %u",
              size, (guint)fileinfo->file_size);
    if (fileinfo->file_size == size + HEADER_SIZE + xres*yres*sizeof(guint16))
        return 100;

    return 0;
}

static GwyContainer*
nxii_load(const gchar *filename,
         G_GNUC_UNUSED GwyRunType mode,
         GError **error)
{
    GwyContainer *container = NULL;
    guchar *buffer = NULL;
    const guchar *p;
    NXIIFile nxiifile;
    guint xres, yres, bmpfilesize, expected_size;
    gsize size = 0;
    GError *err = NULL;
    GwyDataField *dfield;

    if (!gwy_file_get_contents(filename, &buffer, &size, &err)) {
        err_GET_FILE_CONTENTS(error, &err);
        return NULL;
    }
    if (size <= BMP_HEADER_SIZE + HEADER_SIZE) {
        err_TOO_SHORT(error);
        goto fail;
    }

    p = buffer;
    if (!read_bmp_header(p, &xres, &yres, &bmpfilesize)
        || size != bmpfilesize + HEADER_SIZE + xres*yres*sizeof(guint16)) {
        err_FILE_TYPE(error, "NX II");
        goto fail;
    }

    if (!read_nxii_header(buffer + bmpfilesize, &nxiifile, error))
        goto fail;

    expected_size = 2*nxiifile.xres*nxiifile.yres + HEADER_SIZE + bmpfilesize;
    if (err_SIZE_MISMATCH(error, expected_size, size, TRUE))
        goto fail;

    dfield = gwy_data_field_new(nxiifile.xres, nxiifile.yres,
                                nxiifile.xreal*Micron, nxiifile.yreal*Micron,
                                FALSE);
    gwy_convert_raw_data(buffer + bmpfilesize + HEADER_SIZE,
                         nxiifile.xres*nxiifile.yres, 1,
                         GWY_RAW_DATA_UINT16, GWY_BYTE_ORDER_LITTLE_ENDIAN,
                         gwy_data_field_get_data(dfield),
                         nxiifile.zreal*1e-9, 0.0);

    gwy_si_unit_set_from_string(gwy_data_field_get_si_unit_xy(dfield), "m");
    gwy_si_unit_set_from_string(gwy_data_field_get_si_unit_z(dfield), "m");

    container = gwy_container_new();
    gwy_container_set_object_by_name(container, "/0/data", dfield);
    gwy_container_set_string_by_name(container, "/0/data/title",
                                     g_strdup("Topography"));
    g_object_unref(dfield);

    gwy_file_channel_import_log_add(container, 0, NULL, filename);

fail:
    gwy_file_abandon_contents(buffer, size, NULL);

    return container;
}

static gboolean
read_nxii_header(const guchar *p, NXIIFile *nxiifile, GError **error)
{
    const guchar *q = p;

    get_CHARARRAY(nxiifile->file_version, &p);
    gwy_debug("version %.10s", nxiifile->file_version);
    /* Not sure what calendar this is but it does seem to correspond to the
     * vendor's software. */
    nxiifile->year = *(p++);
    nxiifile->month = *(p++);
    nxiifile->day = *(p++);
    nxiifile->hour = *(p++);
    nxiifile->minute = *(p++);
    gwy_debug("strange date-time %u-%u-%u %u:%u",
              nxiifile->year, nxiifile->month, nxiifile->day,
              nxiifile->hour, nxiifile->minute);
    get_CHARARRAY(nxiifile->head_mode, &p);
    gwy_debug("head mode %.5s", nxiifile->head_mode);
    get_CHARARRAY(nxiifile->unknown_2, &p);
    nxiifile->xres = gwy_get_guint16_le(&p);
    nxiifile->yres = gwy_get_guint16_le(&p);
    if (err_DIMENSION(error, nxiifile->xres)
        || err_DIMENSION(error, nxiifile->yres))
        return FALSE;
    gwy_debug("xres %u, yres %u", nxiifile->xres, nxiifile->yres);
    nxiifile->xreal = gwy_get_gdouble_le(&p);
    /* Use negated positive conditions to catch NaNs */
    if (!((nxiifile->xreal = fabs(nxiifile->xreal)) > 0)) {
        g_warning("Real x size is 0.0, fixing to 1.0");
        nxiifile->xreal = 1.0;
    }
    nxiifile->yreal = gwy_get_gdouble_le(&p);
    /* Use negated positive conditions to catch NaNs */
    if (!((nxiifile->yreal = fabs(nxiifile->yreal)) > 0)) {
        g_warning("Real y size is 0.0, fixing to 1.0");
        nxiifile->yreal = 1.0;
    }
    nxiifile->zreal = gwy_get_gdouble_le(&p);
    /* Use negated positive conditions to catch NaNs */
    if (!((nxiifile->zreal = fabs(nxiifile->zreal)) > 0)) {
        g_warning("Real z size is 0.0, fixing to 1.0");
        nxiifile->zreal = 1.0;
    }
    nxiifile->xoff = gwy_get_gdouble_le(&p);
    nxiifile->yoff = gwy_get_gdouble_le(&p);
    gwy_debug("xreal %g, xoff %g", nxiifile->xreal, nxiifile->xoff);
    gwy_debug("yreal %g, yoff %g", nxiifile->yreal, nxiifile->yoff);
    gwy_debug("zreal %g", nxiifile->zreal);
    nxiifile->scan_rate = gwy_get_guint16_le(&p);
    /* For some reason, the software says 150 when we read 300 here. */
    nxiifile->cruise_time = gwy_get_guint16_le(&p);
    get_CHARARRAY(nxiifile->unknown_3, &p);

    g_assert(p - q == HEADER_SIZE);
    return TRUE;
}

/* XXX: Code identical to csmfile.c */
static gboolean
read_bmp_header(const guchar *p,
                guint *xres,
                guint *yres,
                guint *size)
{
    guint x;

    if (p[0] != 'B' || p[1] != 'M')
        return FALSE;
    p += 2;

    if ((*size = gwy_get_guint32_le(&p)) < BMP_HEADER_SIZE)   /* Size */
        return FALSE;
    if ((x = gwy_get_guint32_le(&p)) != 0)   /* Reserved */
        return FALSE;
    if ((x = gwy_get_guint32_le(&p)) != 54)   /* Offset */
        return FALSE;
    if ((x = gwy_get_guint32_le(&p)) != 40)   /* Header size */
        return FALSE;
    if ((*xres = gwy_get_guint32_le(&p)) == 0)   /* Width */
        return FALSE;
    if ((*yres = gwy_get_guint32_le(&p)) == 0)   /* Height */
        return FALSE;
    if ((x = gwy_get_guint16_le(&p)) != 1)   /* Bit planes */
        return FALSE;
    if ((x = gwy_get_guint16_le(&p)) != 24)   /* BPP */
        return FALSE;
    if ((x = gwy_get_guint32_le(&p)) != 0)   /* Compression */
        return FALSE;
    if ((x = gwy_get_guint32_le(&p))
        && x + BMP_HEADER_SIZE != *size)   /* Compresed size, may be 0 */
        return FALSE;

    if (3*(*xres)*(*yres) + BMP_HEADER_SIZE != *size)
        return FALSE;

    return TRUE;
}

/* vim: set cin et ts=4 sw=4 cino=>1s,e0,n0,f0,{0,}0,^0,\:1s,=0,g1s,h0,t0,+1s,c3,(0,u0 : */