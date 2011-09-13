/*** date-io.h -- helper for formats, parsing, printing, escaping, etc. */
#if !defined INCLUDED_date_io_h_
#define INCLUDED_date_io_h_

#include "date-core.h"

static inline struct dt_d_s
dt_io_strpd(const char *input, const char *const *fmt, size_t nfmt)
{
	struct dt_d_s res;

	/* basic sanity check */
	if (input == NULL || strcmp(input, "now") == 0) {
		return dt_date(DT_YMD);
	} else if (nfmt == 0) {
		res = dt_strpd(input, NULL);
	} else {
		for (size_t i = 0; i < nfmt; i++) {
			if ((res = dt_strpd(input, fmt[i])).typ > DT_UNK) {
				break;
			}
		}
	}
	return res;
}

static inline void
dt_io_unescape(char *s)
{
	static const char esc_map[] = "\a\bcd\e\fghijklm\nopq\rs\tu\v";
	char *p, *q;

	if ((p = q = strchr(s, '\\'))) {
		do {
			if (*p != '\\' || !*++p) {
				*q++ = *p++;
			} else if (*p < 'a' || *p > 'v') {
				*q++ = *p++;
			} else {
				*q++ = esc_map[*p++ - 'a'];
			}
		} while (*p);
		*q = '\0';
	}
	return;
}

#endif	/* INCLUDED_date_io_h_ */
