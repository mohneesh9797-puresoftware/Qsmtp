/** \file qsmtpd/spf.c
 \brief functions to query and parse SPF entries
 */
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#define __USE_GNU
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "qsmtpd.h"
#include "antispam.h"
#include "sstring.h"
#include "libowfatconn.h"
#include "match.h"
#include "netio.h"
#include "fmt.h"

#define WSPACE(x) (((x) == ' ') || ((x) == '\t') || ((x) == '\r') || ((x) == '\n'))

static const char spf_delimiters[] = ".-+,/_=";

static int spfmx(const char *domain, char *token);
static int spfa(const char *domain, char *token);
static int spfip4(char *domain);
static int spfip6(char *domain);
static int spflookup(const char *domain, const int rec);
static int spfptr(const char *domain, char *token);
static int spfexists(const char *domain, char *token);
static int spf_domainspec(const char *domain, char *token, char **domainspec, int *ip4cidr, int *ip6cidr);

/**
 * look up SPF records for domain
 *
 * This works a like the check_host in the SPF draft but takes two arguments less. The remote ip and the full
 * sender address can be taken directly from xmitstat.
 *
 * @param domain no idea what this might be for ;)
 * @return one of the SPF_* constants defined in include/antispam.h
 */
int
check_host(const char *domain)
{
	return spflookup(domain, 0);
}

/**
 * look up SPF records for domain
 *
 * @param domain no idea what this might be for
 * @param rec recursion level
 * @return one of the SPF_* constants defined in include/antispam.h or -1 on ENOMEM
 */
static int
spflookup(const char *domain, const int rec)
{
	char *txt, *token, *valid = NULL, *redirect = NULL;
	int i, result = SPF_NONE, prefix;

	if (rec >= 20)
		return SPF_HARD_ERROR;

	/* don't enforce valid domains on redirects */
	if (!rec && domainvalid(domain))
		return SPF_FAIL_MALF;

 	i = dnstxt(&txt, domain);
	if (i) {
		switch (errno) {
			case ENOENT:	return SPF_NONE;
			case ETIMEDOUT:
			case EIO:
			case ECONNREFUSED:
			case EAGAIN:	return SPF_TEMP_ERROR;
			case EINVAL:	return SPF_HARD_ERROR;
			case ENOMEM:
			default:	return -1;
		}
	}
	if (!txt)
		return SPF_NONE;
	token = txt;
	while ((token = strstr(token, "v=spf1"))) {
		if (valid) {
			free(txt);
			return SPF_HARD_ERROR;
		} else {
			token += 6;
			valid = token;
		}
	}
	if (!valid) {
		free(txt);
		return SPF_NONE;
	}
	token = valid;
	while (*token && (result != SPF_PASS)) {
		while (WSPACE(*token)) {
			token++;
		}
		if (!*token)
			break;
		switch(*token) {
			case '-':	token++; prefix = SPF_FAIL_PERM; break;
			case '~':	token++; prefix = SPF_SOFTFAIL; break;
			case '+':	token++; prefix = SPF_PASS; break;
			case '?':	token++; prefix = SPF_NEUTRAL; break;
			default:	if (((*token >= 'a') && (*token <= 'z')) ||
								((*token >= 'A') && (*token <= 'Z'))) {
						prefix = SPF_PASS;
					} else {
						free(txt);
						return SPF_HARD_ERROR;
					}
		}
		if (!strncasecmp(token, "mx", 2) &&
					(WSPACE(*(token + 2)) || !*(token + 2) || (*(token + 2) == ':') ||
						(*(token + 2) == '/'))) {
			token += 2;
			if (*token == ':')
				token++;
			result = spfmx(domain, token);
		} else if (!strncasecmp(token, "ptr", 3) &&
				(WSPACE(*(token + 2)) || !*(token + 2) || (*(token + 2) == ':'))) {
			token += 3;
			if (*token == ':')
				token++;
			result = spfptr(domain, token);
		} else if (!strncasecmp(token, "exists:", 7)) {
			token += 7;
			result = spfexists(domain, token);
		} else if (!strncasecmp(token, "all", 3) && (WSPACE(*(token + 3)) || !*(token + 3))) {
			result = SPF_PASS;
		} else if (((*token == 'a') || (*token == 'A')) &&
					(WSPACE(*(token + 1)) || !*(token + 1) || (*(token + 1) == ':'))) {
			if (*(++token) == ':')
				token++;
			result = spfa(domain, token);
		} else if (!strncasecmp(token, "ip4:", 4)) {
			token += 4;
			result = spfip4(token);
		} else if (!strncasecmp(token, "ip6:", 4)) {
			token += 4;
			result = spfip6(token);
		} else if (!strncasecmp(token, "include:", 8)) {
			char *n;
			int flagnext = 0;

			token += 8;
			n = token;
			while (!WSPACE(*n) && *n) {
				n++;
			}
			if (*n) {
				*n = '\0';
				flagnext = 1;
			}
			result = spflookup(token, rec + 1);
			switch (result) {
				case SPF_NONE:	result = SPF_PASS;
						prefix = SPF_FAIL_NONEX;
						break;
				case SPF_TEMP_ERROR:
				case SPF_HARD_ERROR:
				case SPF_PASS:	prefix = result;
						result = SPF_PASS;
						break;
				case -1:	break;
				default:	result = SPF_NONE;
			}
			token = n;
			if (flagnext)
				*n = ' ';
		} else if (!strncasecmp(token, "redirect=", 9)) {
			token += 9;
			if (!redirect) {
				redirect = token;
			}
		} else {
			result = 0;
		}
/* skip to the end of this token */
		while (*token && !WSPACE(*token)) {
			token++;
		}
		if ((result == SPF_TEMP_ERROR) || (result == SPF_HARD_ERROR)) {
			prefix = result;
			result = SPF_PASS;
		}
	}
	if (result < 0) {
		free(txt);
		return result;
	}
	if (result == SPF_PASS) {
		if (SPF_FAIL(prefix)) {
			char *ex;

			if ( (ex = strcasestr(txt, "exp=")) ) {
				int ip4, ip6;

				if ((i = spf_domainspec(domain, ex, &xmitstat.spfexp, &ip4, &ip6))) {
					xmitstat.spfexp = NULL;
				}
			}
		}
		free(txt);
		return prefix;
	}
	free(txt);
	if (redirect) {
		char *e = redirect;

		while (*e && !WSPACE(*e)) {
			e++;
		}
		*e = '\0';
		return spflookup(redirect, rec + 1);
	}
	return SPF_NEUTRAL;
}

#define WRITE(fd, s, l) if ( (rc = write((fd), (s), (l))) < 0 ) return rc

/**
 * print "Received-SPF:" to message header
 *
 * @param fd file descriptor of message body
 * @param spf SPF status of mail transaction
 * @return 0 if everything goes right, -1 on write error
 */
int
spfreceived(int fd, const int spf) {
	int rc;
	char *fromdomain;

	if (spf == SPF_IGNORE)
		return 0;

	if (xmitstat.mailfrom.len) {
		fromdomain = strchr(xmitstat.mailfrom.s, '@') + 1;
	} else {
		fromdomain = HELOSTR;
	}
	WRITE(fd, "Received-SPF: ", 14);
	WRITE(fd, heloname.s, heloname.len);
	if (spf == SPF_HARD_ERROR) {
		WRITE(fd, ": syntax error while parsing SPF entry for ", 43);
		WRITE(fd, fromdomain, strlen(fromdomain));
	} else if (spf == SPF_TEMP_ERROR) {
		WRITE(fd, ": can't get SPF entry for ", 26);
		WRITE(fd, fromdomain, strlen(fromdomain));
		WRITE(fd, " (DNS problem)", 14);
	} else if (spf == SPF_NONE) {
		WRITE(fd, ": no SPF entry for ", 19);
		WRITE(fd, fromdomain, strlen(fromdomain));
	} else if (spf == SPF_UNKNOWN) {
		WRITE(fd, ": can not figure out SPF status for ", 36);
		WRITE(fd, fromdomain, strlen(fromdomain));
	} else {
		WRITE(fd, ": SPF status for ", 17);
		WRITE(fd, fromdomain, strlen(fromdomain));
		WRITE(fd, " is ", 4);
		switch(spf) {
			case SPF_PASS:		WRITE(fd, "PASS", 4); break;
			case SPF_SOFTFAIL:	WRITE(fd, "SOFTFAIL", 8); break;
			case SPF_NEUTRAL:	WRITE(fd, "NEUTRAL", 7); break;
			case SPF_FAIL_NONEX:
			case SPF_FAIL_MALF:
			case SPF_FAIL_PERM:	WRITE(fd, "FAIL", 4); break;
		}
	}
	WRITE(fd, "\n", 1);
	return 0;
}

/* the SPF routines
 *
 * return values:
 *  SPF_NONE: no match
 *  SPF_PASS: match
 *  SPF_HARD_ERROR: parse error
 * -1: error (ENOMEM)
 */
static int
spfmx(const char *domain, char *token)
{
	int ip6l, ip4l, i;
	struct ips *mx;
	char *domainspec;

	if ( (i = spf_domainspec(domain, token, &domainspec, &ip4l, &ip6l)) ) {
		return i;
	}
	if (ip4l < 0) {
		ip4l = 32;
	}
	if (ip6l < 0) {
		ip6l = 128;
	}
	if (domainspec) {
		i = ask_dnsmx(domainspec, &mx);
		free(domainspec);
	} else {
		i = ask_dnsmx(domain, &mx);
	}
	switch (i) {
		case 1: return SPF_NONE;
		case 2: return SPF_TEMP_ERROR;
		case 3:	return SPF_HARD_ERROR;
		case -1:return -1;
	}
	if (!mx) {
		return SPF_NONE;
	}
/* Don't use the implicit MX for this. There are either all MX records
 * implicit or none so we only have to look at the first one */
	if (mx->priority >= 65536) {
		freeips(mx);
		return SPF_NONE;
	}
	if (IN6_IS_ADDR_V4MAPPED(&xmitstat.sremoteip)) {
		while (mx) {
			if (IN6_IS_ADDR_V4MAPPED(&(mx->addr)) &&
					ip4_matchnet(&xmitstat.sremoteip,
							(struct in_addr *) &(mx->addr.s6_addr32[3]), ip4l)) {
				freeips(mx);
				return SPF_PASS;
			}
			mx = mx->next;
		}
	} else {
		while (mx) {
			if (ip6_matchnet(&xmitstat.sremoteip, &mx->addr, ip6l)) {
				freeips(mx);
				return SPF_PASS;
			}
			mx = mx->next;
		}
	}
	freeips(mx);
	return SPF_NONE;
}

static int
spfa(const char *domain, char *token)
{
	int ip6l, ip4l, i, r = 0;
	struct ips *ip, *thisip;
	char *domainspec;

	if ( (i = spf_domainspec(domain, token, &domainspec, &ip4l, &ip6l)) ) {
		return i;
	}
	if (ip4l < 0) {
		ip4l = 32;
	}
	if (ip6l < 0) {
		ip6l = 128;
	}
	if (domainspec) {
		i = ask_dnsaaaa(domainspec, &ip);
		free(domainspec);
	} else {
		i = ask_dnsaaaa(domain, &ip);
	}

	switch (i) {
		case 0:	thisip = ip;
			r = SPF_NONE;
			while (thisip) {
				if (IN6_ARE_ADDR_EQUAL(&(thisip->addr), &(xmitstat.sremoteip))) {
					r = SPF_PASS;
					break;
				}
				thisip = thisip->next;
			}
			freeips(ip);
			break;
		case 1:	r = SPF_NONE;
			break;
		case 2:	r = SPF_TEMP_ERROR;
			break;
		case -1:	r = -1;
			break;
		default:r = SPF_HARD_ERROR;
	}
	return r;
}

static int
spfexists(const char *domain, char *token)
{
	int ip6l, ip4l, i, r = 0;
	char *domainspec;

	if ( (i = spf_domainspec(domain, token, &domainspec, &ip4l, &ip6l)) ) {
		return i;
	}
	if ((ip4l > 0) || (ip6l > 0) || !domainspec) {
		return SPF_HARD_ERROR;
	}
	i = ask_dnsa(domainspec, NULL);
	free(domainspec);

	switch (i) {
		case 0:	r = SPF_PASS;
			break;
		case 1:	r = SPF_NONE;
			break;
		case 2:	r = SPF_TEMP_ERROR;
			break;
		case -1:	r = -1;
			break;
		default:r = SPF_HARD_ERROR;
	}
	return r;
}

static int
spfptr(const char *domain, char *token)
{
	int ip6l, ip4l, i, r = 0;
	struct ips *ip, *thisip;
	char *domainspec;

	if (!xmitstat.remotehost.len) {
		return SPF_NONE;
	}
	if ( (i = spf_domainspec(domain, token, &domainspec, &ip4l, &ip6l)) ) {
		return i;
	}
	if ((ip4l > 0) || (ip6l > 0)) {
		free(domainspec);
		return SPF_HARD_ERROR;
	}
	if (domainspec) {
		i = ask_dnsaaaa(domainspec, &ip);
		free(domainspec);
	} else {
		i = ask_dnsaaaa(domain, &ip);
	}

	switch (i) {
		case 0:	thisip = ip;
			r = SPF_NONE;
			while (thisip) {
				if (IN6_ARE_ADDR_EQUAL(&(thisip->addr), &(xmitstat.sremoteip))) {
					r = SPF_PASS;
					break;
				}
				thisip = thisip->next;
			}
			freeips(ip);
			break;
		case 1:	r = SPF_NONE;
			break;
		case 2:	r = SPF_TEMP_ERROR;
			break;
		case -1:	r = -1;
			break;
		default:r = SPF_HARD_ERROR;
	}
	return r;
}

static int
spfip4(char *domain)
{
	char *sl = domain;
	char osl;	/* char at *sl before we overwrite it */
	struct in_addr net;
	unsigned long u;

	if (!IN6_IS_ADDR_V4MAPPED(&xmitstat.sremoteip))
		return SPF_NONE;
	while (((*sl >= '0') && (*sl <= '9')) || (*sl == '.')) {
		sl++;
	}
	if (*sl == '/') {
		char *q = sl;

		osl = *sl;
		*sl = '\0';
		u = strtoul(sl + 1, &sl, 10);
		if ((u < 8) || (u > 32) || !WSPACE(*sl))
			return SPF_HARD_ERROR;
		sl = q;
	} else if (WSPACE(*sl) || !*sl) {
		osl = *sl;
		*sl = '\0';
		u = 32;
	} else {
		return SPF_HARD_ERROR;
	}
	if (!inet_pton(AF_INET, domain, &net))
		return SPF_HARD_ERROR;
	*sl = osl;
	return ip4_matchnet(&xmitstat.sremoteip, &net, u) ? SPF_PASS : SPF_NONE;
}

static int
spfip6(char *domain)
{
	char *sl = domain;
	char osl;	/* char at *sl before we overwrite it */
	struct in6_addr net;
	unsigned long u;

	if (IN6_IS_ADDR_V4MAPPED(&xmitstat.sremoteip))
		return SPF_NONE;
	while (((*sl >= '0') && (*sl <= '9')) || ((*sl >= 'a') && (*sl <= 'f')) ||
					((*sl >= 'A') && (*sl <= 'F')) || (*sl == ':') || (*sl == '.')) {
		sl++;
	}
	if (*sl == '/') {
		char *q = sl;

		osl = *sl;
		*sl = '\0';
		u = strtoul(sl + 1, &sl, 10);
		if ((u < 8) || (u > 128) || !WSPACE(*sl))
			return SPF_HARD_ERROR;
		sl = q;
	} else if (WSPACE(*sl) || !*sl) {
		osl = *sl;
		*sl = '\0';
		u = 128;
	} else {
		return SPF_HARD_ERROR;
	}
	osl = *sl;
	*sl = '\0';
	if (!inet_pton(AF_INET6, domain, &net))
		return SPF_HARD_ERROR;
	*sl = osl;
	return ip6_matchnet(&xmitstat.sremoteip, &net, (unsigned char) (u & 0xff)) ? SPF_PASS : SPF_NONE;
}

/**
 * parse the options in an SPF macro
 *
 * @param token token to parse
 * @param num DIGIT
 * @param r if reverse is given
 * @param delim bitmask of delimiters
 * @return number of bytes parsed, -1 on error
 */
static int
spf_makroparam(char *token, int *num, int *r, int *delim)
{
	int res = 0;
	const char *t;

	*r = 0;
	*num = -1;
	*delim = 1;	/* '.' is the default delimiter */

	if ((*token >= '0') && (*token <= '9')) {
		*num = 0;
		do {
			*num = *num * 10 + (*token++ - '0');
			res++;
		} while ((*token >= '0') && (*token <= '9'));
		if (!*num) {
			errno = EINVAL;
			return -1;
		}
	} else {
		*num = 255;
	}
	if (*token == 'r') {
		token++;
		res++;
		*r = 1;
	}
	do {
		int k;

		t = token;
		for (k = 0; k < strlen(spf_delimiters); k++) {
			if (spf_delimiters[k] == *token) {
				*delim |= (1 << k);
				token++;
				res++;
			}
		}
	} while (t != token);

	return res;
}

static int
urlencode(char *token, char **result)
{
	char *res = NULL;
	char *last = token;	/* the first unencoded character in the current chunk */
	unsigned int len = 0;

	while (*token) {
		char *tmp;
		unsigned int newlen;
		char n;

		if (!(((*token >= 'a') && (*token <= 'z')) || ((*token >= 'A') && (*token <= 'Z')) ||
						((*token >= 'A') && (*token <= 'Z')))) {
			switch (*token) {
				case '-':
				case '_':
				case '.':
				case '!':
				case '~':
				case '*':
				case '\'':
				case '(':
				case ')':	break;
				default:	newlen = len + 3 + (token - last);
						tmp = realloc(res, newlen + 1);
						if (!tmp) {
							free(res);
							return -1;
						}
						res = tmp;
						memcpy(res + len, last, token - last);
						len = newlen;
						res[len - 3] = '%';
						n = (*token & 0xf0) >> 4;
						res[len - 2] = ((n > 9) ? 'A' - 10 : '0') + n;
						n = (*token & 0x0f);
						res[len - 1] = ((n > 9) ? 'A' - 10 : '0') + n;
						last = token + 1;
			}
		}
		token++;
	}
	if (!len) {
		/* nothing has changed */
		*result = token;
		return 0;
	}
	if (token - last) {
		/* there is data behind the last encoded char */
		unsigned int newlen = len + 3 + (token - last);
		char *tmp;

		tmp = realloc(res, newlen + 1);
		if (!tmp) {
			free(res);
			return -1;
		}
		res = tmp;
		memcpy(res + len, last, token - last);
		len = newlen;
	}
	res[len] = '\0';
	*result = res;
	return 0;
}

/**
 * append a makro content to the result
 *
 * @param res result string
 * @param l current length of res
 * @param s the raw string to appended, does not need to be terminated by '\0'
 * @param sl strlen(s), must not be 0
 * @param num DIGIT
 * @param r Bit 1: reverse of not; Bit 2: use URL encoding
 * @param delim bit mask of delimiters
 * @return 0 on success, -1 on error
 */
static int
spf_appendmakro(char **res, unsigned int *l, const char *const s, const unsigned int sl, int num,
			const int r, const int delim)
{
	int dc = 0;	/* how many delimiters we find */
	unsigned int nl;
	char *start;
	char *r2;
	unsigned int oldl = *l;
	char *news = malloc(sl + 1);

	if (!news)
		return -1;
	memcpy(news, s, sl);
	news[sl] = '\0';
/* first: go and replace all delimiters with '.' */
	/* delim == 1 means only '.' is delimiter so we only have to count them */
	if (delim == 1) {
		int j = sl;

		while (--j >= 0) {
			 if (s[j] == '.') {
			 	dc++;
			 }
		}
	} else {
		char actdelim[8];
		unsigned int k = 0;
		int m;
		char *d = news;

		/* This constructs the list of actually used delimiters. */
		for (m = strlen(spf_delimiters); m >= 0; m--) {
			if (delim & (1 << m))
				actdelim[k++] = spf_delimiters[m];
		}
		actdelim[k] = '\0';

		while ( (d = strpbrk(d, actdelim)) ) {
			*d = '.';
			dc++;
		}
	}
	if (r & 1) {
		char *tmp, *dot;
		unsigned int v;

		start = news;
		if (num > dc) {
			v = sl;
			num = dc + 1;
		} else {
			for (v = num; v > 0; v--) {
				start = strchr(start, '.') + 1;
			}
			v = start - news - 1;
		}

		tmp = malloc(v + 1);
		dot = news;
		nl = v;
		dc = num - 1;

		while (--dc >= 0) {
			unsigned int o = strchr(dot, '.') - dot;

			memcpy(tmp + v - o, dot, o);
			tmp[v - o - 1] = '.';
			dot += o + 1;
			v -= o + 1;
		}
		if ((start = strchr(dot, '.'))) {
			v = start - dot;
		} else {
			v = strlen(dot);
		}
		memcpy(tmp, dot, v);
		free(news);
		start = news = tmp;
	} else {
		start = news;
		if (dc >= num) {
			while (dc-- >= num) {
				start = strchr(start, '.') + 1;
			}
			nl = strlen(start);
		} else {
			nl = sl;
		}
	}

	if (r & 2) {
		if (urlencode(start, &start)) {
			free(*res);
			free(news);
			return -1;
		}
		nl = strlen(start);
	}

	*l += nl;
	r2 = realloc(*res, *l);
	if (!r2) {
		free(*res);
		free(news);
		return -1;
	}
	*res = r2;
	memcpy(*res + oldl, start, nl);
	free(news);

	return 0;
}

#define APPEND(addlen, addstr) \
	{\
		char *r2;\
		unsigned int oldl = *l;\
		\
		*l += addlen;\
		r2 = realloc(*res, *l);\
		if (!r2) { free(*res); return -1;}\
		*res = r2;\
		memcpy(*res + oldl, addstr, addlen);\
	}

#define PARSEERR	{free(*res); return -1;}

/**
 * expand a SPF makro letter
 *
 * @param p the token to parse
 * @param domain the current domain string
 * @param ex if this is an exp string
 * @param res the resulting string is stored here
 * @param l offset into res
 * @return number of bytes parsed, -1 on error
 */
static int
spf_makroletter(char *p, const char *domain, int ex, char **res, unsigned int *l)
{
	char *q = p, ch;
	int offs, num, r, delim;

	ch = *p++;
	offs = spf_makroparam(p, &num, &r, &delim);
	p += offs;
	if ((offs < 0) || (*p != '}'))
		PARSEERR;
	switch (ch) {
		case 'S':	r |= 0x2;
		case 's':	if (xmitstat.mailfrom.len) {
					if (spf_appendmakro(res, l, xmitstat.mailfrom.s,
					    				xmitstat.mailfrom.len,
									num, r, delim))
						return -1;
				} else {
					unsigned int senderlen = 12 + HELOLEN;
					char *sender = malloc(senderlen--);

					if (!sender)
						return -1;
					memcpy(sender, "postmaster@", 11);
					memcpy(sender + 11, HELOSTR, HELOLEN + 1);
					r = spf_appendmakro(res, l, sender, senderlen, num, r, delim);
					free(sender);
					if (r)
						return -1;
				}
				break;
		case 'L':	r |= 0x2;
		case 'l':	if (xmitstat.mailfrom.len) {
					char *at = strchr(xmitstat.mailfrom.s, '@');

					if (spf_appendmakro(res, l, xmitstat.mailfrom.s,
					    					at - xmitstat.mailfrom.s,
										num, r, delim)) {
						return -1;
					}
				} else {
					/* we can do it the short way here, this can't be changed by
					 * any combination of makro flags */
					APPEND(10, "postmaster");
				}
				break;
		case 'O':	r |= 0x2;
		case 'o':	if (xmitstat.mailfrom.len) {
					char *at = strchr(xmitstat.mailfrom.s, '@');
					unsigned int offset =
							at - xmitstat.mailfrom.s + 1;

					/* the domain name is always the same in normal and url-ified form */
					if (spf_appendmakro(res, l, at + 1, xmitstat.mailfrom.len - offset,
					    				num, r, delim))
						return -1;
				} else {
					if (spf_appendmakro(res, l, HELOSTR, HELOLEN, num, r, delim))
						return -1;
				}
				break;
		case 'D':	r |= 0x2;
		case 'd':	if (spf_appendmakro(res, l, domain, strlen(domain), num, r, delim))
					return -1;
				break;
		case 'C':
		case 'c':	if (!ex)
					PARSEERR;
				/* fallthrough */
		case 'I':
		case 'i':	if (IN6_IS_ADDR_V4MAPPED(&xmitstat.sremoteip)) {
					char ip[INET_ADDRSTRLEN];

					inet_ntop(AF_INET,
							&(xmitstat.sremoteip.s6_addr32[3]),
							ip, sizeof(ip));

					if (spf_appendmakro(res, l, ip, strlen(ip), num, r, delim))
						return -1;
				} else {
					if ((ch == 'i') || (ch == 'I')) {
						char ip[64];

						dotip6(ip);
						ip[63] = '\0';
						if (spf_appendmakro(res, l, ip, 63, num, r, delim))
							return -1;
					} else {
						char a[INET6_ADDRSTRLEN];

						inet_ntop(AF_INET6, &xmitstat.sremoteip, a, sizeof(a));
						APPEND(strlen(a), a);
					}
				}
				break;
		case 'T':
		case 't':	if (!ex) {
					PARSEERR;
				} else {
					char t[ULSTRLEN];

					ultostr(time(NULL), t);
					APPEND(strlen(t), t);
				}
				break;
		case 'P':
		case 'p':	if (xmitstat.remotehost.len) {
					if (spf_appendmakro(res, l, xmitstat.remotehost.s,
					    			xmitstat.remotehost.len, num, r, delim))
						return -1;
				} else {
					APPEND(7, "unknown");
				}
				break;
		case 'R':
		case 'r':	if (!ex) {
					PARSEERR;
				}
				if (spf_appendmakro(res, l, heloname.s, heloname.len, num, r, delim))
					return -1;
				break;
		case 'V':
		case 'v':	if (IN6_IS_ADDR_V4MAPPED(&xmitstat.sremoteip)) {
					if (delim & 2) {
						if (r) {
							if (num == 1) {
								APPEND(2, "in");
							} else {
								APPEND(7, "addr.in");
							}
						} else {
							if (num == 1) {
								APPEND(4, "addr");
							} else {
								APPEND(7, "in.addr");
							}
						}
					} else {
						APPEND(7, "in-addr");
					}
				} else {
					APPEND(3, "ip6");
				}
				break;
		case 'H':
		case 'h':	APPEND(10, "deprecated");
				break;
		default:	APPEND(7, "unknown");
	}
	return p - q;
}

#undef APPEND
#define APPEND(addlen, addstr) \
	{\
		char *r2;\
		unsigned int oldl = l;\
		\
		l += addlen;\
		r2 = realloc(res, l);\
		if (!r2) { free(res); return -1;}\
		res = r2;\
		memcpy(res + oldl, addstr, addlen);\
	}

#undef PARSEERR
#define PARSEERR	{free(res); return SPF_HARD_ERROR;}


/**
 * expand a SPF makro
 *
 * @param token the token to parse
 * @param domain the current domain string
 * @param ex if this is an exp string
 * @param result the resulting string is stored here
 * @return 0 on success, -1 on ENOMEM, SPF_{HARD,TEMP}_ERROR on problems
 *
 * not static, is called from targets/testspf.c
 */
int
spf_makro(char *token, const char *domain, int ex, char **result)
{
	char *res;
	char *p;
	unsigned int l;

	if (!(p = strchr(token, '%'))) {
		l = strlen(token) + 1;

		res = malloc(l);
		if (!res) {
			return -1;
		}
		memcpy(res, token, l);
	} else {
		l = p - token;

		res = malloc(l);
		if (!res) {
			return -1;
		}
		memcpy(res, token, l);
		do {
			char *oldp;
			int z;

			switch (*++p) {
				case '-':	APPEND(3, "%20");
						p++;
						break;
				case '_':	APPEND(1, " ");
						p++;
						break;
				case '%':	APPEND(1, "%");
						p++;
						break;
				case '{':	z = spf_makroletter(++p, domain, ex, &res, &l);
						if (z < 0) {
							return z;
						} else if (!z || (*(p + z) != '}')) {
							PARSEERR;
						}
						p += z + 1;
						break;
				default:	APPEND(1, "%");
						/* no p++ here! */
			}
			if (*p != '%') {
				oldp = p;
				p = strchr(p, '%');
				if (p) {
					APPEND(p - oldp, oldp);
				} else {
					APPEND(strlen(oldp) + 1, oldp);
				}
			}
		} while (p);
	}
	*result = res;
	return 0;
}

/**
 * parse the domainspec
 *
 * @param token pointer to the string after the token
 * @param dparam domain here the expanded domain string is stored (memory will be malloced)
 * @param ip4cidr the length of the IPv4 net (parsed if present in token, -1 if none given)
 * @param ip6cidr same for IPv6 net length
 * @returns:	 0 if everything is ok
 *		-1 on error (ENOMEM)
 *		SPF_TEMP_ERROR, SPF_HARD_ERROR
 */
static int
spf_domainspec(const char *domain, char *token, char **domainspec, int *ip4cidr, int *ip6cidr)
{
	*ip4cidr = -1;
	*ip6cidr = -1;
/* if there is nothing we don't need to do anything */
	*domainspec = NULL;
	if (!*token || WSPACE(*token)) {
		return 0;
/* search for a domain in token */
	} else if (*token != '/') {
		int i = 0;
		char *t = token;

		t++;
		while (*t && !WSPACE(*t) &&
				(((*t >='a') && (*t <='z')) || ((*t >='A') && (*t <='Z')) ||
				((*t >='0') && (*t <='9')) || (*t == '-') || (*t == '_') ||
				((*t == '%') && !i) || ((*t == '{') && (i == 1)) || (*t == '.') ||
				((i == 2) && ((*t == '}') || (*t == ',') || (*t == '+') ||
				(*t == '/') || (*t == '='))))) {
			t++;
			switch (*t) {
				case '%':	i = 1;
						break;
				case '{':	if (*(t - 1) != '%') {
							return SPF_HARD_ERROR;
						}
						i = 2;
						break;
				case '}':	i = 0;
			}
		}
		if (*t && (*t != '/') && !WSPACE(*t)) {
			return SPF_HARD_ERROR;
		}
		if (t != token) {
			char o;

			o = *t;
			*t = '\0';
			if ((i = spf_makro(token, domain, 0, domainspec))) {
				return i;
			}
			*t = o;
			token = t;
/* Maximum length of the domainspec is 255.
 * If it is longer remove subdomains from the left side until it is <255 bytes long. */
			if (strlen(*domainspec) > 255) {
				char *d = *domainspec;

				do {
					d = strchr(d, '.');
				} while (d && (strlen(d) > 255));
				if (!d) {
					free(*domainspec);
					return SPF_HARD_ERROR;
				} else {
					unsigned int l = strlen(d) + 1;
					char *nd = malloc(l);

					if (!nd)
						return -1;
					memcpy(nd, d, l);
					free(*domainspec);
					*domainspec = nd;
				}
			}
		}
	}
/* check if there is a cidr length given */
	if (*token == '/') {
		char *c = NULL;

		*ip4cidr = strtol(token + 1, &c, 10);
		if ((*ip4cidr < 8) || (*ip4cidr > 32) || (!WSPACE(*c) && (*c != '/'))) {
			return SPF_HARD_ERROR;
		}
		if (*c++ != '/') {
			*ip6cidr = -1;
		} else {
			if (*c++ != '/') {
				return SPF_HARD_ERROR;
			}
			*ip6cidr = strtol(c, &c, 10);
			if ((*ip6cidr < 8) || (*ip6cidr > 128) || !WSPACE(*c)) {
				return SPF_HARD_ERROR;
			}
		}
	} else if (!WSPACE(*token) && *token) {
		return SPF_HARD_ERROR;
	}
	return 0;
}
