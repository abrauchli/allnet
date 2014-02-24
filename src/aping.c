/**
 * (MIT License)
 * Copyright (c) 2014, Andreas Brauchli, University of Hawaii at Manoa
 * 
 * aping.c: Discovers nearby allnet nodes using allnet broadcast
 * aping holds two communication pipes (one read, one write) to abc.
 */

#include <stdio.h> /* snprintf */
#include <stdlib.h>
#include <string.h> /* bzero */
#include <unistd.h> /* pipe */
#include "lib/log.h"
#include "lib/priority.h"
#include "packet.h"

#define PIPE_READ 0
#define PIPE_WRITE 1

static void init_ping_packet (char * buf) {
  struct allnet_header * hp = (struct allnet_header *) buf;
  // char * payloadp = hp + sizeof (struct allnet_header);
  hp->version = ALLNET_VERSION;
  hp->message_type = ALLNET_TYPE_CLEAR;
  hp->hops = 0;
  hp->max_hops = 1; // CHECK: experiment with recursive deep ping?
  hp->src_nbits = 0; // ADDRESS_BITS;
  hp->dst_nbits = 0;
  hp->sig_algo = ALLNET_SIGTYPE_NONE; // CHECK: sign pings -> requires nonce since header probably isn't signed?
  hp->transport = ALLNET_TRANSPORT_ACK_REQ;
  // hp->source = 0;
  bzero (hp->source, sizeof (hp->source)); // my addr
  // hp->destination = 0;
  bzero (hp->destination, sizeof (hp->source)); // dest addr
  /* standard header has no field message_id, so we use the payload
     That's where it would be anyway
     hp->message_id = generate_id(); */
  *ALLNET_MESSAGE_ID (hp, ALLNET_TRANSPORT_ACK_REQ, ALLNET_HEADER_SIZE + MESSAGE_ID_SIZE) = 0; // generate_id();
}

void ping (int rwpipes[2]) {
  char packet [ALLNET_HEADER_SIZE + MESSAGE_ID_SIZE];
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

static int create_pipes(int rwpipes[2]) {
  if (pipe (rwpipes) != 0) {
    perror ("pipe");
    snprintf (log_buf, LOG_SIZE, "error creating pipe set\n");
    log_print ();
    return 1;
  }
  return 0;
}

static void itoa(char buf[12], int n) {
    (void) snprintf (buf, 12, "%d", n);
}

static void start_abc (int rwpipes[2], char * iface)
{
  int pid = fork ();
  if (pid == 0) {
    char * args [5];
    char ap[2][12];
    itoa (ap[0], rwpipes [PIPE_READ]);
    itoa (ap[1], rwpipes [PIPE_WRITE]);
    args [0] = "abc"; // make_program_path (path, "abc");
    args [1] = ap[0];
    args [2] = ap[1];
    args [3] = iface;
    args [4] = NULL;
    snprintf (log_buf, LOG_SIZE, "calling %s %s %s %s\n",
              args [0], args [1], args [2], args [3]);
    log_print ();
    execv (args [0], args);
    perror ("execv");
    printf ("error executing abc\n");
    exit (1);
  } else {  /* parent, close the child pipes */
    close (rwpipes [PIPE_READ]);
    close (rwpipes [PIPE_WRITE]);
    snprintf (log_buf, LOG_SIZE, "parent called abc %d %d %s, closed %d %d\n",
              rwpipes [PIPE_READ], rwpipes [PIPE_WRITE], iface,
              rwpipes [PIPE_READ], rwpipes [PIPE_WRITE]);
    log_print ();
  }
}

int main(int argc, char ** argv) {
  init_log ("aping");
  snprintf (log_buf, LOG_SIZE, "AllNet (aping) version %d\n", ALLNET_VERSION);
  log_print ();
  int rwpipes[2]; /* R/W pipes to/from abc */
  if (create_pipes(rwpipes) != 0)
    return 1;
  start_abc (rwpipes, "wlan0");
  ping (rwpipes);
  return 0;
}
