#include <ifaddrs.h>
#include <net/if.h>           /* ifa_flags */
#include <netpacket/packet.h> /* sockaddr_ll */
#include <stdio.h>
#include <stdlib.h>           /* system */
#include <string.h>           /* memcpy */
#include <sys/socket.h>
#include <unistd.h>           /* geteuid */

#include "lib/mgmt.h"         /* struct allnet_mgmt_header */
#include "lib/packet.h"       /* ALLNET_SIZE */
#include "lib/util.h"         /* delta_us */

#define BASIC_CYCLE_SEC		5	/* 5s in a basic cycle */
#define	BEACON_MS		(BASIC_CYCLE_SEC * 1000 / 100)

static void send_beacon (int sockfd, const char * interface, struct sockaddr * addr, socklen_t addrlen, int nbeacons)
{
  int awake_ms = BEACON_MS;
  char buf [ALLNET_BEACON_SIZE (0)];
  int size = sizeof (buf);
  bzero (buf, size);
  struct allnet_mgmt_header * mp =
    (struct allnet_mgmt_header *) (buf + ALLNET_SIZE (0));
  struct allnet_mgmt_beacon * mbp =
    (struct allnet_mgmt_beacon *) (buf + ALLNET_MGMT_HEADER_SIZE (0));

  init_packet (buf, size, ALLNET_TYPE_MGMT, 1, ALLNET_SIGTYPE_NONE,
               NULL, 0, NULL, 0, NULL);

  mp->mgmt_type = ALLNET_MGMT_BEACON;
  int i;
  for (i = 0; i < nbeacons; ++i) {
    random_bytes ((char *)mbp->receiver_nonce, NONCE_SIZE);
    if (nbeacons > 1)
      mbp->receiver_nonce[NONCE_SIZE -1] = (char)i;
    writeb64u (mbp->awake_time,
               ((unsigned long long int) awake_ms) * 1000LL * 1000LL);
    if (sendto (sockfd, buf, size, MSG_DONTWAIT, addr, addrlen) < size)
      perror ("beacon sendto");
    usleep (1000);
  }
}

static void default_broadcast_address (struct sockaddr_ll * bc)
{
  bc->sll_family = AF_PACKET;
  bc->sll_protocol = ALLNET_WIFI_PROTOCOL;
  bc->sll_hatype = 1;   /* used? */
  bc->sll_pkttype = 0;  /* not used */
  bc->sll_halen = 6;
  bc->sll_addr [0] = 0xff;
  bc->sll_addr [1] = 0xff;
  bc->sll_addr [2] = 0xff;
  bc->sll_addr [3] = 0xff;
  bc->sll_addr [4] = 0xff;
  bc->sll_addr [5] = 0xff;
}

/* global debugging variable -- if 1, expect more debugging output */
/* set in main */
int allnet_global_debugging = 0;

int main (int argc, char ** argv) {
  if (argc < 2) {
    printf ("usage %s [interface] [setup-net]\n", argv[0]);
    return 1;
  }
  if (geteuid () != 0)
    printf ("warning: not root\n");
  const char * iface = argv[1];
  if (argc > 2 && strcmp (argv[2], "on") == 0) {
    char cmd[512];
    int ret = system ("rfkill unblock wifi");
    snprintf (cmd, 512, "ifconfig %s up", iface);
    ret = system (cmd);
    snprintf (cmd, 512, "iw dev %s set type ibss", iface);
    ret = system (cmd);
    snprintf (cmd, 512, "iw dev %s ibss join allnet 2412 fixed-freq", iface);
    ret = system (cmd);
    sleep (1);
  }
  int nbeacons = 1;
  if (argc > 3)
    nbeacons = atoi (argv[3]);

  struct sockaddr_ll if_address; /* the address of the interface */
  struct sockaddr_ll bc_address; /* broacast address of the interface */
  struct sockaddr  * bc_sap = (struct sockaddr *) (&bc_address);
  int sockfd;

  struct ifaddrs * ifa;
  if (getifaddrs (&ifa) != 0) {
    perror ("getifaddrs");
    return 0;
  }
  struct ifaddrs * ifa_loop = ifa;
  while (ifa_loop != NULL) {
    if ((ifa_loop->ifa_addr->sa_family == AF_PACKET) &&
        (strcmp (ifa_loop->ifa_name, iface) == 0)) {
      /* create the socket and initialize the address */
      sockfd = socket (AF_PACKET, SOCK_DGRAM, ALLNET_WIFI_PROTOCOL);
      if_address = *((struct sockaddr_ll *) (ifa_loop->ifa_addr));
      if (bind (sockfd, (const struct sockaddr *) &if_address, sizeof (struct sockaddr_ll)) == -1)
        printf ("error binding interface, continuing without..\n");
      if (ifa_loop->ifa_flags & IFF_BROADCAST)
        bc_address = *((struct sockaddr_ll *) (ifa_loop->ifa_broadaddr));
      else if (ifa_loop->ifa_flags & IFF_POINTOPOINT)
        bc_address = *((struct sockaddr_ll *) (ifa_loop->ifa_dstaddr));
      else
        default_broadcast_address (&bc_address);
      bc_address.sll_protocol = ALLNET_WIFI_PROTOCOL;  /* otherwise not set */
      bc_address.sll_ifindex = if_address.sll_ifindex;
      freeifaddrs (ifa);
      send_beacon (sockfd, iface, bc_sap, sizeof (struct sockaddr_ll), nbeacons);
      return 0;
    }
    ifa_loop = ifa_loop->ifa_next;
  }
  freeifaddrs (ifa);
  return 1;
}
