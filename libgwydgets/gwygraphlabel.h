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

#ifndef __GWY_GRAPH_LABEL_H__
#define __GWY_GRAPH_LABEL_H__

#include <gdk/gdk.h>
#include <gtk/gtkadjustment.h>
#include <gtk/gtkwidget.h>

#define GWY_GRAPH_LABEL_NORTHEAST 0
#define GWY_GRAPH_LABEL_NORTHWEST 1
#define GWY_GRAPH_LABEL_SOUTHEAST 2
#define GWY_GRAPH_LABEL_SOUTHWEST 3

#define GWY_GRAPH_POINT_SQUARE         0
#define GWY_GRAPH_POINT_CROSS          1
#define GWY_GRAPH_POINT_CIRCLE         2
#define GWY_GRAPH_POINT_STAR           3
#define GWY_GRAPH_POINT_TIMES          4
#define GWY_GRAPH_POINT_TRIANGLE_UP    5
#define GWY_GRAPH_POINT_TRIANGLE_DOWN  6
#define GWY_GRAPH_POINT_DIAMOND        7

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define GWY_TYPE_GRAPH_LABEL            (gwy_graph_label_get_type())
#define GWY_GRAPH_LABEL(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), GWY_TYPE_GRAPH_LABEL, GwyGraphLabel))
#define GWY_GRAPH_LABEL_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), GWY_TYPE_GRAPH_LABEL, GwyGraphLabel))
#define GWY_IS_GRAPH_LABEL(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), GWY_TYPE_GRAPH_LABEL))
#define GWY_IS_GRAPH_LABEL_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GWY_TYPE_GRAPH_LABEL))
#define GWY_GRAPH_LABEL_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), GWY_TYPE_GRAPH_LABEL, GwyGraphLabelClass))

/*single curve properties*/
typedef struct {
    gint is_line;
    gint is_point;

    gint point_size;
    gint point_type;

    GdkLineStyle line_style;
    gint line_size;

    GString *description;
    GdkColor color;
} GwyGraphAreaCurveParams;
    

typedef struct {
    gboolean is_frame;
    gint frame_thickness;
    gint position;
    PangoFontDescription *font;
} GwyGraphLabelParams;

typedef struct {
    GtkWidget widget;

    GwyGraphLabelParams par; 
    gboolean is_visible;
    gint maxwidth;
    gint maxheight;

    GPtrArray *curve_params;
} GwyGraphLabel;

typedef struct {
     GtkWidgetClass parent_class;
} GwyGraphLabelClass;


GtkWidget* gwy_graph_label_new();

GType gwy_graph_label_get_type(void) G_GNUC_CONST;

void gwy_graph_label_set_visible(GwyGraphLabel *label, gboolean is_visible);

void gwy_graph_label_set_style(GwyGraphLabel *label, GwyGraphLabelParams style);

void gwy_graph_label_add_curve(GwyGraphLabel *label, GwyGraphAreaCurveParams *params);

void gwy_graph_label_clear(GwyGraphLabel *label);

void  gwy_graph_draw_point (GdkWindow *window, 
                            GdkGC *gc, gint i, gint j, 
                            gint type, gint size, GdkColor *color, gboolean clear);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /*__GWY_AXIS_H__*/
