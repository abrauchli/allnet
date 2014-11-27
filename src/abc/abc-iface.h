#ifndef ABC_IFACE_H
#define ABC_IFACE_H
/* abc-iface.h: Interface used by abc for broadcasting messages on a network */

#ifndef __APPLE__
#include <netpacket/packet.h>  /* struct sockaddr_ll */
#endif /* __APPLE__ */
#include <sys/socket.h>        /* struct sockaddr, socklen_t */
#include <netinet/ip.h>        /* struct sockaddr_in */

typedef union {
  struct sockaddr sa;
#ifndef __APPLE__
  struct sockaddr_ll ll;
#endif /* __APPLE__ */
  struct sockaddr_in in;
} sockaddr_t;

#define BC_ADDR(ifaceptr) ((const struct sockaddr *)&(ifaceptr)->bc_address)

/** Accept every sender */
int abc_iface_accept_sender (const struct sockaddr * sender);

/** enum of all compile-time supported abc iface modules */
typedef enum abc_iface_type {
  ABC_IFACE_TYPE_IP,
  ABC_IFACE_TYPE_WIFI
} abc_iface_type;

typedef struct abc_iface {
  /** The interface type this set of callbacks represents */
  abc_iface_type iface_type;
  int iface_is_managed;
  /** Additional parameters passed on to the iface driver */
  const char * iface_type_args;

  int iface_sockfd; /* the socket filedescriptor used with this iface */
  sa_family_t if_family; /* the address family of if_address and bc_address */
  sockaddr_t if_address; /* the address of the interface */
  sockaddr_t bc_address; /* broacast address of the interface */
  socklen_t sockaddr_size; /* the size of the sockaddr_* inside sockaddr_t */
  /**
   * Callback to initialize the interface.
   * The callback must initialize all paramteres except interface
   * @param interface The interface to use (e.g. eth0 or wlan0.)
   * @param sock The interface's communication socket
   * @param address The interface socket's address
   * @param bc The interface's default broadcast address
   * @return 1 if successful, 0 on failure.
   */
  int (* init_iface_cb) (const char * interface);
  /**
   * Time in ms it takes to turn on the interface.
   * The initial value provides a guideline and should be pretty conservative.
   * The value is updated by abc after the first call to iface_set_enabled_cb
   */
  unsigned long long int iface_on_off_ms;
  /**
   * Callback that queries whether the interface is enabled.
   * @return 1 if enabled, 0 if disabled, -1 on failure.
   */
  int (* iface_is_enabled_cb) ();
  /**
   * Callback that enables/disables the interface according to state.
   * @param state 1 to enable, 0 to disable the interface.
   * @return 1 if succeeded in enabling/disabling. 0 otherwise, -1 on failure.
   */
  int (* iface_set_enabled_cb) (int state);
  /**
   * Callback to cleans up the interface and possibly restores the previous state
   * @return 1 on success, 0 on failure.
   */
  int (* iface_cleanup_cb) ();
  /**
   * Callback to check if a message from a given sender is to be accepted.
   * @return 1 if message should be accepted, 0 if it should be rejected.
   */
  int (* accept_sender_cb) (const struct sockaddr *);
  /** Pointer to private additional data */
  void * priv;
} abc_iface;


#ifndef __APPLE__  /* not sure what replaces the sll addresses for apple */
void abc_iface_set_default_sll_broadcast_address (struct sockaddr_ll * bc);
void abc_iface_print_sll_addr (struct sockaddr_ll * a, char * desc);
#endif /* __APPLE__ */

#endif /* ABC_IFACE_H */
