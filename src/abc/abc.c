/* abc.c: get allnet messages from ad, broadcast them to one interface
 *        get allnet messages from the interface, forward them to ad
 * abc stands for (A)llNet (B)road(C)ast
 *
 * single-threaded, uses select to check the pipe from ad and the interface
 * may have to be run with supervisory/root privileges depending on the chosen
 * interface driver (wifi requires root)
 * arguments are:
 * - the fd number of the pipe from ad
 * - the fd number of the pipe to ad
 * - the interface name and optionally interface driver and driver options
 *  e.g. eth0/ip, wlan0/wifi, or wlan0/wifi,nm
 *  Available drivers are ip and wifi, ip being the default.
 *    ip    does not require root but can only be used on interfaces that are
 *          already connected to an IP network.
 *    wifi  Sets up or connects to the adhoc network "allnet" running on
 *          channel 1.
 *          iw: When no further parameters are given the "iw" driver is
 *          used which uses the iw command to attach and detach the interface.
 *          When NetworkManager support is enabled at compile time, it is used
 *          to disable NetworkManager on given interface before using iw.
 *          iw requires root privileges.
 *          nm: With the "nm" driver, NetworkManager is used to connect the
 *          interface to the "allnet" adhoc network. This driver does not
 *          require root privileges for managing the interface but is much
 *          slower (ca. 20s to connect.)
 *          TODO: nm still requires root because of the raw socket (AF_PACKET)
 */

/* TODO: config file "abc" "interface-name" (e.g. ~/.allnet/abc/wlan0)
 * gives the maximum fraction of time the interface should be turned
 * on for allnet ad-hoc traffic.
 * if not found, the maximum fraction is 1 percent, i.e. 0.01
 * this fraction only applies to messages with priority <= 0.5.
 */

/*
 * For managed interfaces (wifi interface driver) there is a 5s basic cycle and
 * two modes:
 * - sending data I care about (priority greater than 0.5)
 * - energy saving mode
 * In either mode, I send a beacon at a random point in time during
 * each cycle, then listen (for fraction * basic cycle time) for senders
 * to contact me.
 * When sending high priority data, I keep the iface on, and forward
 * all the data I can (within their time limit) to anyone who sends
 * me a beacon.
 * I leave send mode as soon as I no longer have high priority data to send.
 *
 * In energy saving mode, the iface is turned on right before sending the
 * beacon.  If someone has contacted us during our beacon interval, and
 * sends us a beacon, we then send them our own queued data (even low
 * priority).  Either way, the iface is then turned off.
 * If we have low priority data to send, then once every 2/fraction cycles,
 * the iface is turned on for two full cycles, and during that time we
 * behave as if we had high priority data to send.
 *
 * Packets are resent in exponential backoff fashion (every 2^i'th cycle).
 * The cycle counter is incremented in every cycle where data is sent:
 *   In managed mode, only in cycles when a beacon grant was received.
 *   In unamanged mode, the cycle counter is incremented every basic (5s) cycle.
 * Packets are dropped when acked or after the maximal backoff threshold is
 * reached at 2^8 == 256.
 * Packets with DO_NOT_CACHE flag are only sent once.
 */

#include <assert.h>
#include <errno.h>
#include <signal.h>           /* signal */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>           /* usleep */
#include <sys/socket.h>       /* sockaddr */

#include "abc-iface.h"        /* sockaddr_t */
#include "abc-ip.h"           /* abc_iface_ip */
#include "abc-wifi.h"         /* abc_iface_wifi */
#include "../social.h"        /* UNKNOWN_SOCIAL_TIER */
#include "lib/mgmt.h"         /* struct allnet_mgmt_header */
#include "lib/log.h"
#include "lib/packet.h"       /* struct allnet_header */
#include "lib/pipemsg.h"      /* receive_pipe_message_fd, receive_pipe_message_any */
#include "lib/priority.h"     /* ALLNET_PRIORITY_FRIENDS_LOW */
#include "lib/util.h"         /* delta_us */
#include "lib/pqueue.h"       /* queue_* */
#include "lib/sha.h"          /* sha512_bytes */


/* we don't know how big messages will be on the interface until we get them */
#define MAX_RECEIVE_BUFFER	ALLNET_MTU

#define BASIC_CYCLE_SEC		5	/* 5s in a basic cycle */
/* a beacon time is 1/100 of a basic cycle */
#define	BEACON_MS		(BASIC_CYCLE_SEC * 1000 / 100)
/* maximum amount of time to wait for a beacon grant */
#define BEACON_MAX_COMPLETION_US	250000    /* 0.25s */

/** Cycle counter. Used for exponential backoff when resending messages. */
static unsigned long cycle = 0ul;

/** exit flag set by TERM signal. Set by term_handler. */
static volatile sig_atomic_t terminate = 0;

/* Managed interface drivers limit the rate. TODO: move into interface */
static unsigned long long int bits_per_s = 1000 * 1000;  /* 1Mb/s default */

/* with managed interface drivers, the state machine has two modes, high
 * priority (keep interface on, and send whenever possible) and low priority
 * (turn on interface only about 1% of the time to send or receive packets).
 * Start out in low-priority mode. */
static int high_priority = 0;

/* when we receive high priority packets, we want to stay in high priority mode
 * one more cycle, in case there are any more packets to receive */
static int received_high_priority = 0;

/* cycles we skipped because of interface activation delay.
 * This is also the number of cycles we leave the interface on
 * in low priority mode to compensate for the activation delay */
static unsigned int if_cycles_skiped = 0;

enum abc_send_type {
    ABC_SEND_TYPE_NONE = 0,  /* nothing to send */
    ABC_SEND_TYPE_REPLY,     /* send a mgmt-type reply */
    ABC_SEND_TYPE_QUEUE      /* send queued messages */
};
static enum { BEACON_NONE, BEACON_SENT, BEACON_REPLY_SENT, BEACON_GRANT_SENT }
  beacon_state = BEACON_NONE,
  pending_beacon_state = BEACON_NONE;
static unsigned char my_beacon_rnonce [NONCE_SIZE];
static unsigned char my_beacon_snonce [NONCE_SIZE];
static unsigned char other_beacon_snonce [NONCE_SIZE];
static unsigned char other_beacon_rnonce [NONCE_SIZE];
static unsigned char zero_nonce [NONCE_SIZE];

/** array of broadcast interface types (wifi, ethernet, ...) */
static abc_iface * iface_types[] = {
  &abc_iface_ip,
  &abc_iface_wifi
};

/* must match length and order of iface_types[] */
static const char * iface_type_strings[] = {
  "ip",
  "wifi"
};
static abc_iface * iface = NULL; /* used interface ptr */

static void term_handler (int sig) {
  terminate = 1;
}

static void clear_nonces (int mine, int other)
{
  if (mine) {
    bzero (my_beacon_rnonce, NONCE_SIZE);
    bzero (my_beacon_snonce, NONCE_SIZE);
  }
  if (other) {
    bzero (other_beacon_rnonce, NONCE_SIZE);
    bzero (other_beacon_snonce, NONCE_SIZE);
  }
}

/** Sets the high priority variable */
static void check_priority_mode ()
{
  high_priority = received_high_priority ||
                  (!high_priority &&
                   queue_max_priority () >= ALLNET_PRIORITY_FRIENDS_LOW);
}

/* returns -1 in case of error, 0 for timeout, and message size otherwise */
/* may return earlier than t if a packet is received or there is an error */
/* if ad_only is set, receives only from ad, otherwise from both ad and
 * abc interface (but ad_only is always zero) */
static int receive_until (struct timeval * t, char ** message,
                          int * from_fd, int * priority, int ad_only)
{
  struct timeval now;
  gettimeofday (&now, NULL);
  unsigned long long int us_to_wait = delta_us (t, &now);  /* 0 or more */
  int timeout_ms = us_to_wait / 1000LL;

  struct sockaddr_storage recv_addr;
  struct sockaddr * sap = (struct sockaddr *) (&recv_addr);
  socklen_t al = sizeof (recv_addr);

#ifdef DEBUG_PRINT
  unsigned long long start = allnet_time_ms ();
#endif /* DEBUG_PRINT */
  int msize;
  if (ad_only) {
    msize = receive_pipe_message_any (timeout_ms, message, from_fd, priority);
  } else {
    msize = receive_pipe_message_fd (timeout_ms, message, iface->iface_sockfd,
                                     sap, &al, from_fd, priority);
#ifdef DEBUG_PRINT
    unsigned long long finish = allnet_time_ms ();
    printf ("receive_pipe_message_fd %d (%d) on fd %d, ", msize, al, *from_fd);
    printf ("after time %lld/%d ms\n", finish - start, timeout_ms);
#endif /* DEBUG_PRINT */
    if (msize > 0 && al > 0 && !iface->accept_sender_cb (sap)) {
      free (*message);
      return 0;
    }
    if (msize < 0)
      terminate = 1;
  }
  return msize;  /* -1 (error), zero (timeout) or positive, the value is correct */
}

static void update_quiet (struct timeval * quiet_end,
                          unsigned long long int quiet_us)
{
  /* do not allow a sender to monopolize the medium too easily */
  if (quiet_us > 50000)  /* 0.05s, 50ms */
    quiet_us = 50000;
  struct timeval new_quiet;
  gettimeofday (&new_quiet, NULL);
  add_us (&new_quiet, quiet_us);
  if (delta_us (&new_quiet, quiet_end) > 0)
    *quiet_end = new_quiet;
}

static void send_beacon (int awake_ms)
{
  char buf [ALLNET_BEACON_SIZE (0)];
  int size = sizeof (buf);
  bzero (buf, size);
  struct allnet_mgmt_header * mp =
    (struct allnet_mgmt_header *) (buf + ALLNET_SIZE (0));
  struct allnet_mgmt_beacon * mbp =
    (struct allnet_mgmt_beacon *) (buf + ALLNET_MGMT_HEADER_SIZE (0));

  init_packet (buf, size, ALLNET_TYPE_MGMT, 1, ALLNET_SIGTYPE_NONE,
               NULL, 0, NULL, 0, NULL, NULL);

  mp->mgmt_type = ALLNET_MGMT_BEACON;
  random_bytes ((char *)my_beacon_rnonce, NONCE_SIZE);
  memcpy (mbp->receiver_nonce, my_beacon_rnonce, NONCE_SIZE);
  writeb64u (mbp->awake_time,
             ((unsigned long long int) awake_ms) * 1000LL * 1000LL);
  if (sendto (iface->iface_sockfd, buf, size, MSG_DONTWAIT, BC_ADDR (iface),
      iface->sockaddr_size) < size) {
    int e = errno;
    /* retry, first packet is sometimes dropped */
    if (sendto (iface->iface_sockfd, buf, size, MSG_DONTWAIT, BC_ADDR (iface),
        iface->sockaddr_size) < size) {
      perror ("beacon sendto (2nd try)");
      if (errno != e)
        printf ("...different error on 2nd try, first was %d\n", e);
    }
  }
}

static void make_beacon_reply (char * buffer, int bsize)
{
  assert (bsize >= ALLNET_MGMT_HEADER_SIZE (0) +
               sizeof (struct allnet_mgmt_beacon_reply));
  /* struct allnet_header * hp = */
  init_packet (buffer, bsize, ALLNET_TYPE_MGMT, 1, ALLNET_SIGTYPE_NONE,
               NULL, 0, NULL, 0, NULL, NULL);

  struct allnet_mgmt_header * mp =
    (struct allnet_mgmt_header *) (buffer + ALLNET_SIZE (0));
  struct allnet_mgmt_beacon_reply * mbrp =
    (struct allnet_mgmt_beacon_reply *) (buffer + ALLNET_MGMT_HEADER_SIZE (0));

  mp->mgmt_type = ALLNET_MGMT_BEACON_REPLY;
  memcpy (mbrp->receiver_nonce, other_beacon_rnonce, NONCE_SIZE);
  random_bytes ((char *)other_beacon_snonce, NONCE_SIZE);
  memcpy (mbrp->sender_nonce, other_beacon_snonce, NONCE_SIZE);
}

static void make_beacon_grant (char * buffer, int bsize,
                               unsigned long long int send_time_ns)
{
  assert (bsize >= ALLNET_MGMT_HEADER_SIZE (0) +
               sizeof (struct allnet_mgmt_beacon_grant));
  init_packet (buffer, bsize, ALLNET_TYPE_MGMT, 1, ALLNET_SIGTYPE_NONE,
               NULL, 0, NULL, 0, NULL, NULL);

  struct allnet_mgmt_header * mp =
    (struct allnet_mgmt_header *) (buffer + ALLNET_SIZE (0));
  struct allnet_mgmt_beacon_grant * mbgp =
    (struct allnet_mgmt_beacon_grant *)
      (buffer + ALLNET_MGMT_HEADER_SIZE (0));

  mp->mgmt_type = ALLNET_MGMT_BEACON_GRANT;
  memcpy (mbgp->receiver_nonce, my_beacon_rnonce, NONCE_SIZE);
  memcpy (mbgp->sender_nonce  , my_beacon_snonce, NONCE_SIZE);
  writeb64u (mbgp->send_time, send_time_ns);
}

/**
 * Send pending messages
 * @param new_only When set, sends only new (unsent) messages
 */
static void unmanaged_send_pending (int new_only)
{
  char * message = NULL;
  int nsize;
  int priority;
  int backoff;
  queue_iter_start ();
  while (queue_iter_next (&message, &nsize, &priority, &backoff)) {
    /* new (unsent) messages have a backoff value of 0 */
    if ((new_only && backoff) || (!new_only && cycle % (1 << backoff) != 0))
      continue;
    if (sendto (iface->iface_sockfd, message, nsize, MSG_DONTWAIT,
        BC_ADDR (iface), iface->sockaddr_size) < nsize) {
      perror ("abc: sendto");
      continue;
    }
    struct allnet_header * hp = (struct allnet_header *) message;
    if (hp->transport & ALLNET_TRANSPORT_DO_NOT_CACHE)
      queue_iter_remove ();
    else
      queue_iter_inc_backoff ();
  }
}

/**
 * Send pending message (and update pending beacon state on beacon messages)
 * @param type if type is ABC_SEND_TYPE_REPLY, sends the beacon message
 *    if type is ABC_SEND_TYPE_QUEUE, sends the specified number of bytes from
 *    the queue, ignoring message.
 *    Exponential backoff is used to resend messages only every 2^i'th cycle.
 *    Messages are removed from the queue when the set threshold is crossed.
 * @param size size of message
 */
static void send_pending (enum abc_send_type type, int size, char * message)
{
  switch (type) {
    case ABC_SEND_TYPE_REPLY:
      if (sendto (iface->iface_sockfd, message, size, MSG_DONTWAIT,
          BC_ADDR (iface), iface->sockaddr_size) < size)
        perror ("abc: sendto (reply)");
      else
        beacon_state = pending_beacon_state;
      pending_beacon_state = BEACON_NONE;
      break;

    case ABC_SEND_TYPE_QUEUE:
    {
      int total_sent = 0;
      char * message = NULL;
      int nsize;
      int priority;
      int backoff;
      queue_iter_start ();
      while ((queue_iter_next (&message, &nsize, &priority, &backoff)) &&
             (total_sent + nsize <= size)) {
        if (cycle % (1 << backoff) != 0)
          continue;
        if (sendto (iface->iface_sockfd, message, nsize, MSG_DONTWAIT,
            BC_ADDR (iface), iface->sockaddr_size) < nsize) {
          perror ("abc: sendto (queue)");
          continue;
        }
        total_sent += nsize;

        struct allnet_header * hp = (struct allnet_header *) message;
        if (hp->transport & ALLNET_TRANSPORT_DO_NOT_CACHE)
          queue_iter_remove ();
        else
          queue_iter_inc_backoff ();
      }
      ++cycle; /* increment cycle after sending data */
      break;
    }

    case ABC_SEND_TYPE_NONE:
    default:
      break; /* nothing to do */
  }
}

/** Returns 1 if message is a beacon (not a regular packet), 0 otherwise.
 * Does no work, expect identifying packet type, when quiet is set.
 *
 * Sets *send_type to ABC_SEND_TYPE_REPLY, *send_size to the message size, and
 * send_message (which must have size ALLNET_MTU) to the message to send, if
 * there is a message to be sent after the quiet time.
 * sets *send_type to ABC_SEND_TYPE_QUEUE, *send_size to the number of bytes
 * that can be sent if we have been granted permission to send that many bytes.
 * If there is nothing to send, sets *send_type to ABC_SEND_TYPE_NONE
 */
static int handle_beacon (const char * message, int msize,
                          struct timeval ** beacon_deadline,
                          struct timeval * time_buffer,
                          struct timeval * quiet_end,
                          enum abc_send_type * send_type, int * send_size,
                          char * send_message, int quiet)
{
  const struct allnet_header * hp = (const struct allnet_header *) message;
  *send_type = 0;  /* don't send anything unless we say otherwise */
  if (hp->message_type != ALLNET_TYPE_MGMT)
    return 0;
  if (msize < ALLNET_MGMT_HEADER_SIZE (hp->transport))
    return 0;
  if (quiet)
    return 1;
  const struct allnet_mgmt_header * mp =
    (const struct allnet_mgmt_header *) (message + ALLNET_SIZE (hp->transport));
  const char * beaconp = message + ALLNET_MGMT_HEADER_SIZE (hp->transport);

  switch (mp->mgmt_type) {
  case ALLNET_MGMT_BEACON:
  {
    /* TODO: only reply if we have something to send */
    if (beacon_state == BEACON_REPLY_SENT /* && is_before (*beacon_deadline) // is implied */)
      return 1;
    /* only reply if we have something to send */
    if (queue_total_bytes () == 0)
      return 1;
    const struct allnet_mgmt_beacon * mbp = (const struct allnet_mgmt_beacon *) beaconp;

    /* compute when to send the reply */
    struct timeval now;
    gettimeofday (&now, NULL);
    unsigned long long awake_us = readb64u (mbp->awake_time) / 1000LL;
    unsigned long long quiet_end_us = delta_us (quiet_end, &now);
    long long int diff_us = awake_us - quiet_end_us;
    if (diff_us <= 0 && awake_us != 0) {
      /* reply instantly and violate silence period */
      diff_us = 0;
      *quiet_end = now;
    } else if (diff_us < 100000 && awake_us != 0) {
      /* send in first half */
      diff_us /= 2;
    } else {
      /* not given, unreasonable or irrelevant, assume 50-99ms / 2 */
      diff_us = 25000 + (random () % 24000);
    }

    if (diff_us)
      add_us (quiet_end, random () % diff_us);

    /* create the reply */
    memcpy (other_beacon_rnonce, mbp->receiver_nonce, NONCE_SIZE);
    *send_type = ABC_SEND_TYPE_REPLY;
    *send_size = ALLNET_MGMT_HEADER_SIZE (0) +
                 sizeof (struct allnet_mgmt_beacon_reply);
    /* make the beacon which will be sent by caller (handle_until()) */
    make_beacon_reply (send_message, ALLNET_MTU);
    pending_beacon_state = BEACON_REPLY_SENT;

    *beacon_deadline = time_buffer;
    gettimeofday (*beacon_deadline, NULL);
    add_us (*beacon_deadline, BEACON_MAX_COMPLETION_US);
    return 1;
  } /* case ALLNET_MGMT_BEACON */

  case ALLNET_MGMT_BEACON_REPLY:
  {
    struct allnet_mgmt_beacon_reply * mbrp =
      (struct allnet_mgmt_beacon_reply *) beaconp;
    /* make sure we are in the right state.  We should have sent a beacon
     * (my_beacon_rnonce not zero) matching this reply, but we should not
     * yet have sent a grant (my_beacon_snonce should be zero) */
    if (beacon_state >= BEACON_GRANT_SENT)
      return 1;

    if (memcmp (mbrp->receiver_nonce, my_beacon_rnonce, NONCE_SIZE) != 0)
      return 1;
    /* grant this sender exclusive permission to send */
    memcpy (my_beacon_snonce, mbrp->sender_nonce, NONCE_SIZE);
    *send_type = ABC_SEND_TYPE_REPLY;
    *send_size = ALLNET_MGMT_HEADER_SIZE (0) +
                 sizeof (struct allnet_mgmt_beacon_grant);
    /* make the beacon grant which will be sent by caller (handle_until()) */
    make_beacon_grant (send_message, ALLNET_MTU, BEACON_MS * 1000LL * 1000LL);
    return 1;
  } /* case ALLNET_MGMT_BEACON_REPLY */

  case ALLNET_MGMT_BEACON_GRANT:
  {
    const struct allnet_mgmt_beacon_grant * mbgp =
      (const struct allnet_mgmt_beacon_grant *) beaconp;
    /* make sure this is a grant for something we signed up for */
    if (memcmp (mbgp->receiver_nonce, other_beacon_rnonce, NONCE_SIZE) == 0) {
      if (memcmp (mbgp->sender_nonce, other_beacon_snonce, NONCE_SIZE) == 0) {
        /* granted to me, so send now */
        *send_type = ABC_SEND_TYPE_QUEUE;   /* send from the queue */
        unsigned long long int bytes_to_send = queue_total_bytes ();
        unsigned long long int send_ns = readb64u (mbgp->send_time);
        /* bytes/second = bits/second / 8
           bytes/nanosecond = bits/second / 8,000,000,000
           bytes I may send = ns I may send * bits/second / 8,000,000,000 */
        unsigned long long int may_send =
          bits_per_s * send_ns / (8 * 1000LL * 1000LL * 1000LL);
        if (bytes_to_send > may_send)
          bytes_to_send = may_send;
        *send_size = bytes_to_send;

      } else {
        /* granted to somebody else, so start listening again */
        /* should keep quiet while they send */
        beacon_state = BEACON_NONE;
        update_quiet (quiet_end, readb64u (mbgp->send_time) / 1000LL);
      }
      clear_nonces (0, 1);      /* be open to new beacon messages */
      *beacon_deadline = NULL;
    }
    return 1;
  } /* case ALLNET_MGMT_BEACON_GRANT */

  default:
    return 0; /* not a beacon packet */
  }
}

static void remove_acked (const char * ack)
{
  char hashed_ack [MESSAGE_ID_SIZE];
  sha512_bytes (ack, MESSAGE_ID_SIZE, hashed_ack, MESSAGE_ID_SIZE);
  char * element = NULL;
  int size;
  int priority;
  int backoff;
  queue_iter_start ();
  while (queue_iter_next (&element, &size, &priority, &backoff)) {
    if (size > ALLNET_HEADER_SIZE) {
      struct allnet_header * hp = (struct allnet_header *) element;
      char * message_id = ALLNET_MESSAGE_ID (hp, hp->transport, size);
      char * packet_id = ALLNET_PACKET_ID (hp, hp->transport, size);
      if (((message_id != NULL) &&
           (memcmp (hashed_ack, message_id, MESSAGE_ID_SIZE) == 0)) ||
          ((packet_id != NULL) &&
           (memcmp (hashed_ack, packet_id, MESSAGE_ID_SIZE) == 0))) {
        queue_iter_remove ();
      }
    }
  }
}

static void remove_acks (const char * message, const char * end)
{
  struct allnet_header * hp = (struct allnet_header *) message;
  if (hp->message_type == ALLNET_TYPE_ACK) {
    const char * ack;
    for (ack = message + ALLNET_SIZE (hp->transport);
           ack < end; ack += MESSAGE_ID_SIZE)
      remove_acked (ack);
  }
}

static void handle_ad_message (const char * message, int msize, int priority)
{
  if (!queue_add (message, msize, priority)) {
    snprintf (log_buf, LOG_SIZE,
              "abc: queue full, unable to add new message of size %d\n", msize);
    log_print ();
  }
  remove_acks (message, message + msize);
}

static void unmanaged_handle_network_message (const char * message,
                                              int msize, int ad_pipe)
{
  /* struct allnet_header * hp = (struct allnet_header *) message; */
  /* send the message to ad */
  int sent = send_pipe_message (ad_pipe, message, msize,
                                ALLNET_PRIORITY_EPSILON);
  /* if (sent <= 0) { */
    snprintf (log_buf, LOG_SIZE, "u sent to ad %d bytes, message %d bytes\n",
              sent, msize);
    log_print ();
  /*  terminate = 1;
  } */
  /* remove any messages that this message acks */
  remove_acks (message, message + msize);
}

static void handle_network_message (const char * message, int msize,
                                    int ad_pipe,
                                    struct timeval ** beacon_deadline,
                                    struct timeval * time_buffer,
                                    struct timeval * quiet_end,
                                    enum abc_send_type * send_type,
                                    int * send_size,
                                    char * send_message, int quiet)
{
  if (! handle_beacon (message, msize, beacon_deadline, time_buffer,
                       quiet_end, send_type, send_size, send_message, quiet)) {
    /* check for high-priority message */
    struct allnet_header * hp = (struct allnet_header *) message;
    int cacheable = ((hp->transport & ALLNET_TRANSPORT_DO_NOT_CACHE) == 0);
    int msgpriority = compute_priority (msize, hp->src_nbits, hp->dst_nbits,
                                        hp->hops, hp->max_hops,
                                        UNKNOWN_SOCIAL_TIER, 1, cacheable);
    if (msgpriority >= ALLNET_PRIORITY_DEFAULT_HIGH)
      received_high_priority = 1;

    /* send the message to ad */
    int sent = send_pipe_message (ad_pipe, message, msize,
                                  ALLNET_PRIORITY_EPSILON);
    /* if (sent <= 0) { */
      snprintf (log_buf, LOG_SIZE, "sent to ad %d bytes, message %d bytes\n",
                sent, msize);
      log_print ();
   /*   terminate = 1;
    } */
    /* remove any messages that this message acks */
    remove_acks (message, message + msize);
  }
}

/* same as handle_until, but does not send any messages or change any
 * global state other than possibly quiet_end */
static void handle_quiet (struct timeval * quiet_end, int rpipe, int wpipe)
{
  check_priority_mode ();
  while (is_before (quiet_end) && !terminate) {
    char * message;
    int from_fd;
    int priority;
    int msize = receive_until (quiet_end, &message, &from_fd, &priority, 0);
    if (msize > 0) {
      if (is_valid_message (message, msize)) {
printf ("%d-byte message from %d (ad is %d)\n", msize, from_fd, rpipe);
        if (from_fd == rpipe)
          handle_ad_message (message, msize, priority);
        else
          handle_network_message (message, msize, wpipe,
                                  NULL, NULL, NULL, NULL, NULL, NULL, 1);
        check_priority_mode ();
      }
else { printf ("invalid message from %d (ad is %d)\n", from_fd, rpipe); }
      free (message);
    } else {
      usleep (10 * 1000); /* 10ms */
    }
  }
}

/* handle incoming packets until time t */
static void unmanaged_handle_until (struct timeval * t, int rpipe, int wpipe)
{
  while (is_before (t) && !terminate) {
    char * message;
    int fd;
    int priority;
    int msize = receive_until (t, &message, &fd, &priority, 0);
    if (msize > 0) {
      if (is_valid_message (message, msize)) {
        if (fd == rpipe) {
          handle_ad_message (message, msize, priority);
          unmanaged_send_pending (1);
        } else {
          unmanaged_handle_network_message (message, msize, wpipe);
        }
      }
      free (message);
    } else {
      usleep (10 * 1000); /* 10ms */
    }
  }
}
/* handle incoming packets until time t.  Do not send before quiet_end */
static void handle_until (struct timeval * t, struct timeval * quiet_end,
                          int rpipe, int wpipe)
{
  check_priority_mode ();
  struct timeval * beacon_deadline = NULL;
  struct timeval time_buffer;   /* beacon_deadline sometimes points here */
  while (is_before (t) && !terminate) {
    char * message;
    int fd;
    int priority;
    struct timeval * deadline = t;
    if ((beacon_deadline != NULL) && (delta_us (t, beacon_deadline) > 0))
      deadline = beacon_deadline;
    int msize = receive_until (deadline, &message, &fd, &priority, 0);
    enum abc_send_type send_type = ABC_SEND_TYPE_NONE;
    int send_size = 0;
    static char send_message [ALLNET_MTU];
    if ((msize > 0) && (is_valid_message (message, msize))) {
      if (fd == rpipe)
        handle_ad_message (message, msize, priority);
      else
        handle_network_message (message, msize, wpipe, &beacon_deadline,
                                &time_buffer, quiet_end,
                                &send_type, &send_size, send_message, 0);
      free (message);
      /* forward any pending messages */
      if (send_type != ABC_SEND_TYPE_NONE) {
        handle_quiet (quiet_end, rpipe, wpipe);
        send_pending (send_type, send_size, send_message);
      }
      /* see if priority has changed */
      check_priority_mode ();

    } else {
      usleep (10 * 1000); /* 10ms */
    }
    if ((beacon_deadline != NULL) && (! is_before (beacon_deadline))) {
      /* we have not been granted permission to send, allow new beacons */
#ifdef DEBUG_PRINT
      struct timeval now;
      gettimeofday (&now, NULL);
      printf ("abc: missed beacon-grant by %lldms\n",
              delta_us (&now, beacon_deadline) / 1000);
#endif /* DEBUG_PRINT */
      beacon_state = BEACON_NONE;
      beacon_deadline = NULL;
      clear_nonces (0, 1);
    }
  }
}

/* sets bstart to a random time between bstart and (bfinish - beacon_ms),
 * and bfinish to beacon_ms ms later
 * parameters are in ms, computation is in us (sec/1,000,000) */
static void beacon_interval (struct timeval * bstart, struct timeval * bfinish,
                             const struct timeval * start,
                             const struct timeval * finish,
                             int beacon_ms)
{
  unsigned long long int interval_us = delta_us (finish, start);
  unsigned long long int beacon_us = beacon_ms * 1000LL;
  unsigned long long int at_end_us = beacon_us;
  *bstart = *start;
  if (interval_us > at_end_us)
    set_time_random (start, 0LL, interval_us - at_end_us, bstart);
  *bfinish = *bstart;
  add_us (bfinish, beacon_us);
  printf ("b_int (%ld.%06ld, %ld.%06ld + %d) => %ld.%06ld, %ld.%06ld\n",
          start->tv_sec, start->tv_usec, finish->tv_sec, finish->tv_usec,
          beacon_ms,
          bstart->tv_sec, bstart->tv_usec, bfinish->tv_sec, bfinish->tv_usec);
}

/* do one basic 5s unmanaged cycle */
static void unmanaged_one_cycle (const char * interface, int rpipe, int wpipe)
{
  struct timeval start, finish;
  gettimeofday (&start, NULL);
  finish.tv_sec = compute_next (start.tv_sec, BASIC_CYCLE_SEC, 0);
  finish.tv_usec = 0;

  unmanaged_handle_until (&finish, rpipe, wpipe);
  unmanaged_send_pending (0); /* resend queued data if needed */
  ++cycle;
}

/* do one basic 5s cycle */
static void one_cycle (const char * interface, int rpipe, int wpipe,
                       struct timeval * quiet_end)
{
  struct timeval if_off, if_on, start, finish, beacon_time, beacon_stop;
  if (if_cycles_skiped-- == 0) {
    gettimeofday (&if_off, NULL);
    /* enabling the iface might take some time causing us to miss a cycle */
    iface->iface_set_enabled_cb (1);
    gettimeofday (&if_on, NULL);

    unsigned long long dms = delta_us (&if_on, &if_off) / 1000LLU;
    if_cycles_skiped = dms / (1000 * BASIC_CYCLE_SEC);
    printf ("took %llums, skipped %d cycle(s)\n", dms, if_cycles_skiped);
  }

  gettimeofday (&start, NULL);
  finish.tv_sec = compute_next (start.tv_sec, BASIC_CYCLE_SEC, 0);
  finish.tv_usec = 0;
  beacon_interval (&beacon_time, &beacon_stop, &start, &finish, BEACON_MS);

  beacon_state = BEACON_NONE;
  clear_nonces (1, 1);   /* start a new cycle */

  handle_until (&beacon_time, quiet_end, rpipe, wpipe);
  send_beacon (BEACON_MS);
  beacon_state = BEACON_SENT;
  handle_until (&beacon_stop, quiet_end, rpipe, wpipe);
  /* clear_nonces (1, 0);  -- if we stay on, denying beacon replies is
   * not really helpful.  If we are off, we will get no beacon replies
   * anyway, so it doesn't matter */
  if (! high_priority && if_cycles_skiped == 0) /* skipped cycle compensation */
    iface->iface_set_enabled_cb (0);
  handle_until (&finish, quiet_end, rpipe, wpipe);
  received_high_priority = 0;
}

static void main_loop (const char * interface, int rpipe, int wpipe)
{
  struct timeval quiet_end;   /* should we keep quiet? */
  gettimeofday (&quiet_end, NULL);  /* not until we overhear a beacon grant */
  if (!iface->init_iface_cb (interface)) {
    snprintf (log_buf, LOG_SIZE,
              "abc: unable to initialize interface %s\n", interface);
    log_print ();
    goto iface_cleanup;
  }
  int is_on = iface->iface_is_enabled_cb ();
  if ((is_on < 0) || ((is_on == 0) && (iface->iface_set_enabled_cb (1) != 1))) {
    snprintf (log_buf, LOG_SIZE,
              "abc: unable to bring up interface %s\n", interface);
    log_print ();
    goto iface_cleanup;
  }
  snprintf (log_buf, LOG_SIZE,
            "interface '%s' on fd %d\n", interface, iface->iface_sockfd);
  log_print ();
  add_pipe (rpipe);      /* tell pipemsg that we want to receive from ad */
  if (iface->iface_is_managed)
    bzero (zero_nonce, NONCE_SIZE);
  while (!terminate) {
    if (iface->iface_is_managed)
      one_cycle (interface, rpipe, wpipe, &quiet_end);
    else
      unmanaged_one_cycle (interface, rpipe, wpipe);
  }

iface_cleanup:
  iface->iface_cleanup_cb ();
}

void abc_main (int rpipe, int wpipe, char * ifopts)
{
  init_log ("abc");
  queue_init (16 * 1024 * 1024);  /* 16MBi */

  const char * interface = ifopts;
  const char * iface_type = NULL;
  char * args = ifopts;
  while (*args != '\0' && *args != '/')
    ++args;
  if (*args == '/') {
    *args = '\0';
    iface_type = ++args;
    const char * iface_type_args = NULL;
    while (*args != '\0' && *args != ',') ++args;
    if (*args == ',') {
      *args = '\0';
      iface_type_args = ++args;
    }

    int i;
    for (i = 0; i < sizeof (iface_types) / sizeof (abc_iface *); ++i) {
      if (strcmp (iface_type_strings [i], iface_type) == 0) {
        iface = iface_types [i];
        iface->iface_type_args = iface_type_args;
        break;
      }
    }
    if (iface == NULL) {
      snprintf (log_buf, LOG_SIZE,
                "No interface driver `%s' found. Using default\n", iface_type);
      log_print ();
    }
  }
  if (iface == NULL)
    iface = iface_types [0];

  snprintf (log_buf, LOG_SIZE, "read pipe is fd %d, write pipe fd %d\n",
            rpipe, wpipe);
  log_print ();
  struct sigaction sa;
  sa.sa_handler = term_handler;
  sa.sa_flags = 0;
  sigemptyset (&sa.sa_mask);
  sigaction (SIGINT, &sa, NULL);
  sigaction (SIGTERM, &sa, NULL);
  main_loop (interface, rpipe, wpipe);
  snprintf (log_buf, LOG_SIZE, "end of abc (%s) main thread\n", interface);
  log_print ();
}

#ifdef DAEMON_MAIN_FUNCTION
int main (int argc, char ** argv)
{
  log_to_output (get_option ('v', &argc, argv));
  if (argc != 4) {
    printf ("arguments must be a read pipe, a write pipe, and an interface\n");
    printf ("argc == %d\n", argc);
    print_usage (argc, argv, 0, 1);
    return -1;
  }
  int rpipe = atoi (argv [1]);  /* read pipe */
  int wpipe = atoi (argv [2]);  /* write pipe */
  abc_main (rpipe, wpipe, argv [3]);
  return 1;
}
#endif /* DAEMON_MAIN_FUNCTION */
