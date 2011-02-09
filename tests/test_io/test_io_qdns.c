#include "testcase_io.h"
#include "testcase_io_p.h"

#include <qdns.h>

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

int
ask_dnsmx(const char *a, struct ips **b)
{
	assert(testcase_ask_dnsmx != NULL);

	return testcase_ask_dnsmx(a, b);
}

int
tc_ignore_ask_dnsmx(const char *a __attribute__ ((unused)), struct ips **b __attribute__ ((unused)))
{
	return 1;
}

int
ask_dnsaaaa(const char *a, struct ips **b)
{
	assert(testcase_ask_dnsaaaa != NULL);

	return testcase_ask_dnsaaaa(a, b);
}

int
tc_ignore_ask_dnsaaaa(const char *a __attribute__ ((unused)), struct ips **b __attribute__ ((unused)))
{
	return 1;
}

int
ask_dnsa(const char *a, struct ips **b)
{
	assert(testcase_ask_dnsa != NULL);

	return testcase_ask_dnsa(a, b);
}

int
tc_ignore_ask_dnsa(const char *a __attribute__ ((unused)), struct ips **b __attribute__ ((unused)))
{
	return 1;
}

int
ask_dnsname(const struct in6_addr *a, char **b)
{
	assert(testcase_ask_dnsname != NULL);

	return testcase_ask_dnsname(a, b);
}

int
tc_ignore_ask_dnsname(const struct in6_addr *a __attribute__ ((unused)), char **b __attribute__ ((unused)))
{
	return 0;
}
