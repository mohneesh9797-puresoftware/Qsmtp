#include <qremote/client.h>

#include <qdns.h>
#include <qremote/qremote.h>
#include "test_io/testcase_io.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define ipstr_example "2001:db8:17:f4:d3::4"
#define ipstr_fail "2001:db8:17:f4:d3::5"
#define ipstr_unknown "2001:db8:17:f4:d3::6"
#define name_example "example.net"

char *partner_fqdn;
char *rhost;
size_t rhostlen;
static const char *netget_input;
static int statusfdout;
extern int statusfd; /* in qremote/status.c */

void
err_mem(const int k __attribute__((unused)))
{
	exit(ENOMEM);
}

int
netget(const unsigned int terminate)
{
	if (netget_input == NULL) {
		fprintf(stderr, "unexpected call to %s(%u)\n", __func__, terminate);
		exit(EFAULT);
	}

	const char *lf = strchr(netget_input, '\n');
	if (lf != NULL)
		linein.len = lf - netget_input;
	else
		linein.len = strlen(netget_input);

	assert(linein.len > 3);
	assert(linein.len < TESTIO_MAX_LINELEN);

	char num[4];
	memcpy(num, netget_input, 3);
	num[3] = 0;

	strncpy(linein.s, netget_input, linein.len);
	linein.s[linein.len] = '\0';

	if (lf == NULL)
		netget_input = NULL;
	else
		netget_input = lf + 1;

	return atoi(num);
}

int
test_ask_dnsname(const struct in6_addr *ip, char **result)
{
	struct in6_addr iptmp;

	inet_pton(AF_INET6, ipstr_example, &iptmp);

	if (!IN6_ARE_ADDR_EQUAL(ip, &iptmp)) {
		*result = strdup(name_example);
		if (*result == NULL)
			exit(ENOMEM);

		return 1;
	}

	inet_pton(AF_INET6, ipstr_fail, &iptmp);

	if (IN6_ARE_ADDR_EQUAL(ip, &iptmp))
		return -2;
	else
		return 0;
}

static int
testcase_valid_return(void)
{
	struct ips mx;
	struct in6_addr ip;

	memset(&mx, 0, sizeof(mx));
	mx.addr = &ip;
	mx.count = 1;
	inet_pton(AF_INET6, ipstr_example, mx.addr);
	mx.priority = MX_PRIORITY_CURRENT;
	mx.name = name_example;

	getrhost(&mx, 0);

	if ((partner_fqdn == NULL) || (rhost == NULL)) {
		fprintf(stderr, "%s: NULL value set\n", __func__);
		return 1;
	}

	if (strcmp(partner_fqdn, name_example) != 0) {
		fprintf(stderr, "%s: FQDN %s expected, but got %s\n", __func__,
				name_example, partner_fqdn);
		return 1;
	}

	if (strcmp(rhost, name_example " [" ipstr_example "]") != 0) {
		fprintf(stderr, "%s: got unexpected rhost '%s'\n", __func__,
				rhost);
		return 1;
	}

	return 0;
}

static int
testcase_noname(const char *ipstr)
{
	struct ips mx;
	struct in6_addr ips[2];

	memset(&mx, 0, sizeof(mx));
	mx.addr = ips;
	mx.count = 2;
	memset(ips, 0, sizeof(ips[0]));
	inet_pton(AF_INET6, ipstr, ips + 1);
	mx.priority = MX_PRIORITY_CURRENT;

	getrhost(&mx, 1);

	if (rhost == NULL) {
		fprintf(stderr, "%s: NULL value set\n", __func__);
		return 1;
	}

	if (partner_fqdn != NULL) {
		fprintf(stderr, "%s: no FQDN expected, but got %s\n", __func__,
				partner_fqdn);
		return 1;
	}

	if ((strncmp(rhost + 1, ipstr, strlen(ipstr)) != 0) || (*rhost != '[') ||
			(rhost[strlen(ipstr) + 1] != ']')) {
		fprintf(stderr, "%s: got unexpected rhost '%s'\n", __func__,
				rhost);
		return 1;
	}

	return 0;
}

/**
 * @brief check the result of checkreply()
 * @param msg the message expected on the status fd, NULL if none
 * @param status the expected result of checkreply()
 * @param statusc the status returned by checkreply()
 */
static int
check_cr(const char *msg, const int statuse, const int statusc)
{
	int ret = 0;
	struct pollfd fds = {
		.fd = statusfdout,
		.events = POLLIN
	};

	if (statusc != statuse) {
		fprintf(stderr, "checkreply() returned %i, but %i was expected\n",
				statusc, statuse);
		ret++;
	}

	int i = poll(&fds, 1, 0);

	if (i == 0) {
		/* nothing sent to statusfd */
		if (msg != NULL) {
			fprintf(stderr, "checkreply() did not write status, but '%s' was expected\n",
					msg);
			ret++;
		}
	} else if (i > 0) {
		char buf[1024];
		const ssize_t r = read(statusfdout, buf, sizeof(buf) - 1);

		if (r < 0) {
			fprintf(stderr, "read error: %i\n", errno);
			exit(1);
		}

		buf[r] = '\0';

		assert(i == 1);

		if (msg == NULL) {
			fprintf(stderr, "checkreply() wrote '%s', but no output was expected\n",
					buf);
			ret++;
		} else {
			/* one-letter messages do not have a newline */
			if (strlen(msg) > 1) {
				/* the output must end in \n\0 */
				if ((r < 2) || (buf[r - 2] != '\n') || (buf[r - 1] != '\0')) {
					fprintf(stderr, "checkreply() wrote '%s', but did not terminate with \\n\\0\n",
						buf);
					ret++;
				}
				buf[r - 2] = '\0';
			} else {
				buf[r] = '\0';
			}
			if (strcmp(buf, msg) != 0) {
				fprintf(stderr, "checkreply() wrote '%s', but '%s' was expected\n",
					buf, msg);
				ret++;
			}
		}
	} else {
		fprintf(stderr, "error %i from poll()\n", errno);
		ret++;
	}

	return ret;
}

static int
testcase_checkreply(void)
{
	int ret = 0;
	int fds[2];
	const char *pre[] = { "pre1", "pre2", NULL };

	statusfd = 1;

	if (pipe(fds) != 0) {
		fprintf(stderr, "%s: cannot create pipes: %i\n", __func__, errno);
		return ++ret;
	}

	statusfd = fds[1];
	statusfdout = fds[0];

	/* no status is legal, nothing should be printed */
	netget_input = "220 ";
	ret += check_cr(NULL, 220, checkreply(NULL, NULL, 0));

	/* space for success means nothing should be printed */
	netget_input = "220 ";
	ret += check_cr(NULL, 220, checkreply(" ZD", NULL, 0));

	netget_input = "220 ";
	ret += check_cr("K220 ", 220, checkreply("KZD", NULL, 0));

	/* check too small status code */
	netget_input = "199 too low";
	ret += check_cr("D199 too low", 599, checkreply(" ZD", NULL, 0));

	netget_input  = "421 temp";
	ret += check_cr("Z421 temp", 421, checkreply(" ZD", NULL, 0));

	netget_input = "220 ";
	ret += check_cr(NULL, 220, checkreply(NULL, pre, 0));

	netget_input = "220 ";
	ret += check_cr(NULL, 220, checkreply(" ZD", pre, 0));

	/* printing out success messages, verify that mask is honored */
	netget_input = "220 ";
	ret += check_cr("K220 ", 220, checkreply("KZD", pre, 0));
	netget_input = "220 ";
	ret += check_cr("Kpre1pre2220 ", 220, checkreply("KZD", pre, 1));
	netget_input = "220 ";
	ret += check_cr("Kpre1pre2220 ", 220, checkreply("KZD", pre, 5));

	/* printing out temporary errors, verify that mask is honored */
	netget_input  = "421 temp";
	ret += check_cr("Z421 temp", 421, checkreply(" ZD", pre, 0));
	netget_input  = "421 temp";
	ret += check_cr("Z421 temp", 421, checkreply(" ZD", pre, 1));
	netget_input  = "421 temp";
	ret += check_cr("Zpre1pre2421 temp", 421, checkreply(" ZD", pre, 2));
	netget_input  = "421 temp";
	ret += check_cr("Zpre1pre2421 temp", 421, checkreply(" ZD", pre, 6));
	netget_input  = "421 temp";
	ret += check_cr("Z421 temp", 421, checkreply(" ZD", pre, 4));

	/* printing out permanent errors, verify that mask is honored */
	netget_input  = "500 perm";
	ret += check_cr("D500 perm", 500, checkreply(" ZD", pre, 0));
	netget_input  = "500 perm";
	ret += check_cr("D500 perm", 500, checkreply(" ZD", pre, 1));
	netget_input  = "500 perm";
	ret += check_cr("D500 perm", 500, checkreply(" ZD", pre, 2));
	netget_input  = "500 perm";
	ret += check_cr("Dpre1pre2500 perm", 500, checkreply(" ZD", pre, 4));
	netget_input  = "500 perm";
	ret += check_cr("Dpre1pre2500 perm", 500, checkreply(" ZD", pre, 6));

	/* now multiline replies */
	netget_input = "500-perm1\n500-perm2\n500 perm3";
	ret += check_cr("D500-perm1\n500-perm2\n500 perm3", 500, checkreply(" ZD", pre, 2));
	netget_input = "500-perm1\n500-perm2\n500 perm3";
	ret += check_cr("Dpre1pre2500-perm1\n500-perm2\n500 perm3", 500, checkreply(" ZD", pre, 4));

	netget_input = "500-perm1\n500-perm2\n500 perm3";
	ret += check_cr(NULL, 500, checkreply(NULL, NULL, 0));

	/* what Qremote actually does for recipients */
	/* old version */
	netget_input  = "200 good";
	ret += check_cr(NULL, 200, checkreply(" sh", NULL, 0));
	netget_input  = "400 temp";
	ret += check_cr("s400 temp", 400, checkreply(" sh", NULL, 0));
	netget_input  = "500 perm";
	ret += check_cr("h500 perm", 500, checkreply(" sh", NULL, 0));
	/* new version */
	netget_input  = "200 good";
	ret += check_cr("r200 good", 200, checkreply("rsh", NULL, 0));
	netget_input  = "200 good";
	ret += check_cr("r", 200, checkreply("rsh", NULL, 8));
	netget_input  = "400 temp";
	ret += check_cr("s400 temp", 400, checkreply("rsh", NULL, 8));
	netget_input  = "500 perm";
	ret += check_cr("h500 perm", 500, checkreply("rsh", NULL, 8));

	close(fds[0]);
	close(fds[1]);

	return ret;
}

int
main(void)
{
	int ret = 0;

	testcase_setup_ask_dnsname(test_ask_dnsname);

	ret += testcase_valid_return();
	ret += testcase_noname(ipstr_unknown);
	ret += testcase_noname(ipstr_fail);

	ret += testcase_checkreply();

	free(partner_fqdn);
	free(rhost);

	return ret;
}
