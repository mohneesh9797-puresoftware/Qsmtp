/** \file qremote.c
 \brief main functions of Qremote

 This file contains the main function, the configuration and error handling of Qremote,
 the drop-in replacement for qmail-remote.
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <fcntl.h>
#include "netio.h"
#include "qdns.h"
#include "control.h"
#include "log.h"
#include "match.h"
#include "sstring.h"
#include "conn.h"
#include "qremote.h"
#include "starttlsr.h"
#include "qrdata.h"
#include "fmt.h"
#include "tls.h"
#include "qmaildir.h"
#include "statuscodes.h"

int socketd = -1;
string heloname;
unsigned int smtpext;	/**< the SMTP extensions supported by the remote server */
char *rhost;		/**< the DNS name (if present) and IP address of the remote server to be used in log messages */
size_t rhostlen;	/**< valid length of rhost */
char *partner_fqdn;	/**< the DNS name of the remote server, or NULL if no reverse lookup exists */
size_t chunksize;
struct in6_addr outip;

static void
quitmsg(void)
{
	netwrite("QUIT\r\n");
	do {
		/* don't care about what he replies: we want to quit, if he don't want us to he must pay money *eg* */
		if (net_read()) {
			log_write(LOG_ERR, "network read error while waiting for QUIT reply");
			break;
		}
	} while ((linelen >= 4) && (linein[3] == '-'));
	close(socketd);
	socketd = -1;
}

void
net_conn_shutdown(const enum conn_shutdown_type sd_type)
{
	if ((sd_type == shutdown_clean) && (socketd >= 0)) {
		quitmsg();
	} else if (socketd >= 0) {
		close(socketd);
		socketd = -1;
	}

	if (ssl != NULL) {
		ssl_free(ssl);
		ssl = NULL;
	}

#ifdef USESYSLOG
	closelog();
#endif

	free(heloname.s);

	exit(0);
}

void
err_mem(const int doquit)
{
/* write text including 0 byte */
	write(1, "Z4.3.0 Out of memory.\n", 23);

	net_conn_shutdown(doquit ? shutdown_clean : shutdown_abort);
}

void
err_conf(const char *errmsg)
{
	const char *msg[] = {errmsg, NULL};
	err_confn(msg);
}

void
err_confn(const char **errmsg)
{
	log_writen(LOG_ERR, errmsg);
	/* write text including 0 byte */
	write(1, "Z4.3.0 Configuration error.\n", 29);
	net_conn_shutdown(shutdown_clean);
}

static void
setup(void)
{
	int j;
	unsigned long chunk;
	char *ipbuf;

#ifdef USESYSLOG
	openlog("Qremote", LOG_PID, LOG_MAIL);
#endif

	if (chdir(AUTOQMAIL)) {
		err_conf("cannot chdir to qmail directory");
	}

	if ( (j = loadoneliner("control/helohost", &heloname.s, 1) ) < 0 ) {
		if ( ( j = loadoneliner("control/me", &heloname.s, 0) ) < 0 ) {
			err_conf("can open neither control/helohost nor control/me");
		}
		if (domainvalid(heloname.s)) {
			err_conf("control/me contains invalid name");
		}
	} else {
		if (domainvalid(heloname.s)) {
			err_conf("control/helohost contains invalid name");
		}
	}
	heloname.len = j;

	if ( (j = loadintfd(open("control/timeoutremote", O_RDONLY), &chunk, 320)) < 0) {
		err_conf("parse error in control/timeoutremote");
	}
	timeout = chunk;

	if ( (j = loadintfd(open("control/chunksizeremote", O_RDONLY), &chunk, 32768)) < 0) {
		err_conf("parse error in control/chunksizeremote");
	} else {
		if (chunk >= (1 << 31)) {
			err_conf("chunksize in control/chunksizeremote too big");
		}
		chunksize = chunk & 0xffffffff;
	}

	if ( (j = loadoneliner("control/outgoingip", &ipbuf, 1) ) >= 0 ) {
		int r = inet_pton(AF_INET6, ipbuf, &outip);

		free(ipbuf);
		if (r <= 0) {
			err_conf("parse error in control/outgoingip");
		}
#ifdef IPV4ONLY
		if (!IN6_IS_ADDR_V4MAPPED(&outip)) {
			err_conf("compiled for IPv4 only but control/outgoingip has IPv6 address");
		}
#endif
	} else {
		outip = in6addr_any;
	}

#ifdef DEBUG_IO
	j = open("control/Qremote_debug", O_RDONLY);
	do_debug_io = (j > 0);
	if (j > 0)
		close(j);
#endif
}

void
quit(void)
{
	net_conn_shutdown(shutdown_clean);
}

/**
 * print remote host information to buffer
 *
 * @param mx list of MX entries, entry with priority 65538 is active
 */
static inline void
getrhost(const struct ips *mx)
{
	const struct ips *m = mx;
	int r;

	free(partner_fqdn);
	free(rhost);

	/* find active mx */
	while (m->priority != 65538)
		m = m->next;

	r = ask_dnsname(&m->addr, &partner_fqdn);
	if (r <= 0) {
		if ((r == 0) || (errno != ENOMEM)) {
			rhost = malloc(INET6_ADDRSTRLEN + 2);
		}
		if (errno == ENOMEM) {
			err_mem(1);
		}
		rhost[0] = '[';
		rhostlen = 1;
		partner_fqdn = NULL;
	} else {
		rhostlen = strlen(partner_fqdn);
		rhost = malloc(rhostlen + INET6_ADDRSTRLEN + 3);

		if (!rhost) {
			err_mem(1);
		}

		memcpy(rhost, partner_fqdn, rhostlen);
		rhost[rhostlen++] = ' ';
		rhost[rhostlen++] = '[';
	}
	/* there can't be any errors here ;) */
	(void) inet_ntop(AF_INET6, &m->addr, rhost + rhostlen, INET6_ADDRSTRLEN);
	rhostlen = strlen(rhost);
	rhost[rhostlen++] = ']';
	rhost[rhostlen] = '\0';
}

/**
 * get one line from the network, handle all error cases
 *
 * @return SMTP return code of the message
 */
int
netget(void)
{
	int q, r;

	if (net_read()) {
		switch (errno) {
			case ENOMEM:	err_mem(1);
			case EINVAL:
			case E2BIG:	goto syntax;
			default:	{
						char *tmp = strerror(errno);

						write(1, "Z", 1);
						write(1, tmp, strlen(tmp) + 1);
						quit();
					}
		}
	}
	if (linelen < 3)
		goto syntax;
	if ((linelen > 3) && ((linein[3] != ' ') && (linein[3] != '-')))
		goto syntax;
	r = linein[0] - '0';
	if ((r < 2) || (r > 5))
		goto syntax;
	q = linein[1] - '0';
	if ((q < 0) || (q > 9))
		goto syntax;
	r = r * 10 + q;
	q = linein[2] - '0';
	if ((q < 0) || (q > 9))
		goto syntax;
	return r * 10 + q;
syntax:
	/* if this fails we're already in bad trouble */
	(void) write(1, "Zsyntax error in server reply\n", 31);
	quit();
}

/**
 * check the reply of the server
 *
 * @param status status codes to print or NULL if not to
 * @param pre text to write to stdout before server reply if mask matches
 * @param mask bitmask for pre: 1: 2xx, 2: 4xx, 4: 5xx
 * @return the SMTP result code
 *
 * status must be at least 3 bytes long but only the first 3 will have any effect. The first
 * one is the status code writen on success (server response is 2xx), the second on on temporary
 * error (4xx) and the third on permanent error (5xx). If no status code should be written status
 * must be set to NULL. If the first character in status is ' ' no message will be printed for
 * success messages.
 */
int
checkreply(const char *status, const char **pre, const int mask)
{
	int res;
	int ignore = 0;

	res = netget();
	if (status) {
		int m;

		if ((res >= SUCCESS_MINIMUM_STATUS) && (res <= SUCCESS_MAXIMUM_STATUS)) {
			if (status[0] == ' ') {
				ignore = 1;
			} else {
				write(1, status, 1);
			}
			m = 1;
		} else if ((res >= TEMP_MINIMUM_STATUS) && (res <= TEMP_MAXIMUM_STATUS)) {
			write(1, status + 1, 1);
			m = 2;
		} else {
			write(1, status + 2, 1);
			m = 4;
		}
		if (!ignore) {
			if (pre && (m & mask)) {
				int i = 0;

				while (pre[i]) {
					write(1, pre[i], strlen(pre[i]));
					i++;
				}
			}
			write(1, linein, linelen);
		}
	}
	while (linein[3] == '-') {
		/* ignore the SMTP code sent here, if it's different from the one before the server is broken */
		(void) netget();
		if (status && !ignore) {
			write(1, linein, linelen);
			write(1, "\n", 1);
		}
	}

	if (status && !ignore)
		write(1, "", 1);
	/* this allows us to check for 2xx with (x < 300) later */
	if (res < 200)
		res = 599;
	return res;
}

static unsigned long remotesize;

static int
cb_size(void)
{
	char *s;

	if (!linein[8])
		return 0;

	remotesize = strtoul(linein + 8, &s, 10);
	return *s;
}

/**
 * greet the server, try ehlo and fall back to helo if needed
 *
 * @return 0 if greeting succeeded, 1 on error
 */
static int
greeting(void)
{
	struct smtpexts {
		const char *name;
		unsigned int len;	/* strlen(name) */
		int (*func)(void);	/* used to handle arguments to this extension, NULL if no arguments allowed */
	} extensions[] = {
#define SMTPEXT_SIZE 0x01
		{ .name = "SIZE",	.len = 4,	.func = cb_size	}, /* 0x01 */
#define SMTPEXT_PIPELINING 0x02
		{ .name = "PIPELINING",	.len = 10,	.func = NULL	}, /* 0x02 */
#define SMTPEXT_STARTTLS 0x04
		{ .name = "STARTTLS",	.len = 8,	.func = NULL	}, /* 0x04 */
#define SMTPEXT_8BITMIME 0x08
		{ .name = "8BITMIME",	.len = 8,	.func = NULL	}, /* 0x08 */
#ifdef CHUNKING
#define SMTPEXT_CHUNKING 0x10
		{ .name = "CHUNKING",	.len = 8,	.func = NULL	}, /* 0x10 */
#endif
		{ .name = NULL }
	};
	const char *cmd[3];
	int s;			/* SMTP status */

	cmd[0] = "EHLO ";
	cmd[1] = heloname.s;
	cmd[2] = NULL;
	net_writen(cmd);
	do {
		s = netget();
		if (s == 250) {
			int j = 0;

			while (extensions[j].name) {
				if (!strncasecmp(linein + 4, extensions[j].name, extensions[j].len)) {
					if (extensions[j].func) {
						int r;

						r = extensions[j].func();
						if (!r) {
							smtpext |= (1 << j);
							break;
/*						} else if (r < 0) {
							return r;
*/						} else {
							const char *logmsg[4] = {"syntax error in EHLO response \"",
									    extensions[j].name,
									    "\"", NULL};

							log_writen(LOG_WARNING, logmsg);
						}
					} else {
						if (!*(linein + 4 + extensions[j].len)) {
							smtpext |= (1 << j);
							break;
						}
					}
				}
				j++;
			}
		}
	} while (linein[3] == '-');

	if (s != 250) {
/* EHLO failed, try HELO */
		cmd[0] = "HELO ";
		net_writen(cmd);
		do {
			s = netget();
		} while (linein[3] == '-');
		if (s == 250) {
			smtpext = 0;
		} else {
			return 1;
		}
	}
	return 0;
}

void
dieerror(int error)
{
	const char *logmsg[] = { "connection to ", rhost, NULL, NULL };

	switch (error) {
	case ETIMEDOUT:
		write(1, "Zconnection to remote timed out\n", 33);
		logmsg[2] = " timed out";
		log_writen(LOG_WARNING, logmsg);
		break;
	case ECONNRESET:
		write(1, "Zconnection to remote server died\n", 35);
		logmsg[2] = " died";
		log_writen(LOG_WARNING, logmsg);
		break;
	}
	net_conn_shutdown(shutdown_abort);
}

static const char *mailerrmsg[] = {"Connected to ", NULL, " but sender was rejected", NULL};

int
main(int argc, char *argv[])
{
	const char *netmsg[6];
	int rcptstat = 1;	/* this means: all recipients have been rejected */
	int i;
	struct ips *mx = NULL;
	int rcptcount = argc - 3;
	struct stat st;
	char sizebuf[ULSTRLEN];
	unsigned int lastmsg;	/* last message in array */
	unsigned int recodeflag;

	setup();

	if (rcptcount <= 0) {
		log_write(LOG_CRIT, "too few arguments");
		write(1, "Zinternal error: Qremote called with invalid arguments\n", 56);
		net_conn_shutdown();
	}

	getmxlist(argv[1], &mx);
	sortmx(&mx);

	/* this shouldn't fail normally: qmail-rspawn did it before successfully */
	i = fstat(0, &st);
	if (i) {
		if (errno == ENOMEM)
			err_mem(0);
		log_write(LOG_CRIT, "can't fstat() input");
		write(1, "Zinternal error: can't fstat() input\n", 38);
		net_conn_shutdown();
	}
	msgsize = st.st_size;
	msgdata = mmap(NULL, msgsize, PROT_READ, MAP_SHARED, 0, 0);

	if (msgdata == MAP_FAILED) {
		log_write(LOG_CRIT, "can't mmap() input");
		write(1, "Zinternal error: can't mmap() input\n", 37);
		net_conn_shutdown();
	}
	dup2(0, 42);

/* for all MX entries we got: try to enable connection, check if the SMTP server wants us
 * (sends 220 response) and EHLO/HELO succeeds. If not, try next. If none left, exit. */
	do {
		int flagerr = 0;

/*		if (i < 0) {
			if (errno == ENOMEM)
				err_mem(1);
			log_write(LOG_ERR, "error parsing EHLO response");
			write(1, "Zinternal error: can't parse EHLO response\n", 33);
			return 0;
		}
*/
		tryconn(mx, &outip);
		dup2(socketd, 0);
		if (netget() != 220) {
			quitmsg();
			continue;
		}
		while (strncmp("220-", linein, 4) == 0) {
			if (net_read() == 0)
				continue;

			flagerr = 1;
			switch (errno) {
			case ENOMEM:
					err_mem(1);
			case EINVAL:
			case E2BIG:
					(void) write(1, "Zsyntax error in server reply\n", 31);
					quitmsg();
					break;
			default:
				{
					const char *tmp = strerror(errno);

					write(1, "Z", 1);
					write(1, tmp, strlen(tmp) + 1);
					quitmsg();
				}
			}
		}
		if (flagerr)
			continue;

		if (strncmp("220 ", linein, 4)) {
			const char *dropmsg[] = {"invalid greeting from ", NULL, NULL};

			getrhost(mx);
			dropmsg[1] = rhost;
			log_writen(LOG_WARNING, dropmsg);
			quitmsg();
		}
	} while ((socketd < 0) || (i = greeting()));

	getrhost(mx);
	freeips(mx);
	mailerrmsg[1] = rhost;

	if (smtpext & SMTPEXT_STARTTLS) {
		if (tls_init()) {
			if (greeting()) {
				write(1, "ZEHLO failed after STARTTLS\n", 29);
				quit();
			}
			successmsg[4] = " encrypted";
		}
	}

/* check if message is plain ASCII or not */
	recodeflag = need_recode(msgdata, msgsize);

	netmsg[0] = "MAIL FROM:<";
	netmsg[1] = argv[2];
	lastmsg = 2;
/* ESMTP SIZE extension */
	if (smtpext & SMTPEXT_SIZE) {
		netmsg[lastmsg++] = "> SIZE=";
		ultostr(msgsize, sizebuf);
		netmsg[lastmsg++] = sizebuf;
	} else {
		netmsg[lastmsg++] = ">";
	}
/* ESMTP 8BITMIME extension */
	if (smtpext & SMTPEXT_8BITMIME) {
		netmsg[lastmsg++] = (recodeflag & 1) ? " BODY=8BITMIME" : " BODY=7BIT";
	}
	netmsg[lastmsg] = NULL;
	net_writen(netmsg);

	netmsg[0] = "RCPT TO:<";
	netmsg[2] = ">";
	netmsg[3] = NULL;
	if (smtpext & SMTPEXT_PIPELINING) {
/* server allows PIPELINING: first send all the messages, then check the replies.
 * This allows to hide network latency. */
		for (i = 3; i < argc; i++) {
			netmsg[1] = argv[i];
			net_writen(netmsg);
		}
/* MAIL FROM: reply */
		if (checkreply(" ZD", mailerrmsg, 6) >= 300) {
			for (i = rcptcount; i > 0; i--)
				checkreply(NULL, NULL, 0);
			quit();
		}
/* RCPT TO: replies */
		for (i = rcptcount; i > 0; i--) {
			if (checkreply(" sh", NULL, 0) < 300) {
				write(1, "r", 2);
				rcptstat = 0;
			}
		}
		if (rcptstat)
			quit();
	} else {
/* server does not allow pipelining: we must do this one by one */
		if (checkreply(" ZD", mailerrmsg, 6) >= 300)
			quit();
		for (i = 3; i < argc; i++) {
			netmsg[1] = argv[i];
			net_writen(netmsg);
			if (checkreply(" sh", NULL, 0) < 300) {
				write(1, "r", 2);
				rcptstat = 0;
			}
		}
		if (rcptstat)
			quit();
	}
	successmsg[0] = rhost;
#ifdef CHUNKING
	if (smtpext & SMTPEXT_CHUNKING) {
		send_bdat(recodeflag);
	} else {
#else
	{
#endif
		send_data(recodeflag);
	}
	quit();
}
