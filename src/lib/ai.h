/* ai.h: utility functions for struct addr_info and struct internet_addr */

#include "packet.h"
#include "mgmt.h"

#ifndef ALLNET_AI_H
#define ALLNET_AI_H

/* prints a newline at the end of the address info */
extern void print_addr_info (struct addr_info * ai);
/* includes a newline at the end of the address info */
extern int addr_info_to_string (struct addr_info * ai, char * buf, int bsize);

/* sap must point to at least sizeof (struct sockaddr_in6) bytes */
/* returns 1 for success, 0 for failure */
extern int ai_to_sockaddr (struct addr_info * ai, struct sockaddr * sap);

extern int sockaddr_to_ai (struct sockaddr * sap, int addr_size,
                           struct addr_info * ai);

/* prints a newline at the end of the internet address */
extern void print_ia (struct internet_addr * ia);
/* includes a newline at the end of the internet address */
extern int ia_to_string (const struct internet_addr * ia,
                         char * buf, int bsize);

/* sap must point to at least sizeof (struct sockaddr_in6) bytes */
/* returns 1 for success, 0 for failure */
extern int ia_to_sockaddr (struct internet_addr * ia, struct sockaddr * sap);

extern int sockaddr_to_ia (struct sockaddr * sap, int addr_size,
                           struct internet_addr * ia);

/* addr must point to 4 bytes if af is AF_INET, 16 bytes for AF_INET6 */
/* if nbits > 0, dest should point to at least (nbits + 7) / 8 bytes */
/* returns 1 for success, 0 for failure */
extern int init_ai (int af, char * addr, int port, int nbits, char * dest,
                    struct addr_info * ai);

/* returns 1 if the two addresses are the same, 0 otherwise */
extern int same_ai (struct addr_info * a, struct addr_info * b);

/* if this is an IPv4-encoded-as-IPv6 address, make it an IPv4 address again */
extern void standardize_ip (struct sockaddr * ap, socklen_t asize);

#endif /* ALLNET_AI_H */
