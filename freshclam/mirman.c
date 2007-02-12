/*
 *  Copyright (C) 2007 Tomasz Kojm <tkojm@clamav.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
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

#ifdef	C_WINDOWS
#define	_USE_32BIT_TIME_T	/* FIXME: mirdat.atime assumes 32bit time_t */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#include "mirman.h"

#include "libclamav/cltypes.h"
#include "libclamav/clamav.h"

#include "shared/output.h"

#ifndef O_BINARY
#define O_BINARY    0
#endif

#define IGNTIME 3 * 86400

void mirman_free(struct mirdat *mdat)
{
    if(mdat && mdat->num) {
	free(mdat->mirtab);
	mdat->num = 0;
    }
}

int mirman_read(const char *file, struct mirdat *mdat, uint8_t active)
{
	struct mirdat_ip mip;
	int fd, bread;


    memset(mdat, 0, sizeof(struct mirdat));

    if(!(mdat->active = active))
	return 0;

    if((fd = open(file, O_RDONLY|O_BINARY)) == -1)
	return -1;

    while((bread = read(fd, &mip, sizeof(mip))) == sizeof(mip)) {
	mdat->mirtab = (struct mirdat_ip *) realloc(mdat->mirtab, (mdat->num + 1) * sizeof(mip));
	if(!mdat->mirtab) {
	    logg("!Can't allocate memory for mdat->mirtab\n");
	    mirman_free(mdat);
	    close(fd);
	    return -1;
	}
	memcpy(&mdat->mirtab[mdat->num], &mip, sizeof(mip));
	mdat->num++;
    }

    close(fd);

    if(bread) {
	logg("^Removing broken %s file.\n");
	unlink(file);
	mirman_free(mdat);
	return -1;
    }

    return 0;
}

int mirman_check(uint32_t ip, struct mirdat *mdat)
{
	unsigned int i, flevel = cl_retflevel();


    if(!mdat->active)
	return 0;

    for(i = 0; i < mdat->num; i++) {
	if(mdat->mirtab[i].ip == ip) {

	    if(mdat->dbflevel && (mdat->dbflevel > flevel) && (mdat->dbflevel - flevel > 3))
		if(time(NULL) - mdat->mirtab[i].atime < 4 * 3600)
		    return 2;

	    if(mdat->mirtab[i].ignore) {
		if(time(NULL) - mdat->mirtab[i].atime > IGNTIME) {
		    mdat->mirtab[i].ignore = 0;
		    return 0;
		} else {
		    return 1;
		}
	    }
	}
    }

    return 0;
}

int mirman_update(uint32_t ip, struct mirdat *mdat, uint8_t broken)
{
	unsigned int i, found = 0;


    if(!mdat->active)
	return 0;

    for(i = 0; i < mdat->num; i++) {
	if(mdat->mirtab[i].ip == ip) {
	    found = 1;
	    break;
	}
    }

    if(found) {
	mdat->mirtab[i].atime = (uint32_t) time(NULL);
	if(broken)
	    mdat->mirtab[i].fail++;
	else
	    mdat->mirtab[i].succ++;

	/*
	 * If the total number of failures is less than 3 then never
	 * enable the ignore flag, in other case use the real status.
	 */
	if(mdat->mirtab[i].fail < 3)
	    mdat->mirtab[i].ignore = 0;
	else
	    mdat->mirtab[i].ignore = broken;

    } else {
	mdat->mirtab = (struct mirdat_ip *) realloc(mdat->mirtab, (mdat->num + 1) * sizeof(struct mirdat_ip));
	if(!mdat->mirtab) {
	    logg("!Can't allocate memory for new element in mdat->mirtab\n");
	    return -1;
	}
	mdat->mirtab[mdat->num].ip = ip;
	mdat->mirtab[mdat->num].atime = (uint32_t) time(NULL);
	mdat->mirtab[mdat->num].succ = 0;
	mdat->mirtab[mdat->num].fail = 0;
	mdat->mirtab[mdat->num].ignore = 0;
	memset(&mdat->mirtab[mdat->num].res, 0xff, sizeof(mdat->mirtab[mdat->num].res));
	if(broken)
	    mdat->mirtab[mdat->num].fail++;
	else
	    mdat->mirtab[mdat->num].succ++;
	mdat->num++;
    }

    return 0;
}

void mirman_list(const struct mirdat *mdat)
{
	unsigned int i;
	unsigned char *ip;

    for(i = 0; i < mdat->num; i++) {
	printf("Mirror #%u\n", i + 1);
	ip = (unsigned char *) &mdat->mirtab[i].ip;
	printf("IP: %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
	printf("Successes: %u\n", mdat->mirtab[i].succ);
	printf("Failures: %u\n", mdat->mirtab[i].fail);
	printf("Last access: %s", ctime((const time_t *)&mdat->mirtab[i].atime));
	printf("Ignore: %s\n", mdat->mirtab[i].ignore ? "Yes" : "No");
	if(i != mdat->num - 1)
	    printf("-------------------------------------\n");
    }
}

int mirman_write(const char *file, struct mirdat *mdat)
{
	int fd;


    if(!mdat->num)
	return 0;

    if((fd = open(file, O_WRONLY|O_CREAT|O_TRUNC|O_BINARY, 0600)) == -1) {
	logg("!Can't open %s for writing\n", file);
	mirman_free(mdat);
	return -1;
    }

    if(write(fd, mdat->mirtab, mdat->num * sizeof(struct mirdat_ip)) == -1) {
	logg("!Can't write to %s\n", file);
	mirman_free(mdat);
	close(fd);
	return -1;
    }

    mirman_free(mdat);
    close(fd);
    return 0;
}