#ifndef __INCLUDE_GUARD_MACROS_H
#define __INCLUDE_GUARD_MACROS_H

/**
 * @file
 * @brief General use macros
 */

/** @brief Abort if var is NULL. */
#define ON_NULL_ABORT(var) \
	if (!var) {        \
		abort();   \
	}

/** @brief Set parameter as unused . */
#define UNUSED_PARAM __attribute__((__unused__))

#endif
