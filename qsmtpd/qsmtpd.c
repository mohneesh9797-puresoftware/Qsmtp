#include <sys/types.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <dirent.h>
#include <time.h>
#include <resolv.h>
#include "antispam.h"
#include "log.h"
#include "netio.h"
#include "dns.h"
#include "control.h"
#include "usercallback.h"
#include "tls.h"
#include "sstring.h"
#include "qsmtpd.h"
#include "version.h"
#include "dns.h"
#include "vpopmail.h"

int smtp_noop(void);
int smtp_quit(void);
int smtp_rset(void);
int smtp_helo(void);
int smtp_ehlo(void);
int smtp_from(void);
int smtp_rcpt(void);
extern int smtp_data(void);
int smtp_vrfy(void);
extern int smtp_auth(void);
extern int smtp_starttls(void);

#define _C(c,l,m,f,s,o) { .name = c, .len = l, .mask = m, .func = f, .state = s, .flags = o }

struct smtpcomm commands[] = {
	_C("NOOP",	 4, 0xffff, smtp_noop,     -1, 0),  /* 0x001 */
	_C("QUIT",	 4, 0xfffd, smtp_quit,      0, 0),  /* 0x002 */
	_C("RSET",	 4, 0xfffd, smtp_rset,    0x1, 0),  /* 0x004 */ /* the status to change to is set in smtp_rset */
	_C("HELO",	 4, 0xfffd, smtp_helo,      0, 1),  /* 0x008 */
	_C("EHLO",	 4, 0xfffd, smtp_ehlo,      0, 1),  /* 0x010 */
	_C("MAIL FROM:",10, 0x0018, smtp_from,      0, 3),  /* 0x020 */
	_C("RCPT TO:",	 8, 0x0060, smtp_rcpt,      0, 1),  /* 0x040 */
	_C("DATA",	 4, 0x0040, smtp_data,   0x10, 0),  /* 0x080 */ /* the status to change to is changed in smtp_data */
	_C("STARTTLS",	 8, 0x0010, smtp_starttls, -1, 0),  /* 0x100 */
	_C("AUTH",	 4, 0x0010, smtp_auth,     -1, 1),  /* 0x200 */
	_C("VRFY",	 4, 0xffff, smtp_vrfy,     -1, 0)   /* 0x400 */
};

#undef _C

static char *rcpth;			/* string of rcpthosts */
static char **rcpthosts;		/* array of hosts to accept mail for */
static unsigned long sslauth;		/* if SMTP AUTH is only allowed after STARTTLS */
static char *vpopbounce;		/* the bounce command in vpopmails .qmail-default */
static unsigned int rcptcount;		/* number of recipients in lists including rejected */
static char *gcbuf;			/* buffer for globalconf array (see below) */
static int relayclient;			/* flag if this client is allowed to relay by IP: 0 unchecked, 1 allowed, 2 denied */

unsigned long databytes;		/* maximum message size */
unsigned int goodrcpt;			/* number of valid recipients */
int badbounce;				/* bounce message with more than one recipient */
struct xmitstat xmitstat;		/* This contains some flags describing the transmission and it's status. */
char *protocol;				/* the protocol string to use (e.g. "ESMTP") */
char *auth_host;			/* hostname for auth */
char *auth_check;			/* checkpassword or one of his friends for auth */
char **auth_sub;			/* subprogram to be invoked by auth_check (usually /bin/true) */
char **globalconf;			/* see usercallback.h */
string heloname;			/* the fqdn to show in helo */

static long comstate = 0x001;		/* status of the command state machine, initialized to noop */

struct tailhead *headp;			/* List head. */
struct recip *thisrecip;

static int badcmds = 0;			/* bad commands in a row */

#define MAXBADCMDS	5		/* maximum number of illegal commands in a row */
#define MAXRCPT		500		/* maximum number of recipients in a single mail */

static inline int
err_badbounce(void)
{
	tarpit();
	return netwrite("550 5.5.3 bounce messages must not have more than one recipient\r\n");
}

int
err_control(const char *fn)
{
	const char *logmsg[] = {"error: unable to open file: \"", fn, "\"\n", NULL};

	log_writen(LOG_ERR, logmsg);
	return netwrite("421 4.3.5 unable to read controls\r\n");
}

static int
err_control2(const char *msg, const char *fn)
{
	const char *logmsg[] = {"error: unable to open file: ", msg, fn, "\n", NULL};

	log_writen(LOG_ERR, logmsg);
	return netwrite("421 4.3.5 unable to read controls\r\n");
}

static int
setup(void)
{
	int j;
	struct sigaction sa;

#ifdef USESYSLOG
	openlog("Qsmtpd", LOG_PID, LOG_MAIL);
#endif

	if (chdir(AUTOQMAIL)) {
		log_write(LOG_ERR, "cannot chdir to qmail directory");
		return EINVAL;
	}

	if ( ( j = loadoneliner("control/me", &heloname.s, 0) ) < 0 )
		return errno;
	heloname.len = j;
	/* we ignore the other DNS errors here, the rest is fault of the admin */
	if (domainvalid(heloname.s, 0) == 1) {
		log_write(LOG_ERR, "control/me contains invalid name");
		return EINVAL;
	}

	if ( (j = loadlistfd(open("control/rcpthosts", O_RDONLY), &rcpth, &rcpthosts, domainvalid, 0))) {
		if (errno == ENOENT)
			log_write(LOG_ERR, "control/rcpthosts not found");
		return errno;
	}
	if (!rcpthosts[0]) {
		log_write(LOG_ERR, "found no valid names in control/rcpthosts");
		return 1;
	}
	xmitstat.remoteip = getenv("TCP6REMOTEIP");
	if (!xmitstat.remoteip || !*xmitstat.remoteip) {
		xmitstat.remoteip = "unknown";
		memset(xmitstat.sremoteip.s6_addr, 0, sizeof(xmitstat.sremoteip));
	} else {
		/* is this just too paranoid? */
		if (inet_pton(AF_INET6, xmitstat.remoteip, &(xmitstat.sremoteip)) <= 0) {
			xmitstat.remoteip = "unknown";
			log_write(LOG_ERR, "TCP6REMOTEIP does not contain a valid AF_INET6 addres");
			memset(xmitstat.sremoteip.s6_addr, 0, sizeof(xmitstat.sremoteip));
		} else {
			xmitstat.ipv4conn = IN6_IS_ADDR_V4MAPPED(xmitstat.sremoteip.s6_addr) ? 1 : 0;
		}
	}
	xmitstat.remotehost.s = getenv("TCPREMOTEHOST");
	if (xmitstat.remotehost.s)
		xmitstat.remotehost.len = strlen(xmitstat.remotehost.s);
	else
		xmitstat.remotehost.len = 0;
	xmitstat.remoteinfo = getenv("TCPREMOTEINFO");

	/* RfC 2821, section 4.5.3.2: "Timeouts"
	 * An SMTP server SHOULD have a timeout of at least 5 minutes while it
	 * is awaiting the next command from the sender. */
	if ( ( j = loadintfd(open("control/timeoutsmtpd", O_RDONLY), &timeout, 320) ) ) {
		int e = errno;
		log_write(LOG_ERR, "parse error in control/timeoutsmtpd");
		return e;
	}
	if ( ( j = loadintfd(open("control/databytes", O_RDONLY), &databytes, 0) ) ) {
		int e = errno;
		log_write(LOG_ERR, "parse error in control/databytes");
		return e;
	}
	if ( (j = loadintfd(open("control/forcesslauth", O_RDONLY), &sslauth, 0)) ) {
		int e = errno;
		log_write(LOG_ERR, "parse error in control/forcesslauth");
		return e;
	}

	if ( (j = loadlistfd(open("control/filterconf", O_RDONLY), &gcbuf, &globalconf, NULL, 0)) ) {
		if (errno == ENOENT) {
			gcbuf = NULL;
			globalconf = NULL;
		} else {
			log_write(LOG_ERR, "error opening control/filterconf");
			return errno;
		}
	}

	if ( (j = lloadfilefd(open("control/vpopbounce", O_RDONLY), &vpopbounce, 0)) < 0) {
		int e = errno;
		err_control("control/vpopbounce");
		return e;
	}

	/* block sigpipe. If we don't we can't handle the errors in smtp_data correctly and remote host
	 * will see a connection drop on error (which is bad and violates RfC) */
	sa.sa_handler = SIG_IGN;
	j = sigaction(SIGPIPE, &sa, NULL);
	relayclient = 0;

	return j;
}

/**
 * freedata - free all ressources allocated for mail transaction
 */
void
freedata(void)
{
	free(xmitstat.mailfrom.s);
	STREMPTY(xmitstat.mailfrom);
	freeips(xmitstat.frommx);
	xmitstat.frommx = NULL;
	while (head.tqh_first != NULL) {
		struct recip *l = head.tqh_first;

		TAILQ_REMOVE(&head, head.tqh_first, entries);
		free(l->to.s);
		free(l);
	}
	rcptcount = 0;
	goodrcpt = 0;
	badbounce = 0;
}

int
smtp_helo(void)
{
	const char *s[] = {"250 ", heloname.s, NULL};

	freedata();
	protocol = realloc(protocol, 5);
	if (!protocol)
		return ENOMEM;
	memcpy(protocol, "SMTP", 5);
	xmitstat.esmtp = 0;
	xmitstat.spf = 0;
	xmitstat.datatype = 0;
	if (helovalid(linein + 5) < 0)
		return errno;
	return net_writen(s) ? errno : 0;
}

int
smtp_ehlo(void)
{
	/* can this be self-growing? */
	const char *msg[] = {"250-", heloname.s, "\r\n250-ENHANCEDSTATUSCODES\r\n250-PIPELINING\r\n250-8BITMIME\r\n",
				NULL, NULL, NULL, NULL, NULL};
	unsigned int next = 3;	/* next index in to be used */
	char *sizebuf = NULL;
	int rc;

	if (!ssl) {
		protocol = realloc(protocol, 6);
		if (!protocol)
			return ENOMEM;
		memcpy(protocol, "ESMTP", 6);	/* also copy trailing '\0' */
	}
	if (helovalid(linein + 5) < 0)
		return errno;
	if (auth_host && (!sslauth || (sslauth && ssl))) {
#ifdef AUTHCRAM
		msg[next++] = "250-AUTH PLAIN LOGIN CRAMMD5\r\n";
#else
		msg[next++] = "250-AUTH PLAIN LOGIN\r\n";
#endif
	}
/* check if STARTTLS should be announced. Don't announce if already in SSL-Mode or if certificate can't be opened */
	if (!ssl) {
		int fd = open("control/servercert.pem", O_RDONLY);

		if (fd >= 0) {
			while (close(fd) && (errno == EINTR));
			msg[next++] = "250-STARTTLS\r\n";
		}
	}

/* this must stay last: it begins with "250 " and does not have "\r\n" at the end so net_writen works */
	if (databytes) {
		msg[next++] = "250 SIZE ";
		sizebuf = ultostr(databytes);
		if (!sizebuf)
			return -1;
		msg[next] = sizebuf;
	} else {
		msg[next] = "250 SIZE";
	}
	rc = (net_writen(msg) < 0) ? errno : 0;
	free(sizebuf);
	xmitstat.spf = 0;
	xmitstat.esmtp = 1;
	xmitstat.datatype = 1;
	return rc;
}

/* values for default

  (def & 1)		append "default"
  (def & 2)		append suff1
 */

static int
qmexists(const string *dirtempl, const char *suff1, const unsigned int len, const int def)
{
	char filetmp[PATH_MAX];
	int fd;
	unsigned int l = dirtempl->len;

	if (l >= PATH_MAX)
		return -1;
	memcpy(filetmp, dirtempl->s, l);
	if (def & 2) {
		char *p;

		if (l + len >= PATH_MAX)
			return -1;
		memcpy(filetmp + l, suff1, len);

		while ( (p = strchr(filetmp + l, '.')) ) {
			*p = ':';
		}
		l += len;
		if (def & 1) {
			if (l + 1 >= PATH_MAX)
				return -1;
			*(filetmp + l) = '-';
			l++;
		}
	}
	if (def & 1) {
		if (l + 7 >= PATH_MAX)
			return -1;
		memcpy(filetmp + l, "default", 7);
		l += 7;
	}
	filetmp[l] = 0;

	fd = open(filetmp, O_RDONLY);
	if (fd == -1)
		if (errno != ENOENT)
			err_control(filetmp);
	return fd;
}

/* Return codes:

  0: user doesn't exist
  1: user exists
  2: mail would be catched by .qmail-default and .qmail-default != vpopbounce
  3: domain is not filtered (use for domains not local)
  4: mail would be catched by .qmail-foo-default (i.e. mailinglist)
  -1: error, errno is set.
*/
static int
user_exists(const string *localpart, struct userconf *ds)
{
	char filetmp[PATH_MAX];
	DIR *dirp;
	unsigned int i = 0;

	memcpy(filetmp, ds->userpath.s, ds->userpath.len);
	filetmp[ds->userpath.len] = '\0';

	/* does directory (ds->domainpath.s)+'/'+localpart exist? */
	dirp = opendir(filetmp);
	if (dirp == NULL) {
		int e = errno;
		int fd;
		string dotqm;

		free(ds->userpath.s);
		ds->userpath.s = NULL;
		ds->userpath.len = 0;
		/* does USERPATH/DOMAIN/.qmail-LOCALPART exist? */
		if (e != ENOENT) {
			if (!err_control(filetmp))
				errno = e;
			return -1;
		}
		i = ds->domainpath.len;
		memcpy(filetmp, ds->domainpath.s, i);
		memcpy(filetmp + i, ".qmail-", 7);
		i += 7;
		filetmp[i] = '\0';
		if ( (fd = newstr(&dotqm, i + 1)) ) {
			return fd;
		}
		memcpy(dotqm.s, filetmp, dotqm.len--);
		fd = qmexists(&dotqm, localpart->s, localpart->len, 2);
		/* try .qmail-user-default instead */
		if (fd < 0) {
			if (errno != ENOENT)
				return fd;
			fd = qmexists(&dotqm, localpart->s, localpart->len, 3);
		}

		if (fd < 0) {
			char *p;
			/* if username contains '-' there may be
			  .qmail-partofusername-default */
			if (errno != ENOENT) {
				free(dotqm.s);
				return fd;
			}
			p = strchr(localpart->s, '-');
			while (p) {
				fd = qmexists(&dotqm, localpart->s, (p - localpart->s), 3);
				if (fd < 0) {
					if (errno != ENOENT) {
						free(dotqm.s);
						return fd;
					}
				} else {
					while (close(fd)) {
						if (errno != EINTR)
							return -1;
					}
					free(dotqm.s);
					return 4;
				}
				p = strchr(p + 1, '-');
			}

			/* does USERPATH/DOMAIN/.qmail-default exist ? */
			fd = qmexists(&dotqm, NULL, 0, 1);
			free(dotqm.s);
			if (fd < 0) {
				/* no local user with that address */
				return (errno == ENOENT) ? 0 : fd;
			} else if (vpopbounce) {
				char buff[2*strlen(vpopbounce)+1];
				int r;

				r = read(fd, buff, sizeof(buff) - 1);
				if (r == -1) {
					e = errno;
					if (!err_control(filetmp))
						errno = e;
					return -1;
				}
				while (close(fd)) {
					if (errno != EINTR)
						return -1;
				}
				buff[r] = 0;

				/* mail would be bounced or catched by .qmail-default */
				return strcmp(buff, vpopbounce) ? 2 : 0;
			} else {
				/* we can't tell if this is a bounce .qmail-default -> accept the mail */
				return 2;
			}
		} else {
			free(dotqm.s);
			while (close(fd)) {
				if (errno != EINTR)
					return -1;
			}
		}
	} else {
		closedir(dirp);
	}
	return 1;
}

/**
 * addrparse - check an email address for syntax errors and/or existence
 *
 * @flags:   1: rcpt to checks (e.g. source route is allowed), 0: mail from checks
 * @addr:    struct string to contain the address (memory will be malloced)
 * @more:    here starts the data behind the first > behind the first < (or 0 if none)
 * @ds:      store the userconf of the user here
 *
 * returns: 0 on success, >0 on error (e.g. ENOMEM), -2 if address not local
 *          (this is of course no error condition for MAIL FROM), -1 if
 *          address local but nonexistent (expired or most probably faked) _OR_ if
 *          domain of address does not exist (in both cases error is sent to network
 *          before leaving)
 */
static int
addrparse(const int flags, string *addr, char **more, struct userconf *ds)
{
	char *at;			/* guess! ;) */
	int result = 0;			/* return code */
	int i = 0, j;
	string localpart;
	unsigned int le;

	STREMPTY(ds->domainpath);
	STREMPTY(ds->userpath);

	if (addrsyntax(linein, flags, addr, more))
		return netwrite("501 5.1.3 domain of mail address syntactically incorrect\r\n") ? errno : EDONE;

	/* empty mail address is valid in MAIL FROM:, this is checked by addrsyntax before
	 * if we find an empty address here it's ok */
	if (!addr->s)
		return 0;
	at = strchr(addr->s, '@');
	/* check if mail goes to global postmaster */
	if (flags && !at)
		return 0;

	/* at this point either @ is set or addrsyntax has already caught this */
	while (rcpthosts[i]) {
		if (!strcasecmp(rcpthosts[i], at + 1))
			break;
		i++;
	}
	if (!rcpthosts[i]) {
		int rc = finddomainmm(open("control/morercpthosts", O_RDONLY), at + 1);

		if (rc < 0) {
			if (errno == ENOMEM) {
				result = errno;
			} else if (err_control("control/morercpthosts")) {
				result = errno;
			} else {
				result = EDONE;
			}
			goto free_and_out;
		} else if (!rc) {
			return -2;
		}

	}

/* get the domain directory from "users/cdb" */
	j = vget_assign(at + 1, &(ds->domainpath));
	if (j < 0) {
		if (errno == ENOENT)
			return 0;
		result = errno;
		goto free_and_out;
	} else if (!j) {
		/* the domain is not local (or at least no vpopmail domain) so we can't say it the user exists or not:
		 * -> accept the mail */
		return 0;
	}

/* get the localpart out of the RCPT TO */
	le = (at - addr->s);
	if ( (j = newstr(&localpart, le + 1) ) ) {
		result = errno;
		goto free_and_out;
	}
	memcpy(localpart.s, addr->s, le);
	localpart.s[--localpart.len] = '\0';
/* now the userpath : userpatth.s = domainpath.s + [localpart of RCPT TO] + '/' */
	if ( (j = newstr(&(ds->userpath), ds->domainpath.len + 2 + localpart.len ) ) ) {
		result = errno;
		goto free_and_out;
	}
	memcpy(ds->userpath.s, ds->domainpath.s, ds->domainpath.len);
	memcpy(ds->userpath.s + ds->domainpath.len, localpart.s, localpart.len);
	ds->userpath.s[--ds->userpath.len - 1] = '\0';
	ds->userpath.s[ds->userpath.len - 1] = '/';

	j = user_exists(&localpart, ds);
	free(localpart.s);
	if (j < 0) {
		result = errno;
		goto free_and_out;
	}

	if (!j) {
		const char *frommsg[] = {"550 5.1.0 sending user <", addr->s,
				"> faked, I will not accept this mail", NULL};
		const char *rcptmsg[] = {"550 5.1.1 no such user <", addr->s, ">", NULL};

		tarpit();
		result = net_writen((flags == 1) ? rcptmsg : frommsg) ? errno : -1;
		goto free_and_out;
	}
	return 0;
free_and_out:
	free(ds->domainpath.s);
	STREMPTY(ds->domainpath);
	free(ds->userpath.s);
	STREMPTY(ds->userpath);
	free(addr->s);
	return result;
}

int
smtp_rcpt(void)
{
	struct recip *r;
	int i = 0, j, e;
	string tmp;
	char *more = NULL;
	struct userconf ds;
	char *errmsg;
	char *ucbuf, *dcbuf;	/* buffer for user and domain "filterconf" file */
	int bt;			/* which policy matched */
	const char *logmsg[] = {"rejected message to <", NULL, "> from <", xmitstat.mailfrom.s,
					"> from IP [", xmitstat.remoteip, "] {", NULL, ", ", NULL, " policy}", NULL};
	const char *okmsg[] = {"250 2.1.0 recipient <", NULL, "> OK", NULL};

	i = addrparse(1, &tmp, &more, &ds);
	if  (i > 0) {
		return i;
	} else if (i == -1) {
		return EBOGUS;
	} else if ((i == -2) && !xmitstat.authname.len && !xmitstat.tlsclient) {
/* check if client is allowed to relay by IP */
		if (!relayclient) {
			int fd;
			const char *fn = xmitstat.ipv4conn ? "control/relayclients" : "control/relayclients6";

			relayclient = 2;
			if ( (fd = open(fn, O_RDONLY)) < 0) {
				if (errno != ENOENT) {
					return err_control(fn) ? errno : EDONE;
				}
			} else {
				int ipbl;

				if ((ipbl = lookupipbl(fd)) < 0) {
					const char *logmess[] = {"parse error in ", fn, NULL};

					/* reject everything on parse error, else this
					 * would turn into an open relay by accident */
					log_writen(LOG_ERR, logmess);
				} else if (ipbl) {
					relayclient = 1;
				}
			 }
		}
		if (relayclient & 2) {
			const char *logmess[] = {"rejected message to <", tmp.s, "> from <", xmitstat.mailfrom.s,
							"> from IP [", xmitstat.remoteip, "] {relaying denied}", NULL};

			log_writen(LOG_INFO, logmess);
			free(tmp.s);
			free(ds.userpath.s);
			free(ds.domainpath.s);
			tarpit();
			return netwrite("551 5.7.1 relaying denied\r\n") ? errno : EBOGUS;
		}
	}
	/* we do not support any ESMTP extensions adding data behind the RCPT TO (now)
	 * so any data behind the '>' is a bug in the client */
	if (more) {
		free(ds.userpath.s);
		free(ds.domainpath.s);
		free(tmp.s);
		return EINVAL;
	}
	if (rcptcount >= MAXRCPT) {
		free(ds.userpath.s);
		free(ds.domainpath.s);
		free(tmp.s);
		if (netwrite("452 4.5.3 Too many recipients"))
			return errno;
		return EDONE;
	}
	r = malloc(sizeof(*r));
	if (!r) {
		free(ds.userpath.s);
		free(ds.domainpath.s);
		free(tmp.s);
		return ENOMEM;
	}
	r->to.s = tmp.s;
	r->to.len = tmp.len;
	r->ok = 0;	/* user will be rejected until we change this explicitely */
	thisrecip = r;
	TAILQ_INSERT_TAIL(&head, r, entries);
	rcptcount++;

/* load user and domain "filterconf" file */
	if ((j = loadlistfd(getfile(&ds, "filterconf", &i), &ucbuf, &(ds.userconf), NULL, 0)) < 0) {
		if (errno == ENOENT) {
			ds.userconf = NULL;
			ds.domainconf = NULL;
			dcbuf = NULL;
			/* ucbuf is already set to NULL by lloadfilefd, called from loadlistfd */
		} else {
			e = errno;

			free(ds.userpath.s);
			free(ds.domainpath.s);
			return err_control2("user/domain filterconf for ", r->to.s) ? errno : e;
		}
	} else {
		if (i) {
			ds.domainconf = ds.userconf;
			ds.userconf = NULL;
			dcbuf = NULL;	/* no matter which buffer we use */
		} else {
			unsigned int l;

			/* make sure this one opens the domain file: just set user path length to 0 */
			l = ds.userpath.len;
			ds.userpath.len = 0;
			if ( (j = loadlistfd(getfile(&ds, "filterconf", &j), &dcbuf, &(ds.domainconf), NULL, 0)) ) {
				if (errno == ENOENT) {
					ds.domainconf = NULL;
				} else {
					e = errno;

					free(ucbuf);
					free(ds.userconf);
					free(ds.userpath.s);
					free(ds.domainpath.s);
					return err_control2("domain filterconf for ", r->to.s) ? errno : e;
				}
			}
			ds.userpath.len = l;
		}
	}

	i = j = 0;
	while (rcpt_cbs[j]) {
		errmsg = NULL;
		if ( ( i = rcpt_cbs[j](&ds, &errmsg, &bt)) ) {
			if (i == 5)
				break;

			if (i == 4) {
				int t;

				if (getsetting(&ds, "fail_hard_on_temp", &t))
					i = 1;
			}
			if (i == 1) {
				int t;

				if (getsetting(&ds, "nonexist_on_block", &t))
					i = 3;
			}

			break;
		}
		j++;
	}
	free(ds.userpath.s);
	free(ds.domainpath.s);
	free(ucbuf);
	free(dcbuf);
	free(ds.userconf);
	free(ds.domainconf);
	if (i && (i != 5))
		goto userdenied;

	if (comstate != 0x20) {
		if (!xmitstat.mailfrom.len) {
			const char *logmess[] = {"rejected message to <", NULL, "> from IP [", xmitstat.remoteip,
							"] {bad bounce}", NULL};
			if (err_badbounce() < 0)
				return errno;
			if (!badbounce) {
				/* there are only two recipients in list until now */
				struct recip *l = head.tqh_first;

				logmess[1] = l->to.s;
				log_writen(LOG_INFO, logmess);
				TAILQ_REMOVE(&head, head.tqh_first, entries);
				badbounce = 1;
				l->ok = 0;
			}
			logmess[1] = r->to.s;
			log_writen(LOG_INFO, logmess);
			free(ds.userpath.s);
			free(ds.domainpath.s);
			goodrcpt = 0;
			rcptcount = 0;
			return EBOGUS;
		}
	}
	goodrcpt++;
	r->ok = 1;
	okmsg[1] = r->to.s;

	return net_writen(okmsg) ? errno : 0;
userdenied:
	e = errno;
	if (errmsg && (i > 0)) {
		logmsg[7] = errmsg;
		logmsg[9] = blocktype[bt];
		logmsg[1] = r->to.s;
		if (!xmitstat.mailfrom.len)
			logmsg[3] = "";
		log_writen(LOG_INFO, logmsg);
	}
	switch (i) {
		case -1:j = 1; break;
		case 2:	tarpit();
			if ( (j = netwrite("550 5.7.1 mail denied for policy reasons\r\n")) )
				e = errno;
			break;
		case 3:	if (42 == 42) {
				/* this is _so_ ugly. I just want a local variable for this case */
				const char *rcptmsg[] = {"550 5.1.1 no such user <", r->to.s, ">", NULL};

				tarpit();
				if ((j = net_writen(rcptmsg)))
					e = errno;
			}
			break;
		case 4:	tarpit();
			if ( (j = netwrite("450 4.7.0 mail temporary denied for policy reasons\r\n")) )
				e = errno;
			break;
	}
	return j ? e : EDONE;
}

int
smtp_from(void)
{
	int i = 0;
	char *more = NULL;
	/* this is the maximum allowed length of the command line. Since every extension
	 * may raise this we use this variable. Every successfully used command extension
	 * will raise this counter by the value defined in the corresponding RfC.
	 * The limit is defined to 512 characters including CRLF (which we do not count)
	 * in RfC 2821, section 4.5.3.1 */
	unsigned int validlength = 510;
	int seenbody = 0;	/* if we found a "BODY=" after mail, there may only be one */
	struct userconf ds;
	struct statvfs sbuf;
	const char *okmsg[] = {"250 2.1.5 sender <", NULL, "> syntactically correct", NULL};

	i = addrparse(0, &(xmitstat.mailfrom), &more, &ds);
	xmitstat.frommx = NULL;
	xmitstat.fromdomain = 0;
	free(ds.userpath.s);
	free(ds.domainpath.s);
	if (i > 0)
		return i;
	else if (i == -1)
		return EBOGUS;
	xmitstat.thisbytes = 0;
	/* data behind the <..> is only allowed in ESMTP */
	if (more && !xmitstat.esmtp)
		return EINVAL;
	while (more && *more) {
		if (!strncasecmp(more, " SIZE=", 6)) {
			char *sizenum = more + 6;

			/* this is only set if we found SIZE before; there should only be one */
			if (xmitstat.thisbytes)
				return EINVAL;
			if ((*sizenum >= '0') && (*sizenum <= '9')) {
				char *end;
				xmitstat.thisbytes = strtoul(sizenum, &end, 10);
				if (*end && (*end != ' '))
					return EINVAL;
				/* the line length limit is raised by 26 characters
				 * in RfC 1870, section 3. */
				validlength += 26;
				more = end;
				continue;
			} else
				return EINVAL;
		} else if (!strncasecmp(more, " BODY=", 6)) {
			char *bodytype = more + 6;

			if (seenbody)
				return EINVAL;
			seenbody = 1;
			if (!strncasecmp(bodytype, "7BIT", 4)) {
				more = bodytype + 4;
				xmitstat.datatype = 0;
			} else if (!strncasecmp(bodytype, "8BITMIME", 8)) {
				more = bodytype + 8;
				xmitstat.datatype = 1;
			} else
				return EINVAL;

			if (*more && (*more != ' '))
				return EINVAL;
			continue;
		}
		return EBADRQC;
	}
	if (linelen > validlength)
		return E2BIG;

	while ( ( i = statvfs("queue/lock/sendmutex", &sbuf)) ) {
		int e;

		switch (e = errno) {
			case EINTR:	break;
			case ENOMEM:	return e;
			case ENOENT:	/* uncritical: only means that qmail-send is not running */
			case ENOSYS:
			/* will happen in most cases because program runs not in group qmail */
			case EACCES:	log_write(LOG_WARNING, "warning: can not get free queue disk space");
					goto next;
/*			case ELOOP:
			case ENAMETOOLONG:
			case ENOTDIR:
			case EOVERFLOW:
			case EIO:*/
			/* the other errors not named above should really never happen so
			 * just use default to get better code */
			default:	log_write(LOG_ERR, "critical: can not get free queue disk space");
					return e;
		}
	}
next:
	if (!i) {
		if (sbuf.f_flag & ST_RDONLY)
			return EROFS;
		/* check if the free disk in queue filesystem is at least the size of the message */
		if ((databytes && (databytes < xmitstat.thisbytes)) || (sbuf.f_bsize*sbuf.f_bavail < xmitstat.thisbytes))
			return netwrite("452 4.3.1 Requested action not taken: insufficient system storage\r\n") ? errno : EDONE;
	}

	/* no need to check existence of sender domain on bounce message */
	if (xmitstat.mailfrom.len) {
		/* strchr can't return NULL here, we have checked xmitstat.mailfrom.s before */
		xmitstat.fromdomain = ask_dnsmx(strchr(xmitstat.mailfrom.s, '@') + 1, &xmitstat.frommx);
		if (xmitstat.fromdomain < 0)
			return errno;
		i = check_host(strchr(xmitstat.mailfrom.s, '@') + 1);
	} else {
		xmitstat.fromdomain = 0;
		xmitstat.frommx = NULL;
		i = check_host(HELOSTR);
	}
	if (i < 0)
		return errno;
	xmitstat.spf = (i & 0x0f);
	badbounce = 0;
	goodrcpt = 0;
	okmsg[1] = xmitstat.mailfrom.len ? xmitstat.mailfrom.s : "";
	return net_writen(okmsg) ? errno : 0;
}

int
smtp_vrfy(void)
{
	return netwrite("252 send some mail, I'll do my very best\r\n") ? errno : 0;
}

int
hasinput(void)
{
	int rc;

	if ( (rc = data_pending()) <= 0)
		return errno;

	/* there is input data pending. This means the client sent some before our
	 * reply. His SMTP engine is broken so we don't let him send the mail */
	/* first: consume the first line of input so client will trigger the bad
	 * commands counter if he ignores everything we send */
	rc = net_read() ? errno : 0;
	if (rc)
		return rc;
	return netwrite("550 5.5.0 you must wait for my reply\r\n") ? errno : EBOGUS;
}

int
smtp_noop(void)
{
	return netwrite("250 2.0.0 ok\r\n") ? errno : 0;
}

int
smtp_rset(void)
{
	/* if there was EHLO or HELO before we reset to the state to immediately after this */
	if (comstate >= 0x008) {
		freedata();
		commands[2].state = (0x008 << xmitstat.esmtp);
	}
	/* we don't need the else case here: if there was no helo/ehlo no one has changed .state */
	return netwrite("250 2.0.0 ok\r\n") ? errno : 0;
}

int
smtp_quit(void)
{
	const char *msg[] = {"221 2.0.0 ", heloname.s, " service closing transmission channel", NULL};
	int rc;

	freedata();

	free(protocol);
	free(gcbuf);
	free(globalconf);
	rc = net_writen(msg);
	exit(rc ? errno : 0);
}

static int
smtp_temperror(void)
{
	return netwrite("451 4.3.5 system config error\r\n") ? errno : EDONE;
}

int
main(int argc, char *argv[]) {
	int flagbogus = 0;

	if (setup()) {
		/* setup failed: make sure we wait until the "quit" of the other host but
		 * do not process any mail. Commands RSET, QUIT and NOOP are still allowed.
		 * The state will not change so a client ignoring our error code will get
		 * "bad sequence of commands" and will be kicked if it still doesn't care */
		int i;
		for (i = (sizeof(commands) / sizeof(struct smtpcomm)) - 1; i > 2; i--) {
			commands[i].func = smtp_temperror;
			commands[i].state = -1;
		}
	} else {
		STREMPTY(xmitstat.authname);
		xmitstat.check2822 = 2;
		TAILQ_INIT(&head);		/* Initialize the recipient list. */
	}
	if (!getenv("BANNER")) {
		const char *msg[] = {"220 ", heloname.s, " " VERSIONSTRING " ESMTP", NULL};

		if (! (flagbogus = hasinput()) ) {
			flagbogus = net_writen(msg) ? errno : 0;
		}
	}

	/* Check if parameters given. If they are given assume they are for auth checking*/
	auth_host = NULL;
	if (argc >= 4) {
		auth_check = argv[2];
		auth_sub = argv + 3;
		if (domainvalid(argv[1],0)) {
			const char *msg[] = {"domainname for auth invalid", auth_host, NULL};

			log_writen(LOG_WARNING, msg);
		} else {
			int fd = open(auth_check, O_RDONLY);
			if (fd < 0) {
				const char *msg[] = {"checkpassword program '", auth_check, "' does not exist", NULL};

				log_writen(LOG_WARNING, msg);
			} else {
				int r;

				while ((r = close(fd)) && (errno == EINTR));
				if (!r) {
					auth_host = argv[1];
				} else {
					flagbogus = errno;
				}
			}
		}
	} else if (argc != 1) {
		log_write(LOG_ERR, "invalid number of parameters given");
	}

/* the state machine */
	while (1) {
		unsigned int i;
/* read the line (but only if there is not already an error condition, in this case handle the error first) */
		if (!flagbogus) {
			flagbogus = net_read();

/* sanity checks */
			/* we are not in DATA here so there MUST NOT be a non-ASCII character,
			* '\0' is also bogus */
			if (!flagbogus) {
				for (i = 0; i < linelen; i++)
					/* linein is signed char, so non-ASCII characters are <0 */
					if (linein[i] <= 0) {
						flagbogus = EINVAL;
						break;
					}
			} else
				flagbogus = errno;
		}

/* error handling */
		if (flagbogus) {
			if (badcmds > MAXBADCMDS) {
				const char *msg[] = {"dropped connection from [", xmitstat.remoteip,
							"] {too many bad commands}", NULL };

				/* -ignore possible errors here, we exit anyway
				 * -don't use tarpit: this might be a virus or something going wild,
				 *  tarpit would allow him to waste even more bandwidth */
				netwrite("550-5.7.1 too many bad commands\r\n");
				log_writen(LOG_INFO, msg);
				netwrite("550 5.7.1 die slow and painful\r\n");
				exit(0);
			}
			badcmds++;
			/* set flagbogus again in the switch statement to check if an error
			 * occured during error handling. This is a very bad sign: either
			 * we are very short of resources or the client is really really broken */
			switch (flagbogus) {
				case EBADRQC:	tarpit();
						flagbogus = netwrite("555 5.5.2 unrecognized command parameter\r\n") ? errno : 0;
						break;
				case EINVAL:	tarpit();
						flagbogus = netwrite("550 5.5.2 command syntax error\r\n") ? errno : 0;
						break;
				case E2BIG:	tarpit();
						flagbogus = netwrite("500 5.5.2 line too long\r\n") ? errno : 0;
						break;
				case ENOMEM:	/* ignore errors for the first 2 messages: if the third
						 * one succeeds everything is ok */
						netwrite("452-4.3.0 out of memory\r\n");
						sleep(30);
						netwrite("452-4.3.0 give me some time to recover\r\n");
						sleep(30);
						badcmds = 0;
						flagbogus = netwrite("452 4.3.0 please try again later\r\n") ? errno : 0;
						break;
				case EIO:	badcmds = 0;
						flagbogus = netwrite("451 4.3.0 IO error, please try again later\r\n") ? errno : 0;
						break;
				case EMSGSIZE:	badcmds = 0;
						flagbogus = netwrite("552 4.3.1 Too much mail data\r\n") ? errno : 0;
						break;
				case EBADE:	flagbogus = netwrite("550 5.7.5 data encryption error\r\n") ? errno : 0;
						break;
				case EROFS:	log_write(LOG_ERR, "HELP! queue filesystem looks read only!");
						badcmds = 0;
						flagbogus = netwrite("452 4.3.5 cannot write to queue\r\n") ? errno : 0;
						break;
				case 1:		tarpit();
						flagbogus = netwrite("503 5.5.1 Bad sequence of commands\r\n") ? errno : 0;
						break;
				case EDONE:	badcmds = 0;	/* fallthrough */
				case EBOGUS:	flagbogus = 0;
						break;
				case EINTR:	log_write(LOG_WARNING, "interrupted by signal");
						_exit(EINTR);
				default:	log_write(LOG_ERR, "writer error. kick me.");
						log_write(LOG_ERR, strerror(flagbogus));
						badcmds = 0;
						flagbogus = netwrite("500 5.3.0 unknown error\r\n") ? errno : 0;
			}
			/* do not work on the command now: it was either not read or was bogus.
			 * Start again and try to read one new to see if it get's better */
			continue;
		}

/* set flagbogus to catch if client writes crap. Will be overwritten if a good command comes in */
		flagbogus = EINVAL;
/* handle the commands */
		for (i = 0; i < sizeof(commands) / sizeof(struct smtpcomm); i++) {
			if (!strncasecmp(linein, commands[i].name, commands[i].len)) {
				if (comstate & commands[i].mask) {
					if (!(commands[i].flags & 2) && (linelen > 510)) {
						/* RfC 2821, section 4.5.3.1 defines the maximum length of a command line
						 * to 512 chars if this limit is not raised by an extension. Since we
						 * stripped CRLF our limit is 510. A command may override this check and
						 * check line length by itself */
						 flagbogus = E2BIG;
						 break;
					}
					if (!(commands[i].flags & 1) && linein[commands[i].len]) {
						flagbogus = EINVAL;
					} else
						flagbogus = commands[i].func();

					/* command succeded */
					if (!flagbogus) {
						if (commands[i].state > 0)
							comstate = commands[i].state;
						else if (!commands[i].state)
							comstate = (1 << i);
						flagbogus = 0;
						badcmds = 0;
					}
				} else
					flagbogus = 1;
				break;
			}
		}
	}
}
