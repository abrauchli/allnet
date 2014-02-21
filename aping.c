/**
 * (MIT License)
 * Copyright (c) 2014, Andreas Brauchli, University of Hawaii at Manoa
 * 
 * aping.c: Discovers nearby allnet nodes using allnet broadcast
 * aping holds two communication pipes (one read, one write) to abc.
 */

#include <stdio.h> /* snprintf */
#include <stdlib.h>
#include "lib/log.h" /* TODO: remove lib/ */
#include "lib/priority.h"
#include "packet.h"

#define PIPE_READ 0
#define PIPE_WRITE 1

static void init_ping_packet (char * buf) {
  struct allnet_header * hp = (struct allnet_header *) buf;
  // char * payloadp = hp + sizeof (struct allnet_header);
  hp->version = ALLNET_VERSION;
  //hp->packet_type = ALLNET_TYPE_MGMT;
  hp->hops = 0;
  hp->max_hops = 1; // CHECK: experiment with recursive deep ping?
  hp->src_nbits = 0; // ADDRESS_BITS;
  hp->dst_nbits = 0;
  hp->sig_algo = ALLNET_SIGTYPE_NONE; // CHECK: sign pings -> requires nonce since header probably isn't signed?
}

void ping (int rwpipes[2]) {
  char packet [ALLNET_HEADER_SIZE];
  int psize = ALLNET_HEADER_SIZE;
  init_ping_packet (packet);
  log_packet ("broadcasting", packet, psize);
  /* Send ping packet with lowest priority */
  if (! send_pipe_message (rwpipes [PIPE_WRITE], packet, psize, EPSILON)) {
    snprintf (log_buf, LOG_SIZE, "Error sending ping packet\n");
    log_print ();
    return;
  }
  int from_pipe;
  int priority;
  while (1) {
    /* Wait for 5s */
    int psize = receive_pipe_message_any (5000, &packet, &from_pipe, &priority);
    switch (psize) {
    case -1: 
      snprintf (log_buf, LOG_SIZE, "Error reading pipe\n");
      log_print ();
      return;
    case 0:
      snprintf (log_buf, LOG_SIZE, "hit 5s timeout\n");
      log_print ();
      break;
    default:
      snprintf (log_buf, LOG_SIZE, "received %d, fd %d\n", psize, from_pipe);
      log_packet ("received packet", packet, psize);
      /* TODO: CHECK: print from address / ip / iface / hops */
    }
  }
}

int main(int argc, char ** argv) {
  init_log ("aping");
  snprintf (log_buf, LOG_SIZE, "AllNet (aping) version %d\n", ALLNET_VERSION);
  log_print ();
  int rwpipes[2]; /* R/W pipes to/from abc */
  ping (rwpipes);
  return 0;
}
