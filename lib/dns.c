#include <resolv.h>
#include <errno.h>
#include <string.h>
#include "dns.h"

/**
 * ask_dnsmx - get info out of the DNS
 *
 * @name: the name to look up
 * @ips: first element of a list of results will be placed
 *
 * returns: 0 on success
 *          1 if host is not existent
 *          2 if temporary DNS error
 *          3 if permanent DNS error
 *         -1 on error
 */
int
ask_dnsmx(const char *name, struct ips **result)
{
	int i;
	char *r;
	unsigned int l;

	i = dnsmx(&r, &l, name);

	if (!i) {
		char *s = r;
		struct ips **q = result;

		/* there is no MX record, so we look for an AAAA record */
		if (!l)
			return ask_dnsa(name, result);

		while (r + l > s) {
			struct ips *p;
			int pri, rc;

			rc = ask_dnsa(r + 2, &p);
			if (rc < 0) {
				freeips(*result);
				if (errno == ENOMEM)
					return -1;
				return 2;
			} else if (!rc) {
				pri = (*s << 8) + *(s + 1);
				*q = p;
				do {
					p->priority = pri;
					p = p->next;
				} while (p);
				q = &(p->next);
				s += 2 + strlen(s + 2);
			}
		}
		free(r);
		return 0;
	}
	switch (errno) {
		case EAGAIN:	return 2;
		case ENFILE:
		case EMFILE:
		case ENOBUFS:	errno = ENOMEM;
		case ENOMEM:	return -1;
		default:	return 3;
	}
}

/**
 * ask_dnsa - get AAAA record from of the DNS
 *
 * @name: the name to look up
 * @ips: first element of a list of results will be placed
 *
 * returns: 0 on success
 *          1 if host is not existent
 *          2 if temporary DNS error
 *          3 if permanent DNS error
 *         -1 on error
 */
int
ask_dnsa(const char *name, struct ips **result)
{
	int i;
	char *r;
	unsigned int l;

	i = dnsip6(&r, &l, name);
	if (!i) {
		char *s = r;
		struct ips **q = result;

		if (!l) {
			*result = NULL;
			return 1;
		}
		while (r + l > s) {
			struct ips *p = malloc(sizeof(*p));

			if (!p) {
				errno = ENOMEM;
				freeips(*result);
				return -1;
			}
			*q = p;
			p->next = NULL;
			memcpy(&(p->addr), s, 16);

			q = &(p->next);
			s += 16;
		}
		return 0;
	}
	switch (errno) {
		case EAGAIN:	return 2;
		case ENFILE:
		case EMFILE:
		case ENOBUFS:	errno = ENOMEM;
		case ENOMEM:	return -1;
		default:	return 3;
	}
}

/**
 * domainvalid - check if a string is a valid fqdn
 *
 * @host:     the name to check
 * @dnsask:   lookup the name in DNS or not
 *
 * returns:  0 if everything is ok 
 *           1 on syntax error
 *
 * if there is a standard function doing the same throw this one away
 */
int
domainvalid(const char *host, const int ignored __attribute__ ((unused)))
{
	int dot = 0;	/* if there is a '.' in the address */
	const char *h = host;

	if (!*host)
		return 1;
	while (*host) {
		if (!(((*host >= 'a') && (*host <= 'z')) ||
				((*host >= 'A') && (*host <= 'Z')) ||
				(*host == '.') || (*host == '-') ||
				((*host >= '0') && (*host <= '9'))))
			return 1;
		if (*host == '.') {
			if (*(host+1) == '.')
				return 1;
			dot = 1;
		}
		*host++;
	}
	if ((host - h) > 255)
		return 1;
	return 1 - dot;
}

void
freeips(struct ips *p)
{
	while (p) {
		struct ips *thisip = p;

		p = thisip->next;
		free(thisip);
	}
}
