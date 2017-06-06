#ifndef DIM_MQTT_SOCKET_BUFFER_H_
#define DIM_MQTT_SOCKET_BUFFER_H_

#include <sys/socket.h>

#if defined(OPENSSL)
#include <openssl/ssl.h>
#endif

typedef struct iovec iobuf;

typedef struct {
  int           socket;
  unsigned int  index;
  size_t        headerLen;
  char          fixed_header[5]; /**< header plus up to 4 length bytes */
  size_t        bufLen;        /**< total length of the buffer */
  size_t        dataLen;       /**< current length of data in buf */
  char*         buf;
} socketQueue;

typedef struct {
  int       socket;
  int       count;
  int       total;
#ifdef OPENSSL
  SSL*      ssl;
#endif
  size_t    bytes;
  iobuf     iovecs[5];
  int       frees[5];
} pendingWriteBuf;

#define SOCKET_BUFFER_COMPLETE 0

#ifndef SOCKET_ERROR
#define SOCKET_ERROR -1
#endif

#define SOCKET_BUFFER_INTERRUPTED -22 /* must be the same value as TCPSOCKET_INTERRUPTED*/

void SocketBuffer_initialize(void);
void SocketBuffer_terminate(void);
void SocketBuffer_cleanup(int socket);
char* SocketBuffer_getQueuedData(int socket, size_t bytes, size_t* actual_len);
int SocketBuffer_getQueuedChar(int socket, char* c);
void SocketBuffer_interrupted(int socket, size_t actual_len);
char* SocketBuffer_complete(int socket);
void SocketBuffer_queueChar(int socket, char c);

#ifdef OPENSSL
void SocketBuffer_pendingWrite(int socket, SSL* ssl, int count, iobuf* iovecs, int* frees, size_t total, size_t bytes);
#else
void SocketBuffer_pendingWrite(int socket, int count, iobuf* iovecs, int* frees, size_t total, size_t bytes);
#endif

pendingWriteBuf* SocketBuffer_getWrite(int socket);
int SocketBuffer_writeComplete(int socket);
pendingWriteBuf* SocketBuffer_updateWrite(int socket, char* topic, char* payload);

#endif
