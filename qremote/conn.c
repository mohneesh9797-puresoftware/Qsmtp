/** \file conn.c
 \brief functions for establishing connection to remote SMTP server
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <syslog.h>
#include <fcntl.h>
#include <assert.h>
#include "qdns.h"
#include "control.h"
#include "match.h"
#include "log.h"
#include "qremote.h"

extern int socketd;
static unsigned int targetport = 25;

static int
conn(const struct in6_addr remoteip, const struct in6_addr *outip)
{
	int rc;
#ifdef IPV4ONLY
	struct sockaddr_in sock;

	socketd = socket(PF_INET, SOCK_STREAM, 0);

	if (socketd < 0)
		return errno;

	sock.sin_family = AF_INET;
	sock.sin_port = 0;
	sock.sin_addr.s_addr = outip->s6_addr32[3];

	rc = bind(socketd, (struct sockaddr *) &sock, sizeof(sock));

	if (rc)
		return errno;

	sock.sin_port = htons(targetport);
	sock.sin_addr.s_addr = remoteip.s6_addr32[3];
#else
	struct sockaddr_in6 sock;

	socketd = socket(PF_INET6, SOCK_STREAM, 0);

	if (socketd < 0)
		return errno;

	sock.sin6_family = AF_INET6;
	sock.sin6_port = 0;
	sock.sin6_flowinfo = 0;
	sock.sin6_addr = *outip;
	sock.sin6_scope_id = 0;

	rc = bind(socketd, (struct sockaddr *) &sock, sizeof(sock));

	if (rc)
		return errno;

	sock.sin6_port = htons(targetport);
	sock.sin6_addr = remoteip;
#endif

	return connect(socketd, (struct sockaddr *) &sock, sizeof(sock)) ? errno : 0;
}

/**
 * try to estabish an SMTP connection to one of the hosts in the ip list
 *
 * @param mx list of IP adresses to try
 * @param outip local IP to use
 *
 * Every entry where a connection attempt was made is marked with a priority of 65537,
 * the last one tried with 65538
 */
void
tryconn(struct ips *mx, const struct in6_addr *outip)
{
	struct ips *thisip;

	while (1) {
		for (thisip = mx; thisip; thisip = thisip->next) {
			if (thisip->priority == 65538)
				thisip->priority = 65537;
			if (thisip->priority <= 65536)
				break;
		}
		if (!thisip) {
			close(socketd);
			write(1, "Zcan't connect to any server\n", 30);
			exit(0);
		}

		if (!conn(thisip->addr, outip)) {
			/* set priority to 65538 to allow getrhost() to find active MX */
			thisip->priority = 65538;
			return;
		}
		thisip->priority = 65537;
	}
}

/**
 * get all IPs for the MX entries of target address
 *
 * @param remhost target address
 * @param mx list of MX entries will be stored here, memory will be malloced
 */
void
getmxlist(char *remhost, struct ips **mx)
{
	size_t reml = strlen(remhost);
#ifdef IPV4ONLY
	struct ips *thisip;
#endif

	if (remhost[0] == '[') {
		if (remhost[reml - 1] == ']') {
			*mx = malloc(sizeof(**mx));
			if (!*mx) {
				err_mem(0);
			}

			remhost[reml - 1] = '\0';
			if (inet_pton(AF_INET6, remhost + 1, &((*mx)->addr)) > 0) {
				(*mx)->priority = 0;
				(*mx)->next = NULL;
				return;
			} else if (inet_pton(AF_INET, remhost + 1, &((*mx)->addr.s6_addr32[3])) > 0) {
				memset((*mx)->addr.s6_addr32, 0, 12);
				(*mx)->priority = 0;
				(*mx)->next = NULL;
				return;
			}
		}
		log_write(LOG_ERR, "parse error in first argument");
		write(1, "Z4.3.0 parse error in first argument\n", 38);
		exit(0);
	}

	*mx = smtproute(remhost, reml, &targetport);
	if ((*mx == NULL) && (errno != 0)) {
		assert(errno == ENOMEM);
		err_mem(0);
	}

#ifdef IPV4ONLY
	for (thisip = *mx; thisip; thisip = thisip->next) {
		if (!IN6_IS_ADDR_V4MAPPED(&thisip->addr))
			thisip->priority = 65537;
	}
#endif
	
	if (!*mx) {
		if (ask_dnsmx(remhost, mx)) {
			write(1, "Z4.4.3 cannot find a mail exchanger for ", 40);
			write(1, remhost, reml);
			write(1, "\n", 2);
			exit(0);
		}
	}
}
