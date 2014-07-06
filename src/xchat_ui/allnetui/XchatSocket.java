package allnetui;

import java.net.*;

/**
 *
 * @author Edoardo Biagioni, esb@hawaii.edu
 *
 * A thread to read messages from xchat_socket and give them to the
 * user interface.  This class also maintains the socket and a buffer
 * (packet) used to send messages back to xchat_socket.
 */

public class XchatSocket extends Thread {

  static final DatagramSocket socket = initSocket();
  static final InetAddress local = InetAddress.getLoopbackAddress();
  UIAPI api = null;
  static final int xchatSocketPort = 0xa11c;  // ALLnet Chat port
  static final int allnetMTU = 12288;

  private static DatagramSocket initSocket() {
    try {
      return new DatagramSocket ();
    } catch (SocketException e) {
      System.out.println ("unable to open socket: " + e);
      System.exit (1);
    }
    return null;
  }

  // constructor.  Initialize the API variable
  public XchatSocket(UIAPI controller) {
    super();    // initialize thread
    this.api = controller;
  }

  // accessor methods
  public static DatagramSocket getSocket() {
    return socket;
  }

  public DatagramPacket makePacket(byte[] buf, int length) {
    DatagramPacket packet =
      new DatagramPacket (buf, length, local, xchatSocketPort);
    return packet;
  }

  private void sendInitial() {
    String s = new String("hello world\n");
    DatagramPacket p = makePacket(s.getBytes(), s.length());
    try {
      socket.send(p);
    } catch (java.io.IOException e) {
      System.out.println ("unable to send initial packet: " + e);
      System.exit (1);
    }
  }

  private static int b32 (byte [] data, int start, int len) {
    if (start + 4 > len)
      return 0;
    return (((((int) data [start    ]) & 0xff) << 24) |
            ((((int) data [start + 1]) & 0xff) << 16) |
            ((((int) data [start + 2]) & 0xff) <<  8) |
            ((((int) data [start + 3]) & 0xff)      ));
  }

  private static void w32 (byte [] data, int start, int value) {
    data [start  ] = ((byte) ((value >> 24) & 0xff));
    data [start+1] = ((byte) ((value >> 16) & 0xff));
    data [start+2] = ((byte) ((value >>  8) & 0xff));
    data [start+3] = ((byte) ((value      ) & 0xff));
  }

  private static long b48 (byte [] data, int start, int len) {
    if (start + 6 > len)
      return 0;
    return (((((long) data [start    ]) & 0xff) << 40) |
            ((((long) data [start + 1]) & 0xff) << 32) |
            ((((long) data [start + 2]) & 0xff) << 24) |
            ((((long) data [start + 3]) & 0xff) << 16) |
            ((((long) data [start + 4]) & 0xff) <<  8) |
            ((((long) data [start + 5]) & 0xff)      ));
  }

  private static void w48 (byte [] data, int start, long value) {
    data [start  ] = ((byte) ((value >> 40) & 0xff));
    data [start+1] = ((byte) ((value >> 32) & 0xff));
    data [start+2] = ((byte) ((value >> 24) & 0xff));
    data [start+3] = ((byte) ((value >> 16) & 0xff));
    data [start+4] = ((byte) ((value >>  8) & 0xff));
    data [start+5] = ((byte) ((value      ) & 0xff));
  }

  private static class IntRef {
    int value;
  }

  private static class LongRef {
    long value;
  }

  private static String bString (byte [] data, int start, int len,
                                 IntRef nextIndex) {
    if (nextIndex == null)
      nextIndex = new IntRef();   // not used, but simplifies the code
    for (int i = start; i < len; i++) {
      if (data [i] == 0) {
        if (i > start) {
          nextIndex.value = i + 1;
          return new String (data, start, i - start);
        } else {
          nextIndex.value = start + 1;   /* skip this null character */
          return new String ("");        /* empty string */
        }
      }
    }
    nextIndex.value = len;               /* reached the end, no null char */
    return null;
  }

  private static int wString (byte [] data, int start, String s) {
    byte [] sbytes = s.getBytes();
    System.arraycopy(sbytes, 0, data, start, s.length());
    int endIndex = start + s.length();
    data [endIndex] = 0;   // null byte, to terminate the string
    return endIndex + 1;   // return the next index after the null byte
  }

  /* Decode a message (if possible), and call the corresponding API function.
   * messages have a length, time, code, peer name, and text
   *   length (4 bytes, big-endian order) includes everything.
   *   time (6 bytes, big-endian order) is the time of original transmission
   *   code is 1 byte, value 0 for a data message, 1 for broadcast,
   *     2 for key exchange (key exchanges don't have the message)
   *     3 for secrets
   *   the peer name and the message are null-terminated
   */
  private void decodeForwardPacket (byte [] data, int dlen) {
    if (dlen < 11)
      return;
    int length = b32 (data, 0, dlen);
    if (length != dlen) {
      System.out.println ("embedded length " + length + ", received " + dlen);
      /* return; */
    }
    long time = b48 (data, 4, dlen);
    int code = data [10];
    if ((code < 0) || (code > 2)) {
      System.out.println ("unknown message code " + code);
      /* return; */
    }
    IntRef nextIndex = new IntRef();
    String peer = bString (data, 11, dlen, nextIndex);
    if ((code == 0) || (code == 1)) {
      String message = bString (data, nextIndex.value, dlen, nextIndex);
      System.out.println ("message '" + message + "' from " + peer);
      api.messageReceived (peer, time * 1000, message);
    } else if (code == 2) {
      System.out.println ("new key from " + peer);
      api.contactCreated(peer);
    } else {
      System.out.println ("unknown code " + code);
    }
  }

  private static DatagramPacket makeMessagePacket(String peer, String text,
                                                  LongRef time, boolean bc) {
    int size = peer.length() + text.length() + 2 + 11;
    byte [] buf = new byte [size];
    DatagramPacket packet =
      new DatagramPacket (buf, size, local, xchatSocketPort);
    w32(buf, 0, size);
    time.value = System.currentTimeMillis();
    w48(buf, 4, time.value / 1000);
    if (bc)
      buf [10] = 1;
    else
      buf [10] = 0;
    int newIndex = wString (buf, 11, peer);
    wString (buf, newIndex, text);
    return packet;
  }

  private static DatagramPacket makeKeyPacket(String peer, String s1,
                                              String s2, int hops) {
    int size = peer.length() + s1.length() + 2 + 11;
    if ((s2 != null) && (s2.length() > 0))
      size += s2.length() + 1;
    byte [] buf = new byte [size];
    DatagramPacket packet =
      new DatagramPacket (buf, size, local, xchatSocketPort);
    w32(buf, 0, size);
    w48(buf, 4, hops);
    buf [10] = 2;
    int newIndex = wString(buf, 11, peer);
    newIndex = wString(buf, newIndex, s1);
    if ((s2 != null) && (s2.length() > 0))
      wString(buf, newIndex, s2);
    return packet;
  }

  public static long sendToPeer(String peer, String text) {
    LongRef time = new LongRef();
    DatagramPacket packet = makeMessagePacket(peer, text, time, false);
    try {
      socket.send (packet);
    } catch (java.io.IOException e) {
      System.out.println ("send exception: " + e);
      return -1;
    }
    return time.value;
  }

  public static boolean sendKeyRequest(String peer, String s1, String s2,
                                       int hops) {
    DatagramPacket packet = makeKeyPacket(peer, s1, s2, hops);
    try {
      socket.send (packet);
    } catch (java.io.IOException e) {
      System.out.println ("send exception: " + e);
      return false;
    }
    return true;
  }

  /* main method, called to start the thread
   * listen to the xchat socket for incoming messages.  Decode them,
   * and call the corresponding API function.
   */

  public void run() {
    System.out.println("running XchatSocket.java");
    sendInitial();
    byte [] input = new byte [allnetMTU];
    DatagramPacket received = new DatagramPacket (input, input.length);
    while (true) {
      try {
        socket.receive (received);
      } catch (java.io.IOException e) {
        System.out.println ("receive got an exception: " + e);
        System.out.println ("unable to receive messages, terminating");
        System.exit (1);
      }
//      System.out.println ("received packet: " + received +
//                          " from " + received.getSocketAddress ());
      if (received.getPort () != xchatSocketPort) {
        System.out.println ("packet from port " + received.getPort() +
                            ", only accepting from " + xchatSocketPort);
      } else if (! received.getAddress().isLoopbackAddress()) {
        System.out.println ("packet from address " + received.getAddress() +
                            ", only accepting from loopback address" );
      } else {
        decodeForwardPacket (received.getData (), received.getLength ());
      }
    }
  }

}
