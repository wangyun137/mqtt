#ifndef DIM_MQTT_SOCKET_H_
#define DIM_MQTT_SOCKET_H_

#include <sys/types.h>

#define INVALID_SOCKET SOCKET_ERROR
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/select.h>
#include <netinet/in.h> /* 包含struct sockaddr_in*/
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#define ULONG size_t
/** socket operation completed successfully */
#define TCPSOCKET_COMPLETE 0

#ifndef SOCKET_ERROR
#define SOCKET_ERROR -1
#endif
/** must be the same as SOCKET_BUFFER_INTERRUPTED */
#define TCPSOCKET_INTERRUPTED -22
#define SSL_FATAL -3

#ifndef INE6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46 /** only needed for gcc/cygwin on windows */
#endif

#ifndef max
#define max(A,B) ((A) > (B) ? (A) : (B))
#endif

#include "LinkedList.h"

/**
 * Structure to hold all socket data for the module
 */
typedef struct
{
	fd_set readSet; /**  socket输入是否就绪的文件描述符集合 */
	fd_set readSetSaved; /** saved socket read set */
	int maxFdp1; /**< max descriptor used +1 (again see select doc) */
	List* clientsList; /** socket文件描述符列表 */
	ListElement* curClient; /** 当前socket文件描述符 */
	List* connectPending; /** 等待连接的socket列表 */
	List* writePending; /** 写操作等待的socket列表 */
	fd_set pendingWriteSet; /** socket输出是否就绪的文件描述符集合 */
} Sockets;

void Socket_outInitialize(void);
void Socket_outTerminate(void);
int Socket_getReadySocket(int more_work, struct timeval *tp);
int Socket_getChar(int socket, char* c);
char *Socket_getData(int socket, size_t bytes, size_t* actual_len);
int Socket_putDatas(int socket, char* buf0, size_t buf0len, int count, char** buffers, size_t* buflens, int* frees);
void Socket_close(int socket);
int Socket_new(char* addr, int port, int* socket);

int Socket_noPendingWrites(int socket);
char* Socket_getPeer(int sock);

void Socket_addPendingWrite(int socket);
void Socket_clearPendingWrite(int socket);

typedef void Socket_writeComplete(int socket);
void Socket_setWriteCompleteCallback(Socket_writeComplete*);


#endif // DIM_MQTT_SOCKET_H_
