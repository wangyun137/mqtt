#ifndef DIM_MQTT_SSL_SOCKET_H_
#define DIM_MQTT_SSL_SOCKET_H_


#ifdef OPENSSL

#include <pthread.h>
#include <semaphore.h>
#include <openssl/ssl.h>

#include "SocketBuffer.h"
#include "Clients.h"

#define URI_SSL "ssl://"

int     SSLSocket_initialize(void);
void    SSLSocket_terminate(void);
int     SSLSocket_setSocketForSSL(NetworkHandles* net, MQTTClient_SSLOptions* opts, char* hostname);
int     SSLSocket_getChar(SSL* ssl, int socket, char* c);
char*   SSLSocket_getData(SSL* ssl, int socket, size_t bytes, size_t* actual_len);

int     SSLSocket_close(NetworkHandles* net);
int     SSLSocket_putDatas(SSL* ssl, int socket, char* buf0, size_t buf0len, int count, char** buffers, size_t* buflens, int* frees);
int     SSLSocket_connect(SSL* ssl, int socket);

int     SSLSocket_getPendingRead(void);
int     SSLSocket_continueWrite(pendingWrite* pw);

#endif // OPENSSL

#endif //DIM_MQTT_SSL_SOCKET_H_
