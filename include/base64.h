/** \file base64.h
 \brief headers of Base64 encode and decode functions
 */
#ifndef BASE64_H
#define BASE64_H

#include "compiler.h"
#include "sstring.h"

extern int b64decode(const char *in, size_t l, string *out) __attribute__ ((nonnull (3))) ATTR_ACCESS(read_only, 1, 2);
extern int b64encode(const string *in, string *out, const unsigned int wraplimit) __attribute__ ((nonnull (1,2)));

#endif
