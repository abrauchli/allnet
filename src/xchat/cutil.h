/* cutil.h: interfaces of functions provided by cutil.c */

#ifndef ALLNET_CHAT_UTIL_H
#define ALLNET_CHAT_UTIL_H

#include "chat.h"
#include "lib/keys.h"

/* returns 1 if successful, 0 otherwise */
extern int init_chat_descriptor (struct chat_descriptor * cp, char * contact);

/* send to the contact, returning 1 if successful, 0 otherwise */
/* if src is NULL, source address is taken from get_source, likewise for dst */
/* if so, uses the lesser of s/dbits and the address bits */
/* unless ack_and_save is 0, requests an ack, and after the message is sent,
 * calls save_outgoing. */
extern int send_to_contact (char * data, int dsize, char * contact, int sock,
                            char * src, int sbits, char * dst, int dbits,
                            int hops, int priority, int ack_and_save);

/* same as send_to_contact, but only sends to the one key corresponding
 * to key, and does not save outgoing.  Does request ack, and
 * uses the addresses saved for the contact. */
extern int resend_packet (char * data, int dsize, char * contact, keyset key,
                          int sock, int hops, int priority);

/* the times that follow must be arrays of TIMESTAMP_SIZE chars */

/* if static_result is 0, returned string is statically allocated, should
 * not be free'd and should not be used by multithreaded programs */
/* otherwise, returned string is dynamically allocated, must be free'd */
extern char * chat_time_to_string (unsigned char * t, int static_result);

extern char * chat_descriptor_to_string (struct chat_descriptor * cdp,
                                         int show_id, int static_result);

/* strip most non-alphabetic characters, and convert the rest to uppercase */
extern void normalize_secret (char * s);

/* creates a chat descriptor for a new chat */
extern void new_chat_descriptor (struct chat_descriptor * cd);

/* increments the counter part of the chat descriptor, and sets the time */
extern void update_chat_descriptor (struct chat_descriptor * cd);

/* only sets the time -- used for corrections */
extern void update_chat_descriptor_time (struct chat_descriptor * cd);

/* the chat descriptor stores time with the main part in the first 48 bits,
 * and the time zone (in signed minutes from UTC -- positive is East) in
 * the lower 16 bits */
extern void get_time_tz (uint64_t raw, uint64_t * time, int * tz);

extern uint64_t make_time_tz (uint64_t time, int tz);

#endif /* ALLNET_CHAT_UTIL_H */
