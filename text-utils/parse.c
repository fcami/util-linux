/*
 * Copyright (c) 1989 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

 /* 1999-02-22 Arkadiusz Miśkiewicz <misiek@pld.ORG.PL>
  * - added Native Language Support
  */

#include <sys/types.h>
#include <sys/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include "hexdump.h"
#include "nls.h"
#include "xalloc.h"
#include "strutils.h"

static void escape(char *p1);
static void badcnt(const char *s);
static void badsfmt(void);
static void badfmt(const char *fmt);
static void badconv(const char *ch);

FU *endfu;					/* format at end-of-data */

void addfile(char *name)
{
	char *p, *buf = NULL;
	FILE *fp;
	size_t n;

	if ((fp = fopen(name, "r")) == NULL)
	        err(EXIT_FAILURE, _("can't read %s"), name);

	while (getline(&buf, &n, fp) != -1) {
		p = buf;

		while (*p && isspace(*p))
			++p;
		if (!*p || *p == '#')
			continue;

		add(p);
	}

	free(buf);
	fclose(fp);
}

void add(const char *fmt)
{
	const unsigned char *p, *savep;
	FS *tfs;
	FU *tfu;

	/* Start new linked list of format units. */
	tfs = xcalloc(1, sizeof(FS));
	INIT_LIST_HEAD(&tfs->fslist);
	INIT_LIST_HEAD(&tfs->fulist);
	list_add_tail(&tfs->fslist, &fshead);

	/* Take the format string and break it up into format units. */
	p = (unsigned char *)fmt;
	while (TRUE) {
		/* Skip leading white space. */
		while (isspace(*p) && ++p)
			;
		if (!*p)
			break;

		/* Allocate a new format unit and link it in. */
		tfu = xcalloc(1, sizeof(FU));
		tfu->reps = 1;

		INIT_LIST_HEAD(&tfu->fulist);
		INIT_LIST_HEAD(&tfu->prlist);
		list_add_tail(&tfu->fulist, &tfs->fulist);

		/* If leading digit, repetition count. */
		if (isdigit(*p)) {
			savep = p;
			while (isdigit(*p) && ++p)
				;
			if (!isspace(*p) && *p != '/')
				badfmt(fmt);
			/* may overwrite either white space or slash */
			tfu->reps = atoi((char *)savep);
			tfu->flags = F_SETREP;
			/* skip trailing white space */
			while (isspace(*++p))
				;
		}

		/* Skip slash and trailing white space. */
		if (*p == '/')
			while (isspace(*++p))
				;

		/* byte count */
		if (isdigit(*p)) {
			savep = p;
			while (isdigit(*p) && ++p)
				;
			if (!isspace(*p))
				badfmt(fmt);
			tfu->bcnt = atoi((char *)savep);
			/* skip trailing white space */
			while (isspace(*++p))
				;
		}

		/* format */
		if (*p != '"')
			badfmt(fmt);
		savep = ++p;
		while (*p != '"') {
			if (!*p++)
				badfmt(fmt);
		}
		tfu->fmt = xmalloc(p - savep + 1);
		xstrncpy(tfu->fmt, (char *)savep, p - savep + 1);
		escape(tfu->fmt);
		++p;
	}
}

static const char *spec = ".#-+ 0123456789";

int block_size(FS *fs)
{
	FU *fu;
	int bcnt, prec, cursize = 0;
	unsigned char *fmt;
	struct list_head *p;

	/* figure out the data block size needed for each format unit */
	list_for_each (p, &fs->fulist) {
		fu = list_entry(p, FU, fulist);
		if (fu->bcnt) {
			cursize += fu->bcnt * fu->reps;
			continue;
		}
		bcnt = prec = 0;
		fmt = (unsigned char *)fu->fmt;
		while (*fmt) {
			if (*fmt != '%') {
				++fmt;
				continue;
			}
			/*
			 * skip any special chars -- save precision in
			 * case it's a %s format.
			 */
			while (strchr(spec + 1, *++fmt))
				;
			if (*fmt == '.' && isdigit(*++fmt)) {
				prec = atoi((char *)fmt);
				while (isdigit(*++fmt))
					;
			}
			switch(*fmt) {
			case 'c':
				bcnt += 1;
				break;
			case 'd': case 'i': case 'o': case 'u':
			case 'x': case 'X':
				bcnt += 4;
				break;
			case 'e': case 'E': case 'f': case 'g': case 'G':
				bcnt += 8;
				break;
			case 's':
				bcnt += prec;
				break;
			case '_':
				switch(*++fmt) {
				case 'c': case 'p': case 'u':
					bcnt += 1;
					break;
				}
			}
			++fmt;
		}
		cursize += bcnt * fu->reps;
	}
	return(cursize);
}

void rewrite(FS *fs)
{
	enum { NOTOKAY, USEBCNT, USEPREC } sokay;
	PR *pr;
	FU *fu;
	struct list_head *p, *q;
	char *p1, *p2, *fmtp;
	char savech, cs[3];
	int nconv, prec = 0;

	list_for_each (p, &fs->fulist) {
		fu = list_entry(p, FU, fulist);
		/*
		 * Break each format unit into print units; each
		 * conversion character gets its own.
		 */
		nconv = 0;
		fmtp = fu->fmt;
		while (*fmtp) {
			pr = xcalloc(1, sizeof(PR));
			INIT_LIST_HEAD(&pr->prlist);
			list_add_tail(&pr->prlist, &fu->prlist);

			/* Skip preceding text and up to the next % sign. */
			p1 = fmtp;
			while (*p1 && *p1 != '%')
				++p1;

			/* Only text in the string. */
			if (!*p1) {
				pr->fmt = fmtp;
				pr->flags = F_TEXT;
				break;
			}

			/*
			 * Get precision for %s -- if have a byte count, don't
			 * need it.
			 */
			if (fu->bcnt) {
				sokay = USEBCNT;
				/* skip to conversion character */
				while (++p1 && strchr(spec, *p1))
					;
			} else {
				/* skip any special chars, field width */
				while (strchr(spec + 1, *++p1))
					;
				if (*p1 == '.' && isdigit(*++p1)) {
					sokay = USEPREC;
					prec = atoi(p1);
					while (isdigit(*++p1))
						;
				} else
					sokay = NOTOKAY;
			}

			p2 = p1 + 1;		/* Set end pointer. */
			cs[0] = *p1;		/* Set conversion string. */
			cs[1] = 0;

			/*
			 * Figure out the byte count for each conversion;
			 * rewrite the format as necessary, set up blank-
			 * padding for end of data.
			 */
			switch(cs[0]) {
				case 'c':
					pr->flags = F_CHAR;
					switch(fu->bcnt) {
						case 0:
						case 1:
							pr->bcnt = 1;
							break;
						default:
							p1[1] = '\0';
							badcnt(p1);
					}
					break;
				case 'd':
				case 'i':
					pr->flags = F_INT;
					goto isint;
				case 'o':
				case 'u':
				case 'x':
				case 'X':
					pr->flags = F_UINT;
	isint:				cs[2] = '\0';
					cs[1] = cs[0];
					cs[0] = 'q';
					switch(fu->bcnt) {
						case 0:
							pr->bcnt = 4;
							break;
						case 1:
						case 2:
						case 4:
						case 8:
							pr->bcnt = fu->bcnt;
							break;
						default:
							p1[1] = '\0';
							badcnt(p1);
					}
					break;
				case 'e':
				case 'E':
				case 'f':
				case 'g':
				case 'G':
					pr->flags = F_DBL;
					switch(fu->bcnt) {
						case 0:
							pr->bcnt = 8;
							break;
						case 4:
						case 8:
							pr->bcnt = fu->bcnt;
							break;
						default:
							p1[1] = '\0';
							badcnt(p1);
					}
					break;
				case 's':
					pr->flags = F_STR;
					switch(sokay) {
					case NOTOKAY:
						badsfmt();
					case USEBCNT:
						pr->bcnt = fu->bcnt;
						break;
					case USEPREC:
						pr->bcnt = prec;
						break;
					}
					break;
				case '_':
					++p2;
					switch(p1[1]) {
						case 'A':
							endfu = fu;
							fu->flags |= F_IGNORE;
							/* FALLTHROUGH */
						case 'a':
							pr->flags = F_ADDRESS;
							++p2;
							switch(p1[2]) {
								case 'd':
								case 'o':
								case 'x':
									cs[0] = 'q';
									cs[1] = p1[2];
									cs[2] = '\0';
									break;
								default:
									p1[3] = '\0';
									badconv(p1);
							}
							break;
						case 'c':
							pr->flags = F_C;
							/* cs[0] = 'c';	set in conv_c */
							goto isint2;
						case 'p':
							pr->flags = F_P;
							cs[0] = 'c';
							goto isint2;
						case 'u':
							pr->flags = F_U;
							/* cs[0] = 'c';	set in conv_u */
		isint2:					switch(fu->bcnt) {
								case 0:
								case 1:
									pr->bcnt = 1;
									break;
								default:
									p1[2] = '\0';
									badcnt(p1);
							}
							break;
						default:
							p1[2] = '\0';
							badconv(p1);
					}
					break;
				default:
					p1[1] = '\0';
					badconv(p1);
			}

			/*
			 * Copy to PR format string, set conversion character
			 * pointer, update original.
			 */
			savech = *p2;
			p1[0] = '\0';
			pr->fmt = xmalloc(strlen(fmtp) + strlen(cs) + 1);
			strcpy(pr->fmt, fmtp);
			strcat(pr->fmt, cs);
			*p2 = savech;
			pr->cchar = pr->fmt + (p1 - fmtp);
			fmtp = p2;

			/* Only one conversion character if byte count */
			if (!(pr->flags&F_ADDRESS) && fu->bcnt && nconv++)
				errx(EXIT_FAILURE,
				    _("byte count with multiple conversion characters"));
		}
		/*
		 * If format unit byte count not specified, figure it out
		 * so can adjust rep count later.
		 */
		if (!fu->bcnt)
			list_for_each(q, &fu->prlist)
				fu->bcnt += (list_entry(q, PR, prlist))->bcnt;
	}
	/*
	 * If the format string interprets any data at all, and it's
	 * not the same as the blocksize, and its last format unit
	 * interprets any data at all, and has no iteration count,
	 * repeat it as necessary.
	 *
	 * If rep count is greater than 1, no trailing whitespace
	 * gets output from the last iteration of the format unit.
	 */
	list_for_each (p, &fs->fulist) {
		fu = list_entry(p, FU, fulist);

		if (list_entry_is_last(&fu->fulist, &fs->fulist) &&
			fs->bcnt < blocksize &&
			!(fu->flags&F_SETREP) && fu->bcnt)
				fu->reps += (blocksize - fs->bcnt) / fu->bcnt;
		if (fu->reps > 1) {
			if (!list_empty(&fu->prlist)) {
				pr = list_last_entry(&fu->prlist, PR, prlist);
				for (p1 = pr->fmt, p2 = NULL; *p1; ++p1)
					p2 = isspace(*p1) ? p1 : NULL;
				if (p2)
					pr->nospace = p2;
			}
		}
	}
}


static void escape(char *p1)
{
	char *p2;

	/* alphabetic escape sequences have to be done in place */
	p2 = p1;
	while (TRUE) {
		if (!*p1) {
			*p2 = *p1;
			break;
		}
		if (*p1 == '\\')
			switch(*++p1) {
			case 'a':
			     /* *p2 = '\a'; */
				*p2 = '\007';
				break;
			case 'b':
				*p2 = '\b';
				break;
			case 'f':
				*p2 = '\f';
				break;
			case 'n':
				*p2 = '\n';
				break;
			case 'r':
				*p2 = '\r';
				break;
			case 't':
				*p2 = '\t';
				break;
			case 'v':
				*p2 = '\v';
				break;
			default:
				*p2 = *p1;
				break;
			}
		++p1; ++p2;
	}
}

static void badcnt(const char *s)
{
        errx(EXIT_FAILURE, _("bad byte count for conversion character %s"), s);
}

static void badsfmt(void)
{
        errx(EXIT_FAILURE, _("%%s requires a precision or a byte count"));
}

static void badfmt(const char *fmt)
{
        errx(EXIT_FAILURE, _("bad format {%s}"), fmt);
}

static void badconv(const char *ch)
{
        errx(EXIT_FAILURE, _("bad conversion character %%%s"), ch);
}
