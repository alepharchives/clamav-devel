/*
 *  Copyright (C) 2002 - 2006 Tomasz Kojm <tkojm@clamav.net>
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
#include <string.h>
#include <unistd.h>
#include <zlib.h>
#include <time.h>
#include <locale.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <dirent.h>

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#include "vba.h"

#include "shared/options.h"
#include "shared/memory.h"
#include "shared/output.h"
#include "shared/cfgparser.h"
#include "shared/misc.h"
#include "shared/cdiff.h"

#include "libclamav/clamav.h"
#include "libclamav/cvd.h"
#include "libclamav/others.h"
#include "libclamav/str.h"
#include "libclamav/ole2_extract.h"
#include "libclamav/htmlnorm.h"
#include "libclamav/sha256.h"

#define MAX_DEL_LOOKAHEAD   50

static int hexdump(void)
{
	char buffer[FILEBUFF], *pt;
	int bytes;


    while((bytes = read(0, buffer, FILEBUFF)) > 0) {
	pt = cli_str2hex(buffer, bytes);
	if(write(1, pt, 2 * bytes) == -1) {
	    mprintf("!hexdump: Can't write to stdout\n");
	    free(pt);
	    return -1;
	}
	free(pt);
    }

    if(bytes == -1)
	return -1;

    return 0;
}

static int md5sig(struct optstruct *opt, unsigned int mdb)
{
	char *md5, *filename;
	int i;
	struct stat sb;


    if(opt->filename) {
	for(i = 0; (filename = cli_strtok(opt->filename, i, "\t")); i++) {
	    if(stat(filename, &sb) == -1) {
		mprintf("!md5sig: Can't access file %s\n", filename);
		perror("md5sig");
		free(filename);
		return -1;
	    } else {
		if((sb.st_mode & S_IFMT) == S_IFREG) {
		    if((md5 = cli_md5file(filename))) {
			if(mdb)
			    mprintf("%d:%s:%s\n", sb.st_size, md5, filename);
			else
			    mprintf("%s:%d:%s\n", md5, sb.st_size, filename);
			free(md5);
		    } else {
			mprintf("!md5sig: Can't generate MD5 checksum for %s\n", filename);
			free(filename);
			return -1;
		    }
		}
	    }

	    free(filename);
	}

    } else { /* stream */
	md5 = cli_md5stream(stdin, NULL);
	if(!md5) {
	    mprintf("!md5sig: Can't generate MD5 checksum for input stream\n");
	    return -1;
	}
	mprintf("%s\n", md5);
	free(md5);
    }

    return 0;
}

static int htmlnorm(struct optstruct *opt)
{
	int fd;


    if((fd = open(opt_arg(opt, "html-normalise"), O_RDONLY)) == -1) {
	mprintf("!htmlnorm: Can't open file %s\n", opt_arg(opt, "html-normalise"));
	return -1;
    }

    html_normalise_fd(fd, ".", NULL);
    close(fd);

    return 0;
}

static int utf16decode(struct optstruct *opt)
{
	const char *fname;
	char *newname, buff[512], *decoded;
	int fd1, fd2, bytes;


    fname = opt_arg(opt, "utf16-decode");
    if((fd1 = open(fname, O_RDONLY)) == -1) {
	mprintf("!utf16decode: Can't open file %s\n", fname);
	return -1;
    }

    newname = malloc(strlen(fname) + 7);
    sprintf(newname, "%s.ascii", fname);

    if((fd2 = open(newname, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU)) < 0) {
	mprintf("!utf16decode: Can't create file %s\n", newname);
	free(newname);
	close(fd1);
	return -1;
    }

    while((bytes = read(fd1, buff, sizeof(buff))) > 0) {
	decoded = cli_utf16toascii(buff, bytes);
	if(decoded) {
	    if(write(fd2, decoded, strlen(decoded)) == -1) {
		mprintf("!utf16decode: Can't write to file %s\n", newname);
		free(decoded);
		unlink(newname);
		free(newname);
		close(fd1);
		close(fd2);
		return -1;
	    }
	    free(decoded);
	}
    }

    free(newname);
    close(fd1);
    close(fd2);

    return 0;
}

static unsigned int countlines(const char *filename)
{
	FILE *fd;
	char buff[1024];
	unsigned int lines = 0;


    if((fd = fopen(filename, "r")) == NULL)
	return 0;

    while(fgets(buff, sizeof(buff), fd)) {
	if(buff[0] == '#') continue;
	lines++;
    }

    fclose(fd);
    return lines;
}

static char *getdsig(const char *host, const char *user, const char *data, unsigned int datalen, unsigned short mode)
{
	char buff[512], cmd[128], pass[30], *pt;
        struct sockaddr_in server;
	int sockd, bread, len;
#ifdef HAVE_TERMIOS_H
	struct termios old, new;
#endif


    if((pt = getenv("SIGNDPASS"))) {
	strncpy(pass, pt, sizeof(pass));
    } else {
	fflush(stdin);
	mprintf("Password: ");

#ifdef HAVE_TERMIOS_H
	if(tcgetattr(0, &old)) {
	    mprintf("!getdsig: tcgetattr() failed\n");
	    return NULL;
	}
	new = old;
	new.c_lflag &= ~ECHO;
	if(tcsetattr(0, TCSAFLUSH, &new)) {
	    mprintf("!getdsig: tcsetattr() failed\n");
	    return NULL;
	}
#endif

	if(fgets(pass, sizeof(pass), stdin)) {
	    cli_chomp(pass);
	} else {
	    mprintf("!getdsig: Can't get password\n");
	    return NULL;
	}

#ifdef HAVE_TERMIOS_H
	if(tcsetattr(0, TCSAFLUSH, &old)) {
	    mprintf("!getdsig: tcsetattr() failed\n", host);
	    memset(pass, 0, strlen(pass));
	    return NULL;
	}
#endif
	mprintf("\n");
    }

#ifdef PF_INET
    if((sockd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
#else
    if((sockd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
#endif
	perror("socket()");
	mprintf("!getdsig: Can't create socket\n");
	memset(pass, 0, strlen(pass));
	return NULL;
    }

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr(host);
    server.sin_port = htons(33101);

    if(connect(sockd, (struct sockaddr *) &server, sizeof(struct sockaddr_in)) < 0) {
        close(sockd);
	perror("connect()");
	mprintf("!getdsig: Can't connect to ClamAV Signing Service at %s\n", host);
	memset(pass, 0, strlen(pass));
	return NULL;
    }
    memset(cmd, 0, sizeof(cmd));

    if(mode == 1)
	snprintf(cmd, sizeof(cmd) - datalen, "ClamSignPSS:%s:%s:", user, pass);
    else
	snprintf(cmd, sizeof(cmd) - datalen, "ClamSign:%s:%s:", user, pass);

    len = strlen(cmd);
    pt = cmd + len;
    memcpy(pt, data, datalen);
    len += datalen;

    if(write(sockd, cmd, len) < 0) {
	mprintf("!getdsig: Can't write to socket\n");
	close(sockd);
	memset(cmd, 0, len);
	memset(pass, 0, strlen(pass));
	return NULL;
    }

    memset(cmd, 0, len);
    memset(pass, 0, strlen(pass));
    memset(buff, 0, sizeof(buff));

    if((bread = cli_readn(sockd, buff, sizeof(buff))) > 0) {
	if(!strstr(buff, "Signature:")) {
	    mprintf("!getdsig: Error generating digital signature\n");
	    mprintf("!getdsig: Answer from remote server: %s\n", buff);
	    close(sockd);
	    return NULL;
	} else {
	    mprintf("Signature received (length = %u)\n", strlen(buff) - 10);
	}
    } else {
	mprintf("!getdsig: Communication error with remote server\n");
	close(sockd);
	return NULL;
    }

    close(sockd);

    pt = buff;
    pt += 10;
    return strdup(pt);
}

static int writeinfo(const char *db, const char *header)
{
	FILE *fh;
	int i;
	struct stat sb;
	char file[32], *md5;
	char *extlist[] = { "db", "fp", "hdb", "mdb", "ndb", "pdb", "rmd", "zmd", "sdb", "cfg", NULL };


    snprintf(file, sizeof(file), "%s.info", db);
    if(stat(file, &sb) != -1) {
	if(unlink(file) == -1) {
	    mprintf("!writeinfo: Can't unlink %s\n", file);
	    return -1;
	}
    }

    if(!(fh = fopen(file, "w"))) {
	mprintf("!writeinfo: Can't create file %s\n", file);
	return -1;
    }

    if(fprintf(fh, "%s\n", header) < 0) {
	mprintf("!writeinfo: Can't write to %s\n", file);
	fclose(fh);
	return -1;
    }

    for(i = 0; extlist[i]; i++) {
	snprintf(file, sizeof(file), "%s.%s", db, extlist[i]);
	if(stat(file, &sb) != -1) {
	    if(!(md5 = cli_md5file(file))) {
		mprintf("!writeinfo: Can't generate MD5 checksum for %s\n", file);
		fclose(fh);
		return -1;
	    }
	    if(fprintf(fh, "%s.%s:%s\n", db, extlist[i], md5) < 0) {
		mprintf("!writeinfo: Can't write to info file\n");
		fclose(fh);
		free(md5);
		return -1;
	    }
	    free(md5);
	}
    }

    fclose(fh);
    return 0;
}

static int diffdirs(const char *old, const char *new, const char *patch);
static int verifydiff(const char *diff, const char *cvd, const char *incdir);

static int script2cdiff(const char *script, const char *builder, struct optstruct *opt)
{
	char *cdiff, *pt, buffer[FILEBUFF];
	unsigned char digest[32];
	SHA256_CTX ctx;
	struct stat sb;
	FILE *scripth, *cdiffh;
	gzFile *gzh;
	unsigned int ver, osize;
	int bytes;


    if(stat(script, &sb) == -1) {
	mprintf("!script2diff: Can't stat file %s\n", script);
	return -1;
    }
    osize = (unsigned int) sb.st_size;

    cdiff = strdup(script);
    pt = strstr(cdiff, ".script");
    if(!pt) {
	mprintf("!script2cdiff: Incorrect file name (no .script extension)\n");
	free(cdiff);
	return -1;
    }
    strcpy(pt, ".cdiff");

    if(!(pt = strchr(script, '-'))) {
	mprintf("!script2cdiff: Incorrect file name syntax\n");
	free(cdiff);
	return -1;
    }
    sscanf(++pt, "%u.script", &ver);

    if(!(cdiffh = fopen(cdiff, "wb"))) {
	mprintf("!script2cdiff: Can't open %s for writing\n", cdiff);
	free(cdiff);
	return -1;
    }

    if(fprintf(cdiffh, "ClamAV-Diff:%u:%u:", ver, osize) < 0) {
	mprintf("!script2cdiff: Can't write to %s\n", cdiff);
	fclose(cdiffh);
	free(cdiff);
	return -1;
    }
    fclose(cdiffh);

    if(!(scripth = fopen(script, "rb"))) {
	mprintf("!script2cdiff: Can't open file %s for reading\n", script);
	unlink(cdiff);
	free(cdiff);
	return -1;
    }

    if(!(gzh = gzopen(cdiff, "ab"))) {
	mprintf("!script2cdiff: Can't open file %s for appending\n", cdiff);
	unlink(cdiff);
	free(cdiff);
	fclose(scripth);
	return -1;
    }

    while((bytes = fread(buffer, 1, sizeof(buffer), scripth)) > 0) {
	if(!gzwrite(gzh, buffer, bytes)) {
	    mprintf("!script2cdiff: Can't gzwrite to %s\n", cdiff);
	    unlink(cdiff);
	    free(cdiff);
	    fclose(scripth);
	    gzclose(gzh);
	    return -1;
	}
    }
    fclose(scripth);
    gzclose(gzh);

    if(!(cdiffh = fopen(cdiff, "rb"))) {
	mprintf("!script2cdiff: Can't open %s for reading/writing\n", cdiff);
	unlink(cdiff);
	free(cdiff);
	return -1;
    }

    sha256_init(&ctx);

    while((bytes = fread(buffer, 1, sizeof(buffer), cdiffh)))
	sha256_update(&ctx, (unsigned char *) buffer, bytes);

    fclose(cdiffh);
    sha256_final(&ctx);
    sha256_digest(&ctx, digest);

    if(!(pt = getdsig(opt_arg(opt, "server"), builder, (char *) digest, 32, 1))) {
	mprintf("!script2cdiff: Can't get digital signature from remote server\n");
	unlink(cdiff);
	free(cdiff);
	return -1;
    }

    if(!(cdiffh = fopen(cdiff, "ab"))) {
	mprintf("!script2cdiff: Can't open %s for appending\n", cdiff);
	unlink(cdiff);
	free(cdiff);
	return -1;
    }
    fprintf(cdiffh, ":%s", pt);
    free(pt);
    fclose(cdiffh);

    mprintf("Created %s\n", cdiff);
    free(cdiff);

    return 0;
}

static int build(struct optstruct *opt)
{
	int ret, inc = 1, dn;
	size_t bytes;
	unsigned int sigs = 0, oldsigs = 0, lines = 0, version, real_header, fl;
	struct stat foo;
	char buffer[FILEBUFF], *tarfile, *gzfile, header[513], smbuff[32],
	     builder[32], *pt, *dbname, olddb[512], patch[32], broken[32];
        struct cl_engine *engine = NULL;
	FILE *tar, *cvd;
	gzFile *gz;
	time_t timet;
	struct tm *brokent;
	struct cl_cvd *oldcvd;


    if(!opt_check(opt, "server")) {
	mprintf("!build: --server is required for --build\n");
	return -1;
    }

    if(stat("COPYING", &foo) == -1) {
	mprintf("!build: COPYING file not found in current working directory.\n");
	return -1;
    }

    if(stat("main.db", &foo) == -1 && stat("daily.db", &foo) == -1 &&
       stat("main.hdb", &foo) == -1 && stat("daily.hdb", &foo) == -1 &&
       stat("main.mdb", &foo) == -1 && stat("daily.mdb", &foo) == -1 &&
       stat("main.ndb", &foo) == -1 && stat("daily.ndb", &foo) == -1 &&
       stat("main.pdb", &foo) == -1 && stat("daily.pdb", &foo) == -1 &&
       stat("main.sdb", &foo) == -1 && stat("daily.sdb", &foo) == -1 &&
       stat("main.zmd", &foo) == -1 && stat("daily.zmd", &foo) == -1 &&
       stat("main.rmd", &foo) == -1 && stat("daily.rmd", &foo) == -1)
    {
	mprintf("!build: No virus database file  found in current directory\n");
	return -1;
    }

    if((ret = cl_load(".", &engine, &sigs, CL_DB_STDOPT))) {
	mprintf("!build: Can't load database: %s\n", cl_strerror(ret));
	return -1;
    } else {
	cl_free(engine);
    }

    if(!sigs) {
	mprintf("!build: There are no signatures in database files\n");
    } else {
	lines = countlines("main.db") + countlines("daily.db") +
		countlines("main.hdb") + countlines("daily.hdb") +
		countlines("main.mdb") + countlines("daily.mdb") +
		countlines("main.ndb") + countlines("daily.ndb") +
		countlines("main.sdb") + countlines("daily.sdb") +
		countlines("main.zmd") + countlines("daily.zmd") +
		countlines("main.rmd") + countlines("daily.rmd") +
		countlines("main.fp") + countlines("daily.fp");

	if(lines != sigs) {
	    mprintf("^build: Signatures in database: %d, loaded by libclamav: %d\n", lines, sigs);
	    mprintf("^build: Please check the current directory and remove unnecessary databases\n");
	    mprintf("^build: or install the latest ClamAV version.\n");
	}
    }

    /* try to read cvd header of current database */
    dbname = opt_arg(opt, "build");
    if(strstr(dbname, "main"))
	dbname = "main";
    else
	dbname = "daily";


    if(opt->filename) {
	if(cli_strbcasestr(opt->filename, ".cvd")) {
	    strncpy(olddb, opt->filename, sizeof(olddb));
	    inc = 0;
	} else if(cli_strbcasestr(opt->filename, ".inc")) {
	    snprintf(olddb, sizeof(olddb), "%s/%s.info", opt->filename, dbname);
	} else {
	    mprintf("!build: The optional argument points to neither CVD nor incremental directory\n");
	    return -1;
	}

    } else {
	pt = freshdbdir();
	snprintf(olddb, sizeof(olddb), "%s/%s.inc/%s.info", pt, dbname, dbname);
	if(stat(olddb, &foo) == -1) {
	    inc = 0;
	    snprintf(olddb, sizeof(olddb), "%s/%s.cvd", pt, dbname);
	}
	free(pt);
    }

    if(!(oldcvd = cl_cvdhead(olddb))) {
	mprintf("^build: CAN'T READ CVD HEADER OF CURRENT DATABASE %s\n", olddb);
	sleep(3);
    }

    if(oldcvd) {
	version = oldcvd->version + 1;
	oldsigs = oldcvd->sigs;
	cl_cvdfree(oldcvd);
    } else {
	fflush(stdin);
	mprintf("Version number: ");
	scanf("%u", &version);
    }

    mprintf("Total sigs: %u\n", sigs);
    if(sigs > oldsigs)
	mprintf("New sigs: %u\n", sigs - oldsigs);

    strcpy(header, "ClamAV-VDB:");

    /* time */
    time(&timet);
    brokent = localtime(&timet);
    setlocale(LC_TIME, "C");
    strftime(smbuff, sizeof(smbuff), "%d %b %Y %H-%M %z", brokent);
    strcat(header, smbuff);

    /* version */
    sprintf(smbuff, ":%d:", version);
    strcat(header, smbuff);

    /* number of signatures */
    sprintf(smbuff, "%d:", sigs);
    strcat(header, smbuff);

    /* functionality level */
    if(!strcmp(dbname, "main")) {
	fflush(stdin);
	mprintf("Functionality level: ");
	scanf("%u", &fl);
    } else {
	fl = cl_retflevel();
    }
    sprintf(smbuff, "%u:", fl);
    strcat(header, smbuff);

    real_header = strlen(header);

    /* add fake MD5 and dsig (for writeinfo) */
    strcat(header, "X:X:");

    if((pt = getenv("SIGNDUSER"))) {
	strncpy(builder, pt, sizeof(builder));
    } else {
	/* ask for builder name */
	fflush(stdin);
	mprintf("Builder name: ");
	if(fgets(builder, sizeof(builder), stdin)) {
	    cli_chomp(builder);
	} else {
	    mprintf("!build: Can't get builder name\n");
	    return -1;
	}
    }

    /* add builder */
    strcat(header, builder);

    /* add current time */
    sprintf(header + strlen(header), ":%d", (int) timet);

    if(writeinfo(dbname, header) == -1) {
	mprintf("!build: Can't generate info file\n");
	return -1;
    }

    header[real_header] = 0;

    if(!(tarfile = cli_gentemp("."))) {
	mprintf("!build: Can't generate temporary name for tarfile\n");
	return -1;
    }

    switch(fork()) {
	case -1:
	    mprintf("!build: Can't fork.\n");
	    free(tarfile);
	    return -1;
	case 0:
	    {
		char *args[] = { "tar", "-cvf", NULL, "COPYING", "main.db",
				 "daily.db", "main.hdb", "daily.hdb",
				 "main.ndb", "daily.ndb", "main.sdb",
				 "daily.sdb", "main.zmd", "daily.zmd",
				 "main.rmd", "daily.rmd", "main.fp",
				 "daily.fp", "main.mdb", "daily.mdb",
				 "daily.info", "main.info", "main.wdb",
				 "daily.wdb", "main.pdb", "daily.pdb",
				 "main.cfg", "daily.cfg",
				 NULL };
		args[2] = tarfile;
		if(!opt_check(opt, "debug")) {
		    if((dn = open("/dev/null", O_WRONLY)) == -1) {
			mprintf("^Cannot open /dev/null\n");
			close(1);
			close(2);
		    } else {
			dup2(dn, 1);
			dup2(dn, 2);
			close(dn);
		    }
		}
		execv("/bin/tar", args);
		mprintf("!build: Can't execute tar\n");
		perror("tar");
		free(tarfile);
		return -1;
	    }
	default:
	    wait(NULL);
    }

    if(stat(tarfile, &foo) == -1) {
	mprintf("!build: Tar archive was not created\n");
	free(tarfile);
	return -1;
    }

    if((tar = fopen(tarfile, "rb")) == NULL) {
	mprintf("!build: Can't open file %s\n", tarfile);
	free(tarfile);
	return -1;
    }

    if(!(gzfile = cli_gentemp("."))) {
	mprintf("!build: Can't generate temporary name for gzfile\n");
	free(tarfile);
	fclose(tar);
	return -1;
    }

    if((gz = gzopen(gzfile, "wb")) == NULL) {
	mprintf("!build: Can't open file %s to write.\n", gzfile);
	free(tarfile);
	fclose(tar);
	free(gzfile);
	return -1;
    }

    while((bytes = fread(buffer, 1, FILEBUFF, tar)) > 0) {
	if(!gzwrite(gz, buffer, bytes)) {
	    mprintf("!build: Can't gzwrite to %s\n", gzfile);
	    fclose(tar);
	    gzclose(gz);
	    free(tarfile);
	    free(gzfile);
	    return -1;
	}
    }

    fclose(tar);
    gzclose(gz);
    unlink(tarfile);
    free(tarfile);

    /* MD5 */
    if(!(pt = cli_md5file(gzfile))) {
	mprintf("!build: Can't generate MD5 checksum for gzfile\n");
	unlink(gzfile);
	free(gzfile);
	return -1;
    }
    strcat(header, pt);
    free(pt);
    strcat(header, ":");

    /* digital signature */
    if(!(tar = fopen(gzfile, "rb"))) {
	mprintf("!build: Can't open file %s for reading\n", gzfile);
	unlink(gzfile);
	free(gzfile);
	return -1;
    }

    if(!(pt = cli_md5stream(tar, (unsigned char *) buffer))) {
	mprintf("!build: Can't generate MD5 checksum for %s\n", gzfile);
	unlink(gzfile);
	free(gzfile);
	return -1;
    }
    free(pt);
    rewind(tar);

    if(!(pt = getdsig(opt_arg(opt, "server"), builder, buffer, 16, 0))) {
	mprintf("!build: Can't get digital signature from remote server\n");
	unlink(gzfile);
	free(gzfile);
	fclose(tar);
	return -1;
    }
    strcat(header, pt);
    free(pt);
    strcat(header, ":");

    /* add builder */
    strcat(header, builder);

    /* add current time */
    sprintf(header + strlen(header), ":%d", (int) timet);

    /* fill up with spaces */
    while(strlen(header) < sizeof(header) - 1)
	strcat(header, " ");

    /* build the final database */
    pt = opt_arg(opt, "build");
    if(!(cvd = fopen(pt, "wb"))) {
	mprintf("!build: Can't create final database %s\n", pt);
	unlink(gzfile);
	free(gzfile);
	fclose(tar);
	return -1;
    }

    if(fwrite(header, 1, 512, cvd) != 512) {
	mprintf("!build: Can't write to %s\n", pt);
	fclose(cvd);
	fclose(tar);
	unlink(pt);
	unlink(gzfile);
	free(gzfile);
	return -1;
    }

    while((bytes = fread(buffer, 1, FILEBUFF, tar)) > 0) {
	if(fwrite(buffer, 1, bytes, cvd) != bytes) {
	    fclose(tar);
	    fclose(cvd);
	    unlink(pt);
	    mprintf("!build: Can't write to %s\n", gzfile);
	    unlink(gzfile);
	    free(gzfile);
	    return -1;
	}
    }

    fclose(tar);
    fclose(cvd);
    if(unlink(gzfile) == -1) {
	mprintf("^build: Can't unlink %s\n", gzfile);
	return -1;
    }
    free(gzfile);

    mprintf("Created %s\n", pt);

    /* generate patch */
    if(opt->filename) {
	strncpy(olddb, opt->filename, sizeof(olddb));
    } else {
	if(inc) {
	    pt = freshdbdir();
	    snprintf(olddb, sizeof(olddb), "%s/%s.inc", pt, dbname);
	    free(pt);
	} else {
	    pt = freshdbdir();
	    snprintf(olddb, sizeof(olddb), "%s/%s.cvd", pt, dbname);
	    free(pt);
	}
    }

    if(!inc) {
	pt = cli_gentemp(NULL);
	if(mkdir(pt, 0700)) {
	    mprintf("!build: Can't create temporary directory %s\n", pt);
	    return -1;
	}
	if(cvd_unpack(olddb, pt) == -1) {
	    mprintf("!build: Can't unpack CVD file %s\n", olddb);
	    rmdirs(pt);
	    free(pt);
	    return -1;
	}
	strncpy(olddb, pt, sizeof(olddb));
    }

    pt = cli_gentemp(NULL);
    if(mkdir(pt, 0700)) {
	mprintf("!build: Can't create temporary directory %s\n", pt);
	free(pt);
	if(!inc)
	    rmdirs(olddb);
	return -1;
    }
    if(cvd_unpack(opt_arg(opt, "build"), pt) == -1) {
	mprintf("!build: Can't unpack CVD file %s\n", opt_arg(opt, "build"));
	rmdirs(pt);
	free(pt);
	if(!inc)
	    rmdirs(olddb);
	return -1;
    }

    if(!strcmp(dbname, "main"))
	snprintf(patch, sizeof(patch), "main-%u.script", version);
    else
	snprintf(patch, sizeof(patch), "daily-%u.script", version);

    ret = diffdirs(olddb, pt, patch);

    rmdirs(pt);
    free(pt);

    if(ret == -1) {
	if(!inc)
	    rmdirs(olddb);
	return -1;
    }

    ret = verifydiff(patch, NULL, olddb);

    if(!inc)
	rmdirs(olddb);

    if(ret == -1) {
	snprintf(broken, sizeof(broken), "%s.broken", patch);
	if(rename(patch, broken)) {
	    unlink(patch);
	    mprintf("!Generated file is incorrect, removed");
	} else {
	    mprintf("!Generated file is incorrect, renamed to %s\n", broken);
	}
    } else {
	ret = script2cdiff(patch, builder, opt);
    }

    return ret;
}

static int unpack(struct optstruct *opt)
{
	char *name, *dbdir;
	struct stat sb;


    if(opt_check(opt, "unpack-current")) {
	dbdir = freshdbdir();
	name = mcalloc(strlen(dbdir) + strlen(opt_arg(opt, "unpack-current")) + 32, sizeof(char));
	sprintf(name, "%s/%s.inc", dbdir, opt_arg(opt, "unpack-current"));
	if(stat(name, &sb) != -1) {

	    if(dircopy(name, ".") == -1) {
		mprintf("!unpack: Can't copy incremental directory %s to local directory\n", name);
		free(name);
		free(dbdir);
		return -1;
	    }

	    return 0;

	} else {
	    sprintf(name, "%s/%s.cvd", dbdir, opt_arg(opt, "unpack-current"));
	}
	free(dbdir);

    } else
	name = strdup(opt_arg(opt, "unpack"));

    if(cvd_unpack(name, ".") == -1) {
	mprintf("!unpack: Can't unpack CVD file %s\n", name);
	free(name);
	return -1;
    }

    free(name);
    return 0;
}

static int cvdinfo(struct optstruct *opt)
{
	struct cl_cvd *cvd;
	char *pt;
	int ret;


    pt = opt_arg(opt, "info");
    if((cvd = cl_cvdhead(pt)) == NULL) {
	mprintf("!cvdinfo: Can't read/parse CVD header of %s\n", pt);
	return -1;
    }

    pt = strchr(cvd->time, '-');
    *pt = ':';
    mprintf("Build time: %s\n", cvd->time);
    mprintf("Version: %d\n", cvd->version);
    mprintf("Signatures: %d\n", cvd->sigs);
    mprintf("Functionality level: %d\n", cvd->fl);
    mprintf("Builder: %s\n", cvd->builder);
    mprintf("MD5: %s\n", cvd->md5);
    mprintf("Digital signature: %s\n", cvd->dsig);

#ifndef HAVE_GMP
    mprintf("^Digital signature support not compiled in.\n");
#endif

    pt = opt_arg(opt, "info");
    if((ret = cl_cvdverify(pt)))
	mprintf("!cvdinfo: Verification: %s\n", cl_strerror(ret));
    else
	mprintf("Verification OK.\n");

    cl_cvdfree(cvd);
    return 0;
}

static int listdb(const char *filename);

static int listdir(const char *dirname)
{
	DIR *dd;
	struct dirent *dent;
	char *dbfile;


    if((dd = opendir(dirname)) == NULL) {
        mprintf("!listdir: Can't open directory %s\n", dirname);
        return -1;
    }

    while((dent = readdir(dd))) {
#ifndef C_INTERIX
	if(dent->d_ino)
#endif
	{
	    if(strcmp(dent->d_name, ".") && strcmp(dent->d_name, "..") &&
	    (cli_strbcasestr(dent->d_name, ".db")  ||
	     cli_strbcasestr(dent->d_name, ".hdb") ||
	     cli_strbcasestr(dent->d_name, ".mdb") ||
	     cli_strbcasestr(dent->d_name, ".ndb") ||
	     cli_strbcasestr(dent->d_name, ".sdb") ||
	     cli_strbcasestr(dent->d_name, ".zmd") ||
	     cli_strbcasestr(dent->d_name, ".rmd") ||
	     cli_strbcasestr(dent->d_name, ".inc") ||
	     cli_strbcasestr(dent->d_name, ".cvd"))) {

		dbfile = (char *) mcalloc(strlen(dent->d_name) + strlen(dirname) + 2, sizeof(char));

		if(!dbfile) {
		    mprintf("!listdir: Can't allocate memory for dbfile\n");
		    closedir(dd);
		    return -1;
		}
		sprintf(dbfile, "%s/%s", dirname, dent->d_name);

		if(listdb(dbfile) == -1) {
		    mprintf("!listdb: Error listing database %s\n", dbfile);
		    free(dbfile);
		    closedir(dd);
		    return -1;
		}
		free(dbfile);
	    }
	}
    }

    closedir(dd);
    return 0;
}

static int listdb(const char *filename)
{
	FILE *fd;
	char *buffer, *pt, *start, *dir;
	int line = 0;
	const char *tmpdir;


    if(cli_strbcasestr(filename, ".inc")) { /* incremental directory */
	if(listdir(filename) == -1) {
	    mprintf("!listdb: Can't list incremental directory %s\n", filename);
	    return -1;
	}
	return 0;
    }

    if((fd = fopen(filename, "rb")) == NULL) {
	mprintf("!listdb: Can't open file %s\n", filename);
	return -1;
    }

    if(!(buffer = (char *) mcalloc(FILEBUFF, 1))) {
	mprintf("!listdb: Can't allocate memory for buffer\n");
	fclose(fd);
	return -1;
    }

    /* check for CVD file */
    fgets(buffer, 12, fd);
    rewind(fd);

    if(!strncmp(buffer, "ClamAV-VDB:", 11)) {
	free(buffer);
	fclose(fd);

	tmpdir = getenv("TMPDIR");
	if(tmpdir == NULL)
#ifdef P_tmpdir
	    tmpdir = P_tmpdir;
#else
	    tmpdir = "/tmp";
#endif

	if(!(dir = cli_gentemp(tmpdir))) {
	    mprintf("!listdb: Can't generate temporary name\n");
	    return -1;
	}

	if(mkdir(dir, 0700)) {
	    mprintf("!listdb: Can't create temporary directory %s\n", dir);
	    free(dir);
	    return -1;
	}

	if(cvd_unpack(filename, dir) == -1) {
	    mprintf("!listdb: Can't unpack CVD file %s\n", filename);
	    rmdirs(dir);
	    free(dir);
	    return -1;
	}

	/* list extracted directory */
	if(listdir(dir) == -1) {
	    mprintf("!listdb: Can't list directory %s\n", filename);
	    rmdirs(dir);
	    free(dir);
	    return -1;
	}

	rmdirs(dir);
	free(dir);

	return 0;
    }

    if(cli_strbcasestr(filename, ".db")) { /* old style database */

	while(fgets(buffer, FILEBUFF, fd)) {
	    line++;
	    pt = strchr(buffer, '=');
	    if(!pt) {
		mprintf("!listdb: Malformed pattern line %d (file %s)\n", line, filename);
		fclose(fd);
		free(buffer);
		return -1;
	    }

	    start = buffer;
	    *pt = 0;

	    if((pt = strstr(start, " (Clam)")))
		*pt = 0;

	    mprintf("%s\n", start);
	}

    } else if(cli_strbcasestr(filename, ".hdb") || cli_strbcasestr(filename, ".mdb")) { /* hash database */

	while(fgets(buffer, FILEBUFF, fd)) {
	    line++;
	    cli_chomp(buffer);
	    start = cli_strtok(buffer, 2, ":");

	    if(!start) {
		mprintf("!listdb: Malformed pattern line %d (file %s)\n", line, filename);
		fclose(fd);
		free(buffer);
		return -1;
	    }

	    if((pt = strstr(start, " (Clam)")))
		*pt = 0;

	    mprintf("%s\n", start);
	    free(start);
	}

    } else if(cli_strbcasestr(filename, ".ndb") || cli_strbcasestr(filename, ".sdb") || cli_strbcasestr(filename, ".zmd") || cli_strbcasestr(filename, ".rmd")) {

	while(fgets(buffer, FILEBUFF, fd)) {
	    line++;
	    cli_chomp(buffer);
	    start = cli_strtok(buffer, 0, ":");

	    if(!start) {
		mprintf("!listdb: Malformed pattern line %d (file %s)\n", line, filename);
		fclose(fd);
		free(buffer);
		return -1;
	    }

	    if((pt = strstr(start, " (Clam)")))
		*pt = 0;

	    mprintf("%s\n", start);
	    free(start);
	}
    }

    fclose(fd);
    free(buffer);
    return 0;
}

static int listsigs(struct optstruct *opt)
{
	int ret;
	const char *name;
	char *dbdir;


    mprintf_stdout = 1;

    if((name = opt_arg(opt, "list-sigs"))) {
	ret = listdb(name);
    } else {
	dbdir = freshdbdir();
	ret = listdir(dbdir);
	free(dbdir);
    }

    return ret;
}

static int vbadump(struct optstruct *opt)
{
	int fd, hex_output=0;
	char *dir;


    if(opt_check(opt, "vba-hex"))
	hex_output = 1;
 
    if((fd = open(opt_arg(opt, "vba"), O_RDONLY)) == -1) {
	if((fd = open(opt_arg(opt, "vba-hex"), O_RDONLY)) == -1) {
	    mprintf("!vbadump: Can't open file %s\n", opt_arg(opt, "vba"));
	    return -1;
	}
    }

    /* generate the temporary directory */
    dir = cli_gentemp(NULL);
    if(mkdir(dir, 0700)) {
	mprintf("!vbadump: Can't create temporary directory %s\n", dir);
	free(dir);
	close(fd);
        return -1;
    }

    if(cli_ole2_extract(fd, dir, NULL)) {
	cli_rmdirs(dir);
        free(dir);
	close(fd);
        return -1;
    }

    close(fd);
    sigtool_vba_scandir(dir, hex_output);
    rmdirs(dir);
    free(dir);
    return 0;
}

static int rundiff(struct optstruct *opt)
{
	int fd, ret;
	unsigned short mode;
	const char *diff;


    diff = opt_arg(opt, "run-cdiff");
    if(strstr(diff, ".cdiff")) {
	mode = 1;
    } else if(strstr(diff, ".script")) {
	mode = 0;
    } else {
	mprintf("!rundiff: Incorrect file name (no .cdiff/.script extension)\n");
	return -1;
    }

    if((fd = open(diff, O_RDONLY)) == -1) {
	mprintf("!rundiff: Can't open file %s\n", diff);
	return -1;
    }

    ret = cdiff_apply(fd, mode);
    close(fd);

    return ret;
}

static int compare(const char *oldpath, const char *newpath, FILE *diff)
{
	FILE *old, *new;
	char obuff[1024], nbuff[1024], tbuff[1024], *pt, *omd5, *nmd5;
	unsigned int oline = 0, tline, found, i;
	long opos;


    if(!(new = fopen(newpath, "r"))) {
	mprintf("!compare: Can't open file %s for reading\n", newpath);
	return -1;
    }

    if((omd5 = cli_md5file(oldpath))) {
	if(!(nmd5 = cli_md5file(newpath))) {
	    mprintf("!compare: Can't get MD5 checksum of %s\n", newpath);
	    free(omd5);
	    return -1;
	}
	if(!strcmp(omd5, nmd5)) {
	    free(omd5);
	    free(nmd5);
	    return 0;
	}
	free(omd5);
	free(nmd5);
    }

    fprintf(diff, "OPEN %s\n", newpath);

    old = fopen(oldpath, "r");

    while(fgets(nbuff, sizeof(nbuff), new)) {
	cli_chomp(nbuff);

	if(!old) {
	    fprintf(diff, "ADD %s\n", nbuff);
	} else {
	    if(fgets(obuff, sizeof(obuff), old)) {
		oline++;
		cli_chomp(obuff);
		if(!strcmp(nbuff, obuff)) {
		    continue;
		} else {
		    tline = 0;
		    found = 0;
		    opos = ftell(old);
		    while(fgets(tbuff, sizeof(tbuff), old)) {
			tline++;
			cli_chomp(tbuff);

			if(tline > MAX_DEL_LOOKAHEAD)
			    break;

			if(!strcmp(tbuff, nbuff)) {
			    found = 1;
			    break;
			}
		    }
		    fseek(old, opos, SEEK_SET);

		    if(found) {
			strncpy(tbuff, obuff, sizeof(tbuff));
			for(i = 0; i < tline; i++) {
			    tbuff[16] = 0;
			    if((pt = strchr(tbuff, ' ')))
				*pt = 0;
			    fprintf(diff, "DEL %u %s\n", oline + i, tbuff);
			    fgets(tbuff, sizeof(tbuff), old);
			}
			oline += tline;

		    } else {
			obuff[16] = 0;
			if((pt = strchr(obuff, ' ')))
			    *pt = 0;
			fprintf(diff, "XCHG %u %s %s\n", oline, obuff, nbuff);
		    }
		}
	    } else {
		fclose(old);
		old = NULL;
		fprintf(diff, "ADD %s\n", nbuff);
	    }
	}
    }

    if(old) {
	while(fgets(obuff, sizeof(obuff), old)) {
	    oline++;
	    obuff[16] = 0;
	    if((pt = strchr(obuff, ' ')))
		*pt = 0;
	    fprintf(diff, "DEL %u %s\n", oline, obuff);
	}
	fclose(old);
    }

    fprintf(diff, "CLOSE\n");
    return 0;
}

static int verifydiff(const char *diff, const char *cvd, const char *incdir)
{
	char *tempdir, cwd[512], buff[1024], info[32], *md5, *pt;
	const char *cpt;
	FILE *fh;
	int ret = 0, fd;
	unsigned short mode;


    if(strstr(diff, ".cdiff")) {
	mode = 1;
    } else if(strstr(diff, ".script")) {
	mode = 0;
    } else {
	mprintf("!verifydiff: Incorrect file name (no .cdiff/.script extension)\n");
	return -1;
    }

    tempdir = cli_gentemp(NULL);
    if(!tempdir) {
	mprintf("!verifydiff: Can't generate temporary name for tempdir\n");
	return -1;
    }

    if(mkdir(tempdir, 0700) == -1) {
	mprintf("!verifydiff: Can't create directory %s\n", tempdir);
	free(tempdir);
	return -1;
    }

    if(cvd) {
	if(cvd_unpack(cvd, tempdir) == -1) {
	    mprintf("!verifydiff: Can't unpack CVD file %s\n", cvd);
	    rmdirs(tempdir);
	    free(tempdir);
	    return -1;
	}
    } else {
	if(dircopy(incdir, tempdir) == -1) {
	    mprintf("!verifydiff: Can't copy dir %s to %s\n", incdir, tempdir);
	    rmdirs(tempdir);
	    free(tempdir);
	    return -1;
	}
    }

    if((fd = open(diff, O_RDONLY)) == -1) {
	mprintf("!verifydiff: Can't open diff file %s\n", diff);
	rmdirs(tempdir);
	free(tempdir);
	return -1;
    }

    getcwd(cwd, sizeof(cwd));

    if(chdir(tempdir) == -1) {
	mprintf("!verifydiff: Can't chdir to %s\n", tempdir);
	rmdirs(tempdir);
	free(tempdir);
	close(fd);
	return -1;
    }

    if(cdiff_apply(fd, mode) == -1) {
	mprintf("!verifydiff: Can't apply %s\n", diff);
	chdir(cwd);
	rmdirs(tempdir);
	free(tempdir);
	close(fd);
	return -1;
    }
    close(fd);

    cvd ? (cpt = cvd) : (cpt = incdir);

    if(strstr(cpt, "main.cvd"))
	strcpy(info, "main.info");
    else
	strcpy(info, "daily.info");

    if(!(fh = fopen(info, "r"))) {
	mprintf("!verifydiff: Can't open %s\n", info);
	chdir(cwd);
	rmdirs(tempdir);
	free(tempdir);
	return -1;
    }

    fgets(buff, sizeof(buff), fh);

    if(strncmp(buff, "ClamAV-VDB", 10)) {
	mprintf("!verifydiff: Incorrect info file %s\n", info);
	chdir(cwd);
	rmdirs(tempdir);
	free(tempdir);
	return -1;
    }

    while(fgets(buff, sizeof(buff), fh)) {
	cli_chomp(buff);
	if(!(pt = strchr(buff, ':'))) {
	    mprintf("!verifydiff: Incorrect format of %s\n", info);
	    ret = -1;
	    break;
	}
	*pt++ = 0;
	if(!(md5 = cli_md5file(buff))) {
	    mprintf("!verifydiff: Can't generate MD5 for %s\n", buff);
	    ret = -1;
	    break;
	}
	if(strcmp(pt, md5)) {
	    mprintf("!verifydiff: %s has incorrect checksum\n", buff);
	    ret = -1;
	    break;
	}
    }

    fclose(fh);
    chdir(cwd);
    rmdirs(tempdir);
    free(tempdir);

    if(!ret) {
	if(cvd)
	    mprintf("Verification: %s correctly applies to %s\n", diff, cvd);
	else
	    mprintf("Verification: %s correctly applies to the previous version\n", diff);
    }

    return ret;
}

static int diffdirs(const char *old, const char *new, const char *patch)
{
	FILE *diff;
	DIR *dd;
	struct dirent *dent;
	char cwd[512], opath[1024];


    getcwd(cwd, sizeof(cwd));

    if(!(diff = fopen(patch, "w"))) {
        mprintf("!diffdirs: Can't open %s for writing\n", patch);
	return -1;
    }

    if(chdir(new) == -1) {
	mprintf("!diffdirs: Can't chdir to %s\n", new);
	fclose(diff);
	return -1;
    }

    if((dd = opendir(new)) == NULL) {
        mprintf("!diffdirs: Can't open directory %s\n", new);
	fclose(diff);
	return -1;
    }

    while((dent = readdir(dd))) {
#ifndef C_INTERIX
	if(dent->d_ino)
#endif
	{
	    if(!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, ".."))
		continue;

	    snprintf(opath, sizeof(opath), "%s/%s", old, dent->d_name);
	    if(compare(opath, dent->d_name, diff) == -1) {
		fclose(diff);
		unlink(patch);
		closedir(dd);
		return -1;
	    }
	}
    }

    closedir(dd);

    fclose(diff);
    mprintf("Generated diff file %s\n", patch);
    chdir(cwd);

    return 0;
}

static int makediff(struct optstruct *opt)
{
	char *odir, *ndir, name[32], broken[32];
	struct cl_cvd *cvd;
	unsigned int oldver, newver;
	int ret;


    if(!opt->filename) {
	mprintf("!makediff: --diff requires two arguments\n");
	return -1;
    }

    if(!(cvd = cl_cvdhead(opt->filename))) {
	mprintf("!makediff: Can't read CVD header from %s\n", opt->filename);
	return -1;
    }
    newver = cvd->version;
    free(cvd);

    if(!(cvd = cl_cvdhead(opt_arg(opt, "diff")))) {
	mprintf("!makediff: Can't read CVD header from %s\n", opt_arg(opt, "diff"));
	return -1;
    }
    oldver = cvd->version;
    free(cvd);

    if(oldver + 1 != newver) {
	mprintf("!makediff: The old CVD must be %u\n", newver - 1);
	return -1;
    }

    odir = cli_gentemp(NULL);
    if(!odir) {
	mprintf("!makediff: Can't generate temporary name for odir\n");
	return -1;
    }

    if(mkdir(odir, 0700) == -1) {
	mprintf("!makediff: Can't create directory %s\n", odir);
	free(odir);
	return -1;
    }

    if(cvd_unpack(opt_arg(opt, "diff"), odir) == -1) {
	mprintf("!makediff: Can't unpack CVD file %s\n", opt_arg(opt, "diff"));
	rmdirs(odir);
	free(odir);
	return -1;
    }

    ndir = cli_gentemp(NULL);
    if(!ndir) {
	mprintf("!makediff: Can't generate temporary name for ndir\n");
	rmdirs(odir);
	free(odir);
	return -1;
    }

    if(mkdir(ndir, 0700) == -1) {
	mprintf("!makediff: Can't create directory %s\n", ndir);
	free(ndir);
	rmdirs(odir);
	free(odir);
	return -1;
    }

    if(cvd_unpack(opt->filename, ndir) == -1) {
	mprintf("!makediff: Can't unpack CVD file %s\n", opt->filename);
	rmdirs(odir);
	rmdirs(ndir);
	free(odir);
	free(ndir);
	return -1;
    }

    if(strstr(opt->filename, "main"))
	snprintf(name, sizeof(name), "main-%u.script", newver);
    else
	snprintf(name, sizeof(name), "daily-%u.script", newver);

    ret = diffdirs(odir, ndir, name);

    rmdirs(odir);
    rmdirs(ndir);
    free(odir);
    free(ndir);

    if(ret == -1)
	return -1;

    if(verifydiff(name, opt_arg(opt, "diff"), NULL) == -1) {
	snprintf(broken, sizeof(broken), "%s.broken", name);
	if(rename(name, broken)) {
	    unlink(name);
	    mprintf("!Generated file is incorrect, removed");
	} else {
	    mprintf("!Generated file is incorrect, renamed to %s\n", broken);
	}
	return -1;
    }

    return 0;
}

void help(void)
{
    mprintf("\n");
    mprintf("             Clam AntiVirus: Signature Tool (sigtool)  "VERSION"\n");
    mprintf("    (C) 2002 - 2006 ClamAV Team - http://www.clamav.net/team.html\n\n");

    mprintf("    --help                 -h              show help\n");
    mprintf("    --version              -V              print version number and exit\n");
    mprintf("    --quiet                                be quiet, output only error messages\n");
    mprintf("    --debug                                enable debug messages\n");
    mprintf("    --stdout                               write to stdout instead of stderr\n");
    mprintf("    --hex-dump                             convert data from stdin to a hex\n");
    mprintf("                                           string and print it on stdout\n");
    mprintf("    --md5 [FILES]                          generate MD5 checksum from stdin\n");
    mprintf("                                           or MD5 sigs for FILES\n");
    mprintf("    --mdb [FILES]                          generate .mdb sigs\n");
    mprintf("    --html-normalise=FILE                  create normalised parts of HTML file\n");
    mprintf("    --utf16-decode=FILE                    decode UTF16 encoded files\n");
    mprintf("    --info=FILE            -i FILE         print database information\n");
    mprintf("    --build=NAME [cvd/inc] -b NAME         build a CVD file\n");
    mprintf("    --server=ADDR                          ClamAV Signing Service address\n");
    mprintf("    --unpack=FILE          -u FILE         Unpack a CVD file\n");
    mprintf("    --unpack-current=SHORTNAME             Unpack local CVD/INCDIR in cwd\n");
    mprintf("    --list-sigs[=FILE]     -l[FILE]        List signature names\n");
    mprintf("    --vba=FILE                             Extract VBA/Word6 macro code\n");
    mprintf("    --vba-hex=FILE                         Extract Word6 macro code with hex values\n");
    mprintf("    --diff=OLD NEW         -d OLD NEW      Create diff for OLD and NEW CVDs\n");
    mprintf("    --run-cdiff=FILE       -r FILE         Execute update script FILE in cwd\n");
    mprintf("    --verify-cdiff=DIFF CVD/INCDIR         Verify DIFF against CVD\n");
    mprintf("\n");

    return;
}

int main(int argc, char **argv)
{
	int ret = 1;
        struct optstruct *opt;
	struct stat sb;
	const char *short_options = "hvVb:i:u:l::r:d:";
	static struct option long_options[] = {
	    {"help", 0, 0, 'h'},
	    {"quiet", 0, 0, 0},
	    {"debug", 0, 0, 0},
	    {"verbose", 0, 0, 'v'},
	    {"stdout", 0, 0, 0},
	    {"version", 0, 0, 'V'},
	    {"tempdir", 1, 0, 0},
	    {"hex-dump", 0, 0, 0},
	    {"md5", 0, 0, 0},
	    {"mdb", 0, 0, 0},
	    {"html-normalise", 1, 0, 0},
	    {"utf16-decode", 1, 0, 0},
	    {"build", 1, 0, 'b'},
	    {"server", 1, 0, 0},
	    {"unpack", 1, 0, 'u'},
	    {"unpack-current", 1, 0, 0},
	    {"info", 1, 0, 'i'},
	    {"list-sigs", 2, 0, 'l'},
	    {"vba", 1, 0 ,0},
	    {"vba-hex", 1, 0, 0},
	    {"diff", 1, 0, 'd'},
	    {"run-cdiff", 1, 0, 'r'},
	    {"verify-cdiff", 1, 0, 0},
	    {0, 0, 0, 0}
    	};

    opt = opt_parse(argc, argv, short_options, long_options, NULL);
    if(!opt) {
	mprintf("!Can't parse the command line\n");
	return 1;
    }

    if(opt_check(opt, "quiet"))
	mprintf_quiet = 1;

    if(opt_check(opt, "stdout"))
	mprintf_stdout = 1;

    if(opt_check(opt, "debug"))
	cl_debug();

    if(opt_check(opt, "version")) {
	print_version();
	opt_free(opt);
	return 0;
    }

    if(opt_check(opt, "help")) {
	opt_free(opt);
    	help();
	return 0;
    }

    if(opt_check(opt, "hex-dump"))
	ret = hexdump();
    else if(opt_check(opt, "md5"))
	ret = md5sig(opt, 0);
    else if(opt_check(opt, "mdb"))
	ret = md5sig(opt, 1);
    else if(opt_check(opt, "html-normalise"))
	ret = htmlnorm(opt);
    else if(opt_check(opt, "utf16-decode"))
	ret = utf16decode(opt);
    else if(opt_check(opt, "build"))
	ret = build(opt);
    else if(opt_check(opt, "unpack"))
	ret = unpack(opt);
    else if(opt_check(opt, "unpack-current"))
	ret = unpack(opt);
    else if(opt_check(opt, "info"))
	ret = cvdinfo(opt);
    else if(opt_check(opt, "list-sigs"))
	ret = listsigs(opt);
    else if(opt_check(opt, "vba") || opt_check(opt, "vba-hex"))
	ret = vbadump(opt);
    else if(opt_check(opt, "diff"))
	ret = makediff(opt);
    else if(opt_check(opt, "run-cdiff"))
	ret = rundiff(opt);
    else if(opt_check(opt, "verify-cdiff")) {
	if(!opt->filename) {
	    mprintf("!--verify-cdiff requires two arguments\n");
	    ret = -1;
	} else {
	    if(stat(opt->filename, &sb) == -1) {
		mprintf("--verify-cdiff: Can't get status of %s\n", opt->filename);
		ret = -1;
	    } else {
		if(S_ISDIR(sb.st_mode))
		    ret = verifydiff(opt_arg(opt, "verify-cdiff"), NULL, opt->filename);
		else
		    ret = verifydiff(opt_arg(opt, "verify-cdiff"), opt->filename, NULL);
	    }
	}
    } else
	help();

    opt_free(opt);
    return ret ? 1 : 0;
}