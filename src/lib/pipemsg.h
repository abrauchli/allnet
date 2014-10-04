/* pipemsg.h: transmit messages over pipes */

#ifndef PIPEMSG_H
#define PIPEMSG_H

#include <sys/types.h>
#include <sys/socket.h>


/* the send functions return 1 in case of success and 0 in case of failure.
 * if 0 is returned, it means the pipe is no longer valid.
 * the receive functions return the number of bytes received, 0
 * in case of timeout, and -1 in case of error, including the pipe
 * no longer being usable. */

extern int send_pipe_message (int pipe, const char * message, int mlen, int priority);

/* same as send_pipe_message, but frees the memory referred to by message */
extern int send_pipe_message_free (int pipe, char * message, int mlen,
                                   int priority);

/* send multiple messages at once, again to avoid the mysterious system
 * delay when sending multiple times in close succession on a socket.
 * messages are not freed */
extern int send_pipe_multiple (int pipe, int num_messages,
                               const char ** messages, const int * mlens, const int * priorities);
/* same, but messages are freed */
extern int send_pipe_multiple_free (int pipe, int num_messages,
                                    char ** messages, const int * mlens,
                                    const int * priorities);

/* receives the message into a buffer it allocates for the purpose. */
/* the caller is responsible for freeing the message buffer. */
extern int receive_pipe_message (int pipe, char ** message, int * priority);

/* keeps track of which pipes are needed for receive_pipe_message_any,
 * which buffers partial messages received on a socket. */
extern void add_pipe (int pipe);
extern void remove_pipe (int pipe);

#define PIPE_MESSAGE_WAIT_FOREVER	-1
#define PIPE_MESSAGE_NO_WAIT		0

/* receive on the first ready pipe, returning the size and message
 * for the first one received, and returning 0 in case of timeout
 * and -1 in case of error, including a closed pipe.
 * timeout is specified in ms.
 * the message (if any) is malloc'd, and must be free'd
 * The pipe from which the message is received (or which has an error)
 * is returned in *from_pipe if not NULL.
 */
extern int receive_pipe_message_any (int timeout, char ** message,
                                     int * from_pipe, int * priority);

/* same as receive_pipe_message_any, but listens to the given socket as
 * well as the pipes added previously,  The socket is assumed to be a
 * UDP or raw socket.  If the first message is received on this socket,
 * the message is read with recvfrom, assuming the size of the message
 * to be ALLNET_MTU or less (any more will be in the return value of
 * receive_pipe_message_fd, but not in the message)
 * sa and salen are passed directly as the last parameters to recvfrom.
 *
 * in case some other socket is ready first, or if fd is -1,
 * this call is the same as receive_pipe_message_any
 */
extern int receive_pipe_message_fd (int timeout, char ** message, int fd,
                                    struct sockaddr * sa, socklen_t * salen,
                                    int * from_pipe, int * priority);

#endif /* PIPEMSG_H */
