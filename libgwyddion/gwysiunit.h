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

#ifndef __GWY_SI_UNIT_H__
#define __GWY_SI_UNIT_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <glib.h>
#include <gtk/gtk.h>
#include <libgwyddion/gwyserializable.h>
    
#define GWY_TYPE_SI_UNIT                  (gwy_si_unit_get_type())
#define GWY_SI_UNIT(obj)                  (G_TYPE_CHECK_INSTANCE_CAST((obj), GWY_TYPE_SI_UNIT, GwySIUnit))
#define GWY_SI_UNIT_CLASS(klass)          (G_TYPE_CHECK_CLASS_CAST((klass), GWY_TYPE_SI_UNIT, GwySIUnit))
#define GWY_IS_SI_UNIT(obj)               (G_TYPE_CHECK_INSTANCE_TYPE((obj), GWY_TYPE_SI_UNIT))
#define GWY_IS_SI_UNIT_CLASS(klass)       (G_TYPE_CHECK_CLASS_TYPE((klass), GWY_TYPE_SI_UNIT))
#define GWY_SI_UNIT_GET_CLASS(obj)        (G_TYPE_INSTANCE_GET_CLASS((obj), GWY_TYPE_SI_UNIT, GwySIUnitClass))

    
typedef struct{
   gchar *unitstr;
} GwySIUnit;

typedef struct{
    GObjectClass parent_class;
} GwySIUnitClass;

GType gwy_si_unit_get_type  (void) G_GNUC_CONST;

GObject* gwy_si_unit_new(gchar *unit_string);
void gwy_si_unit_free();

void gwy_si_unit_set_unit_string(GwySIUnit *siunit, gchar *unit_string);
gchar* gwy_si_unit_get_unit_string(GwySIUnit *siunit);

void gwy_si_unit_get_prefix(GwySIUnit *siunit, gdouble value, gint precision, gchar *prefix, gdouble *power);
void gwy_si_unit_get_prefixed(GwySIUnit *siunit, gdouble value, gint precision, gchar *prefix, gdouble *power);

void gwy_si_unit_copy(GwySIUnit *target, GwySIUnit *example);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __GWY_SI_UNIT_H__ */

/* vim: set cin et ts=4 sw=4 cino=>1s,e0,n0,f0,{0,}0,^0,\:1s,=0,g1s,h0,t0,+1s,c3,(0,u0 : */
