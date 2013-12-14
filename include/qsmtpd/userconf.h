/** \file vpop.h
 \brief definition of the helper functions around the user configuration
 */
#ifndef USERCONF_H
#define USERCONF_H

#include <sstring.h>

struct userconf {
	string domainpath;		/**< Path of the domain for domain settings */
	string userpath;		/**< Path of the user directory where the user stores it's own settings */
	char **userconf;		/**< contents of the "filterconf" file in user directory (or NULL) */
	char *ucbuf;			/**< buffer for userconf array (or NULL), should not be accessed directly */
	char **domainconf;		/**< dito for domain directory */
	char *dcbuf;			/**< buffer for domainconf array (or NULL), should not be accessed directly */
};

/**
 * @brief initialize the struct userconf
 * @param ds the struct to initialize
 *
 * All fields of the struct are reset to a safe invalid value.
 */
void userconf_init(struct userconf *ds) __attribute__ ((nonnull (1)));

/**
 * @brief free all information in a struct userconf
 * @param ds the struct to clear
 *
 * This will not free the struct itself so it is safe to use a static or
 * stack allocated struct. It will reset all values to a safe value so
 * the struct can be reused.
 */
void userconf_free(struct userconf *ds) __attribute__ ((nonnull (1)));

/**
 * @brief load the filter settings for user and domain
 * @param ds the userconf buffer to hold the information
 * @return if filters were successfully loaded or error code
 * @retval 0 filters were loaded (or no configuration is present)
 */
int userconf_load_configs(struct userconf *ds) __attribute__ ((nonnull (1)));

#endif
