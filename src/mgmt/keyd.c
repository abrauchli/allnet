/* keyd.c: standalone application to respond to key requests */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "lib/packet.h"
#include "lib/media.h"
#include "lib/util.h"
#include "lib/app_util.h"
#include "lib/pipemsg.h"
#include "lib/priority.h"
#include "lib/sha.h"
#include "lib/log.h"
#include "lib/cipher.h"
#include "lib/keys.h"

#define CONFIG_DIR	"~/.allnet/keys"

static void send_key (int sock, struct bc_key_info * key, char * return_key,
                      int rksize, unsigned char * address, int abits, int hops)
{
#ifdef DEBUG_PRINT
  printf ("send_key ((%p, %d), %p)\n", key->pub_key, key->pub_klen, return_key);
#endif /* DEBUG_PRINT */
  int dlen = allnet_rsa_pubkey_size (key->pub_key) + 1;
  char * data = malloc_or_fail (dlen, "keyd send_key");
  if (! allnet_pubkey_to_raw (key->pub_key, data, dlen))
    return;
  int type = ALLNET_TYPE_CLEAR;
  int allocated = 0;
  int amhsize = sizeof (struct allnet_app_media_header);
  int bytes;
  struct allnet_header * hp =
    create_packet (dlen + amhsize, type, hops, ALLNET_SIGTYPE_NONE,
                   key->address, 16, address, abits, NULL, NULL, &bytes);
  char * adp = ALLNET_DATA_START(hp, hp->transport, bytes);
  struct allnet_app_media_header * amhp =
    (struct allnet_app_media_header *) adp;
  writeb32u (amhp->app, 0x6b657964 /* keyd */ );
  writeb32u (amhp->media, ALLNET_MEDIA_PUBLIC_KEY);
  char * dp = adp + amhsize;
  memcpy (dp, data, dlen);
  if (allocated)
    free (data);
  print_buffer (dp, dlen, "key", 10, 1);
#ifdef DEBUG_PRINT
printf ("verification is %d\n", verify_bc_key ("testxyzzy@by_sign.that_health", dp, dlen, "en", 16, 0));
#endif /* DEBUG_PRINT */

  /* send with relatively low priority */
  char * message = (char *) hp;
  send_pipe_message (sock, message, bytes, ALLNET_PRIORITY_DEFAULT);
}

#ifdef DEBUG_PRINT
void ** keyd_debug = NULL;
#endif /* DEBUG_PRINT */

static void handle_packet (int sock, char * message, int msize)
{
  struct allnet_header * hp = (struct allnet_header *) message;
  if (hp->message_type != ALLNET_TYPE_KEY_REQ)
    return;
#ifdef DEBUG_PRINT
  print_packet (message, msize, "key request", 1);
#endif /* DEBUG_PRINT */
  packet_to_string (message, msize, "key request", 1, log_buf, LOG_SIZE);
  log_print ();
  char * kp = message + ALLNET_SIZE (hp->transport);
#ifdef DEBUG_PRINT
  keyd_debug = ((void **) (&kp));
#endif /* DEBUG_PRINT */
  unsigned int nbits = (*kp) & 0xff;
  int offset = (nbits + 7) / 8;
  /* ignore the fingerprint for now -- not implemented */
  kp += offset + 1;
  int ksize = msize - (kp - message);
#ifdef DEBUG_PRINT
  printf ("kp is %p\n", kp);
#endif /* DEBUG_PRINT */
  if (((msize - (kp - message)) != 513) ||
      (*kp != KEY_RSA4096_E65537)) {
    snprintf (log_buf, LOG_SIZE,
              "msize %d - (%p - %p = %zd) =? 513, *kp %d\n",
              msize, kp, message, kp - message, *kp);
    log_print ();
    kp = NULL;
    ksize = 0;
  }
#ifdef DEBUG_PRINT
  printf (" ==> kp is %p (%d bytes)\n", kp, ksize);
#endif /* DEBUG_PRINT */

  struct bc_key_info * keys;
  unsigned int nkeys = get_own_keys (&keys);
#ifdef DEBUG_PRINT
  printf (" ==> kp %p, %d keys %p\n", kp, nkeys, keys);
#endif /* DEBUG_PRINT */
  if (nkeys <= 0) {
    snprintf (log_buf, LOG_SIZE, "no keys found\n");
    log_print ();
    return;
  }

  int i;
  for (i = 0; i < nkeys; i++) {
    int matching_bits =
      matches (hp->destination, hp->dst_nbits,
               (unsigned char *) (keys [i].address), ADDRESS_BITS);
    printf ("%02x <> %02x (%s): %d matching bits, %d needed\n",
            hp->destination [0] & 0xff, keys [i].address [0] & 0xff,
            keys [i].identifier, matching_bits, hp->dst_nbits);
    snprintf (log_buf, LOG_SIZE, "%02x <> %02x: %d matching bits, %d needed\n",
              hp->destination [0] & 0xff,
              keys [i].address [0] & 0xff, matching_bits, hp->dst_nbits);
    log_print ();
    if (matching_bits >= hp->dst_nbits) {  /* send the key */
#ifdef DEBUG_PRINT
      printf ("sending key %d, kp %p, %d bytes to %x/%d\n", i, kp, ksize,
hp->source [0] & 0xff, hp->src_nbits);
#endif /* DEBUG_PRINT */
      send_key (sock, keys + i, kp, ksize,
                hp->source, hp->src_nbits, hp->hops + 4);
    }
  }
}

/* used for debugging the generation of spare keys */
/* #define DEBUG_PRINT_SPARES */

static int gather_random_and_wait (int bsize, char * buffer, time_t until)
{
  int fd = -1;
  int count = 0;
  if (bsize > 0) {
    fd = open ("/dev/random", O_RDONLY);
    while ((fd >= 0) && (count < bsize)) {
#ifdef DEBUG_PRINT_SPARES
      printf ("graw, count %d, bsize %d\n", count, bsize);
#endif /* DEBUG_PRINT_SPARES */
      char data [1];
      ssize_t found = read (fd, data, 1);
      if (found == 1)
        buffer [count++] = data [0];
      else if (found < 0) {
        close (fd);
        fd = -1;
      }
    }
    if (fd >= 0)
      close (fd);
  }
#ifdef DEBUG_PRINT_SPARES
  printf ("at %ld: generated %d bytes, until %ld\n", time (NULL), count, until);
#endif /* DEBUG_PRINT_SPARES */
  while (time (NULL) < until) {
#ifdef DEBUG_PRINT_SPARES
    printf ("graw, time %ld, until %ld\n", time (NULL), until);
#endif /* DEBUG_PRINT_SPARES */
    if (sleep (10)) {  /* interrupted */
#ifdef DEBUG_PRINT_SPARES
      printf ("graw killed\n");
#endif /* DEBUG_PRINT_SPARES */
      exit (1);
    }
  }
  return ((fd >= 0) && (count == bsize));
}

#define KEY_GEN_BITS	4096
#define KEY_GEN_BYTES	(KEY_GEN_BITS / 8)
/* run from astart as a separate process */
void keyd_generate (char * pname)
{
  if (setpriority (PRIO_PROCESS, 0, 15) == 0) {
  /* sleep 10 min, or 100 * the time to generate a key, whichever is longer */
    time_t sleep_time = 60 * 10;  /* 10 minutes, in seconds */
    time_t start = time (NULL);
    /* generate up to 100 keys, then generate more as they are used */
    while (1) {
      time_t finish = start + sleep_time;
      if (create_spare_key (-1, NULL, 0) < 100) {
        static char buffer [KEY_GEN_BYTES];
        char * bp = NULL;
#ifdef DEBUG_PRINT_SPARES
        printf ("gathering %d bytes and waiting %ld until %ld\n",
                KEY_GEN_BYTES, finish - time (NULL), finish);
#endif /* DEBUG_PRINT_SPARES */
        if (gather_random_and_wait (KEY_GEN_BYTES, buffer, finish))
          bp = buffer;
        start = time (NULL);
#ifdef DEBUG_PRINT_SPARES
        printf ("%ld: %d spare keys\n", start, create_spare_key (-1, NULL, 0));
#endif /* DEBUG_PRINT_SPARES */
        create_spare_key (KEY_GEN_BITS, bp, KEY_GEN_BYTES);
      } else {
        if (gather_random_and_wait (0, NULL, finish))
          printf ("generate_spare_keys: unexpected positive\n");
        start = time (NULL);
      }
      sleep_time = (time (NULL) - start) * 100;
      if (sleep_time < (60 * 10))
        sleep_time = (60 * 10);
#ifdef DEBUG_PRINT_SPARES
      printf ("%ld: sleep time %ld (from %ld)\n", time (NULL), sleep_time,
              time (NULL) - start);
#endif /* DEBUG_PRINT_SPARES */
    }
  }
}

void keyd_main (char * pname)
{
  int sock = connect_to_local (pname, pname);
  if (sock < 0)
    return;

  while (1) {  /* loop forever */
    int pipe;
    int pri;
    char * message;                      /* sleep for up to a minute */
    int found = receive_pipe_message_any (60 * 1000, &message, &pipe, &pri);
    if (found < 0) {
      snprintf (log_buf, LOG_SIZE, "keyd pipe closed, exiting\n");
      log_print ();
      exit (1);
    }
    if ((found > 0) && (is_valid_message (message, found)))
      handle_packet (sock, message, found);
    if (found > 0)
      free (message);
  }
  snprintf (log_buf, LOG_SIZE, "keyd infinite loop ended, exiting\n");
  log_print ();
}

#ifdef DAEMON_MAIN_FUNCTION
int main (int argc, char ** argv)
{
  log_to_output (get_option ('v', &argc, argv));
  keyd_main (argv [0]);
  return 0;
}
#endif /* DAEMON_MAIN_FUNCTION */

