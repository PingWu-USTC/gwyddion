/*
 *  $Id: spmlabf.c 8773 2008-11-18 10:09:10Z yeti-dn $
 *  Copyright (C) 2008 David Necas (Yeti).
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
#define DEBUG
/**
 * [FILE-MAGIC-FREEDESKTOP]
 * <mime-type type="application/x-wyko-opd">
 *   <comment>Wyko OPD data</comment>
 *   <magic priority="50">
 *     <match type="string" offset="0" value="\x01\x00Directory"/>
 *   </magic>
 *   <glob pattern="*.opd"/>
 *   <glob pattern="*.OPD"/>
 * </mime-type>
 **/

/**
 * [FILE-MAGIC-FREEDESKTOP]
 * <mime-type type="application/x-wyko-asc">
 *   <comment>Wyko ASCII data</comment>
 *   <magic priority="50">
 *     <match type="string" offset="0" value="Wyko ASCII Data File Format 0\t0\t1"/>
 *   </magic>
 *   <glob pattern="*.asc"/>
 *   <glob pattern="*.ASC"/>
 * </mime-type>
 **/

#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <libgwyddion/gwymacros.h>
#include <libgwyddion/gwymath.h>
#include <libgwyddion/gwyutils.h>
#include <libprocess/stats.h>
#include <libgwymodule/gwymodule-file.h>
#include <app/gwymoduleutils-file.h>

#include "err.h"

/* Not a real magic header, but should catch the stuff */
#define MAGIC "\x01\x00" "Directory"
#define MAGIC_SIZE (sizeof(MAGIC)-1)
#define EXTENSION ".opd"

#define MAGIC_ASC "Wyko ASCII Data File Format 0\t0\t1"
#define MAGIC_ASC_SIZE (sizeof(MAGIC_ASC)-1)
#define EXTENSION_ASC ".asc"

#define Nanometer (1e-9)
#define Milimeter (1e-3)
#define OPD_BAD_FLOAT 1e38
#define OPD_BAD_INT16 32766
#define OPD_BAD_ASC "Bad"

enum {
    BLOCK_SIZE = 24,
    BLOCK_NAME_SIZE = 16,
};

enum {
    OPD_DIRECTORY = 1,
    OPD_ARRAY = 3,
    OPD_TEXT = 5,
    OPD_SHORT = 6,
    OPD_FLOAT = 7,
    OPD_DOUBLE = 8,
    OPD_LONG = 12,
} OPDDataType;

typedef enum {
    OPD_ARRAY_FLOAT = 4,
    OPD_ARRAY_INT16 = 2,
    OPD_ARRAY_BYTE = 1,
} OPDArrayType;

/* The header consists of a sequence of these creatures. */
typedef struct {
    /* This is in the file */
    char name[BLOCK_NAME_SIZE + 1];
    guint type;
    guint size;
    guint flags;
    /* Derived info */
    guint pos;
    const guchar *data;
} OPDBlock;

static gboolean      module_register (void);
static gint          opd_detect      (const GwyFileDetectInfo *fileinfo,
                                      gboolean only_name);
static gint          opd_asc_detect  (const GwyFileDetectInfo *fileinfo,
                                      gboolean only_name);
static GwyContainer* opd_load        (const gchar *filename,
                                      GwyRunType mode,
                                      GError **error);
static GwyContainer* opd_asc_load    (const gchar *filename,
                                      GwyRunType mode,
                                      GError **error);
static void          get_block       (OPDBlock *block,
                                      const guchar **p);
static gboolean      get_float       (const OPDBlock *header,
                                      guint nblocks,
                                      const gchar *name,
                                      gdouble *value,
                                      GError **error);
static gboolean      check_sizes     (const OPDBlock *header,
                                      guint nblocks,
                                      GError **error);
static guint         find_block      (const OPDBlock *header,
                                      guint nblocks,
                                      const gchar *name);
static const guchar* get_array_params(const guchar *p,
                                      guint *xres,
                                      guint *yres,
                                      OPDArrayType *type);
static guint         remove_bad_data (GwyDataField *dfield,
                                      GwyDataField *mfield);

static GwyModuleInfo module_info = {
    GWY_MODULE_ABI_VERSION,
    &module_register,
    N_("Imports Wyko OPD and ASC files."),
    "Yeti <yeti@gwyddion.net>",
    "0.3",
    "David Nečas (Yeti)",
    "2008",
};

GWY_MODULE_QUERY(module_info)

static gboolean
module_register(void)
{
    gwy_file_func_register("opdfile",
                           N_("Wyko OPD files (.opd)"),
                           (GwyFileDetectFunc)&opd_detect,
                           (GwyFileLoadFunc)&opd_load,
                           NULL,
                           NULL);

    gwy_file_func_register("opdfile-asc",
                           N_("Wyko ASCII export files (.asc)"),
                           (GwyFileDetectFunc)&opd_asc_detect,
                           (GwyFileLoadFunc)&opd_asc_load,
                           NULL,
                           NULL);

    return TRUE;
}

/***** Native binary OPD file *********************************************/

static gint
opd_detect(const GwyFileDetectInfo *fileinfo,
           gboolean only_name)
{
    if (only_name)
        return g_str_has_suffix(fileinfo->name_lowercase, EXTENSION) ? 10 : 0;

    if (fileinfo->file_size < BLOCK_SIZE + 2
        || memcmp(fileinfo->head, MAGIC, MAGIC_SIZE) != 0)
        return 0;

    return 100;
}

static GwyContainer*
opd_load(const gchar *filename,
         G_GNUC_UNUSED GwyRunType mode,
         GError **error)
{
    GwyContainer *meta, *container = NULL;
    guchar *buffer = NULL;
    OPDBlock directory_block, *header = NULL;
    gsize size = 0;
    GError *err = NULL;
    GwyDataField *dfield = NULL, *mfield = NULL;
    GwySIUnit *siunit;
    const guchar *p;
    gdouble *data, *drow, *mdata, *mrow;
    guint nblocks, idata, offset, mcount, xres, yres, i, j;
    gdouble pixel_size, wavelength, mult = 1.0, aspect = 1.0;
    OPDArrayType datatype;

    if (!gwy_file_get_contents(filename, &buffer, &size, &err)) {
        err_GET_FILE_CONTENTS(error, &err);
        return NULL;
    }
    if (size < BLOCK_SIZE + 2) {
        err_TOO_SHORT(error);
        goto fail;
    }

    p = buffer + 2;
    get_block(&directory_block, &p);
    directory_block.pos = 2;
    directory_block.data = buffer + 2;
    gwy_debug("<%s> size=0x%08x, pos=0x%08x, type=%u, flags=0x%04x",
              directory_block.name, directory_block.size, directory_block.pos,
              directory_block.type, directory_block.flags);
    /* This check may need to be relieved a bit */
    if (!gwy_strequal(directory_block.name, "Directory")
        || directory_block.type != OPD_DIRECTORY
        || directory_block.flags != 0xffff) {
        err_FILE_TYPE(error, "Wyko OPD data");
        goto fail;
    }

    nblocks = directory_block.size/BLOCK_SIZE;
    if (size < BLOCK_SIZE*nblocks + 2) {
        g_set_error(error, GWY_MODULE_FILE_ERROR, GWY_MODULE_FILE_ERROR_DATA,
                    _("File header is truncated"));
        goto fail;
    }

    /* Read info block.  We've already read the directory, do not count it */
    nblocks--;
    header = g_new(OPDBlock, nblocks);
    offset = directory_block.pos + directory_block.size;
    for (i = j = 0; i < nblocks; i++) {
        get_block(header + j, &p);
        header[j].pos = offset;
        header[j].data = buffer + offset;
        offset += header[j].size;
        /* Skip void header blocks */
        if (header[j].size) {
            gwy_debug("<%s> size=0x%08x, pos=0x%08x, type=%u, flags=0x%04x",
                      header[j].name, header[j].size, header[j].pos,
                      header[j].type, header[j].flags);
            j++;
        }
        if (offset > size) {
            g_set_error(error, GWY_MODULE_FILE_ERROR,
                        GWY_MODULE_FILE_ERROR_DATA,
                        _("Item `%s' is beyond the end of the file."),
                        header[i].name);
            goto fail;
        }
    }
    nblocks = j;

    if (!check_sizes(header, nblocks, error))
        goto fail;

    /* Find data */
    idata = find_block(header, nblocks, "OPD");
    if (idata == nblocks)
        idata = find_block(header, nblocks, "SAMPLE_DATA");
    if (idata == nblocks)
        idata = find_block(header, nblocks, "RAW_DATA");
    if (idata == nblocks)
        idata = find_block(header, nblocks, "RAW DATA");
    if (idata == nblocks) {
        err_NO_DATA(error);
        goto fail;
    }
    if (header[idata].type != OPD_ARRAY) {
        err_DATA_TYPE(error, header[idata].type);
        goto fail;
    }

    /* Physical scales */
    if (!get_float(header, nblocks, "Pixel_size", &pixel_size, error)
        || !get_float(header, nblocks, "Wavelength", &wavelength, error))
        goto fail;

    wavelength *= Nanometer;
    pixel_size *= Milimeter;
    get_float(header, nblocks, "Mult", &mult, NULL);
    get_float(header, nblocks, "Aspect", &aspect, NULL);

    /* Read the data */
    p = get_array_params(header[idata].data, &xres, &yres, &datatype);
    dfield = gwy_data_field_new(xres, yres,
                                aspect*xres*pixel_size, yres*pixel_size, FALSE);

    siunit = gwy_si_unit_new("m");
    gwy_data_field_set_si_unit_xy(dfield, siunit);
    g_object_unref(siunit);

    siunit = gwy_si_unit_new("m");
    gwy_data_field_set_si_unit_z(dfield, siunit);
    g_object_unref(siunit);

    mfield = gwy_data_field_new_alike(dfield, FALSE);
    gwy_data_field_fill(mfield, 1.0);

    data = gwy_data_field_get_data(dfield);
    mdata = gwy_data_field_get_data(mfield);
    for (i = 0; i < yres; i++) {
        drow = data + (yres-1 - i)*xres;
        mrow = mdata + (yres-1 - i)*xres;
        if (datatype == OPD_ARRAY_FLOAT) {
            for (j = 0; j < xres; j++) {
                gdouble v = gwy_get_gfloat_le(&p);
                if (v < OPD_BAD_FLOAT)
                    drow[j] = wavelength/mult*v;
                else
                    mrow[j] = 0.0;
            }
        }
        else if (datatype == OPD_ARRAY_INT16) {
            for (j = 0; j < xres; j++) {
                guint v = gwy_get_guint16_le(&p);
                if (v < OPD_BAD_INT16)
                    drow[j] = wavelength/mult*v;
                else
                    mrow[j] = 0.0;
            }
        }
        else if (datatype == OPD_ARRAY_BYTE) {
            /* FIXME: Bad data? */
            for (j = 0; j < xres; j++) {
                drow[j] = wavelength/mult*(*(p++));
                mrow[j] = 0.0;
            }
        }
        else {
            err_DATA_TYPE(error, datatype);
            goto fail;
        }
    }
    mcount = remove_bad_data(dfield, mfield);

    container = gwy_container_new();
    gwy_container_set_object_by_name(container, "/0/data", dfield);
    if (mcount)
        gwy_container_set_object_by_name(container, "/0/mask", mfield);

    if ((i = find_block(header, nblocks, "Title")) != nblocks)
        gwy_container_set_string_by_name(container, "/0/data/title",
                                         g_strndup(header[i].data,
                                                   header[i].size));
    else
        gwy_app_channel_title_fall_back(container, 0);

    if (aspect != 1.0)
        gwy_container_set_boolean_by_name(container, "/0/data/realsquare",
                                          TRUE);

fail:
    g_free(header);
    gwy_object_unref(dfield);
    gwy_object_unref(mfield);
    gwy_file_abandon_contents(buffer, size, NULL);

    return container;
}

static void
get_block(OPDBlock *block, const guchar **p)
{
    memset(block->name, 0, BLOCK_NAME_SIZE + 1);
    strncpy(block->name, *p, BLOCK_NAME_SIZE);
    *p += BLOCK_NAME_SIZE;
    g_strstrip(block->name);
    block->type = gwy_get_guint16_le(p);
    block->size = gwy_get_guint32_le(p);
    block->flags = gwy_get_guint16_le(p);
}

static gboolean
get_float(const OPDBlock *header,
          guint nblocks,
          const gchar *name,
          gdouble *value,
          GError **error)
{
    const guchar *p;
    guint i;

    if ((i = find_block(header, nblocks, name)) == nblocks) {
        err_MISSING_FIELD(error, name);
        return FALSE;
    }
    if (header[i].type != OPD_FLOAT) {
        err_INVALID(error, name);
        return FALSE;
    }
    p = header[i].data;
    *value = gwy_get_gfloat_le(&p);
    gwy_debug("%s = %g", name, *value);
    return TRUE;
}

/* TODO: Improve error messages */
static gboolean
check_sizes(const OPDBlock *header,
            guint nblocks,
            GError **error)
{
    /*                             0  1  2  3  4  5  6  7  8  9 10 11 12 */
    static const guint sizes[] = { 0, 0, 0, 0, 0, 0, 2, 4, 8, 0, 0, 0, 4 };

    guint i, type;

    for (i = 0; i < nblocks; i++) {
        type = header[i].type;
        if (type < G_N_ELEMENTS(sizes)) {
            if (sizes[type]) {
                if (header[i].size != sizes[type]) {
                    err_INVALID(error, header[i].name);
                    return FALSE;
                }
            }
            else if (type == OPD_DIRECTORY) {
                g_set_error(error, GWY_MODULE_FILE_ERROR,
                            GWY_MODULE_FILE_ERROR_DATA,
                            _("Nested directories found"));
                return FALSE;
            }
            else if (type == OPD_ARRAY) {
                guint xres, yres;

                /* array params */
                if (header[i].size < 3*2) {
                    err_INVALID(error, header[i].name);
                    return FALSE;
                }
                /* array contents */
                get_array_params(header[i].data, &xres, &yres, &type);
                gwy_debug("%s xres=%u yres=%u type=%u size=%u",
                          header[i].name, xres, yres, type, header[i].size);
                if (header[i].size < 3*2 + xres*yres*type) {
                    err_INVALID(error, header[i].name);
                    return FALSE;
                }
            }
            else if (type == OPD_TEXT) {
                /* Nothing to do here, text can fill the field completely */
            }
            else {
                g_warning("Unknown item type %u", type);
            }
        }
    }

    return TRUE;
}

static const guchar*
get_array_params(const guchar *p, guint *xres, guint *yres, OPDArrayType *type)
{
    *yres = gwy_get_guint16_le(&p);
    *xres = gwy_get_guint16_le(&p);
    *type = gwy_get_guint16_le(&p);
    switch (*type) {
        case OPD_ARRAY_FLOAT:
        case OPD_ARRAY_INT16:
        case OPD_ARRAY_BYTE:
        break;

        default:
        g_warning("Unknown array type %u", *type);
        break;
    }

    return p;
}

static guint
find_block(const OPDBlock *header,
           guint nblocks,
           const gchar *name)
{
    unsigned int i;

    for (i = 0; i < nblocks; i++) {
        if (gwy_strequal(header[i].name, name))
            return i;
    }
    return nblocks;
}

/***** ASCII data *********************************************************/

static gint
opd_asc_detect(const GwyFileDetectInfo *fileinfo,
               gboolean only_name)
{
    if (only_name)
        return g_str_has_suffix(fileinfo->name_lowercase,
                                EXTENSION_ASC) ? 10 : 0;

    if (fileinfo->file_size < MAGIC_ASC_SIZE + 2
        || memcmp(fileinfo->head, MAGIC_ASC, MAGIC_ASC_SIZE) != 0)
        return 0;

    return 100;
}

/* FIXME: This is woefuly confusing spaghetti. */
static GwyContainer*
opd_asc_load(const gchar *filename,
             G_GNUC_UNUSED GwyRunType mode,
             GError **error)
{
    GwyContainer *container = NULL;
    GwyDataField *dfield = NULL, *mfield = NULL;
    GwySIUnit *siunit;
    gchar *line, *p, *s, *buffer = NULL;
    GHashTable *hash = NULL;
    guint mcount, xres = 0, yres = 0, in_data = 0, ignoring = 0;
    gdouble pixel_size, wavelength = 0.0, mult = 1.0, aspect = 1.0;
    gdouble *data = NULL, *drow, *mdata = NULL, *mrow;
    gsize size;
    GError *err = NULL;

    if (!g_file_get_contents(filename, &buffer, &size, &err)) {
        err_GET_FILE_CONTENTS(error, &err);
        goto fail;
    }

    p = buffer;
    line = gwy_str_next_line(&p);
    if (!gwy_strequal(line, MAGIC_ASC)) {
        err_FILE_TYPE(error, "Wyko ASC data");
        goto fail;
    }

    hash = g_hash_table_new(g_str_hash, g_str_equal);
    while ((s = line = gwy_str_next_line(&p))) {
        if (ignoring) {
            ignoring--;
            continue;
        }

        if (in_data) {
            guint i;

            in_data--;
            drow = data + in_data*xres;
            mrow = mdata + in_data*xres;
            for (i = 0; i < xres; i++) {
                if (strncmp(s, "Bad", 3) == 0) {
                    mrow[i] = 0.0;
                    s += 3;
                }
                else
                    drow[i] = g_ascii_strtod(s, &s)*wavelength/mult;

                while (g_ascii_isspace(*s))
                    s++;
            }
            continue;
        }

        /* XXX: make noise */
        if (!(s = strchr(s, '\t')))
            continue;

        *s = '\0';
        s++;

        if (gwy_strequal(line, "Y Size")) {
            xres = atoi(s);
            gwy_debug("xres=%u", xres);
            continue;
        }
        if (gwy_strequal(line, "X Size")) {
            yres = atoi(s);
            gwy_debug("yres=%u", yres);
            continue;
        }

        /* Skip type and length, they seem useless in the ASCII file */
        /* XXX: make noise */
        if (!(s = strchr(s, '\t')))
            continue;
        s++;
        if (!(s = strchr(s, '\t')))
            continue;
        s++;

        if (gwy_strequal(line, "RAW DATA")
            || gwy_strequal(line, "RAW_DATA")
            || gwy_strequal(line, "SAMPLE_DATA")
            || gwy_strequal(line, "OPD")
            || gwy_strequal(line, "Intensity")) {
            if (!xres) {
                err_MISSING_FIELD(error, "Y Size");
                goto fail;
            }
            if (!yres) {
                err_MISSING_FIELD(error, "X Size");
                goto fail;
            }

            if (!(s = g_hash_table_lookup(hash, "Pixel_size"))
                || !(pixel_size = fabs(g_ascii_strtod(s, NULL)))) {
                err_MISSING_FIELD(error, "Pixel_size");
                goto fail;
            }
            gwy_debug("pixel_size = %g", pixel_size);
            if (!(s = g_hash_table_lookup(hash, "Wavelength"))
                || !(wavelength = fabs(g_ascii_strtod(s, NULL)))) {
                err_MISSING_FIELD(error, "Wavelength");
                goto fail;
            }
            gwy_debug("wavelength = %g", wavelength);

            wavelength *= Nanometer;
            pixel_size *= Milimeter;

            if (dfield) {
                ignoring = yres;
                gwy_debug("Ignoring the following %u lines", ignoring);
            }
            else {
                in_data = yres;
                gwy_debug("Reading the following %u lines as data", in_data);
                dfield = gwy_data_field_new(xres, yres,
                                            aspect*xres*pixel_size,
                                            yres*pixel_size,
                                            FALSE);

                siunit = gwy_si_unit_new("m");
                gwy_data_field_set_si_unit_xy(dfield, siunit);
                g_object_unref(siunit);

                siunit = gwy_si_unit_new("m");
                gwy_data_field_set_si_unit_z(dfield, siunit);
                g_object_unref(siunit);

                mfield = gwy_data_field_new_alike(dfield, FALSE);
                gwy_data_field_fill(mfield, 1.0);

                data = gwy_data_field_get_data(dfield);
                mdata = gwy_data_field_get_data(mfield);
            }
            continue;
        }

        if (gwy_strequal(line, "Block Name"))
            continue;

        gwy_debug("<%s> = <%s>", line, s);

        g_hash_table_insert(hash, line, s);
    }

    if (!dfield) {
        err_NO_DATA(error);
        goto fail;
    }

    mcount = remove_bad_data(dfield, mfield);
    container = gwy_container_new();
    gwy_container_set_object_by_name(container, "/0/data", dfield);
    if (mcount)
        gwy_container_set_object_by_name(container, "/0/mask", mfield);

    if ((s = g_hash_table_lookup(hash, "Title")))
        gwy_container_set_string_by_name(container, "/0/data/title",
                                         g_strdup(s));
    else
        gwy_app_channel_title_fall_back(container, 0);

    if (aspect != 1.0)
        gwy_container_set_boolean_by_name(container, "/0/data/realsquare",
                                          TRUE);

fail:

    gwy_object_unref(dfield);
    gwy_object_unref(mfield);
    g_free(buffer);
    if (hash)
        g_hash_table_destroy(hash);

    return container;
}

/***** Common *************************************************************/

static guint
remove_bad_data(GwyDataField *dfield, GwyDataField *mfield)
{
    gdouble *data = gwy_data_field_get_data(dfield);
    gdouble *mdata = gwy_data_field_get_data(mfield);
    gdouble *drow, *mrow;
    gdouble avg;
    guint i, j, mcount, xres, yres;

    xres = gwy_data_field_get_xres(dfield);
    yres = gwy_data_field_get_yres(dfield);
    avg = gwy_data_field_area_get_avg(dfield, mfield, 0, 0, xres, yres);
    mcount = 0;
    for (i = 0; i < yres; i++) {
        mrow = mdata + i*xres;
        drow = data + i*xres;
        for (j = 0; j < xres; j++) {
            if (!mrow[j]) {
                drow[j] = avg;
                mcount++;
            }
            mrow[j] = 1.0 - mrow[j];
        }
    }

    gwy_debug("mcount = %u", mcount);

    return mcount;
}

/* vim: set cin et ts=4 sw=4 cino=>1s,e0,n0,f0,{0,}0,^0,\:1s,=0,g1s,h0,t0,+1s,c3,(0,u0 : */
