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

#ifndef __GWY_CWT_H__
#define __GWY_CWT_H__

#include <glib.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum {
  GWY_2DCWT_GAUSS       = 0,
  GWY_2DCWT_HAT         = 1
} Gwy2DCWTWaveletType;

typedef enum {
  GWY_CWT_GAUSS       = 0,
  GWY_CWT_HAT         = 1,
  GWY_CWT_MORLET      = 2
} GwyCWTWaveletType;


gdouble wfunc_2d(gdouble scale, gdouble mval, gint xres, Gwy2DCWTWaveletType wtype);


#ifdef __cplusplus
}
#endif /* __cplusplus */


#endif /*__GWY_CWT__*/
