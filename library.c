/*	$Id$ */
/*
 * Copyright (c) 2017, Kristaps Dzonsons
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "config.h"

#include <ctype.h>
#if HAVE_ERR
# include <err.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "lowdown.h"
#include "extern.h"

#define DEF_IUNIT 1024
#define DEF_OUNIT 64
#define DEF_MAX_NESTING 16

static	const char *const errs[LOWDOWN_ERR__MAX] = {
	"space before link (CommonMark violation)",
	"bad character in metadata key (MultiMarkdown violation)",
};

const char *
lowdown_errstr(enum lowdown_err err)
{

	return(errs[err]);
}

void
lowdown_buf(const struct lowdown_opts *opts,
	const unsigned char *data, size_t datasz,
	unsigned char **res, size_t *rsz,
	struct lowdown_meta **m, size_t *msz)
{
	hbuf	 	*ob, *spb;
	hrend 		*renderer = NULL;
	hdoc 		*document;

	/*
	 * Begin by creating our buffers, renderer, and document.
	 */

	ob = hbuf_new(DEF_OUNIT);
	spb = hbuf_new(DEF_OUNIT);

	renderer = NULL == opts || LOWDOWN_HTML == opts->type ?
		hrend_html_new
		(NULL == opts ? 0 : opts->oflags, 0) :
		hrend_nroff_new(opts->oflags, 
			LOWDOWN_MAN == opts->type);

	document = hdoc_new
		(renderer, opts, NULL == opts ?
		 0 : opts->feat, DEF_MAX_NESTING,
		 NULL != opts &&
		 LOWDOWN_HTML != opts->type);

	/* Parse the output and free resources. */

	hdoc_render(document, ob, data, datasz, m, msz);
	hdoc_free(document);
	if (NULL == opts || LOWDOWN_HTML == opts->type)
		hrend_html_free(renderer);
	else
		hrend_nroff_free(renderer);

	/* Reprocess the output as smartypants. */

	if (NULL != opts && 
	    LOWDOWN_SMARTY & opts->oflags) {
		if (LOWDOWN_HTML == opts->type)
			hsmrt_html(spb, ob->data, ob->size);
		else
			hsmrt_nroff(spb, ob->data, ob->size);
		hbuf_free(ob);
		*res = spb->data;
		*rsz = spb->size;
		spb->data = NULL;
		hbuf_free(spb);
	} else {
		*res = ob->data;
		*rsz = ob->size;
		ob->data = NULL;
		hbuf_free(ob);
	}
}

int
lowdown_file(const struct lowdown_opts *opts,
	FILE *fin, unsigned char **res, size_t *rsz,
	struct lowdown_meta **m, size_t *msz)
{
	hbuf *ib = NULL;

	ib = hbuf_new(DEF_IUNIT);

	if (hbuf_putf(ib, fin)) {
		hbuf_free(ib);
		return(0);
	}

	lowdown_buf(opts, ib->data, ib->size, res, rsz, m, msz);
	hbuf_free(ib);
	return(1);
}

static char *
date2str(const char *v)
{
	unsigned int	y, m, d;
	int		rc;
	static char	buf[32];

	if (NULL == v)
		return(NULL);

	rc = sscanf(v, "%u/%u/%u", &y, &m, &d);
	if (3 != rc) {
		rc = sscanf(v, "%u-%u-%u", &y, &m, &d);
		if (3 != rc) {
			warnx("malformed ISO-8601 date");
			return(NULL);
		}
	}

	snprintf(buf, sizeof(buf), "%u-%.2u-%.2u", y, m, d);
	return(buf);
}

static char *
rcsdate2str(const char *v)
{
	unsigned int	y, m, d, h, min, s;
	int		rc;
	static char	buf[32];

	if (NULL == v)
		return(NULL);

	if (strlen(v) < 7) {
		warnx("malformed RCS date");
		return(NULL);
	}

	v += 7;
	rc = sscanf(v, "%u/%u/%u %u:%u:%u", 
		&y, &m, &d, &h, &min, &s);

	if (6 != rc) {
		warnx("malformed RCS date");
		return(NULL);
	}

	snprintf(buf, sizeof(buf), "%u-%.2u-%.2u", y, m, d);
	return(buf);
}

static void
serialise_roff(hbuf *op, const char *v, int block)
{

	while (isspace((int)*v))
		v++;
	if (block && '.' == *v)
		HBUF_PUTSL(op, "\\&");
	for ( ; '\0' != *v; v++) 
		if ('\\' == *v)
			HBUF_PUTSL(op, "\\e");
		else if ( ! block && '"' == *v)
			HBUF_PUTSL(op, "\\(dq");
		else if (isspace((int)*v))
			hbuf_putc(op, ' ');
		else
			hbuf_putc(op, *v);
	if (block)
		HBUF_PUTSL(op, "\n");
}

void
lowdown_standalone_open(const struct lowdown_opts *opts,
	const struct lowdown_meta *m, size_t msz,
	unsigned char **res, size_t *rsz)
{
	const char	*date = NULL, *author = NULL,
	      		*title = "Untitled article";
	time_t		 t;
	char		 buf[32];
	struct tm	*tm;
	size_t		 i;
	hbuf		*op;

	op = hbuf_new(DEF_OUNIT);

	/* Acquire metadata that we'll fill in. */

	for (i = 0; i < msz; i++) 
		if (0 == strcmp(m[i].key, "title"))
			title = m[i].value;
		else if (0 == strcmp(m[i].key, "author"))
			author = m[i].value;
		else if (0 == strcmp(m[i].key, "rcsdate"))
			date = rcsdate2str(m[i].value);
		else if (0 == strcmp(m[i].key, "date"))
			date = date2str(m[i].value);

	/* FIXME: convert to buf without strftime. */

	if (NULL == date) {
		t = time(NULL);
		tm = localtime(&t);
		strftime(buf, sizeof(buf), "%F", tm);
		date = buf;
	}

	switch (opts->type) {
	case LOWDOWN_HTML:
		HBUF_PUTSL(op, 
		      "<!DOCTYPE html>\n"
		      "<html>\n"
		      "<head>\n"
		      "<meta charset=\"utf-8\">\n"
		      "<meta name=\"viewport\" content=\""
		       "width=device-width,initial-scale=1\">\n"
		      "<title>");
		while (isspace((int)*title))
			title++;
		for ( ; '\0' != *title; title++)
			if ('<' == *title)
				HBUF_PUTSL(op, "&lt;");
			else if ('>' == *title)
				HBUF_PUTSL(op, "&gt;");
			else if (isspace((int)*title))
				hbuf_putc(op, ' ');
			else
				hbuf_putc(op, *title);
		HBUF_PUTSL(op, 
		      "</title>\n"
		      "</head>\n"
		      "<body>\n");
		break;
	case LOWDOWN_NROFF:
		hbuf_printf(op, ".DA %s\n.TL\n", date);
		serialise_roff(op, title, 1);
		if (NULL != author) {
			HBUF_PUTSL(op, ".AU\n");
			serialise_roff(op, author, 1);
		}
		break;
	case LOWDOWN_MAN:
		HBUF_PUTSL(op, ".TH \"");
		serialise_roff(op, title, 0);
		hbuf_printf(op, "\" 7 %s\n", date);
		break;
	}

	*res = op->data;
	*rsz = op->size;

	op->data = NULL;
	hbuf_free(op);
}

void
lowdown_standalone_close(const struct lowdown_opts *opts,
	unsigned char **res, size_t *rsz)
{
	hbuf	*op;

	op = hbuf_new(DEF_OUNIT);

	switch (opts->type) {
	case LOWDOWN_HTML:
		HBUF_PUTSL(op, "</body>\n</html>\n");
		break;
	default:
		break;
	}

	*res = op->data;
	*rsz = op->size;

	op->data = NULL;
	hbuf_free(op);
}
