/*
 *  Copyright (C) 2002 Tomasz Kojm <tkojm@clamav.net>
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
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#ifndef __SCANNER_H
#define __SCANNER_H

#include "libclamav/clamav.h"
#include "shared/cfgparser.h"

int dirscan(const char *dirname, const char **virname, unsigned long int *scanned, const struct cl_engine *engine, const struct cl_limits *limits, unsigned int options, const struct cfgstruct *copt, int odesc, unsigned int *reclev, short contscan);

int scan(const char *filename, unsigned long int *scanned, const struct cl_engine *engine, const struct cl_limits *limits, unsigned int options, const struct cfgstruct *copt, int odesc, short contscan);

int scanfd(const int fd, unsigned long int *scanned, const struct cl_engine *engine, const struct cl_limits *limits, unsigned int options, const struct cfgstruct *copt, int odesc);

int scanstream(int odesc, unsigned long int *scanned, const struct cl_engine *engine, const struct cl_limits *limits, unsigned int options, const struct cfgstruct *copt);

int checksymlink(const char *path);

#endif