/** \file dns.c
 \brief DNS query functions
 */
#include <errno.h>
#include <string.h>
#include "dns.h"
#include "libowfatconn.h"

/**
 * get info out of the DNS
 *
 * @param name the name to look up
 * @param result first element of a list of results will be placed
 * @return  0 on success
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
	unsigned int l = 0;

	i = dnsmx(&r, &l, name);

	if (!i || ((i < 0) && (errno == ENOENT)) ) {
		char *s = r;
		struct ips **q = result;
		int errtype = 0;

		/* there is no MX record, so we look for an AAAA record */
		if (!l) {
			int rc = ask_dnsaaaa(name, result);

			if (!rc) {
				struct ips *a = *result;

				while (a) {
					/* the DNS priority is 2 bytes long so 65536 can
					   never be returned from a real DNS_MX lookup */
					a->priority = 65536;
					a = a->next;
				}
			}
			return rc;
		}

		while (r + l > s) {
			struct ips *p;
			int rc;

			rc = ask_dnsaaaa(s + 2, &p);
			if (rc < 0) {
				freeips(*result);
				free(r);
				if (errno == ENOMEM)
					return -1;
				return 2;
			} else if (!rc) {
				struct ips *u;
				unsigned int pri = ntohs(*((unsigned short *) s));

				/* add the new results to the list */
				*q = p;
				/* set priority for each of the new entries */
				for (u = p; u; u = p->next) {
					u->priority = pri;
					p = u;
				}
				q = &(p->next);
			} else {
				errtype = (1 << rc);
			}
			s += 3 + strlen(s + 2);
		}
		free(r);
		if (!*result) {
			if (errtype & 4) {
				return 2;
			} else if (errtype & 2) {
				return 1;
			} else {
				return 3;
			}
		}
		return 0;
	}
	switch (errno) {
		case ETIMEDOUT:
		case EAGAIN:	return 2;
		case ENFILE:
		case EMFILE:
		case ENOBUFS:	errno = ENOMEM;
		case ENOMEM:	return -1;
		case ENOENT:	return 1;
		default:	return 3;
	}
}

/**
 * get AAAA record from of the DNS
 *
 * @param name the name to look up
 * @param result first element of a list of results will be placed
 * @return  0 on success
 *          1 if host is not existent
 *          2 if temporary DNS error
 *          3 if permanent DNS error
 *         -1 on error
 */
int
ask_dnsaaaa(const char *name, struct ips **result)
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
				freeips(*result);
				free(r);
				return -1;
			}
			*q = p;
			p->next = NULL;
			memcpy(&(p->addr), s, 16);

			q = &(p->next);
			s += 16;
		}
		free(r);
		return 0;
	}
	free(r);
	switch (errno) {
		case ETIMEDOUT:
		case EAGAIN:	return 2;
		case ENFILE:
		case EMFILE:
		case ENOBUFS:	errno = ENOMEM;
		case ENOMEM:	return -1;
		case ENOENT:	return 1;
		default:	return 3;
	}
}

/**
 * get A record from of the DNS
 *
 * @param name the name to look up
 * @param result first element of a list of results will be placed, or NULL if only return code is of interest
 * @return  0 on success
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

	i = dnsip4(&r, &l, name);
	if (!i) {
		if (result) {
			char *s = r;
			struct ips **q = result;

			if (!l) {
				*result = NULL;
				return 1;
			}
			while (r + l > s) {
				struct ips *p = malloc(sizeof(*p));

				if (!p) {
					freeips(*result);
					free(r);
					return -1;
				}
				*q = p;
				p->next = NULL;
				p->addr.s6_addr32[0] = 0;
				p->addr.s6_addr32[1] = 0;
				p->addr.s6_addr32[2] = htonl(0xffff);
				memcpy(&(p->addr.s6_addr32[3]), s, 4);

				q = &(p->next);
				s += 4;
			}
		}
		free(r);
		return l ? 0 : 1;
	}
	switch (errno) {
		case ETIMEDOUT:
		case EAGAIN:	return 2;
		case ENFILE:
		case EMFILE:
		case ENOBUFS:	errno = ENOMEM;
		case ENOMEM:	return -1;
		case ENOENT:	return 1;
		default:	return 3;
	}
}

/**
 * check if a string is a valid fqdn
 *
 * @param host the name to check
 * @return 0 if everything is ok 
 *         1 on syntax error
 *
 * if there is a standard function doing the same throw this one away
 */
int
domainvalid(const char *host)
{
	int dot = 0;	/* if there is a '.' in the address */
	const char *h = host;

	if (!*host || (*host == '.'))
		return 1;
	while (*host) {
		if (!((*host >= 'a') && (*host <= 'z')) && !((*host >= 'A') && (*host <= 'Z'))  &&
			 (*host != '.') && (*host != '-') && !((*host >= '0') && (*host <= '9'))) {
			 return 1;
		}
		if (*host == '.') {
			*host++;
			dot = 1;
			if (*host == '.')
				return 1;
			continue;
		}
		*host++;
	}
	if (((host - h) > 255) || ((host - h) < 5))
		return 1;
	/* there is no top level domain ending with something different from a letter */
	host--;
	if (!(((*host >= 'a') && (*host <= 'z')) || ((*host >= 'A') && (*host <= 'Z'))))
		return 1;
	return 1 - dot;
}

/**
 * get host name for IP address
 *
 * @param ip the IP to look up
 * @param result name will be stored here
 * @return  0 on success
 *          1 if host is not existent
 *          2 if temporary DNS error
 *          3 if permanent DNS error
 *         -1 on error
 */
int
ask_dnsname(const struct in6_addr *ip, char **result)
{
	int r;

	r = dnsname(result, ip->s6_addr);
	if (!r)
		return *result ? 0 : 1;
	switch (errno) {
		case ETIMEDOUT:
		case EAGAIN:	return 2;
		case ENFILE:
		case EMFILE:
		case ENOBUFS:	errno = ENOMEM;
		case ENOMEM:	return -1;
		case ENOENT:	return 1;
		default:	return 3;
	}
}

/**
 * free memory of IP list
 *
 * @param p IP list to free
 */
void
freeips(struct ips *p)
{
	while (p) {
		struct ips *thisip = p;

		p = thisip->next;
		free(thisip);
	}
}

/**
 * sort MX list by priority
 *
 * @param p list of MX entries
 */
void
sortmx(struct ips **p)
{
	struct ips *next, *res = NULL;

	/* make us live easy: copy first entry */
	res = *p;
	next = (*p)->next;
	(*p)->next = NULL;
	*p = next;

	while (next) {
		struct ips *this = res;
		struct ips *tmp = next->next;

		if (res->priority > next->priority) {
			next->next = res;
			res = next;
		} else {
			while (this->next && (this->next->priority <= next->priority)) {
				this = this->next;
			}
			tmp = next->next;
			next->next = this->next;
			this->next = next;
		}
		next = tmp;
	}

	*p = res;
}
