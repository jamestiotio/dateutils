/*** yuck-scmver.c -- snarf versions off project cwds
 *
 * Copyright (C) 2013-2022 Sebastian Freundt
 *
 * Author:  Sebastian Freundt <freundt@ga-group.nl>
 *
 * This file is part of yuck.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the author nor the names of any contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ***/
#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "yuck-scmver.h"

#if !defined LIKELY
# define LIKELY(_x)	__builtin_expect((_x), 1)
#endif	/* !LIKELY */
#if !defined UNLIKELY
# define UNLIKELY(_x)	__builtin_expect((_x), 0)
#endif	/* UNLIKELY */
#if !defined UNUSED
# define UNUSED(_x)	_x __attribute__((unused))
#endif	/* !UNUSED */

#if !defined countof
# define countof(x)	(sizeof(x) / sizeof(*x))
#endif	/* !countof */

#define _paste(x, y)	x ## y
#define paste(x, y)	_paste(x, y)
#if !defined with
# define with(args...)							\
	for (args, *paste(__ep, __LINE__) = (void*)1;			\
	     paste(__ep, __LINE__); paste(__ep, __LINE__) = 0)
#endif	/* !with */

#define DEBUG(args...)

/* globals */
const char *const yscm_strs[] = {
	[YUCK_SCM_TARBALL] = "tarball",
	[YUCK_SCM_GIT] = "git",
	[YUCK_SCM_BZR] = "bzr",
	[YUCK_SCM_HG] = "hg",
};


static __attribute__((format(printf, 1, 2))) void
error(const char *fmt, ...)
{
	va_list vap;
	va_start(vap, fmt);
	vfprintf(stderr, fmt, vap);
	va_end(vap);
	if (errno) {
		fputc(':', stderr);
		fputc(' ', stderr);
		fputs(strerror(errno), stderr);
	}
	fputc('\n', stderr);
	return;
}

static inline __attribute__((const, always_inline)) char*
deconst(const char *s)
{
	union {
		const char *c;
		char *p;
	} x = {s};
	return x.p;
}

static __attribute__((unused)) size_t
xstrlcpy(char *restrict dst, const char *src, size_t dsz)
{
	size_t ssz;

	if (UNLIKELY(dsz == 0U)) {
		return 0U;
	}
	if ((ssz = strlen(src)) > dsz) {
		ssz = dsz - 1U;
	}
	memcpy(dst, src, ssz);
	dst[ssz] = '\0';
	return ssz;
}

static __attribute__((unused)) size_t
xstrlncpy(char *restrict dst, size_t dsz, const char *src, size_t ssz)
{
	if (UNLIKELY(dsz == 0U)) {
		return 0U;
	}
	if (ssz > dsz) {
		ssz = dsz - 1U;
	}
	memcpy(dst, src, ssz);
	dst[ssz] = '\0';
	return ssz;
}

static char*
xdirname(char *restrict fn, const char *fp)
{
/* find next dir in FN from FP backwards */
	if (fp == NULL) {
		fp = fn + strlen(fn);
	} else if (fp <= fn) {
		return NULL;
	}

	for (--fp; fp >= fn && *fp != '/'; fp--);
	while (fp >= fn && *--fp == '/');
	if (fp >= fn) {
		/* replace / by \nul and return pointer */
		char *dp = fn + (++fp - fn);
		*dp = '\0';
		return dp;
	}
	/* return \nul */
	return NULL;
}

static char*
xmemmem(const char *hay, const size_t hayz, const char *ndl, const size_t ndlz)
{
	const char *const eoh = hay + hayz;
	const char *const eon = ndl + ndlz;
	const char *hp;
	const char *np;
	const char *cand;
	unsigned int hsum;
	unsigned int nsum;
	unsigned int eqp;

	/* trivial checks first
         * a 0-sized needle is defined to be found anywhere in haystack
         * then run strchr() to find a candidate in HAYSTACK (i.e. a portion
         * that happens to begin with *NEEDLE) */
	if (ndlz == 0UL) {
		return deconst(hay);
	} else if ((hay = memchr(hay, *ndl, hayz)) == NULL) {
		/* trivial */
		return NULL;
	}

	/* First characters of haystack and needle are the same now. Both are
	 * guaranteed to be at least one character long.  Now computes the sum
	 * of characters values of needle together with the sum of the first
	 * needle_len characters of haystack. */
	for (hp = hay + 1U, np = ndl + 1U, hsum = *hay, nsum = *hay, eqp = 1U;
	     hp < eoh && np < eon;
	     hsum ^= *hp, nsum ^= *np, eqp &= *hp == *np, hp++, np++);

	/* HP now references the (NZ + 1)-th character. */
	if (np < eon) {
		/* haystack is smaller than needle, :O */
		return NULL;
	} else if (eqp) {
		/* found a match */
		return deconst(hay);
	}

	/* now loop through the rest of haystack,
	 * updating the sum iteratively */
	for (cand = hay; hp < eoh; hp++) {
		hsum ^= *cand++;
		hsum ^= *hp;

		/* Since the sum of the characters is already known to be
		 * equal at that point, it is enough to check just NZ - 1
		 * characters for equality,
		 * also CAND is by design < HP, so no need for range checks */
		if (hsum == nsum && memcmp(cand, ndl, ndlz - 1U) == 0) {
			return deconst(cand);
		}
	}
	return NULL;
}

static unsigned int
hextou(const char *sp, char **ep)
{
	register unsigned int res = 0U;
	size_t i;

	if (UNLIKELY(sp == NULL)) {
		goto out;
	} else if (*sp == '\0') {
		goto out;
	}
	for (i = 0U; i < sizeof(res) * 8U / 4U - 1U; sp++, i++) {
		register unsigned int this;
		switch (*sp) {
		case '0' ... '9':
			this = *sp - '0';
			break;
		case 'a' ... 'f':
			this = *sp - 'a' + 10U;
			break;
		case 'A' ... 'F':
			this = *sp - 'A' + 10U;
			break;
		default:
			goto fucked;
		}

		res <<= 4U;
		res |= this;
	}
fucked:
	res <<= 4U;
	res |= i;

	/* keep reading the hexstring as long as it lasts */
	for (;; sp++) {
		switch (*sp) {
		case '0' ... '9':
		case 'a' ... 'f':
		case 'A' ... 'F':
			continue;
		default:
			goto out;
		}
	}
out:
	if (ep != NULL) {
		*ep = (char*)1U + (sp - (char*)1U);
	}
	return res;
}


/* version snarfers */
static __attribute__((noinline)) pid_t
run(int *fd, ...)
{
	static char *cmdline[16U];
	va_list vap;
	pid_t p;
	/* to snarf off traffic from the child */
	int intfd[2];

	va_start(vap, fd);
	for (size_t i = 0U;
	     i < countof(cmdline) &&
		     (cmdline[i] = va_arg(vap, char*)) != NULL; i++);
	va_end(vap);
	assert(*cmdline);

	if (pipe(intfd) < 0) {
		error("pipe setup to/from %s failed", cmdline[0U]);
		return -1;
	}

	switch ((p = vfork())) {
	case -1:
		/* i am an error */
		error("vfork for %s failed", cmdline[0U]);
		return -1;

	default:
		/* i am the parent */
		close(intfd[1]);
		if (fd != NULL) {
			*fd = intfd[0];
		} else {
			close(intfd[0]);
		}
		return p;

	case 0:
		/* i am the child */
		break;
	}

	/* child code here */
	close(intfd[0]);
	dup2(intfd[1], STDOUT_FILENO);

	execvp(cmdline[0U], cmdline);
	error("execvp(%s) failed", cmdline[0U]);
	_exit(EXIT_FAILURE);
}

static int
fin(pid_t p)
{
	int rc = 2;
	int st;

	while (waitpid(p, &st, 0) != p);
	if (WIFEXITED(st)) {
		rc = WEXITSTATUS(st);
	}
	return rc;
}

static yuck_scm_t
find_scm(char *restrict fn, size_t fz, const char *path)
{
	struct stat st[1U];
	char *restrict dp = fn;

	/* make a copy so we can fiddle with it */
	if (UNLIKELY(path == NULL)) {
	cwd:
		/* just use "." then */
		*dp++ = '.';
		*dp = '\0';
	} else if ((dp += xstrlcpy(fn, path, fz)) == fn) {
		goto cwd;
	}
again:
	if (stat(fn, st) < 0) {
		return YUCK_SCM_ERROR;
	} else if (UNLIKELY((size_t)(dp - fn) + 5U >= fz)) {
		/* not enough space */
		return YUCK_SCM_ERROR;
	} else if (!S_ISDIR(st->st_mode)) {
		/* not a directory, get the dir bit and start over */
		if ((dp = xdirname(fn, dp)) == NULL) {
			dp = fn;
			goto cwd;
		}
		goto again;
	}

scm_chk:
	/* now check for .git, .bzr, .hg */
	xstrlcpy(dp, "/.git", fz - (dp - fn));
	DEBUG("trying %s ...\n", fn);
	if (stat(fn, st) == 0 && S_ISDIR(st->st_mode)) {
		/* yay it's a .git */
		*dp = '\0';
		return YUCK_SCM_GIT;
	}

	xstrlcpy(dp, "/.bzr", fz - (dp - fn));
	DEBUG("trying %s ...\n", fn);
	if (stat(fn, st) == 0 && S_ISDIR(st->st_mode)) {
		/* yay it's a .git */
		*dp = '\0';
		return YUCK_SCM_BZR;
	}

	xstrlcpy(dp, "/.hg", fz - (dp - fn));
	DEBUG("trying %s ...\n", fn);
	if (stat(fn, st) == 0 && S_ISDIR(st->st_mode)) {
		/* yay it's a .git */
		*dp = '\0';
		return YUCK_SCM_HG;
	}
	/* nothing then, traverse upwards */
	if (*fn != '/') {
		/* make sure we don't go up indefinitely
		 * comparing the current inode to ./.. */
		with (ino_t curino) {
			*dp = '\0';
			if (stat(fn, st) < 0) {
				return YUCK_SCM_ERROR;
			}
			/* memorise inode */
			curino = st->st_ino;
			/* go upwards by appending /.. */
			dp += xstrlcpy(dp, "/..", fz - (dp - fn));
			/* check inode again */
			if (stat(fn, st) < 0) {
				return YUCK_SCM_ERROR;
			} else if (st->st_ino == curino) {
				break;
			}
			goto scm_chk;
		}
	} else if ((dp = xdirname(fn, dp)) != NULL) {
		goto scm_chk;
	}
	return YUCK_SCM_TARBALL;
}


static int
rd_version(struct yuck_version_s *restrict v, const char *buf, size_t bsz)
{
/* reads a normalised version string vX.Y.Z-DIST-SCM RVSN[-dirty] */
	static const char dflag[] = "dirty";
	const char *vtag = NULL;
	const char *eov;
	const char *dist = NULL;
	const char *eod;
	const char *bp = buf;
	const char *const ep = buf + bsz;

	/* parse buf */
	switch (*bp) {
	case 'v':
	case 'V':
		bp++;
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		break;
	default:
		/* weird, we req'd v-tags */
		return -1;
	}

	if ((eov = memchr(vtag = bp, '-', ep - bp)) == NULL) {
		/* last field */
		eov = ep;
	} else {
		dist = eov + 1U;
	}
	/* just for the fun of it, look for .git, .hg and .bzr as well */
	with (const char *altp) {
		if ((altp = xmemmem(vtag, ep - vtag, ".git", 4U))) {
			v->scm = YUCK_SCM_GIT;
			eov = altp;
			dist = altp + 4U;
		} else if ((altp = xmemmem(vtag, ep - vtag, ".bzr", 4U))) {
			/* oooh looks like the alternative version
			 * vX.Y.Z.gitDD.HASH */
			v->scm = YUCK_SCM_BZR;
			eov = altp;
			dist = altp + 4U;
		} else if ((altp = xmemmem(vtag, ep - vtag, ".hg", 3U))) {
			/* oooh looks like the alternative version
			 * vX.Y.Z.hgDD.HASH */
			v->scm = YUCK_SCM_HG;
			eov = altp;
			dist = altp + 3U;
		}
	}

	/* bang vtag */
	xstrlncpy(v->vtag, sizeof(v->vtag), vtag, eov - vtag);

	/* snarf distance */
	if (dist == NULL) {
		return 0;
	}
	/* read distance */
	with (char *on) {
		v->dist = strtoul(dist, &on, 10);
		eod = on;
	}

	switch (*eod) {
	default:
	case '\0':
		return 0;
	case '.':
		if (v->scm <= YUCK_SCM_TARBALL) {
			/* huh? */
			return -1;
		}
		/*@fallthrough@*/
	case '-':
		/* the show is going on, like it must */
		bp = eod + 1U;
		break;
	}
	switch (*bp++) {
	case 'g':
		/* git repo */
		v->scm = YUCK_SCM_GIT;
		break;
	case 'h':
		/* hg repo */
		v->scm = YUCK_SCM_HG;
		break;
	case 'b':
		if (v->scm <= YUCK_SCM_TARBALL) {
			v->scm = YUCK_SCM_BZR;
			break;
		}
		/* else probably git or hg hash starting with b */
		/*@fallthrough@*/
	default:
		/* could have been set already then */
		if (v->scm > YUCK_SCM_TARBALL) {
			/* rewind bp and continue */
			bp--;
			break;
		}
		/* otherwise we simply don't know */
		return 0;
	}
	/* read scm revision */
	with (char *on) {
		v->rvsn = hextou(bp, &on);
		bp = on;
	}

	if (bp >= ep) {
		;
	} else if (*bp != '-' && *bp != '.') {
		;
	} else if (bp + sizeof(dflag) > ep) {
		/* too short to fit `dirty' */
		;
	} else if (!memcmp(++bp, dflag, sizeof(dflag) - 1U)) {
		v->dirty = 1U;
	}
	return 0;
}

static ssize_t
wr_version(char *restrict buf, size_t bsz, const struct yuck_version_s *v)
{
	static const char yscm_abbr[] = "tgbh";
	const char *const ep = buf + bsz;
	char *bp = buf;

	if (UNLIKELY(buf == NULL || bsz == 0U)) {
		return -1;
	}
	*bp++ = 'v';
	bp += xstrlcpy(bp, v->vtag, ep - bp);
	if (!v->dist) {
		goto out;
	} else if (bp + 1U >= ep) {
		/* not enough space */
		return -1;
	}
	/* get the dist bit on the wire */
	*bp++ = '-';
	bp += snprintf(bp, ep - bp, "%u", v->dist);
	if (!v->rvsn || v->scm <= YUCK_SCM_TARBALL) {
		goto out;
	} else if (bp + 2U + 8U >= ep) {
		/* not enough space */
		return -1;
	}
	*bp++ = '-';
	*bp++ = yscm_abbr[v->scm];
	bp += snprintf(bp, ep - bp, "%0*x",
		       (int)(v->rvsn & 0x07U), v->rvsn >> 4U);
	if (!v->dirty) {
		goto out;
	} else if (bp + 1U + 5U >= ep) {
		/* not enough space */
		return -1;
	}
	bp += xstrlcpy(bp, "-dirty", ep - bp);
out:
	return bp - buf;
}

static int
git_version(struct yuck_version_s v[static 1U])
{
	pid_t chld;
	int fd[1U];
	int rc = 0;

	if ((chld = run(fd, "git", "describe",
			"--tags", "--match=v[0-9]*",
			"--abbrev=8", "--dirty", NULL)) < 0) {
		return -1;
	}
	/* shouldn't be heaps, so just use a single read */
	with (char buf[256U]) {
		const char *vtag;
		const char *dist;
		char *bp;
		ssize_t nrd;

		if ((nrd = read(*fd, buf, sizeof(buf))) <= 0) {
			/* no version then aye */
			rc = -1;
			break;
		}
		buf[nrd - 1U/* for \n*/] = '\0';
		/* parse buf */
		bp = buf;
		if (*bp++ != 'v') {
			/* weird, we req'd v-tags though */
			rc = -1;
			break;
		} else if ((bp = strchr(vtag = bp, '-')) != NULL) {
			/* tokenise sting */
			*bp++ = '\0';
		}
		/* bang vtag */
		xstrlcpy(v->vtag, vtag, sizeof(v->vtag));

		/* snarf distance */
		if (bp == NULL) {
			break;
		} else if ((bp = strchr(dist = bp, '-')) != NULL) {
			/* tokenize */
			*bp++ = '\0';
		}
		/* read distance */
		v->dist = strtoul(dist, &bp, 10);

		if (*++bp == 'g') {
			bp++;
			/* read scm revision */
			v->rvsn = hextou(bp, &bp);
		}
		if (*bp == '\0') {
			break;
		} else if (*bp == '-') {
			bp++;
		}
		if (!strcmp(bp, "dirty")) {
			v->dirty = 1U;
		}
	}
	close(*fd);
	if (fin(chld) != 0) {
		rc = -1;
	}
	return rc;
}

static int
hg_version(struct yuck_version_s v[static 1U])
{
	pid_t chld;
	int fd[1U];
	int rc = 0;

	if ((chld = run(fd, "hg", "log",
			"--rev", ".",
			"--template",
			"{latesttag}\t{latesttagdistance}\t{node|short}\n",
			NULL)) < 0) {
		return -1;
	}
	/* shouldn't be heaps, so just use a single read */
	with (char buf[256U]) {
		const char *vtag;
		const char *dist;
		char *bp;
		ssize_t nrd;

		if ((nrd = read(*fd, buf, sizeof(buf))) <= 0) {
			/* no version then aye */
			rc = -1;
			break;
		}
		buf[nrd - 1U/* for \n*/] = '\0';
		/* parse buf */
		bp = buf;
		if (*bp++ != 'v') {
			/* technically we could request the latest v-tag
			 * but i'm no hg buff so fuck it */
			rc = -1;
			break;
		} else if ((bp = strchr(vtag = bp, '\t')) != NULL) {
			/* tokenise */
			*bp++ = '\0';
		}
		/* bang vtag */
		xstrlcpy(v->vtag, vtag, sizeof(v->vtag));

		if (UNLIKELY(bp == NULL)) {
			/* huh? */
			rc = -1;
			break;
		} else if ((bp = strchr(dist = bp, '\t')) != NULL) {
			/* tokenise */
			*bp++ = '\0';
		}
		/* bang distance */
		v->dist = strtoul(dist, NULL, 10);

		/* bang revision */
		v->rvsn = hextou(bp, NULL);
	}
	close(*fd);
	if (fin(chld) != 0) {
		rc = -1;
	}
	return rc;
}

static int
bzr_version(struct yuck_version_s v[static 1U])
{
	pid_t chld;
	int fd[1U];
	int rc = 0;

	/* first get current revision number */
	if ((chld = run(fd, "bzr", "revno", NULL)) < 0) {
		return -1;
	}
	/* shouldn't be heaps, so just use a single read */
	with (char buf[256U]) {
		ssize_t nrd;

		if ((nrd = read(*fd, buf, sizeof(buf))) <= 0) {
			/* no version then aye */
			break;
		}
		with (char *on) {
			v->rvsn = strtoul(buf, &on, 10);
			if (LIKELY(on != NULL)) {
				v->rvsn <<= 4U;
				v->rvsn |= on - buf;
			}
		}
	}
	close(*fd);
	if (fin(chld) != 0) {
		return -1;
	}

	if ((chld = run(fd, "bzr", "tags",
			"--sort=time", NULL)) < 0) {
		return -1;
	}
	/* could be a lot, we only need the last line though */
	with (char buf[4096U]) {
		const char *vtag;
		size_t bz;
		char *bp;
		ssize_t nrd;

		bp = buf;
		bz = sizeof(buf);
		while ((nrd = read(*fd, bp, bz)) == (ssize_t)bz) {
			/* find last line */
			while (bz-- > 0 && buf[bz] != '\n');
			/* reassess bz */
			bz++;
			/* reassess bp */
			bp = buf + (sizeof(buf) - bz);
			if (LIKELY(bz < sizeof(buf))) {
				memmove(buf, buf + bz, sizeof(buf) - bz);
			}
		}
		if (nrd <= 0) {
			/* no version then aye */
			break;
		}
		bp[nrd - 1U/* for \n*/] = '\0';
		/* find last line */
		bp += nrd;
		while (--bp >= buf && *bp != '\n');

		/* parse buf */
		if (*++bp != 'v') {
			/* we want v tags, we could go back and see if
			 * there are any */
			rc = -1;
			break;
		} else if ((bp = strchr(vtag = ++bp, ' ')) != NULL) {
			/* tokenise */
			*bp++ = '\0';
		}
		/* bang vtag */
		xstrlcpy(v->vtag, vtag, sizeof(v->vtag));

		if (bp == NULL) {
			break;
		}
		/* read over all the whitespace to find the tag's revno */
		with (unsigned int rno = strtoul(bp, NULL, 10)) {
			v->dist = v->rvsn - rno;
		}
	}
	close(*fd);
	if (fin(chld) != 0) {
		rc = -1;
	}
	return rc;
}


/* public api */
#if !defined PATH_MAX
# define PATH_MAX	(256U)
#endif	/* !PATH_MAX */

int
yuck_version(struct yuck_version_s *restrict v, const char *path)
{
	char cwd[PATH_MAX];
	char fn[PATH_MAX];
	int rc = -1;

	/* initialise result structure */
	memset(v, 0, sizeof(*v));

	if (getcwd(cwd, sizeof(cwd)) == NULL) {
		return -1;
	}

	switch ((v->scm = find_scm(fn, sizeof(fn), path))) {
	case YUCK_SCM_ERROR:
	case YUCK_SCM_TARBALL:
	default:
		/* can't determine version numbers in tarball, can we? */
		return -1;
	case YUCK_SCM_GIT:
	case YUCK_SCM_BZR:
	case YUCK_SCM_HG:
		if (chdir(fn) < 0) {
			break;
		}
		switch (v->scm) {
		case YUCK_SCM_GIT:
			rc = git_version(v);
			break;
		case YUCK_SCM_BZR:
			rc = bzr_version(v);
			break;
		case YUCK_SCM_HG:
			rc = hg_version(v);
			break;
		default:
			break;
		}
		if (chdir(cwd) < 0) {
			/* oh big cluster fuck */
			rc = -1;
		}
		break;
	}
	return rc;
}

int
yuck_version_read(struct yuck_version_s *restrict ref, const char *fn)
{
	int rc = 0;
	int fd;

	/* initialise result structure */
	memset(ref, 0, sizeof(*ref));

	if (fn[0U] == '-' && fn[1U] == '\0') {
		fd = STDIN_FILENO;
	} else if ((fd = open(fn, O_RDONLY)) < 0) {
		return -1;
	}
	/* otherwise read and parse the string */
	with (char buf[256U]) {
		ssize_t nrd;
		char *bp;

		if ((nrd = read(fd, buf, sizeof(buf))) <= 0) {
			/* no version then aye */
			rc = -1;
			break;
		} else if ((bp = memchr(buf, '\n', nrd)) != NULL) {
			/* just go with the first line */
			*bp = '\0';
			nrd = bp - buf;
		} else if ((size_t)nrd < sizeof(buf)) {
			/* finalise with \nul */
			buf[nrd] = '\0';
		} else {
			/* finalise with \nul, cutting off the last byte */
			buf[--nrd] = '\0';
		}
		/* otherwise just read him */
		rc = rd_version(ref, buf, nrd);
	}
	close(fd);
	return rc;
}

ssize_t
yuck_version_write_fd(int fd, const struct yuck_version_s *ref)
{
	char buf[256U];
	ssize_t nwr;

	if ((nwr = wr_version(buf, sizeof(buf), ref)) <= 0) {
		return -1;
	}
	/* otherwise write */
	buf[nwr++] = '\n';
	return write(fd, buf, nwr);
}

int
yuck_version_write(const char *fn, const struct yuck_version_s *ref)
{
	int rc = 0;
	int fd;

	if (fn[0U] == '-' && fn[1U] == '\0') {
		fd = STDOUT_FILENO;
	} else if ((fd = open(fn, O_RDWR | O_CREAT | O_TRUNC, 0666)) < 0) {
		return -1;
	}
	if (yuck_version_write_fd(fd, ref) < 0) {
		rc = -1;
	}
	close(fd);
	return rc;
}

int
yuck_version_cmp(yuck_version_t v1, yuck_version_t v2)
{
	if (v1->dist == 0U && v2->dist == 0U) {
		/* must be a tag then, innit? */
		return memcmp(v1->vtag, v2->vtag, sizeof(v1->vtag));
	}
	/* just brute force the comparison, consider -dirty > -clean */
	return memcmp(v1, v2, sizeof(*v1));
}


#if defined BOOTSTRAP
int
main(int argc, char *argv[])
{
/* usage would be yuck-scmver SCMDIR [REFERENCE] */
	static struct yuck_version_s v[1U];
	int rc = 0;

	/* prefer reference file */
	if (argc > 2 && (rc = yuck_version_read(v, argv[2U])) == 0) {
		/* just use this one */
		;
	} else {
		rc = yuck_version(v, argv[1U]);
	}
	if (rc == 0) {
		fputs("define(YUCK_SCMVER_VERSION, ", stdout);
		fputs(v->vtag, stdout);
		if (v->scm > YUCK_SCM_TARBALL && v->dist) {
			fputc('.', stdout);
			fputs(yscm_strs[v->scm], stdout);
			fprintf(stdout, "%u.%0*x",
				v->dist,
				(int)(v->rvsn & 0x07U), v->rvsn >> 4U);
		}
		if (v->dirty) {
			fputs(".dirty", stdout);
		}
		fputs(")\n", stdout);
	}
	return -rc;
}
#endif	/* BOOTSTRAP */


#if defined CONFIGURE
int
main(int argc, char *argv[])
{
/* usage would be yuck-scmver [REFERENCE] */
	static struct yuck_version_s v[1U];
	int rc = 0;

	if (argc > 1) {
		rc = yuck_version_read(v, argv[1U]);
#if defined VERSION_FILE
	} else if ((rc = yuck_version_read(v, VERSION_FILE)) == 0) {
		;
#endif	/* VERSION_FILE */
	} else {
		rc = yuck_version(v, NULL);
	}
	/* print if successful */
	if (rc == 0) {
		fputs(v->vtag, stdout);
		if (v->scm > YUCK_SCM_TARBALL && v->dist) {
			fputc('.', stdout);
			fputs(yscm_strs[v->scm], stdout);
			fprintf(stdout, "%u.%0*x",
				v->dist,
				(int)(v->rvsn & 0x07U), v->rvsn >> 4U);
		}
		if (v->dirty) {
			fputs(".dirty", stdout);
		}
		fputc('\n', stdout);
	}
	return -rc;
}
#endif	/* CONFIGURE */

/* yuck-scmver.c ends here */
