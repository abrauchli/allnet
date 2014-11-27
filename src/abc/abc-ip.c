/* abc-ip.c: Bradcast abc messages onto a generic ip interface */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>           /* close */
#include <ifaddrs.h>
#include <net/if.h>           /* ifa_flags */
#include <netinet/in.h>       /* struct sockaddr_in */
#include <netpacket/packet.h>
#include <sys/socket.h>       /* struct sockaddr */
#include <sys/time.h>         /* gettimeofday */

#include "lib/packet.h"       /* ALLNET_WIFI_PROTOCOL */
#include "lib/util.h"         /* delta_us */

#include "abc-iface.h"        /* sockaddr_t */

#include "abc-ip.h"

/* forward declarations */
static int abc_ip_init (const char * interface);
static int abc_ip_is_enabled ();
static int abc_ip_set_enabled (int state);
static int abc_ip_cleanup ();
static int abc_ip_accept_sender (const struct sockaddr *);

struct abc_iface_ip_priv {
  struct ifaddrs * ifa;
} abc_iface_ip_priv;

abc_iface abc_iface_ip = {
  .iface_type = ABC_IFACE_TYPE_IP,
  .iface_is_managed = 0,
  .iface_type_args = NULL,
  .iface_sockfd = -1,
  .if_address = {},
  .bc_address = {},
  .sockaddr_size = sizeof (struct sockaddr_in),
  .init_iface_cb = abc_ip_init,
  .iface_on_off_ms = 0, /* always on iface */
  .iface_is_enabled_cb = abc_ip_is_enabled,
  .iface_set_enabled_cb = abc_ip_set_enabled,
  .iface_cleanup_cb = abc_ip_cleanup,
  .accept_sender_cb = abc_ip_accept_sender,
  .priv = NULL
};

static int abc_ip_is_enabled ()
{
  return ((struct abc_iface_ip_priv *)abc_iface_ip.priv)->ifa->ifa_flags & IFF_UP;
}

static int abc_ip_set_enabled (int state)
{
  return 0;
}

/**
 * Init abc ip interface and UDP socket
 * @param interface Interface string of iface to init
 * @return 1 on success, 0 otherwise
 */
static int abc_ip_init (const char * interface)
{
  abc_iface_ip.priv = &abc_iface_ip_priv;
  struct ifaddrs * ifa;
  if (getifaddrs (&ifa) != 0) {
    perror ("abc-ip: getifaddrs");
    return 0;
  }
  int ret = 0;
  struct ifaddrs * ifa_loop = ifa;
  while (ifa_loop != NULL) {
    if ((ifa_loop->ifa_addr->sa_family == AF_INET) &&
        (strcmp (ifa_loop->ifa_name, interface) == 0)) {
      ((struct abc_iface_ip_priv *)abc_iface_ip.priv)->ifa = ifa_loop;
#ifdef TRACKING_TIME
      struct timeval start;
      gettimeofday (&start, NULL);
#endif /* TRACKING_TIME */
      if (abc_ip_is_enabled () == 0)
        abc_ip_set_enabled (1);
#ifdef TRACKING_TIME
      struct timeval midtime;
      gettimeofday (&midtime, NULL);
      long long mtime = delta_us (&midtime, &start);
#endif /* TRACKING_TIME */
      /* create the socket and initialize the address */
      abc_iface_ip.iface_sockfd = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (abc_iface_ip.iface_sockfd == -1) {
        perror ("abc-ip: error creating socket");
        goto abc_ip_init_cleanup;
      }
      int flag = 1;
      if (setsockopt (abc_iface_ip.iface_sockfd, SOL_SOCKET, SO_BROADCAST,
                      &flag, sizeof (flag)) != 0)
        perror ("abc-ip: error setting broadcast flag");
      if (setsockopt (abc_iface_ip.iface_sockfd, SOL_SOCKET, SO_BINDTODEVICE,
                      interface, strlen (interface)) != 0)
        perror ("abc-ip: error binding to device");
      abc_iface_ip.if_address.in.sin_family = AF_INET;
      abc_iface_ip.if_address.in.sin_addr.s_addr = htonl (INADDR_ANY);
      abc_iface_ip.if_address.in.sin_port = htons (ALLNET_ABC_IP_PORT);
      memset (&abc_iface_ip.if_address.in.sin_zero, 0, sizeof (abc_iface_ip.if_address.in.sin_zero));
      if (bind (abc_iface_ip.iface_sockfd, &abc_iface_ip.if_address.sa, sizeof (abc_iface_ip.if_address.sa)) == -1) {
        perror ("abc-ip: error binding interface");
        close (abc_iface_ip.iface_sockfd);
        abc_iface_ip.iface_sockfd = -1;
        goto abc_ip_init_cleanup;
      }
      if (ifa_loop->ifa_flags & IFF_BROADCAST) {
        abc_iface_ip.bc_address.sa = *(ifa_loop->ifa_broadaddr);
      } else {
        abc_iface_ip.bc_address.in.sin_addr.s_addr = htonl (INADDR_BROADCAST);
        printf ("abc-ip: set default broadcast address on %s\n", interface);
      }
      abc_iface_ip.bc_address.in.sin_family = AF_INET;
      abc_iface_ip.bc_address.in.sin_port = htons (ALLNET_ABC_IP_PORT);
      memset (&abc_iface_ip.bc_address.in.sin_zero, 0, sizeof (abc_iface_ip.bc_address.in.sin_zero));
      ret = 1;
      goto abc_ip_init_cleanup;
    }
    ifa_loop = ifa_loop->ifa_next;
  }
abc_ip_init_cleanup:
  freeifaddrs (ifa);
  return ret;
}

static int abc_ip_cleanup () {
  if (abc_iface_ip.iface_sockfd != -1) {
    if (close (abc_iface_ip.iface_sockfd) != 0) {
      perror ("abc-ip: error closing socket");
      return 0;
    }
    abc_iface_ip.iface_sockfd = -1;
  }
  return 1;
}

/**
 * Accept a sender if it's not coming from our own address
 * @param sender struct sockaddr_in * of the sender
 * @return 0 if we are the sender, 1 otherwise
 */
static int abc_ip_accept_sender (const struct sockaddr * sender)
{
  const struct sockaddr_in * sai = (const struct sockaddr_in *)sender;
  struct abc_iface_ip_priv * priv =
      (struct abc_iface_ip_priv *) abc_iface_ip.priv;
  struct sockaddr_in * own = (struct sockaddr_in *)priv->ifa->ifa_addr;
  return (own->sin_addr.s_addr != sai->sin_addr.s_addr);
}
