/** \file netio.c
 \brief functions for network I/O
 */
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "netio.h"
#include "log.h"
#include "ssl_timeoutio.h"
#include "tls.h"

char linein[1002];			/**< buffer for the line to read: max 1000 chars including CRLF,
					 * leading extra '.', closing '\\0' */
size_t linelen;				/**< length of the line */
static char lineinn[sizeof(linein)];	/**< if more than one line was in linein the rest is stored here */
static size_t linenlen;			/**< length of the lineinn */
time_t timeout;				/**< how long to wait for data */

#ifdef DEBUG_IO
#include <syslog.h>

int do_debug_io;
int in_data;

void DEBUG_IN(const size_t len)
{
	char buffer[len + 4];
	size_t en = 0;
	size_t i;

	if (!do_debug_io || in_data)
		return;

	buffer[0] = '<';
	if (ssl) {
		en = 1;
		buffer[1] = 'e';
	}
	buffer[1 + en] = ' ';
	memcpy(buffer + 2 + en, linein, len);
	buffer[2 + en + len] = '\0';
	for (i = len + 1 + en; i > 0; i--) {
		if (buffer[i] < 32)
			buffer[i] = '?';
	}

	log_write(LOG_DEBUG, buffer);
}

void DEBUG_OUT(const char *s, const size_t l)
{
	char buffer[l + 4];
	int en = 0;
	const char *b, *c;

	if (!do_debug_io || in_data)
		return;

	buffer[0] = '>';
	if (ssl) {
		en = 1;
		buffer[1] = 'e';
	}
	buffer[1 + en] = ' ';

	b = s;
	while ( (c = strchr(b, '\r')) && (b < s + l) ) {
		memcpy(buffer + 2 + en, b, c - b);
		buffer[2 + c - b + en] = '\0';
		log_write(LOG_DEBUG, buffer);
		b = c + 2;
	}
}

#else

#define DEBUG_IN(l) {}
#define DEBUG_OUT(s, l) {}

#endif

/**
 * read characters from (network) input
 *
 * @param buffer buffer to put the data in
 * @param len maximum length of data to read (one char less is read, the last one is set to '\0')
 * @return number of bytes read
 * @retval -1 on error (errno is set)
 */
static size_t
readinput(char *buffer, const size_t len)
{
	size_t retval;
	fd_set rfds;
	struct timeval tv = {
		.tv_sec = timeout,
		.tv_usec = 0,
	};

	if (ssl) {
		retval = ssl_timeoutread(tv.tv_sec, buffer, len - 1);
	} else {
		FD_ZERO(&rfds);
		FD_SET(0, &rfds);

		retval = select(1, &rfds, NULL, NULL, &tv);

		if (!retval) {
			dieerror(ETIMEDOUT);
		} else if (retval == (size_t) -1) {
			return retval;
		}
		retval = read(0, buffer, len - 1);
	}
	if (!retval) {
		dieerror(ECONNRESET);
	} else if (retval != (size_t) -1) {
		buffer[retval] = '\0';
	}
	return retval;
}

/**
 * detect the end of the first line in the given buffer
 *
 * @param buffer input buffer to check
 * @param buflen amount of data in buffer
 * @param valid a flag is stored here if the line is valid
 * @return the first character after the line end
 * @retval NULL buffer does neither contain CR nor LF
 *
 * If the buffer begins with a sequence of characters that contains
 * a stray CR or LF a pointer one character behind this is returned and
 * valid is set to 0. If the buffer begins with a valid line ending in
 * CRLF a pointer behind the LF is returned. If neither CR nor LF is
 * found NULL is returned.
 */
static const char *
find_eol(const char *buffer, const size_t buflen, int *valid)
{
	const char *cr, *lf;

	cr = memchr(buffer, '\r', buflen);
	lf = memchr(buffer, '\n', buflen);

	/* both are found and form a correct CRLF pair */
	if ((cr != NULL) && (lf == cr + 1)) {
		*valid = 1;
		return lf + 1;
	/* neither is found */
	} else if ((cr == NULL) && (lf == NULL)) {
		*valid = 0;
		return NULL;
	/* something went wrong */
	} else {
		*valid = 0;

		if (cr == NULL) {
			return lf + 1;
		} else if (lf == NULL) {
			return cr + 1;
		} else if (cr < lf) {
			/* check if LF is also a stray one, possibly skip
			 * to there in one step */
			if (*(lf - 1) != '\r')
				return lf + 1;
			else
				return cr + 1;
		} else {
			/* check if the CR is also a stray one and not
			 * exactly at the end of the buffer */
			if ((cr < buffer + buflen - 2) &&
					(*(cr + 1) != '\n'))
				return cr + 1;
			else
				return lf + 1;
		}
	}
}

/**
 * read input until a line with a valid length is in buffer
 * @param has_cr if the previous buffer ended with CR
 *
 * This function will set errno to the proper error code before
 * returning.
 */
static void
loop_long(int has_cr)
{
	const char *p;
	do {
		int valid;
		/* The idea here is to read input until we find a valid line end (CRLF),
		 * drop everything until this point (i.e. the too long line) and keep
		 * the rest in the buffer, but still return with an error code. */
		linenlen = readinput(linein, sizeof(linein));

		if (linenlen == (size_t) -1) {
			/* reset that to 0, otherwise it will confuse net_read() */
			linenlen = 0;
			return;
		}
		/* detect if the linebreak is interrupted by buffer end */
		if (has_cr && (linein[0] == '\n')) {
			p = linein + 1;
			linenlen--;
			break;
		}
		has_cr = 0;

		p = find_eol(linein, linenlen, &valid);

		if (!valid && (p == linein + linenlen) && (*(p - 1) == '\r')) {
			/* we need to read more data */
			has_cr = 1;
		} else if (p != NULL) {
			/* skip the broken part */
			linenlen -= (p - linein);
		}
	} while ((p == NULL) || has_cr);

	memcpy(lineinn, p, linenlen);
	errno = E2BIG;
}

/**
 * read one line from the network
 *
 * @retval 0 on success
 * @retval -1 on error (errno is set)
 *
 * does not return on timeout, programm will be cancelled
 */
int
net_read(void)
{
	size_t datain;
	size_t readoffset = 0;
	const char *p;
	int valid;

	if (linenlen) {
		p = find_eol(lineinn, linenlen, &valid);

		if (valid) {
			linelen = p - lineinn - 2;
			memcpy(linein, lineinn, linelen);
			linein[linelen] = '\0';
			linenlen -= (linelen + 2);
			/* still data in input buffer */
			if (linenlen != 0)
				memmove(lineinn, p, linenlen);

			DEBUG_IN(linelen);
			return 0;
		/* neither is found, so everything currently in the
		 * buffer is read, and more must be read from network */
		} else if ((p == NULL) ||
		/* only CR is found and it is at the end of the input buffer */
				((*(p - 1) == '\r') && (p == lineinn + linenlen))) {
			readoffset = linenlen;
			memcpy(linein, lineinn, linenlen);
			linenlen = 0;
		/* something went wrong */
		} else {
			linenlen -= (p - lineinn);
			if (linenlen != 0)
				memmove(lineinn, p, linenlen);
			errno = EINVAL;
			return -1;
		}
	}

	do {
		datain = readinput(linein + readoffset, sizeof(linein) - readoffset);
		/* now the first readoffset characters of linein are filled with the stuff from the last buffer (if any),
		* the next datain characters are filled with the data just read, then there is a '\0' */

		if (datain == (size_t) -1)
			return -1;

		/* RfC 2821, section 2.3.7:
		* "Conforming implementations MUST NOT recognize or generate any other
		* character or character sequence [than <CRLF>] as a line terminator" */

		readoffset += datain;

		p = find_eol(linein, readoffset, &valid);
	} while ((p == NULL) && (readoffset < sizeof(linein) - 1));

	if (valid) {
		linelen = p - linein - 2;
		linein[linelen] = '\0';
		/* if there is more data put it back into the buffer */
		if (p != linein + readoffset) {
			linenlen = readoffset - (p - linein);
			if (linenlen != 0)
				memcpy(lineinn, p, linenlen);
		}
	} else if (p == NULL) {
		/* the whole buffer is filled, but neither CR nor LF is found */
		loop_long(0);
		return -1;
	} else if ((p == linein + sizeof(linein) - 1) && (*(p - 1) == '\r')) {
		/* We found a CR, but a too long line. Let's find out if an LF will follow. */
		loop_long(1);
		return -1;
	} else {
		/* copy the rest of the input buffer back to lineinn, then return error */
		if (p != linein + readoffset) {
			linenlen = readoffset - (p - linein);
			if (linenlen != 0)
				memcpy(lineinn, p, linenlen);
		}
		errno = EINVAL;
		return -1;
	}

	/* do this again here: if there is a broken client that
	 * handles '.' duplication in data phase wrong this allows
	 * smtp_data to get his '\n.\n' and throw him out. If he
	 * is broken once why not twice? */

	DEBUG_IN(linelen);

	return 0;
}

/**
 * write one line to the network
 *
 * @param s line to be written (nothing else it written so it should contain CRLF)
 * @retval 0 on success
 * @retval -1 on error (errno is set)
 *
 * -does not return on timeout, programm will be cancelled
 * -same as net_write but expects that line is <= 512 characters and includes CRLF
 */
int
netwrite(const char *s)
{
	return netnwrite(s, strlen(s));
}

/**
 * write one line to the network
 *
 * @param s line to be written (nothing else it written so it should contain CRLF)
 * @param l length of s
 * @retval 0 on success
 * @retval -1 on error (errno is set)
 *
 * does not return on timeout, programm will be cancelled
 */
int
netnwrite(const char *s, const size_t l)
{
	fd_set wfds;
	struct timeval tv = {
		.tv_sec = timeout,
		.tv_usec = 0,
	};
	int retval;

	DEBUG_OUT(s, l);

	if (ssl) {
		if (ssl_timeoutwrite(tv.tv_sec, s, l) <= 0) {
			errno = EPROTO;
			return -1;
		}
		return 0;
	}
	FD_ZERO(&wfds);
	FD_SET(socketd, &wfds);

	retval = select(socketd + 1, NULL, &wfds, NULL, &tv);

	if (retval == -1)
		return retval;
	else if (!retval)
		dieerror(ETIMEDOUT);

	if (write(socketd, s, l) < 0) {
		if (errno == EPIPE)
			dieerror(ECONNRESET);
		else if ((errno == ECONNRESET) || (errno == ETIMEDOUT))
			dieerror(errno);
		return -1;
	}
	return 0;
}

/**
 * write one line to the network, fold if needed
 *
 * @param s array of strings to send
 * @retval 0 on success
 * @retval -1 on error (errno is set)
 *
 * does not return on timeout, programm will be cancelled
 *
 * \warning s[0] must be short enough to fit completely into the buffer
 * \warning every s[] must not have a sequence longer then 506 characters without a space (' ') in them
 */
int
net_writen(const char *const *s)
{
	unsigned int i;
	size_t len = 0;
	/* RfC 2821, section 4.5.3: reply line
	 *   The maximum total length of a reply line including the reply code
	 *   and the <CRLF> is 512 characters.  More information may be
	 *   conveyed through multiple-line replies. */
	char msg[512];

	for (i = 0; s[i]; i++) {
		size_t off = 0;
		const size_t l = strlen(s[i]);

		if (len + l > sizeof(msg) - 2) {
			char c = msg[3];

			msg[3] = '-';
			msg[len++] = '\r';
			msg[len++] = '\n';
			/* ignore if this fails: if the last on succeeds this must be enough for the client */
			(void) netnwrite(msg, len);
			len = 4;
			/* check if s[i] itself is too big */
			if (l + 6 > sizeof(msg)) {
				const char *sp = s[i], *nsp;

				while (l > off + sizeof(msg) - 6) {
					size_t m;

					nsp = strchr(s[i] + off, ' ');

					while (nsp - s[i] - off < sizeof(msg) - 6) {
						sp = nsp;
						nsp = strchr(sp + 1, ' ');
					}
					m = sp - s[i] - off;
					memcpy(msg + 4, s[i] + off, m);
					m += 4;
					msg[m++] = '\r';
					msg[m++] = '\n';
					netnwrite(msg, m);
					off += m - 6;
				}
			}
			msg[3] = c;
		}
		memcpy(msg + len, s[i] + off, l - off);
		len += l - off;
	}
	msg[len++] = '\r';
	msg[len++] = '\n';
	return netnwrite(msg, len);
}

/**
 * read a given number of bytes from network as binary data (i.e. without any mangling)
 *
 * @param num number of bytes to read
 * @param buf buffer to store data (must have enough space for (num + 1) bytes)
 * @return number of bytes read
 * @retval -1 on error
 */
size_t
net_readbin(size_t num, char *buf)
{
	size_t offs = 0;

	if (linenlen) {
		if (linenlen > num) {
			memcpy(buf, lineinn, num);
			memmove(lineinn, lineinn + num, linenlen - num);
			linenlen -= num;
			return num;
		} else {
			memcpy(buf, lineinn, linenlen);
			num -= linenlen;
			offs = linenlen;
			linenlen = 0;
		}
	}
	while (num) {
		size_t r;

		r = readinput(buf + offs, num + 1);
		if (r == (size_t) -1)
			return -1;
		offs += r;
		num -= r;
	}
	return offs;
}

/**
 * read up to a given number of bytes from network but stop at the first CRLF
 *
 * @param num number of bytes to read, must be < 1002 so everything behind the CRLF can be copied back to lineinn
 * @param buf buffer to store data (must have enough space)
 * @return number of bytes read
 * @retval -1 on error
 */
size_t
net_readline(size_t num, char *buf)
{
	size_t offs = 0;

	if (linenlen) {
		char *n = memchr(lineinn, '\n', linenlen);

		if (n) {
			size_t m = (n - lineinn);

			if (m < num)
				num = m;
		}
		/* now num is the number of bytes to copy from lineinn */
		if (linenlen > num) {
			memcpy(buf, lineinn, num);
			memmove(lineinn, lineinn + num, linenlen - num);
			linenlen -= num;
			return num;
		} else {
			memcpy(buf, lineinn, linenlen);
			num -= linenlen;
			offs = linenlen;
			linenlen = 0;
		}
	}
	while (num) {
		size_t r;
		char *n;

		r = readinput(buf + offs, num);
		if (r == (size_t) -1)
			return -1;
		n = memchr(buf + offs, '\n', r);
		/* if there is a LF in the buffer copy everything behind it to lineinn */
		if (n) {
			size_t rest = buf + offs + r - n - 1;

			memcpy(lineinn, n + 1, rest);
			linenlen = rest;
			offs += r - rest;
			return offs;
		}
		offs += r;
		num -= r;
	}
	return offs;
}

/**
 * check if there is data ready to be read without blocking
 *
 * @retval 0 if no data
 * @retval 1 if data
 * @retval -1 on error
 */
int
data_pending(void)
{
	if (linenlen) {
		return 1;
	} else if (ssl) {
		return SSL_pending(ssl);
	} else {
		fd_set rfds;
		struct timeval tv = {
			.tv_sec = 0,
			.tv_usec = 0,
		};

		FD_ZERO(&rfds);
		FD_SET(0, &rfds);

		return select(1, &rfds, NULL, NULL, &tv);
	}
}
