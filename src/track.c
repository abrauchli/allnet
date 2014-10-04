/* track.c: keep track of recently received packets */

#include <stdio.h>
#include <string.h>

#include "lib/packet.h"
#include "lib/priority.h"
#include "lib/util.h"
#include "lib/log.h"

#define SAVED_ADDRESSES	128

struct rate_record {
  unsigned char address [ADDRESS_SIZE];
  unsigned char num_bits;
  int packet_size;
};

static struct rate_record record [SAVED_ADDRESSES];

static int next = -1;

#define DEFAULT_MAX	(ALLNET_PRIORITY_MAX - 1)

int largest_rate ()
{
  return DEFAULT_MAX;
}

/* record that this source is sending this packet of given size */
/* return an integer, as a fraction of ALLNET_PRIORITY_MAX, to indicate what
 * fraction of the available bandwidth this source is using.
 * ALLNET_PRIORITY_MAX is defined in priority.h
 */
int track_rate (unsigned char * source, int sbits, int packet_size)
{
  int i;
  if (next < 0) {    /* initialize */
    for (i = 0; i < SAVED_ADDRESSES; i++) {
      memset (record [i].address, 0, ADDRESS_SIZE);
      record [i].num_bits = 0;
      record [i].packet_size = 0;
    }
    next = 0;
  }

  /* how many saved packets have the same source as this one? */
  int nmatches = 0;
  int total = 0;
  for (i = 0; i < SAVED_ADDRESSES; i++) {
    if (record [i].packet_size > 0) {
      total += record [i].packet_size;
      if (matches (source, sbits, record [i].address, record [i].num_bits))
        nmatches += record [i].packet_size;
    }
  }

  /* save this packet */
  memcpy (record [next].address, source, ADDRESS_SIZE);
  record [next].num_bits = sbits;
  record [next].packet_size = packet_size;
  next = (next + 1) % SAVED_ADDRESSES;

  nmatches += packet_size;    /* add in this packet */
  total += packet_size;    /* add in this packet */

  if (total == 0) {
    printf ("error in track_rate: illegal total size %d, returning one\n",
            total);
    return DEFAULT_MAX;
  }
#ifdef DEBUG_PRINT
  printf ("total %d, matching %d\n", total, nmatches);
#endif /* DEBUG_PRINT */
  return (ALLNET_PRIORITY_MAX / total) * nmatches;
}

