/** \file mime.h
 * \brief functions for parsing MIME messages
 */
#ifndef MIME_H
#define MIME_H

#include <sys/types.h>
#include "sstring.h"
#include <qremote/qrdata.h>

extern const char *skipwhitespace(const char *line, const size_t len) __attribute__ ((pure)) __attribute__ ((nonnull(1)));
extern int is_multipart(const cstring *, cstring *) __attribute__ ((pure)) __attribute__ ((nonnull(1,2)));
extern size_t getfieldlen(const char *, const size_t) __attribute__ ((pure)) __attribute__ ((nonnull(1)));
extern size_t mime_param(const char *, const size_t) __attribute__ ((pure)) __attribute__ ((nonnull(1)));
extern size_t mime_token(const char *, const size_t) __attribute__ ((pure)) __attribute__ ((nonnull(1)));
extern off_t find_boundary(const char *, const off_t, const cstring *) __attribute__ ((pure)) __attribute__ ((nonnull (1,3)));

#endif