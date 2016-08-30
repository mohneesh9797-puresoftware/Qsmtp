/** \file starttls.c
 \brief functions for STARTTLS SMTP command
 */

#include <qsmtpd/starttls.h>

#include <control.h>
#include <fmt.h>
#include <log.h>
#include <netio.h>
#include <qdns.h>
#include <qsmtpd/addrparse.h>
#include <qsmtpd/qsmtpd.h>
#include <qsmtpd/syntax.h>
#include <ssl_timeoutio.h>
#include <tls.h>
#include <version.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

static RSA *
tmp_rsa_cb(SSL *s __attribute__ ((unused)), int export __attribute__ ((unused)), int keylen)
{
	BIGNUM *bn;
#if !((OPENSSL_VERSION_NUMBER >= 0x10100000L) && !defined(LIBRESSL_VERSION_NUMBER))
	BIGNUM bn_store;
#endif
	RSA *rsa;

	if (keylen < 2048)
		keylen = 2048;
	if (keylen == 2048) {
		FILE *in = fdopen(openat(controldir_fd, "rsa2048.pem", O_RDONLY | O_CLOEXEC), "r");
		if (in) {
			rsa = PEM_read_RSAPrivateKey(in, NULL, NULL, NULL);

			fclose(in);

			if (rsa)
				return rsa;
		}
	}

	rsa = RSA_new();
	if (rsa == NULL)
		return NULL;

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L) && !defined(LIBRESSL_VERSION_NUMBER)
	bn = BN_new();
	if (bn == NULL)
		return NULL;
#else
	bn = &bn_store;
#endif

	BN_set_word(bn, RSA_F4);
	if (RSA_generate_key_ex(rsa, keylen, bn, NULL) == 0) {
		RSA_free(rsa);
		rsa = NULL;
	}

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L) && !defined(LIBRESSL_VERSION_NUMBER)
	BN_free(bn);
#endif

	return rsa;
}

static DH *
tmp_dh_cb(SSL *s __attribute__ ((unused)), int export __attribute__ ((unused)), int keylen)
{
	FILE *in = NULL;
	char fname[ULSTRLEN + strlen("dh.pem") + 1];
	DH *dh;

	if (keylen < 2048)
		keylen = 2048;

	strcpy(fname, "dh");
	ultostr(keylen, fname + strlen("dh"));
	strcat(fname, ".pem");

	in = fdopen(openat(controldir_fd, fname, O_RDONLY | O_CLOEXEC), "r");
	if (in) {
		dh = PEM_read_DHparams(in, NULL, NULL, NULL);

		fclose(in);

		if (dh)
			return dh;
	}

	dh = DH_new();
	if (dh == NULL)
		return NULL;

	if (DH_generate_parameters_ex(dh, keylen, DH_GENERATOR_2, NULL) != 1) {
		DH_free(dh);
		dh = NULL;
	}

	return dh;
}

static int __attribute__((nonnull(1, 2)))
tls_out(const char *s1, const char *s2, const int def_return)
{
	const char *msg[] = {"454 4.3.0 TLS ", s1, ": ", s2, NULL};
	int r = net_writen(msg);

	return r ? r : def_return;
}

static int __attribute__((nonnull(1)))
tls_err(const char *s)
{
	return -tls_out(s, ssl_error(), -EDONE);
}

#define CLIENTCA "control/clientca.pem"
#define CLIENTCRL "control/clientcrl.pem"

static int ssl_verified;

/**
 * @brief callback for SSL_set_verify() that accepts any certicate
 * @returns 1
 *
 * This will accept any certificate chain, so the SSL session can be reestablished.
 * The errors will be checked later by calling SSL_get_verify_result(). */
static int
verify_callback(int preverify_ok __attribute__ ((unused)), X509_STORE_CTX *x509_ctx __attribute__ ((unused)))
{
	return 1;
}

/**
 * @brief verify is authenticated to relay by SSL certificate
 *
 * @retval <1 error code
 * @retval 0 if client is not authenticated
 * @retval >0 if client is authenticated
 */
int
tls_verify(void)
{
	char **clients;
	STACK_OF(X509_NAME) *sk;
	int tlsrelay = 0;

	if (!ssl || ssl_verified || is_authenticated_client())
		return 0;
	ssl_verified = 1; /* don't do this twice */

	/* request client cert to see if it can be verified by one of our CAs
	 * and the associated email address matches an entry in tlsclients */
	if (loadlistfd(openat(controldir_fd, "tlsclients", O_RDONLY | O_CLOEXEC), &clients, checkaddr) < 0)
		return -errno;

	if (clients == NULL)
		return 0;

	sk = SSL_load_client_CA_file(CLIENTCA);
	if (sk == NULL) {
		/* if CLIENTCA contains all the standard root certificates, a
		 * 0.9.6b client might fail with SSL_R_EXCESSIVE_MESSAGE_SIZE;
		 * it is probably due to 0.9.6b supporting only 8k key exchange
		 * data while the 0.9.6c release increases that limit to 100k */
		free(clients);
		return 0;
	}

	SSL_set_client_CA_list(ssl, sk);
	SSL_set_verify(ssl, SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE, verify_callback);

	do { /* one iteration */
		X509 *peercert;
		X509_NAME *subj;
		cstring email = { .len = 0, .s = NULL };
		int n;

		if (SSL_set_session_id_context(ssl, VERSIONSTRING, strlen(VERSIONSTRING)) != 1) {
			const char *err = ssl_strerror();
			tlsrelay = tls_out("setting session id failed", err, -EPROTO);
			break;
		}

		/* renegotiate to force the client to send it's certificate */
		n = ssl_timeoutrehandshake(timeout);
		if (n == -ETIMEDOUT) {
			dieerror(ETIMEDOUT);
		} else if (n < 0) {
			const char *err = ssl_strerror();
			tlsrelay = tls_out("rehandshake failed", err, n);
			break;
		}

		if (SSL_get_verify_result(ssl) != X509_V_OK)
			break;

		peercert = SSL_get_peer_certificate(ssl);
		if (!peercert)
			break;

		subj = X509_get_subject_name(peercert);
		/* try if this is a user authenticating with a personal certificate */
		n = X509_NAME_get_index_by_NID(subj, NID_pkcs9_emailAddress, -1);
		if (n < 0)
			/* seems not, maybe it is a host authenticating for relaying? */
			n = X509_NAME_get_index_by_NID(subj, NID_commonName, -1);
		if (n >= 0) {
			const ASN1_STRING *s = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subj, n));
			if (s) {
				int l = ASN1_STRING_length(s);
				email.len = (l > 0) ? l : 0;
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L) && !defined(LIBRESSL_VERSION_NUMBER)
				email.s = ASN1_STRING_get0_data(s);
#else
				email.s = M_ASN1_STRING_data(s);
#endif
			}
		}

		if (email.len != 0) {
			unsigned int i;

			for (i = 0; clients[i] != NULL; i++) {
				/* protect against malicious '\0' chars in the cert fields */
				if (strlen(clients[i]) != email.len)
					continue;

				if (strcmp(email.s, clients[i]) == 0) {
					xmitstat.tlsclient = strdup(email.s);

					if (xmitstat.tlsclient == NULL)
						tlsrelay = -ENOMEM;
					else
						tlsrelay = 1;
					break;
				}
			}
		}

		X509_free(peercert);
	} while (0);

	free(clients);
	SSL_set_client_CA_list(ssl, NULL);
	SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);

	return tlsrelay;
}

static int
tls_init()
{
	SSL *myssl;
	SSL_CTX *ctx;
	const char *ciphers = "DEFAULT";
	string saciphers;
	X509_STORE *store;
	X509_LOOKUP *lookup;
	const char ciphfn[] = "tlsserverciphers";
	int j;
	long ssl_options = SSL_OP_SINGLE_DH_USE;

	SSL_library_init();
	STREMPTY(saciphers);

	/* a new SSL context with the bare minimum of options */
	ctx = SSL_CTX_new(SSLv23_server_method());
	if (!ctx) {
		return tls_err("unable to initialize ctx");
	}

	/* disable obsolete and insecure protocol versions */
	SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

	if (!SSL_CTX_use_certificate_chain_file(ctx, certfilename)) {
		SSL_CTX_free(ctx);
		return tls_err("missing certificate");
	}
	SSL_CTX_load_verify_locations(ctx, CLIENTCA, NULL);

	/* crl checking */
	store = SSL_CTX_get_cert_store(ctx);
	if ((lookup = X509_STORE_add_lookup(store, X509_LOOKUP_file())) &&
				(X509_load_crl_file(lookup, CLIENTCRL, X509_FILETYPE_PEM) == 1))
		X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK |
						X509_V_FLAG_CRL_CHECK_ALL);

	saciphers.len = lloadfilefd(openat(controldir_fd, ciphfn, O_RDONLY | O_CLOEXEC), &(saciphers.s), 1);
	if (saciphers.len == (size_t)-1) {
		int e = errno;
		SSL_CTX_free(ctx);
		err_control2("control/", ciphfn);
		errno = e;
		return -1;
	} else if (saciphers.len) {
		/* convert all '\0's except the last one to ':' */
		size_t i;

		for (i = 0; i < saciphers.len - 1; ++i)
			if (!saciphers.s[i])
				saciphers.s[i] = ':';
		ciphers = saciphers.s;

		ssl_options |= SSL_OP_CIPHER_SERVER_PREFERENCE;
	}

	SSL_CTX_set_options(ctx, ssl_options);

	/* a new SSL object, with the rest added to it directly to avoid copying */
	myssl = SSL_new(ctx);
	SSL_CTX_free(ctx);
	if (!myssl) {
		free(saciphers.s);
		return tls_err("unable to initialize ssl");
	}

	SSL_set_verify(myssl, SSL_VERIFY_NONE, NULL);

	/* this will also check whether public and private keys match */
	if (!SSL_use_RSAPrivateKey_file(myssl, certfilename, SSL_FILETYPE_PEM)) {
		ssl_free(myssl);
		free(saciphers.s);
		return tls_err("no valid RSA private key");
	}

	j = SSL_set_cipher_list(myssl, ciphers);
	free(saciphers.s);
	if (j != 1) {
		ssl_free(myssl);
		return tls_err("unable to set ciphers");
	}

	SSL_set_tmp_rsa_callback(myssl, tmp_rsa_cb);
	SSL_set_tmp_dh_callback(myssl, tmp_dh_cb);
	j = SSL_set_rfd(myssl, 0);
	if (j == 1)
		j = SSL_set_wfd(myssl, socketd);
	if (j != 1) {
		ssl_free(myssl);
		return tls_err("unable to set fd");
	}

	/* protection against CVE-2011-1431 */
	sync_pipelining();

	if (netwrite("220 2.0.0 ready for tls\r\n")) {
		ssl_free(myssl);
		return errno;
	}

	/* can't set ssl earlier, else netwrite above would try to send the data encrypted with the unfinished ssl */
	ssl = myssl;
	j = ssl_timeoutaccept(timeout);
	if (j == -ETIMEDOUT) {
		dieerror(ETIMEDOUT);
	} else if (j < 0) {
		/* neither cleartext nor any other response here is part of a standard */
		const char *err = ssl_strerror();

		ssl_free(ssl);
		ssl = NULL;
		return -tls_out("connection failed", err, -EDONE);
	}

	/* have to discard the pre-STARTTLS HELO/EHLO argument, if any */
	return 0;
}

/**
 * initialize STARTTLS mode
 *
 * @return 0 on successful initialization, else error code
 */
int
smtp_starttls(void)
{
	if (ssl || !xmitstat.esmtp)
		return 1;
	return tls_init();
}
