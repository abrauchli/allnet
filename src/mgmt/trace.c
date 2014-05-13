/* trace.c: standalone application to generate and handle AllNet traces */
/* can be called as daemon (traced) or client (any other name)
 * both the daemon and the client may take as argument:
   - an address (in hex, with or without separating :,. )
   - optionally, followed by / and the number of bits of the address, in 0..64
   the argument and bits default to 0/0 if not specified
 * for the daemon, the specified address is my address, used to fill in
   the response.
 * the daemon will optionally take a '-m' option, to specify tracing
   only when we match the address.
 * for the client, the specified address is the address to trace
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/rsa.h>

#include "lib/packet.h"
#include "lib/mgmt.h"
#include "lib/util.h"
#include "lib/pipemsg.h"
#include "lib/priority.h"
#include "lib/log.h"
#include "lib/dcache.h"

static int get_nybble (char * string, int * offset)
{
  char * p = string + *offset;
  while ((*p == ':') || (*p == ',') || (*p == '.'))
    p++;
  *offset = (p + 1) - string;
  if ((*p >= '0') && (*p <= '9'))
    return *p - '0';
  if ((*p >= 'a') && (*p <= 'f'))
    return 10 + *p - 'a';
  if ((*p >= 'A') && (*p <= 'F'))
    return 10 + *p - 'A';
  *offset = p - string;   /* point to the offending character */
  return -1;
}

static int get_byte (char * string, int * offset, char * result)
{
  int first = get_nybble (string, offset);
  if (first == -1)
    return 0;
  *result = (first << 4);
  int second = get_nybble (string, offset);
  if (second == -1)
      return 4;
  *result = (first << 4) | second;
  /* printf ("get_byte returned %x\n", (*result) & 0xff); */
  return 8;
}

static int get_address (char * address, char * result, int rsize)
{
  int offset = 0;
  int value = 0;
  int index = 0;
  int bits = 0;
  while (index < rsize) {
    int new_bits = get_byte (address, &offset, result + index);
    if (new_bits <= 0)
      break;
    bits += new_bits;
    if (new_bits < 8)
      break;
    index++;
  }
  if (address [offset] == '/') { /* number of bits follows */
    char * end;
    int given_bits = strtol (address + offset + 1, &end, 10);
    if ((end != address + offset + 1) && (given_bits <= bits))
      bits = given_bits;
  }
  return bits;
}

static void callback (int type, int count, void * arg)
{
  if (type == 0)
    printf (".");
  else if (type == 1)
    printf (",");
  else if (type == 2)
    printf ("!");
  else if (type == 3)
    printf (":");
  else
    printf ("?");
  fflush (stdout);
}
  
static void init_entry (struct allnet_mgmt_trace_entry * new_entry, int hops,
                        struct timeval * now, char * my_address, int abits)
{
  unsigned long long int time = now->tv_sec;
  if (time < ALLNET_Y2K_SECONDS_IN_UNIX) {
    /* incorrect clock, or time travel */
    new_entry->precision = 0;
    writeb64 (new_entry->seconds, 0);
    writeb64 (new_entry->seconds_fraction, 0);
  } else {
    unsigned long long int usec = now->tv_usec;
    if (hops == 0) {  /* special case: microsecond precision */
      new_entry->precision = 64 + 6;   /* 6 digits, 1us precision */
      writeb64 (new_entry->seconds, time - ALLNET_Y2K_SECONDS_IN_UNIX);
      writeb64 (new_entry->seconds_fraction, usec);
    } else {   /* the common case */
      new_entry->precision = 64 + 3;   /* 3 digits, 1ms precision */
      writeb64 (new_entry->seconds, time - ALLNET_Y2K_SECONDS_IN_UNIX);
      writeb64 (new_entry->seconds_fraction, usec / 1000);
    }
  }
  new_entry->nbits = abits;
  new_entry->hops_seen = hops;
  memcpy (new_entry->address, my_address, (abits + 7) / 8);
}

static int add_my_entry (char * in, int insize, struct allnet_header * inhp,
                         struct allnet_mgmt_header * inmp,
                         struct allnet_mgmt_trace_req * intrp,
                         struct timeval * now, char * my_address, int abits,
                         char * * result)
{
  *result = NULL;
  if (intrp->num_entries >= 255)
    return 0;

  int n = intrp->num_entries + 1;
  int t = inhp->transport;
  int k = readb16 (intrp->pubkey_size);
  int needed = ALLNET_TRACE_REQ_SIZE (t, n, k);
  *result = calloc (needed, 1);
  if (*result == NULL) {
    printf ("add_my_entry unable to allocate %d bytes for %d\n", needed, n);
    return 0;
  }
  packet_to_string (in, insize, "add_my_entry original packet", 1,
                    log_buf, LOG_SIZE);
  log_print ();

  /* can copy the header verbatim, and all of the trace request
   * except the pubkey */
  int copy_size = ALLNET_TRACE_REQ_SIZE (t, intrp->num_entries, 0);
  memcpy (*result, in, copy_size);
  
  struct allnet_mgmt_trace_req * trp =
    (struct allnet_mgmt_trace_req *)
      ((*result) + ALLNET_MGMT_HEADER_SIZE (t));
  trp->num_entries = n;
  struct allnet_mgmt_trace_entry * new_entry = trp->trace + (n - 1);
  init_entry (new_entry, inhp->hops, now, my_address, abits);
  if (k > 0) {
    char * inkey = ((char *) (intrp->trace)) +
                   (sizeof (struct allnet_mgmt_trace_entry) * (n - 1));
    char * key = ((char *) (trp->trace)) +
                 (sizeof (struct allnet_mgmt_trace_entry) * n);
    memcpy (key, inkey, k);
  }
  packet_to_string (*result, needed, "add_my_entry packet copy", 1,
                    log_buf, LOG_SIZE);
  log_print ();
  return needed;
}

/* returns the size of the message to send, or 0 in case of failure */
/* no encryption yet */
static int make_trace_reply (struct allnet_header * inhp, int insize,
                             struct timeval * now, char * my_address, int abits,
                             struct allnet_mgmt_trace_req * intrp,
                             int intermediate, int num_entries,
                             char ** result)
{
  *result = NULL;
  snprintf (log_buf, LOG_SIZE, "making trace reply with %d entries, int %d\n",
            num_entries, intermediate);
  log_print ();
  int insize_needed =
    ALLNET_TRACE_REQ_SIZE (inhp->transport, intrp->num_entries, 0);
  if (insize < insize_needed) {
    printf ("error: trace req needs %d, has %d\n", insize_needed, insize);
    return 0;
  }
  if (num_entries < 1) {
    printf ("error: trace reply num_entries %d < 1 \n", num_entries);
    return 0;
  }
  int size_needed = ALLNET_TRACE_REPLY_SIZE (0, num_entries);
  int total;
  struct allnet_header * hp =
    create_packet (size_needed - ALLNET_HEADER_SIZE, ALLNET_TYPE_MGMT,
                   inhp->hops + 4, ALLNET_SIGTYPE_NONE, my_address, abits,
                   inhp->source, inhp->src_nbits, NULL, &total);
  if ((hp == NULL) || (total != size_needed)) {
    printf ("hp is %p, total is %d, size_needed %d\n", hp, total, size_needed);
    return 0;
  }
  *result = (char *) hp;

  struct allnet_mgmt_header * mp =
    (struct allnet_mgmt_header *) (ALLNET_DATA_START(hp, hp->transport, total));
  mp->mgmt_type = ALLNET_MGMT_TRACE_REPLY;

  struct allnet_mgmt_trace_reply * trp =
    (struct allnet_mgmt_trace_reply *)
      (((char *) mp) + (sizeof (struct allnet_mgmt_header)));

  trp->encrypted = 0;
  trp->intermediate_reply = intermediate;
  trp->num_entries = num_entries;
  memcpy (trp->trace_id, intrp->trace_id, MESSAGE_ID_SIZE);
  int i;
  /* if num_entries is 1, this loop never executes */
  /* if num_entries is 2, this loop executes once to copy
   * intrp->trace [intrp->num_entries - 1] to trp->trace [0] */
  for (i = 0; i + 1 < num_entries; i++)
    trp->trace [i] = intrp->trace [i + intrp->num_entries - (num_entries - 1)];
  struct allnet_mgmt_trace_entry * new_entry = trp->trace + (num_entries - 1);
  init_entry (new_entry, inhp->hops, now, my_address, abits);

  int ksize = readb16 (intrp->pubkey_size);
  if (ksize > 0) {
    printf ("to do: encryption of trace replies\n");
    char * key = ((char *) (intrp->trace)) +
                 (sizeof (struct allnet_mgmt_trace_entry) * intrp->num_entries);
    print_buffer (key, ksize, "key", 15, 1);
  }
  packet_to_string (*result, total, "my reply: ", 1, log_buf, LOG_SIZE);
  log_print ();
  return size_needed;
}

static void debug_prt_trace_id (void * state, void * n)
{
  print_buffer (n, MESSAGE_ID_SIZE, NULL, MESSAGE_ID_SIZE, 1);
  int offset = * ((int *) state);
  if (offset > 20)
    offset += snprintf (log_buf + offset, LOG_SIZE - offset, ", ");
  offset += buffer_to_string (n, MESSAGE_ID_SIZE, NULL, MESSAGE_ID_SIZE, 0,
                              log_buf + offset, LOG_SIZE - offset);
  * ((int *) state) = offset;
}

static int same_trace_id (void * n1, void * n2)
{
  int result = (memcmp (n1, n2, MESSAGE_ID_SIZE) == 0);
/*
  int off = snprintf (log_buf, LOG_SIZE, "same_trace_id (");
  off += buffer_to_string (n1, MESSAGE_ID_SIZE, NULL, MESSAGE_ID_SIZE, 0,
                           log_buf + off, LOG_SIZE - off);
  off += snprintf (log_buf + off, LOG_SIZE - off, ", ");
  off += buffer_to_string (n2, MESSAGE_ID_SIZE, NULL, MESSAGE_ID_SIZE, 0,
                           log_buf + off, LOG_SIZE - off);
  off += snprintf (log_buf + off, LOG_SIZE - off, ") => %d\n", result);
  log_print ();
*/
  return result;
}

static void acknowledge_bcast (int sock, char * message, int msize)
{
  /* ignore any packet other than unencrypted packets requesting an ack */
  if (msize <= ALLNET_HEADER_SIZE)
    return;
  struct allnet_header * hp = (struct allnet_header *) message;
  if ((hp->message_type != ALLNET_TYPE_CLEAR) ||
      (hp->transport & ALLNET_TRANSPORT_ACK_REQ == 0))  /* ignore */
    return;
  int hsize = ALLNET_SIZE (hp->transport);
  if (msize < hsize + MESSAGE_ID_SIZE)
    return;
  int asize;
  struct allnet_header * ack = create_ack (hp, message + hsize,
                                           NULL, 0, &asize);
  if ((asize == 0) || (ack == NULL))
    return;
  if (! send_pipe_message_free (sock, (char *) ack, asize,
                                ALLNET_PRIORITY_DEFAULT_LOW))
    printf ("unable to send trace response\n");
}

static void respond_to_trace (int sock, char * message, int msize,
                              int priority, char * my_address, int abits,
                              int match_only, int forward_only,
                              void * cache)
{
  /* ignore any packet other than valid trace requests with at least 1 entry */
  if (msize <= ALLNET_HEADER_SIZE)
    return;
  snprintf (log_buf, LOG_SIZE, "got %d bytes, %d %d\n", msize,
            match_only, forward_only);
  log_print ();
  packet_to_string (message, msize, "respond_to_trace", 1, log_buf, LOG_SIZE);
  log_print ();
  struct allnet_header * hp = (struct allnet_header *) message;
  if ((hp->message_type != ALLNET_TYPE_MGMT) ||
      (msize < ALLNET_TRACE_REQ_SIZE (hp->transport, 1, 0)))
    return;
/* snprintf (log_buf, LOG_SIZE, "survived msize %d/%zd\n", msize,
            ALLNET_TRACE_REQ_SIZE (hp->transport, 1, 0));
  log_print (); */
  struct allnet_mgmt_header * mp =
    (struct allnet_mgmt_header *) (message + ALLNET_SIZE (hp->transport));
  if (mp->mgmt_type != ALLNET_MGMT_TRACE_REQ)
    return;
  struct allnet_mgmt_trace_req * trp =
    (struct allnet_mgmt_trace_req *)
      (message + ALLNET_MGMT_HEADER_SIZE (hp->transport));
  int n = (trp->num_entries & 0xff);
  int k = readb16 (trp->pubkey_size);
/* snprintf (log_buf, LOG_SIZE, "packet has %d entries %d key, size %d/%zd\n",
            n, k, msize, ALLNET_TRACE_REQ_SIZE (hp->transport, n, k));
  log_print (); */
  if ((n < 1) || (msize < ALLNET_TRACE_REQ_SIZE (hp->transport, n, k)))
    return;

  /* found a valid trace request */
  if (cache_get_match (cache, same_trace_id, trp->trace_id) != NULL) {
    buffer_to_string (trp->trace_id, MESSAGE_ID_SIZE, "duplicate trace_id",
                      5, 1, log_buf, LOG_SIZE);
    log_print ();
    return;     /* duplicate */
  }
  /* else new trace, save it in the cache so we only forward it once */
  cache_add (cache, memcpy_malloc (trp->trace_id, MESSAGE_ID_SIZE, "trace id"));
/*
  int debug_off = snprintf (log_buf, LOG_SIZE, "cache contains: ");
  printf ("cache contains: ");
  cache_map (cache, debug_prt_trace_id, &debug_off);
  debug_off += snprintf (log_buf + debug_off, LOG_SIZE - debug_off, "\n");
  log_print ();
*/

  struct timeval timestamp;
  gettimeofday (&timestamp, NULL);

  /* do two things: forward the trace, and possibly respond to the trace. */

  int mbits = abits;
  if (mbits > hp->dst_nbits)
    mbits = hp->dst_nbits;   /* min of abits, and hp->dst_nbits */
  int nmatch = matches (my_address, abits, hp->destination, hp->dst_nbits);
#ifdef DEBUG_PRINT
  printf ("matches (");
  print_buffer (my_address, abits, NULL, (abits + 7) / 8, 0);
  printf (", ");
  print_buffer (hp->destination, hp->dst_nbits, NULL,
                (hp->dst_nbits + 7) / 8, 0);
  printf (") => %d (%d needed)\n", nmatch, mbits);
#endif /* DEBUG_PRINT */
  /* when forwarding, use a low priority > epsilon, to tell ad it is from us */
  int fwd_priority = ALLNET_PRIORITY_TRACE_FWD;
  if ((forward_only) || ((match_only) && (nmatch < mbits))) {
    /* forward without adding my entry */
    if (! send_pipe_message (sock, message, msize, fwd_priority))
      printf ("unable to forward trace response\n");
    snprintf (log_buf, LOG_SIZE, "forwarded %d bytes\n", msize);
  } else {   /* add my entry before forwarding */
    char * new_msg;
    int n = add_my_entry (message, msize, hp, mp, trp, &timestamp,
                          my_address, abits, &new_msg);
    packet_to_string (new_msg, n, "forwarding packet", 1, log_buf, LOG_SIZE);
    log_print ();
    if ((n <= 0) || (! send_pipe_message (sock, new_msg, n, fwd_priority)))
      printf ("unable to forward new trace response of size %d\n", n);
    else if (! send_pipe_message (sock, message, msize, fwd_priority))
      printf ("unable to forward old trace response\n");
    snprintf (log_buf, LOG_SIZE, "added and forwarded %d %d\n", n, msize);
    if (new_msg != NULL)
      free (new_msg);
  }
  log_print ();
  if ((forward_only) || ((match_only) && (nmatch < mbits)) ||
      (! trp->intermediate_replies))   /* do not reply, we are done */
    return;

  /* RSVP, the favor of your reply is requested -- respond to the trace */
  char * response;
  int rsize = 0;
  if (nmatch >= mbits)  /* exact match, send final response */
    rsize = make_trace_reply (hp, msize, &timestamp, my_address, abits,
                              trp, 0, trp->num_entries + 1, &response);
  else if (hp->hops > 0) /* not my local sender, send back 2 trace entries */
    rsize = make_trace_reply (hp, msize, &timestamp, my_address, abits,
                              trp, 1, 2, &response);
  else   /* my local sender, send a 1-entry response */
    rsize = make_trace_reply (hp, msize, &timestamp, my_address, abits,
                              trp, 1, 1, &response);
  if (rsize <= 0)
    return;
/* trace messages go with the lowest possible priority */
  if (! send_pipe_message (sock, response, rsize, ALLNET_PRIORITY_TRACE))
    printf ("unable to send trace response\n");
  free (response);
}

static void main_loop (int sock, char * my_address, int nbits,
                       int match_only, int forward_only)
{
  void * cache = cache_init (100, free);
  while (1) {
    char * message;
    int pipe, pri;
    int timeout = PIPE_MESSAGE_WAIT_FOREVER;
    int found = receive_pipe_message_any (timeout, &message, &pipe, &pri);
    if (found < 0) {
      printf ("pipe closed, exiting\n");
      exit (1);
    }
#ifdef DEBUG_PRINT
    print_packet (message, found, "received", 1);
#endif /* DEBUG_PRINT */
    acknowledge_bcast (sock, message, found);
    respond_to_trace (sock, message, found, pri + 1, my_address, nbits,
                      match_only, forward_only, cache);
    free (message);
  }
}

static void send_trace (int sock, char * address, int abits, char * trace_id,
                        char * my_address, int my_abits)
{
  int total_size = ALLNET_TRACE_REQ_SIZE (0, 1, 0);
  int data_size = total_size - ALLNET_HEADER_SIZE;
  int allocated = 0;
  struct allnet_header * hp =
    create_packet (data_size, ALLNET_TYPE_MGMT, 10, ALLNET_SIGTYPE_NONE,
                   my_address, my_abits, address, abits, NULL, &allocated);
  if (allocated != total_size) {
    printf ("error in send_trace: %d %d %d\n", allocated,
            total_size, data_size);
    if (hp != NULL)
      free (hp);
    return;
  }

  char * buffer = (char *) hp;
  struct allnet_mgmt_header * mp =
    (struct allnet_mgmt_header *) (buffer + ALLNET_SIZE (hp->transport));
  struct allnet_mgmt_trace_req * trp =
    (struct allnet_mgmt_trace_req *)
      (buffer + ALLNET_MGMT_HEADER_SIZE (hp->transport));

  mp->mgmt_type = ALLNET_MGMT_TRACE_REQ;

  trp->intermediate_replies = 1;
  trp->num_entries = 1;
  writeb16 (trp->pubkey_size, 0);
  /* pubkey_size is 0, so no public key */
  memcpy (trp->trace_id, trace_id, MESSAGE_ID_SIZE);
  struct timeval time;
  gettimeofday (&time, NULL);
  init_entry (trp->trace, 0, &time, my_address, my_abits);

/*  print_packet (buffer, total_size, "sending trace", 1); */
  snprintf (log_buf, LOG_SIZE, "sending trace of size %d\n", total_size);
  log_print ();
  /* sending with priority epsilon indicates to ad that we only want to
   * send to the trace server, which then forwards to everyone else */
  if (! send_pipe_message (sock, buffer, total_size, ALLNET_PRIORITY_TRACE))
    printf ("unable to send trace message of %d bytes\n", total_size);
  free (hp);
}

static unsigned long long int power10 (int n)
{
  if (n < 1)
    return 1;
  return 10 * power10 (n - 1);
}

static struct timeval intermediate_arrivals [256];

static void print_times (struct allnet_mgmt_trace_entry * entry,
                         struct timeval * start, struct timeval * now,
                         int save_to_intermediate)
{
  int index = (entry->hops_seen) & 0xff;
/* printf ("print_times index %d time %d.%06d, save %d\n",
index, now->tv_sec, now->tv_usec, save_to_intermediate); */
  if ((start != NULL) && (now != NULL)) {
    unsigned long long int fraction = readb64 (entry->seconds_fraction);
    if (entry->precision <= 64)
      fraction = fraction / (((unsigned long long int) (-1LL)) / 1000000LL);
    else if (entry->precision <= 70)  /* decimal in low-order bits */
      fraction = fraction * (power10 (70 - entry->precision));
    else
      fraction = fraction / (power10 (entry->precision - 70));
    if (fraction >= 1000000LL) {  /* should be converted to microseconds */
      printf ("error: fraction (%u) %lld gives %lld >= 1000000 microseconds\n",
              entry->precision, readb64 (entry->seconds_fraction), fraction);
      fraction = 0LL;
    }
    struct timeval timestamp;
    timestamp.tv_sec = readb64 (entry->seconds);
    timestamp.tv_usec = fraction;
    unsigned long long int delta = delta_us (&timestamp, start);
  /* printf ("%ld.%06ld - %ld.%06ld = %lld\n",
          timestamp.tv_sec, timestamp.tv_usec,
          start->tv_sec, start->tv_usec, delta); */
    printf (" %6lld.%03lldms", delta / 1000LL, delta % 1000LL);
  
    delta = delta_us (now, start);
    if ((save_to_intermediate) && (intermediate_arrivals [index].tv_sec == 0))
      intermediate_arrivals [index] = *now;
    else if (intermediate_arrivals [index].tv_sec != 0)
      delta = delta_us (intermediate_arrivals + index, start);
    printf (" timestamp, %6lld.%03lldms rtt,", delta / 1000LL, delta % 1000LL);
  }
}

static void print_entry (struct allnet_mgmt_trace_entry * entry,
                         struct timeval * start, struct timeval * now,
                         int save_to_intermediate, int print_eol)
{
  int index = (entry->hops_seen) & 0xff;
  printf ("%3d ", index);

  if (entry->nbits > 0)
    printf ("%02x", entry->address [0] % 0xff);
  int i;
  for (i = 1; ((i < ADDRESS_SIZE) && (i < (entry->nbits + 7) / 8)); i++)
    printf (".%02x", entry->address [i] % 0xff);
  printf ("/%d", entry->nbits);

  if (print_eol)
    printf ("\n");
}

static void print_trace_result (struct allnet_mgmt_trace_reply * trp,
                                struct timeval * start,
                                struct timeval * finish)
{
  /* put the unix times into allnet format */
  start->tv_sec -= ALLNET_Y2K_SECONDS_IN_UNIX;
  finish->tv_sec -= ALLNET_Y2K_SECONDS_IN_UNIX;
  if (trp->encrypted) {
    printf ("to do: implement decrypting encrypted trace result\n");
    return;
  }
  if (trp->intermediate_reply == 0) {      /* final reply */
    if (trp->num_entries > 1) {
      printf ("trace to matching destination:\n");
      int i;
      for (i = 1; i < trp->num_entries; i++) {
        printf ("         ");
        print_times (trp->trace + i, start, finish, 1);
        print_entry (trp->trace + i, start, finish, 0, 1);
      }
    }
  } else if (trp->num_entries == 2) {
    /* generally two trace entries for intermediate replies */
    printf ("forward: ");
    print_times (trp->trace + 1, start, finish, 1);
    print_entry (trp->trace + 0, NULL, NULL, 0, 0);
    printf ("  to");
    print_entry (trp->trace + 1, start, finish, 1, 1);
  } else if (trp->num_entries == 1) {
    /* generally only one trace entry, so always print the first */
    printf ("local:   ");
    print_times (trp->trace, start, finish, 1);
    print_entry (trp->trace, start, finish, 1, 1);
  } else {
    printf ("intermediate response with %d entries\n", trp->num_entries);
  }
}

static void handle_packet (char * message, int msize, char * seeking,
                           struct timeval * start)
{
/* print_packet (message, msize, "handle_packet got", 1); */
  if (! is_valid_message (message, msize))
    return;
  struct allnet_header * hp = (struct allnet_header *) message;
    

  int min_size = ALLNET_TRACE_REPLY_SIZE (0, 1);
  if (msize < min_size)
    return;
  if (hp->message_type != ALLNET_TYPE_MGMT)
    return;
  min_size = ALLNET_TRACE_REPLY_SIZE (hp->transport, 1);
  if (msize < min_size)
    return;

  struct allnet_mgmt_header * mp =
    (struct allnet_mgmt_header *) (message + ALLNET_SIZE (hp->transport));
  if (mp->mgmt_type != ALLNET_MGMT_TRACE_REPLY)
    return;

  struct allnet_mgmt_trace_reply * trp =
    (struct allnet_mgmt_trace_reply *)
      (message + ALLNET_MGMT_HEADER_SIZE (hp->transport));
  char * trace_id = trp->trace_id;
  if (memcmp (trace_id, seeking, MESSAGE_ID_SIZE) != 0) {
    printf ("received trace_id does not match expected trace_id\n");
    print_buffer (seeking , MESSAGE_ID_SIZE, "expected trace_id", 100, 1);
    print_buffer (trace_id, MESSAGE_ID_SIZE, "received trace_id", 100, 1);
    return;
  }
  struct timeval now;
  gettimeofday (&now, NULL);
/*
  printf ("%ld.%06ld: ", now.tv_sec - ALLNET_Y2K_SECONDS_IN_UNIX, now.tv_usec);
  print_packet (message, msize, "trace reply packet received", 1);
*/
  print_trace_result (trp, start, &now);
}

static void wait_for_responses (int sock, char * trace_id, int sec)
{
  int i;
  for (i = 0; i < 256; i++) {
    intermediate_arrivals [i].tv_sec = 0;
    intermediate_arrivals [i].tv_usec = 0;
  }

  struct timeval start;
  gettimeofday (&start, NULL);
  int remaining = time (NULL) - start.tv_sec;
  while (remaining < sec) {
    int pipe;
    int pri;
    char * message;
    int ms = remaining * 1000 + 999;
    int found = receive_pipe_message_any (ms, &message, &pipe, &pri);
    if (found <= 0) {
      printf ("trace pipe closed, exiting\n");
      exit (1);
    }
    struct timeval start_copy = start;
    handle_packet (message, found, trace_id, &start_copy);
    free (message);
    remaining = time (NULL) - start.tv_sec;
  }
  printf ("timeout\n");
}

static void usage (char * pname, int daemon)
{
  if (daemon) {
    printf ("usage: %s [-m] [<my_address_in_hex>[/<number_of_bits>]]\n", pname);
    printf ("       -m specifies tracing only when we match the address\n"); 
  } else {
    printf ("usage: %s [<my_address_in_hex>[/<number_of_bits>]]\n", pname);
  }
}

int main (int argc, char ** argv)
{
  int is_daemon = 0;
  if (strstr (argv [0], "traced") != NULL)  /* called as daemon */
    is_daemon = 1;

  int match_only = 0;
  int i, j;
  for (i = 1; i < argc; i++) {
    if (match_only) {
      argv [i] = argv [i + 1];
    } else if (strcmp (argv [i], "-m") == 0) {
      match_only = 1;
      argc--;
    }
   }
      
  if (argc > 2) {
    usage (argv [0], is_daemon);
    return 1;
  }

  char address [100];
  bzero (address, sizeof (address));  /* set unused part to all zeros */
  int abits = 0;
  if (argc > 1) {
    abits = get_address (argv [1], address, sizeof (address));
    if (abits <= 0) {
      usage (argv [0], is_daemon);
      return 1;
    }
  }

  int sock = connect_to_local (argv [0], argv [0]);
  if (sock < 0)
    return 1;
  add_pipe (sock);
/* print_buffer (address, abits, "argument address", 8, 1); */

  if (is_daemon) {     /* called as daemon */
#ifdef DEBUG_PRINT
    printf ("trace daemon (m %d) for %d bits: ", match_only, abits);
    print_bitstring (address, 0, abits, 1);
#endif /* DEBUG_PRINT */
    main_loop (sock, address, abits, match_only, 0);
    printf ("trace error: main loop returned\n");
  } else {                                    /* called as client */
#ifdef DEBUG_PRINT
    printf ("tracing %d bits: ", abits);
    print_bitstring (address, 0, abits, 1);
#endif /* DEBUG_PRINT */
    char trace_id [MESSAGE_ID_SIZE];
    char my_addr [ADDRESS_SIZE];
    random_bytes (trace_id, sizeof (trace_id));
    random_bytes (my_addr, sizeof (my_addr));
    send_trace (sock, address, abits, trace_id, my_addr, 5);
    wait_for_responses (sock, trace_id, 60);
  }
}
