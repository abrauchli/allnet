/* app_util.h: utilities for apps */

#ifndef ALLNET_APP_UTIL_H
#define ALLNET_APP_UTIL_H

/* returns a TCP socket used to send messages to the allnet daemon
 * (specifically, alocal) or receive messages from alocal
 * returns -1 in case of failure
 * arg0 is the first argument that main gets -- useful for finding binaries
 * the application MUST receive messages, even if it ignores them all.
 * otherwise, after a while (once the buffer is full) allnet/alocal
 * will close the socket. */
extern int connect_to_local (char * program_name, char * arg0);

/* retrieve or request a public key.
 *
 * if successful returns the key length and sets *key to point to
 * statically allocated storage for the key (do not modify in any way)
 * if not successful, returns 0.
 *
 * max_time_ms and max_hops are only used if the address has not
 * been seen before.  If so, a key request is sent with max_hops, and
 * we wait at most max_time_ms (or quit after receiving max_keys).
 */
extern unsigned int get_bckey (char * address, char ** key,
                               int max_time_ms, int max_keys, int max_hops);


#endif /* ALLNET_APP_UTIL_H */
