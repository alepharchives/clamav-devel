/*
 *  By Per Jessen <per@computer.org>
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

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "shared/output.h"
#include "execute.h"

#define MAX_CHILDREN 5

int active_children;

void execute( const char *type, const char *text )
{
	pid_t pid;

	if ( active_children<MAX_CHILDREN )
	switch( pid=fork() ) {
	case 0:
		if ( -1==system(text) )
		{
		logg("^%s: couldn't execute \"%s\".\n", type, text);
		}
		exit(0);
	case -1:
		logg("^%s::fork() failed, %s.\n", type, strerror(errno));
		break;
	default:
		active_children++;
	}
	else
	{
		logg("^%s: already %d processes active.\n", type, active_children);
	}
}