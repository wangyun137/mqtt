/**
* Socket相关功能
*/

#include "Socket.h"
#include "SocketBuffer.h"
#ifdef OPENSSL
#include "SSLSocket.h"
#endif

#include "Log.h"

int Socket_setNonBlocking(int sock);
int Socket_error(char* aString, int sock);
int Socket_addSocket(int newSd);
int isReady(int socket, fd_set* read_set, fd_set* write_set);
int Socket_writev(int socket, iobuf* iovecs, int count, unsigned long* bytes);
int Socket_close_only(int socket);
int Socket_continueWrite(int socket);
int Socket_continueWrites(fd_set* pwSet);
char* Socket_getAddrName(struct sockaddr* sa, int sock);

/* 持有socket的全部数据*/
Sockets s;
static fd_set wset;

/**
* 设置一个非阻塞的socket
* @param sock 被设置的socket文件描述符
* @return TCP错误码
*/
int Socket_setNonBlocking(int sock) {
    int rc;
    int flags;
    if ((flags = fcntl(sock, F_GETFL, 0))) {
        flags = 0;
    }
    rc = fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    return rc;
}

/**
* 获取socket相应的错误
* @param aString
* @param sock 发生错误的socket文件描述符
* @return TCP错误码
*/
int Socket_error(char* aString, int sock) {
    if (errno != EINTR && errno != EAGAIN && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        if (strcmp(aString, "shutdown") != 0 || (errno != ENOTCONN && errno != ECONNRESET)) {
            LOG("Socket error %s in %s for socket %d", strerror(errno), aString, sock);
        }
    }
    return errno;
}

/**
* 初始化Sockets结构
*/
void Socket_outInitialize(void) {
    //忽略SIGPIPE信号(管道断开)
    //TODO:建议改为sigaction()函数
    signal(SIGPIPE, SIG_IGN);

    SocketBuffer_initialize();
    s.clientsList = ListInitialize();
    s.connectPending= ListInitialize();
    s.writePending = ListInitialize();
    s.curClient = NULL;
    FD_ZERO(&(s.readSet));
    FD_ZERO(&(s.pendingWriteSet));
    s.maxFdp1 = 0;
    memcpy((void*)&(s.readSetSaved), (void*)&(s.readSet), sizeof(s.readSetSaved));
}

/**
* 释放Sockets结构内存
*/
void Socket_outTerminate(void) {
    ListFree(s.connectPending);
    ListFree(s.writePending);
    ListFree(s.clientsList);
    SocketBuffer_terminate();
}

/**
* 新增一个socket文件描述符到socket列表中
*/
int Socket_addSocket(int newSd) {
    int rc = 0;
    if (ListFindItem(s.clientsList, &newSd, intCompare) == NULL) {
        /* 如果在列表中没有找到目标socket, 将socket添加至clientsList和readSetSaved
        并设置socket为非阻塞的*/
        int* pnewSd = (int*)malloc(sizeof(newSd));
        *pnewSd = newSd;
        ListAppend(s.clientsList, pnewSd, sizeof(newSd));
        //将newSd放入readSetSaved集合中
        FD_SET(newSd, &(s.readSetSaved));
        s.maxFdp1 = max(s.maxFdp1, newSd + 1);
        rc = Socket_setNonBlocking(newSd);
    }

    return rc;
}

/**
* select以后，会改变集合，里面包含的就是已处于就绪状态的文件描述符
* 如果socket在write_set且在read_set中同时不在延迟写队列中，表示该socket就绪;
* 或者如果socket在write_set中且在延迟连接队列中，也表示该socket就绪
*/
int isReady(int socket, fd_set* read_set, fd_set* write_set) {
    int rc = 1;
    if (ListFindItem(s.connectPending, &socket, intCompare) && FD_ISSET(socket, write_set)) {
        //如果socket在等待连接队列中且在write_set中，从connectPending中删除socket
        ListRemoveItem(s.connectPending, &socket, intCompare);
    } else {
        //如果socket在read_set，同时在write_set，同时socket不再writePending中
        rc = FD_ISSET(socket, read_set) && FD_ISSET(socket, write_set) && Socket_noPendingWrites(socket);
    }

    return rc;
}

/**
* 返回下一个已就绪的socket
* @param more_work 指明是否还有更多work在等待
* @param tp select调用的超时时间
* @return 0:没有就绪的socket, 或下一个就绪的socket
*/
int Socket_getReadySocket(int more_work, struct timeval *tp) {
    int rc = 0;
    //0秒
    static struct timeval zero = {0L, 0L};
    static struct timeval one = {1L, 0L};
    struct timeval timeout = one;

    if (s.clientsList->count == 0) {
        goto exit;
    }

    if (more_work) {
        timeout = zero;
    } else if (tp) {
        timeout = *tp;
    }

    //先遍历clientsList列表，查看那个socket是处于就绪的
    while (s.curClient != NULL) {
        if (isReady(*((int*)(s.curClient->content)), &(s.readSet), &wset)) {
            break;
        }
        //遍历clientsList, 获取下一个节点
        ListNextElement(s.clientsList, &s.curClient);
    }

    if (s.curClient == NULL) {
        int rc1;
        fd_set pwset;

        //将readSetSaved中的socket文件描述符拷贝到readSet中
        memcpy((void*)&(s.readSet), (void*)&(s.readSetSaved), sizeof(s.readSet));
        memcpy((void*)&(pwset), (void*)&(s.pendingWriteSet), sizeof(pwset));
        /* 等待readSet和pendingWriteSet中有文件描述符处于就绪状态, 1秒的超时时间*/
        if ((rc1 = select(s.maxFdp1, &(s.readSet), &pwset, NULL, &timeout)) == SOCKET_ERROR) {
            Socket_error("read_select", 0);
            goto exit;
        }
        if (Socket_continueWrites(&pwset) == SOCKET_ERROR) {
            rc = 0;
            goto exit;
        }
        //将readSetSaved中的socket文件描述符拷贝到wse中
        /* 创建一个Socket后, socket的文件描述符会被添加至readSetSaved，再往readSet和wset里都复制进去*/
        memcpy((void*)&wset, (void*)&(s.readSetSaved), sizeof(wset));
        //0秒的超时时间，即不会阻塞，直接返回有没有就绪的文件描述符
        //只检查wset中的文件描述符
        if ((rc1 = select(s.maxFdp1, NULL, &(wset), NULL, &zero)) == SOCKET_ERROR) {
            Socket_error("write select", 0);
            rc = rc1;
            goto exit;
        }

        if (rc == 0 && rc1 == 0) {
            goto exit;
        }

        //遍历clientsList, 查看那个socket目前是处于就绪的
        s.curClient = s.clientsList->first;
        while (s.curClient != NULL) {
            int curSock = *((int*)(s.curClient->content));
            if (isReady(curSock, &(s.readSet), &wset)) {
                break;
            }
            ListNextElement(s.clientsList, &s.curClient);
        }
    }

    if (s.curClient == NULL) {
        rc = 0;
    } else {
        rc = *((int*)(s.curClient->content));
        //将curClient指向下一个socket节点
        ListNextElement(s.clientsList, &s.curClient);
    }

exit:
    return rc;
}


/**
* 从socket中读取一个字节
*/
int Socket_getChar(int socket, char* c) {
    int rc = SOCKET_ERROR;
    if ((rc = SocketBuffer_getQueuedChar(socket, c)) != SOCKET_BUFFER_INTERRUPTED) {
        goto exit;
    }
    //从socket中读取一个字节
    if ((rc = recv(socket, c, (size_t)1, 0)) == SOCKET_ERROR) {
        int err = Socket_error("recv - getChar", socket);
        if (err == EWOULDBLOCK || err == EAGAIN) {
            rc = TCPSOCKET_INTERRUPTED;
            //如果出现错误，将socket放入缓冲队列
            SocketBuffer_interrupted(socket, 0);
        }
    } else if (rc == 0) {
        rc = SOCKET_ERROR;
    } else if (rc == 1) {
        SocketBuffer_queueChar(socket, *c);
        rc = TCPSOCKET_COMPLETE;
    }
exit:
    return rc;
}


/**
* 非阻塞的从socket中读取字节,如果之前有未读完的数据，一起读取
*/
char* Socket_getData(int socket, size_t bytes, size_t* actual_len) {
    int rc;
    char* buf;

    if (bytes == 0) {
        buf = SocketBuffer_complete(socket);
        goto exit;
    }

    //先查看是否有上次未读完的数据, 如果没有则actual_len会置为0,同时分配一块1000bytes的缓冲区并返回
    buf = SocketBuffer_getQueuedData(socket, bytes, actual_len);
    if ((rc = recv(socket, buf + (*actual_len), (int)(bytes - (*actual_len)), 0)) == SOCKET_ERROR) {
        rc = Socket_error("recv - getData", socket);
        if (rc != EAGAIN && rc != EWOULDBLOCK) {
            buf = NULL;
            goto exit;
        }
    } else if (rc == 0) {
        buf = NULL;
        goto exit;
    } else {
        *actual_len += rc;
    }

    if (*actual_len == bytes) {
        SocketBuffer_complete(socket);
    } else {
        SocketBuffer_interrupted(socket, *actual_len);
    }

exit:
    return buf;
}

/**
 * 查看socket是否处于写操作等待列表中
 * @param socket
 * @return 1: socket不在写操作等待列表中
 */
int Socket_noPendingWrites(int socket) {
    int curSock = socket;
    return ListFindItem(s.writePending, &curSock, intCompare) == NULL;
}


/**
* 一次性写入一系列数据
*/
int Socket_writev(int socket, iobuf* iovecs, int count, unsigned long* bytes) {
    int rc;
    *bytes = 0L;
    rc = writev(socket, iovecs, count);
    if (rc == SOCKET_ERROR) {
        int err = Socket_error("writev - putDatas", socket);
        if (err == EWOULDBLOCK || err == EAGAIN) {
            rc = TCPSOCKET_INTERRUPTED;
        }
    } else {
        *bytes = rc;
    }

    return rc;
}

/**
* 使用缓存区，一次性写入一系列数据
* @param socket
* @param buf0 第一块缓存区
* @param buf0len 第一块缓存区的长度
* @param count 其他缓存区数量
* @param buffers 缓存数组
* @param bufLens 数组，每一个item记录每一个缓存区的长度
* @return
*/
int Socket_putDatas(int socket, char* buf0, size_t buf0len, int count, char** buffers, size_t* bufLens, int* frees) {
    unsigned long bytes = 0L;
    iobuf iovecs[5];
    int frees1[5];
    int rc = TCPSOCKET_INTERRUPTED;
    int i;
    size_t total = buf0len;

    if (!Socket_noPendingWrites(socket)) {
        //如果socket在pendingWrite中, 直接返回
        rc = SOCKET_ERROR;
        goto exit;
    }

    for (i = 0; i < count; i++) {
        //计算总的要写的bytes数
        total += bufLens[i];
    }

    //将buf0设为第一个iovecs
    iovecs[0].iov_base = buf0;
    iovecs[0].iov_len = (ULONG)buf0len;
    frees1[0] = 1;

    for (i = 0; i < count; i++) {
        iovecs[i+1].iov_base = buffers[i];
        iovecs[i+1].iov_len = (ULONG)bufLens[i];
        frees1[i+1] = frees[i];
    }

    if ((rc = Socket_writev(socket, iovecs, count+1, &bytes)) != SOCKET_ERROR) {
        if (bytes == total) {
            //跟预期要写入的bytes数一致
            rc = TCPSOCKET_COMPLETE;
        } else {
            int* sockMem = (int*)malloc(sizeof(int));
#ifdef OPENSSL
            SocketBuffer_pendingWrite(socket, NULL, count+1, iovecs, frees1, total, bytes);
#else
            SocketBuffer_pendingWrite(socket, count+1, iovecs, frees1, total, bytes);
#endif

            *sockMem = socket;
            ListAppend(s.writePending, sockMem, sizeof(int));
            FD_SET(socket, &(s.pendingWriteSet));
            rc = TCPSOCKET_INTERRUPTED;
        }
    }

exit:
    return rc;
}

void Socket_addPendingWrite(int socket) {
    FD_SET(socket, &(s.pendingWriteSet));
}

void Socket_clearPendingWrite(int socket) {
    if (FD_ISSET(socket, &(s.pendingWriteSet))) {
        FD_CLR(socket, &(s.pendingWriteSet));
    }
}

/**
* 关闭Socket但并不从select列表中删除
*/
int Socket_close_only(int socket) {
    int rc;
    if (shutdown(socket, SHUT_WR) == SOCKET_ERROR) {
        //禁止socket传输
        Socket_error("shutdown", socket);
    }

    if ((rc = recv(socket, NULL, (size_t)0, 0)) == SOCKET_ERROR) {
        Socket_error("shutdown", socket);
    }

    if ((rc = close(socket)) == SOCKET_ERROR) {
        Socket_error("close", socket);
    }

    return rc;
}

void Socket_close(int socket) {
    Socket_close_only(socket);
    //清空readSetSaved
    FD_CLR(socket, &(s.readSetSaved));
    if (FD_ISSET(socket, &(s.pendingWriteSet))) {
        FD_CLR(socket, &(s.pendingWriteSet));
    }

    if (s.curClient != NULL && *(int*)(s.curClient->content) == socket) {
        s.curClient = s.curClient->next;
    }

    ListRemoveItem(s.connectPending, &socket, intCompare);
    ListRemoveItem(s.writePending, &socket, intCompare);
    SocketBuffer_cleanup(socket);

    if (ListRemoveItem(s.clientsList, &socket, intCompare)) {
        LOG("Success to remove socket %d", socket);
    }

    if (socket + 1 >= s.maxFdp1) {
        ListElement* curClient = NULL;

        s.maxFdp1 = 0;
        while (ListNextElement(s.clientsList, &curClient)) {
            s.maxFdp1 = max(*((int*)(curClient->content)), s.maxFdp1);
        }
        ++(s.maxFdp1);
    }
}

/**
* 创建一个新socket
*/
int Socket_new(char* addr, int port, int* sock) {
    int type = SOCK_STREAM;
    /*
    struct in_addr {
      in_addr_t s_addr; //32位整数值
    }

    struct sockaddr_in {
        sa_family_t sin_family; //地址族，这里为AF_INET
        in_port_t   sin_port; //端口号
        struct in_addr sin_addr; //IPV4地址
        unsigned char __pad[X]; //
    };
    */
    struct sockaddr_in address;
#ifdef AF_INET6
    struct sockaddr_in6 address6;
#endif
    int rc = SOCKET_ERROR;

    sa_family_t family = AF_INET;

    /*
    struct addrinfo{
        int ai_flags; //AI_*
        int ai_family;//地址族,AF_INET or AF_INET6,或其他AF_开头的， AF_UNSPEC获取所有种类
        int ai_socktype; SOCK_STREAM, SOCK_DGRAM
        int ai_protocol;
        size_t ai_addrlen;//ai_addr的大小
        char* ai_canonname; //host名称
        struct sockaddr *ai_addr;
        struct addrinfo *ai_next;
    }
    */
    struct addrinfo* result = NULL;
    struct addrinfo hints = {0, AF_UNSPEC, SOCK_STREAM, IPPROTO_TCP, 0, NULL, NULL, NULL};

    *sock = -1;
#ifdef AF_INET6
    memset(&address6, '\0', sizeof(address6));
#endif

    if (addr[0] == '[') {
        ++addr;
    }
    //getaddrinfo()将主机和服务名装换成IP地址和端口号
    //主机名转换成ip是通过DNS
    //服务名和端口号是对应的，每一份端口对应一个服务名，端口和服务名会记录在/etc/services文件中
    //getnameinfo()是逆函数，即将ip+port转换成主机名+服务名
    /* getaddrinfo()会动态的分配一个包含addrinfo的链表并将result指向这个链表的表头
    result参数返回一个结构列表而不是单个结构, 因为在host, service, hints指定的标准对应的
    主机和服务组合可能有多个*/
    if ((rc = getaddrinfo(addr, NULL, &hints, &result)) == 0) {
        struct addrinfo* res = result;
        while (res) {
            if (res->ai_family == AF_INET || res->ai_next == NULL) {
                break;
            }
            res = res->ai_next;
        }

        if (res == NULL) {
            rc = -1;
        } else
#ifdef AF_INET6
        if (res->ai_family == AF_INET6) {
            address6.sin6_port = htons(port);
            address6.sin6_family = family = AF_INET6;
            memcpy(&(address6.sin6_addr), &((struct sockaddr_in6*)(res->ai_addr))->sin6_addr, sizeof(address6.sin6_addr));
        }
#endif
        if (res->ai_family == AF_INET) {
            /* htons将主机的无符号短整形数转换成网络字节顺序(大端序)*/
            address.sin_port = htons(port);
            address.sin_family = family = AF_INET;
            address.sin_addr = ((struct sockaddr_in*)(res->ai_addr))->sin_addr;
        } else {
            rc = -1;
        }
        //释放地址列表
        freeaddrinfo(result);
    }

    if (rc != 0) {
        LOG("%s is not a valid IP address in %s-%d", addr, __FILE__, __LINE__);
    } else {
        *sock = (int)socket(family, type, 0);
        if (*sock == INVALID_SOCKET) {
            rc = Socket_error("socket", *sock);
        } else {
#ifdef NOSIGPIPE
            int opt = 1;
            if (setsockopt(*sock, SOL_SOCKET, SO_NOSIGPIPE, (void*)&opt, sizeof(opt)) != 0) {
                LOG("Could not set SO_NOSIGPIPE for socket %d in %s-%d", *sock, __FILE__, __LINE__);
            }
#endif

            if (Socket_addSocket(*sock) == SOCKET_ERROR) {
                rc = Socket_error("setNonBlocking", *sock);
            } else {
                if (family == AF_INET) {
                    rc = connect(*sock, (struct sockaddr*)&address, sizeof(address));
                }
#ifdef AF_INET6
                else {
                    rc = connect(*sock, (struct sockaddr*)&address6, sizeof(address6));
                }
#endif

                if (rc == SOCKET_ERROR) {
                    rc = Socket_error("connect", *sock);
                }

                if (rc == EINPROGRESS || rc == EWOULDBLOCK) {
                    int* pnewSd = (int*)malloc(sizeof(int));
                    *pnewSd = *sock;
                    ListAppend(s.connectPending, pnewSd, sizeof(int));
                }
            }
        }
    }
    return rc;
}


static Socket_writeComplete* writeComplete = NULL;

void Socket_setWriteCompleteCallback(Socket_writeComplete* myWriteComplete) {
    writeComplete = myWriteComplete;
}

int Socket_continueWrite(int socket) {
    int rc = 0;
    pendingWriteBuf* pw;
    unsigned long curBufLen = 0L;
    unsigned long bytes;

    int curBuf = -1, i;
    iobuf iovecs1[5];

    pw = SocketBuffer_getWrite(socket);

    #ifdef OPENSSL
    if (pw->ssl) {
        rc = SSLSocket_continueWrite(pw);
        goto exit;
    }
    #endif

    for (i = 0; i < pw->count; ++i) {
        if (pw->bytes <= curBufLen) {
            iovecs1[++curBuf].iov_len = pw->iovecs[i].iov_len;
            iovecs1[curBuf].iov_base = pw->iovecs[i].iov_base;
        } else if (pw->bytes < curBufLen + pw->iovecs[i].iov_len) {
            size_t offset = pw->bytes - curBufLen;
            iovecs1[++curBuf].iov_len = pw->iovecs[i].iov_len - (ULONG)offset;
            iovecs1[curBuf].iov_base = pw->iovecs[i].iov_base + offset;
            break;
        }
        curBufLen += pw->iovecs[i].iov_len;
    }

    if ((rc = Socket_writev(socket, iovecs1, curBuf+1, &bytes)) != SOCKET_ERROR) {
        pw->bytes += bytes;
        if ((rc = (pw->bytes == pw->total))) {
            for (i = 0; i < pw->count; i++) {
                if (pw->frees[i]) {
                    free(pw->iovecs[i].iov_base);
                }
            }
        }
    }
#ifdef OPENSSL
exit:
#endif

    return rc;
}



int Socket_continueWrites(fd_set* pwSet) {
    int rc1 = 0;
    ListElement* curPending = s.writePending->first;

    while (curPending) {
        int socket = *(int*)(curPending->content);
        if (FD_ISSET(socket, pwSet) && Socket_continueWrite(socket)) {
            if (!SocketBuffer_writeComplete(socket)) {
                LOG("Failed to remove pending write from socket buffer list in %s-%d", __FILE__, __LINE__);
            }
            FD_CLR(socket, &(s.pendingWriteSet));
            if (!ListRemove(s.writePending, curPending->content)) {
                ListNextElement(s.writePending, &curPending);
            }
            curPending = s.writePending->current;

            if (writeComplete) {
                (*writeComplete)(socket);
            }
        } else {
            ListNextElement(s.writePending, &curPending);
        }
    }
    return rc1;
}

/**
* 将二进制整数ip地址转换为点分十进制ip地址, 再转换为字符串形式
*/
char* Socket_getAddrName(struct sockaddr* sa, int sock) {

//ip地址最大长度
#define ADDRLEN INET6_ADDRSTRLEN+1
//端口最大长度
#define PORTLEN 10

    static char addr_string[ADDRLEN + PORTLEN];

    struct sockaddr_in *sin = (struct sockaddr_in *)sa;
    inet_ntop(sin->sin_family, &sin->sin_addr, addr_string, ADDRLEN);
    sprintf(&addr_string[strlen(addr_string)], ":%d", ntohs(sin->sin_port));

    return addr_string;
}

/**
 *  获取对等socket地址
 */
char* Socket_getPeer(int sock){
	struct sockaddr_in6 sa;
	socklen_t sal = sizeof(sa);
	int rc;

	if ((rc = getpeername(sock, (struct sockaddr*)&sa, &sal)) == SOCKET_ERROR){
		  Socket_error("getpeername", sock);
		    return "unknown";
	}
	return Socket_getAddrName((struct sockaddr*)&sa, sock);
}
