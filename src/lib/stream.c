/* stream.c: cryptography for data streams */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "stream.h"
#include "crypt_sel.h"
#include "util.h"
#include "sha.h"
#include "wp_aes.h"

struct allnet_state {
  char key [ALLNET_STREAM_KEY_SIZE];
  char secret [ALLNET_STREAM_SECRET_SIZE];
  int counter_size;
  int hash_size;
  uint64_t counter;
  int block_offset;   /* how many bytes we are into the block */
};

/* allnet_stream_init allocates and initializes state for encrypting and
 * decrypting.  It can do so from a given key and secret, or it can
 * initialize the the key and secret for the caller.
 *
 * key must have size ALLNET_STREAM_KEY_SIZE, and must be initialized by
 * the caller prior to calling allnet_stream_init (if init_key is 0) or
 * will be initialized by allnet_stream_init (if init_key is nonzero).
 * the secret is used for authentication, giving the hash, must be of size
 * ALLNET_STREAM_SECRET_SIZE, and is initialized by the caller
 * or by allnet_stream_init as for the key, depending on init_secret.
 * state will be given a pointer to malloc'd memory, NULL in case of failure
 * counter size and hash size are the number of bytes of counter and hmac
 * to be added to each outgoing packet, and checked on each incoming packet */
void allnet_stream_init (char ** state, char * key, int init_key,
                         char * secret, int init_secret,
                         int counter_size, int hash_size)
{
  if ((counter_size <= 0) || (counter_size > 8) ||
      (hash_size < 0) || (hash_size > SHA512_SIZE))
    return;
  int size = sizeof (struct allnet_state);
  *state = malloc_or_fail (size, "allnet_stream_init");
  if (*state == NULL)
    return;
  if (init_key)
    random_bytes (key, ALLNET_STREAM_KEY_SIZE);
  if (init_secret)
    random_bytes (secret, ALLNET_STREAM_SECRET_SIZE);
  struct allnet_state * sp = (struct allnet_state *) (*state);
  memcpy (sp->key, key, ALLNET_STREAM_KEY_SIZE);
  memcpy (sp->secret, secret, ALLNET_STREAM_KEY_SIZE);
  sp->counter_size = counter_size;
  sp->hash_size = hash_size;
  sp->counter = 1;  /* start with counter value of 1 */
  sp->block_offset = 0;
}

static void update_counter (char * bytes, uint64_t value)
{
  int write_offset = WP_AES_BLOCK_SIZE - sizeof (uint64_t);
  bzero (bytes, write_offset);
  writeb64 (bytes + write_offset, value);
}

/* serves two purposes: initializing the block, or getting the next aes char */
static int aes_next_byte (struct allnet_state * sp, char * block, int init,
                          allnet_aes_key * aes)
{
  if ((init) || ((sp->block_offset % WP_AES_BLOCK_SIZE) == 0)) {
    if (! init) {
      (sp->counter)++;
      sp->block_offset = 0;
    } else {
      aes = NULL;
    }
    char counter [WP_AES_BLOCK_SIZE];
    update_counter (counter, sp->counter);
    if (! allnet_aes_encrypt_block (sp->key, counter, block, &aes)) {
      printf ("aes unknown error, unable to encrypt\n");
      return -1;
    }
    if (init)
      return 0;
  }
  int index = sp->block_offset;
  (sp->block_offset)++;
  return block [index];
}

/* allnet_stream_encrypt_buffer encrypts a buffer given an encryption state
 * state must have been initialized by allnet_stream_init
 * rsize must be >= tsize + counter_size + hash_size specified for state
 * returns the encrypted size for success, 0 for failure */
int allnet_stream_encrypt_buffer (char * state,
                                  const char * text, int tsize,
                                  char * result, int rsize)
{
  if (tsize <= 0) {
    printf ("error: aes encryption needs at least 1 text byte, %d given\n",
            tsize);
    return 0;
  }
  struct allnet_state * sp = (struct allnet_state *) state;
  if (tsize + sp->counter_size + sp->hash_size > rsize) {
    printf ("error: aes encryption needs %d bytes for text, ", tsize);
    printf ("%d for counter, %d for hash, ", sp->counter_size, sp->hash_size);
    printf ("but only %d available\n", rsize);
    return 0;
  }
  /* compute the initial counter value, measured in bytes */
  uint64_t send_counter = sp->counter * WP_AES_BLOCK_SIZE + sp->block_offset;
  /* encrypt the data */
  char crypt_block [WP_AES_BLOCK_SIZE];
  allnet_aes_key * aes = NULL;
  aes_next_byte (sp, crypt_block, 1, aes);   /* init crypt_block */
  int i;
  for (i = 0; i < tsize; i++)
    result [i] = text [i] ^ aes_next_byte (sp, crypt_block, 0, aes);
  int written = tsize;
  /* write the least significant sp->counter_size bytes of the send
   * counter to the result */
  if (sp->counter_size > 0) {
    char send_counter_bytes [sizeof (uint64_t)];
    writeb64 (send_counter_bytes, send_counter);
    bzero (result + tsize, sp->counter_size);
    int num_bytes = sp->counter_size;
    char * send_pointer = result + tsize;
    if (num_bytes > sizeof (uint64_t)) {
      send_pointer = result + tsize + (num_bytes - sizeof (uint64_t));
      num_bytes = sizeof (uint64_t);
    }
    memcpy (send_pointer,
            send_counter_bytes + (sizeof (uint64_t) - num_bytes), num_bytes);
    written += num_bytes;
  }
  /* compute and add the hmac */
  if (sp->hash_size > 0) {
    char hmac [SHA512_SIZE];
    /* the hmac covers the ciphertext and the counter bytes */
    sha512hmac (result, written, sp->secret, ALLNET_STREAM_SECRET_SIZE, hmac);
    int num_bytes = sp->hash_size;
    if (sp->hash_size > SHA512_SIZE) {
      bzero (result + written, sp->hash_size - SHA512_SIZE);
      written += sp->hash_size - SHA512_SIZE;
      num_bytes = SHA512_SIZE;
    }
    memcpy (result + written, hmac, num_bytes);
    written += num_bytes;
  }
  return written;
}

/* allnet_stream_encrypt_buffer decrypts a buffer given an encryption state
 * the buffer must normally have been created by a corresponding call to
 * allnet_stream_encrypt_buffer, usually on a remote system.
 *
 * state must have been initialized by allnet_stream_init
 * tsize must be >= csize - counter_size - hash_size specified for state
 * returns 1 for successful authentication and decryption, 0 otherwise
 * note: an attacker has a 256^-hash_size chance of sending a packet that
 * decrypt_buffer will consider authentic.  In such cases, decrypt_buffer
 * will return 1 but, in most cases, the decrypted text will be meaningless */
int allnet_stream_decrypt_buffer (char * state,
                                  const char * packet, int psize,
                                  char * text, int tsize)
{
  if (psize <= 0) {
    printf ("error: aes decryption needs at least 1 byte, %d given\n",
            tsize);
    return 0;
  }
  struct allnet_state * sp = (struct allnet_state *) state;
  if (sp->counter_size + sp->hash_size >= psize) {
    printf ("aes decryption with %d-byte counter, %d-byte hash, ",
            sp->counter_size, sp->hash_size);
    printf ("can only decrypt packets > %d bytes, but %d given\n",
            sp->counter_size + sp->hash_size, psize);
    return 0;
  }
  if (psize - (sp->counter_size + sp->hash_size) > tsize) {
    printf ("aes decryption with %d-byte counter, %d-byte hash, ",
            sp->counter_size, sp->hash_size);
    printf ("%d-byte packet produces %d-byte result, but only %d given\n",
            psize, psize - (sp->counter_size + sp->hash_size), tsize);
    return 0;
  }
  /* check the hmac */
  if (sp->hash_size > 0) {
    char hmac [SHA512_SIZE];
    /* the hmac covers the ciphertext and the counter bytes */
    sha512hmac (packet, psize - sp->hash_size,
                sp->secret, ALLNET_STREAM_SECRET_SIZE, hmac);
    int num_bytes = sp->hash_size;
    if (sp->hash_size > SHA512_SIZE)
      num_bytes = SHA512_SIZE;
    if (memcmp (packet + (psize - num_bytes), hmac, num_bytes) != 0) {
      printf ("aes authentication failed\n");
      return 0;
    }
  }
  /* hmac checks out, decrypt the packet */
  uint64_t counter = sp->counter * WP_AES_BLOCK_SIZE + sp->block_offset;
  if (sp->counter_size > 0) {
    char counter_bytes [sizeof (uint64_t)];
    bzero (counter_bytes, sizeof (counter_bytes));
    int num_bytes = sp->counter_size;
    if (sp->counter > sizeof (uint64_t))
      num_bytes = sizeof (uint64_t);
    memcpy (counter_bytes + (sizeof (uint64_t) - num_bytes),
            packet + (psize - sp->hash_size - num_bytes), num_bytes);
    uint64_t received_counter = readb64 (counter_bytes);
    int shift = 8 * num_bytes;
    if (shift >= 64)
      counter = received_counter;
    else
      counter = (((counter >> shift) << shift) | received_counter);
  }
  sp->block_offset = counter % WP_AES_BLOCK_SIZE;
  sp->counter = counter / WP_AES_BLOCK_SIZE;
  /* decrypt and return */
  int rsize = psize - (sp->counter_size + sp->hash_size);
  char crypt_block [WP_AES_BLOCK_SIZE];
  allnet_aes_key * aes = NULL;
  aes_next_byte (sp, crypt_block, 1, aes);   /* init crypt_block */
  int i;
  for (i = 0; i < rsize; i++)
    text [i] = packet [i] ^ aes_next_byte (sp, crypt_block, 0, aes);
  return rsize;
}
