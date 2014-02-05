/* trace.c: standalone application to generate and handle AllNet traces */
/* can be called as daemon (traced) or client (any other name)
/* both the daemon and the client take 1 or two arguments:
   - an address (in hex, with or without separating :,. )
   - optionally, a number of bits of the address we want to send out, in 0..64
 * for the daemon, the specified address is my address, used to fill in
   the response.
     if nbits is negative, only answers if we match the address.
     if nbits is not specified, only forwards trace requests, never
       adds our info to outgoing trace requests
 * for the client, the specified address is the address to trace
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/rsa.h>

#include "../packet.h"
#include "../mgmt.h"
#include "../lib/util.h"
#include "../lib/pipemsg.h"
#include "../lib/priority.h"
#include "../lib/log.h"
#include "../lib/dcache.h"

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
  return -1;
}

static int old_get_byte (char * string, int * offset)
{
  int first = get_nybble (string, offset);
  if (first != -1) {
    int second = get_nybble (string, offset);
    if (second != -1)
      return (first << 8) | second;
  }
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
      return bits;
    bits += new_bits;
    if (new_bits < 8)
      return bits;
    index++;
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
  if (time < Y2K_SECONDS_IN_UNIX) {   /* incorrect clock, or time travel */
    new_entry->precision = 0;
    writeb64 (new_entry->seconds, 0);
    writeb64 (new_entry->seconds_fraction, 0);
  } else {
    unsigned long long int usec = now->tv_usec;
    new_entry->precision = 64 + 3;   /* 3 digits, 1ms precision */
    writeb64 (new_entry->seconds, time - Y2K_SECONDS_IN_UNIX);
    writeb64 (new_entry->seconds_fraction, usec / 1000);
  }
  new_entry->nbits = abits;
  new_entry->hops_seen = hops;
  memcpy (new_entry->address, my_address, ADDRESS_SIZE);
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
                             char * result, int rsize)
{
  snprintf (log_buf, LOG_SIZE, "making trace reply with %d entries, int %d\n",
            num_entries, intermediate);
  log_print ();
  int insize_needed =
    ALLNET_TRACE_REQ_SIZE (inhp->transport, intrp->num_entries, 0);
  if (insize < insize_needed) {
    printf ("error: trace req needs %d, has %d\n", insize_needed, insize);
    return 0;
  }
  int size_needed = ALLNET_TRACE_REPLY_SIZE (0, num_entries);
  if (rsize < size_needed) {
    printf ("error: trace reply needs %d, has %d\n", size_needed, rsize);
    return 0;
  }
  if (num_entries < 1) {
    printf ("error: trace reply num_entries %d < 1 \n", num_entries);
    return 0;
  }
  bzero (result, size_needed);

  struct allnet_header * hp = (struct allnet_header *) result;
  struct allnet_mgmt_header * mp =
    (struct allnet_mgmt_header *) (result + ALLNET_SIZE (0));
  struct allnet_mgmt_trace_reply * trp =
    (struct allnet_mgmt_trace_reply *) (result + ALLNET_MGMT_HEADER_SIZE (0));

  hp->version = ALLNET_VERSION;
  hp->message_type = ALLNET_TYPE_MGMT;
  hp->hops = 0;
  hp->max_hops = inhp->hops + 4;
  hp->src_nbits = abits;
  hp->dst_nbits = inhp->src_nbits;
  hp->sig_algo = ALLNET_SIGTYPE_NONE;
  hp->transport = 0;
  memcpy (hp->source, my_address, (abits + 7) / 8);
  memcpy (hp->destination, inhp->source, ADDRESS_SIZE);

  mp->mgmt_type = ALLNET_MGMT_TRACE_REPLY;

  trp->encrypted = 0;
  trp->intermediate_reply = intermediate;
  trp->num_entries = num_entries;
  memcpy (trp->nonce, intrp->nonce, MESSAGE_ID_SIZE);
  int i;
  /* if num_entries is 1, this loop never executes */
  for (i = 0; i + 1 < num_entries; i++)
    trp->trace [i] = intrp->trace [i];
  struct allnet_mgmt_trace_entry * new_entry = trp->trace + (num_entries - 1);
  init_entry (new_entry, inhp->hops, now, my_address, abits);

  int ksize = readb16 (intrp->pubkey_size);
  if (ksize > 0) {
    printf ("to do: encryption of trace replies\n");
    char * key = ((char *) (intrp->trace)) +
                 (sizeof (struct allnet_mgmt_trace_entry) * intrp->num_entries);
    print_buffer (key, ksize, "key", 15, 1);
  }
  packet_to_string (result, size_needed, "my reply: ", 1, log_buf, LOG_SIZE);
  log_print ();
  return size_needed;
}

static void debug_prt_nonce (void * state, void * n)
{
  print_buffer (n, MESSAGE_ID_SIZE, NULL, MESSAGE_ID_SIZE, 1);
  int offset = * ((int *) state);
  if (offset > 20)
    offset += snprintf (log_buf + offset, LOG_SIZE - offset, ", ");
  offset += buffer_to_string (n, MESSAGE_ID_SIZE, NULL, MESSAGE_ID_SIZE, 0,
                              log_buf + offset, LOG_SIZE - offset);
  * ((int *) state) = offset;
}

static int same_nonce (void * n1, void * n2)
{
  int result = (memcmp (n1, n2, MESSAGE_ID_SIZE) == 0);
/*
  int off = snprintf (log_buf, LOG_SIZE, "same_nonce (");
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
  if (cache_get_match (cache, same_nonce, trp->nonce) != NULL) {
    buffer_to_string (trp->nonce, MESSAGE_ID_SIZE, "duplicate nonce", 5, 1,
                      log_buf, LOG_SIZE);
    log_print ();
    return;     /* duplicate */
  }
  /* else new trace, save it in the cache so we only forward it once */
  cache_add (cache, memcpy_malloc (trp->nonce, MESSAGE_ID_SIZE, "trace nonce"));
/*
  int debug_off = snprintf (log_buf, LOG_SIZE, "cache contains: ");
  printf ("cache contains: ");
  cache_map (cache, debug_prt_nonce, &debug_off);
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
/*
  printf ("matches (");
  print_buffer (my_address, abits, NULL, (abits + 7) / 8, 0);
  printf (", ");
  print_buffer (hp->destination, hp->dst_nbits, NULL,
                (hp->dst_nbits + 7) / 8, 0);
  printf (") => %d (%d needed)\n", nmatch, mbits); */
  if ((forward_only) || ((match_only) && (nmatch < mbits))) {
    /* forward without adding my entry */
    if (! send_pipe_message (sock, message, msize, priority + 1))
      printf ("unable to forward trace response\n");
    snprintf (log_buf, LOG_SIZE, "forwarded %d bytes\n", msize);
  } else {   /* add my entry before forwarding */
    char * new_msg;
    int n = add_my_entry (message, msize, hp, mp, trp, &timestamp,
                          my_address, abits, &new_msg);
    packet_to_string (new_msg, n, "forwarding packet", 1, log_buf, LOG_SIZE);
    log_print ();
    if ((n <= 0) || (! send_pipe_message (sock, new_msg, n, priority + 1)))
      printf ("unable to forward new trace response of size %d\n", n);
    else if (! send_pipe_message (sock, message, msize, priority + 1))
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
  static char response [ALLNET_MTU];
  bzero (response, sizeof (response));
  int rsize = 0;
  if (nmatch >= mbits)  /* exact match, send final response */
    rsize = make_trace_reply (hp, msize, &timestamp, my_address, abits,
                              trp, 0, trp->num_entries + 1,
                              response, sizeof (response));
  else
    rsize = make_trace_reply (hp, msize, &timestamp, my_address, abits,
                              trp, 1, 1,  /* only our intermediate entry */
                              response, sizeof (response));
  if (rsize <= 0)
    return;
  if (! send_pipe_message (sock, response, rsize, EPSILON))
    printf ("unable to send trace response\n");
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
    respond_to_trace (sock, message, found, pri + 1, my_address, nbits,
                      match_only, forward_only, cache);
    free (message);
  }
}

static void send_trace (int sock, char * address, int abits, char * nonce,
                        char * my_address, int my_abits)
{
  static char buffer [ALLNET_MTU];
  bzero (buffer, sizeof (buffer));
  struct allnet_header * hp = (struct allnet_header *) buffer;
  struct allnet_mgmt_header * mp =
    (struct allnet_mgmt_header *) (buffer + ALLNET_SIZE (0));
  struct allnet_mgmt_trace_req * trp =
    (struct allnet_mgmt_trace_req *) (buffer + ALLNET_MGMT_HEADER_SIZE (0));
  int total_size = ALLNET_TRACE_REQ_SIZE (0, 1, 0);
  if (total_size > sizeof (buffer)) {
    printf ("error: need size %d, only have %zd\n", total_size,
            sizeof (buffer));
    exit (1);
  }

  hp->version = ALLNET_VERSION;
  hp->message_type = ALLNET_TYPE_MGMT;
  hp->hops = 0;
  hp->max_hops = 10;
  hp->src_nbits = my_abits;
  hp->dst_nbits = abits;
  hp->sig_algo = ALLNET_SIGTYPE_NONE;
  hp->transport = 0;
  memcpy (hp->source, my_address, ADDRESS_SIZE);
  memcpy (hp->destination, address, ADDRESS_SIZE);

  mp->mgmt_type = ALLNET_MGMT_TRACE_REQ;

  trp->intermediate_replies = 1;
  trp->num_entries = 1;
  /* pubkey_size is 0, so no public key */
  memcpy (trp->nonce, nonce, MESSAGE_ID_SIZE);
  struct timeval time;
  gettimeofday (&time, NULL);
  init_entry (trp->trace, 0, &time, my_address, my_abits);

/*  printf ("sending trace of size %d\n", total_size);
  print_packet (buffer, total_size, "sending trace", 1); */
  /* sending with priority epsilon indicates we only want to send to the
   * trace server, which then forwards to everyone else */
  if (! send_pipe_message (sock, buffer, total_size, EPSILON))
    printf ("unable to send trace message of %d bytes\n", total_size);
}

static unsigned long long int power10 (int n)
{
  if (n < 1)
    return 1;
  return 10 * power10 (n - 1);
}

static struct timeval intermediate_arrivals [256];

static void print_entry (struct allnet_mgmt_trace_entry * entry,
                         struct timeval * start, struct timeval * now,
                         int save_to_intermediate)
{
  int index = (entry->hops_seen) & 0xff;
  if (save_to_intermediate)
    printf ("forwarded by: ");
  printf ("%3d ", index);
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
  if (save_to_intermediate)
    intermediate_arrivals [index] = *now;
  else if (intermediate_arrivals [index].tv_sec != 0)
    delta = delta_us (intermediate_arrivals + index, start);
  printf (" (%lld.%03lldms rtt)", delta / 1000LL, delta % 1000LL);

  printf (" %d", entry->nbits);
  int i;
  for (i = 0; ((i < ADDRESS_SIZE) && (i < (entry->nbits + 7) / 8)); i++)
    printf (".%02x", entry->address [i] % 0xff);
  printf ("\n");
}

static void print_trace_result (struct allnet_mgmt_trace_reply * trp,
                                struct timeval * start,
                                struct timeval * finish)
{
  /* put the unix times into allnet format */
  start->tv_sec -= Y2K_SECONDS_IN_UNIX;
  finish->tv_sec -= Y2K_SECONDS_IN_UNIX;
  if (trp->encrypted) {
    printf ("to do: implement decrypting encrypted trace result\n");
    return;
  }
  if (trp->intermediate_reply == 0) {      /* final reply */
    int i;
    for (i = 1; i < trp->num_entries; i++)
      print_entry (trp->trace + i, start, finish, 0);
  } else if (trp->num_entries == 1) {
    /* generally only one trace entry, so always print the first */
    print_entry (trp->trace, start, finish, 1);
  } else {
    printf ("intermediate response with %d entries\n", trp->num_entries);
  }
}

static void handle_packet (char * message, int msize, char * seeking,
                           struct timeval * start)
{
  int min_size = ALLNET_TRACE_REPLY_SIZE (0, 1);
  if (msize < min_size)
    return;
/*
  print_packet (message, msize, "handle_packet got", 1);
*/
  struct allnet_header * hp = (struct allnet_header *) message;
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
  char * nonce = trp->nonce;
  if (memcmp (nonce, seeking, MESSAGE_ID_SIZE) != 0) {
    printf ("received nonce does not match expected nonce\n");
    print_buffer (seeking, MESSAGE_ID_SIZE, "expected nonce", 100, 1);
    print_buffer (  nonce, MESSAGE_ID_SIZE, "received nonce", 100, 1);
    return;
  }
  struct timeval now;
  gettimeofday (&now, NULL);
/*
  printf ("%ld.%06ld: ", now.tv_sec - Y2K_SECONDS_IN_UNIX, now.tv_usec);
  print_packet (message, msize, "trace reply packet received", 1);
*/
  print_trace_result (trp, start, &now);
}

static void wait_for_responses (int sock, char * nonce, int sec)
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
    handle_packet (message, found, nonce, &start_copy);
    free (message);
    remaining = time (NULL) - start.tv_sec;
  }
  printf ("timeout\n");
}

static void usage (char * pname)
{
  printf ("usage: %s <my_address_in_hex> <number_of_bits>\n", pname);
}

int main (int argc, char ** argv)
{
  if (argc < 2) {
    usage (argv [0]);
    return 1;
  }

  char address [100];
  bzero (address, sizeof (address));  /* set unused part to all zeros */
  int asize = get_address (argv [1], address, sizeof (address));
  if (asize <= 0) {
    usage (argv [0]);
    return 1;
  }
  int nbits = asize;
  int match_only = 0;
  if (argc >= 3) {
    char * end;
    int abits = strtol (argv [2], &end, 10);
    if (abits < 0) {
      match_only = 1;
      abits = - abits;
    }
    if ((end != argv [2]) && (abits > 0) && (abits < nbits))
      nbits = abits;
  }

  int sock = connect_to_local (argv [0]);
  if (sock < 0)
    return 1;
  add_pipe (sock);
/* print_buffer (address, nbits, "argument address", 8, 1); */

  if (strstr (argv [0], "traced") != NULL) {  /* called as daemon */
    main_loop (sock, address, nbits, match_only, argc < 3);
    printf ("trace error: main loop returned\n");
  } else {                                    /* called as client */
    char nonce [MESSAGE_ID_SIZE];
    char my_addr [ADDRESS_SIZE];
    random_bytes (nonce, sizeof (nonce));
    random_bytes (my_addr, sizeof (my_addr));
    send_trace (sock, address, nbits, nonce, my_addr, 5);
    wait_for_responses (sock, nonce, 60);
  }
}
