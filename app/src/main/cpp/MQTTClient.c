/* 同步的API*/

#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/time.h>

#include "MQTTClient.h"

#ifndef NO_PERSISTENCE
#include "MQTTPersistence.h"
#endif

#include "utf-8.h"
#include "MQTTProtocol.h"
#include "Thread.h"
#include "Heap.h"
#include "Log.h"
#include "SocketBuffer.h"

#ifdef OPENSSL
#include "SSLSocket.h"
#endif


#define URI_TCP "tcp://"

static ClientStates ClientState = {
        "1.1.0", //version
        NULL //client list
};

ClientStates* bState = &ClientState;

MQTTProtocol state;

static pthread_mutex_t mqttClientMutexStore = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t* mqttClientMutex = &mqttClientMutexStore;

static pthread_mutex_t socketMutexStore = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t* socketMutex = &socketMutexStore;

static pthread_mutex_t subscribeMutexStore = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t* subscribeMutex = &subscribeMutexStore;

static pthread_mutex_t unsubscribeMutexStore = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t* unsubscribeMutex = &unsubscribeMutexStore;

static pthread_mutex_t connectMutexStore = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t* connectMutex = &connectMutexStore;


void MQTTClient_init(void) {
    pthread_mutexattr_t attr;
    int rc;

    //初始化互斥量属性
    pthread_mutexattr_init(&attr);
    //设置互斥量的所有操作都进行错误检查
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    if ((rc = pthread_mutex_init(mqttClientMutex, &attr)) != 0) {
        LOG("MQTTClient: error %d initializing client_mutex", rc);
    }

    if ((rc = pthread_mutex_init(socketMutex, &attr)) != 0) {
        LOG("MQTTClient: error %d initializing socketMutex", rc);
    }

    if ((rc = pthread_mutex_init(subscribeMutex, &attr)) != 0) {
        LOG("MQTTClient: error %d initializing subscribeMutex", rc);
    }

    if ((rc = pthread_mutex_init(unsubscribeMutex, &attr)) != 0) {
        LOG("MQTTClient: error %d initializing unsubscribeMutex", rc);
    }

    if ((rc = pthread_mutex_init(connectMutex, &attr)) != 0) {
        LOG("MQTTClient: error %d initializing connectMutex", rc);
    }

}

static volatile int initialized = 0;
static List* handles = NULL;
static time_t last;
static int running  = 0;
static int toStop = 0;
static pthread_t runId = 0;

typedef struct {
    MQTTClient_message* msg;
    char*               topicName;
    int                 topicLen;
    unsigned int        seqNo;/* only used on restore */
} qEntry;


typedef struct {
    char* serverURI;
#ifdef OPENSSL
    int ssl;
#endif
    Clients*                        c;
    MQTTClient_connectionLost*      cl;
    MQTTClient_messageArrived*      ma;
    MQTTClient_deliveryComplete*    dc;
    void*                           context;

    sem_type                        connect_sem;//connect信号量
    int                             rc;
    sem_type                        connack_sem;//connack信号量
    sem_type                        suback_sem;//suback信号量
    sem_type                        unsuback_sem;//unsuback信号量
    MQTTPacket*                     pack;
} MQTTClients;


void MQTTClient_sleep(long milliseconds) {
    usleep(milliseconds * 1000);
}


struct timeval MQTTClient_start_clock(void) {
    static struct timeval start;
    gettimeofday(&start, NULL);
    return start;
}


/**
 * 用当前时间减去起始时间，将差值转换为long型数据
 * @param start
 * @return
 */
long MQTTClient_elapsed(struct timeval start) {
    /**
     * struct timeval {
     *      time_t      tc_sec; //自1970.1.1 00:00:00以来的秒数
     *      suseconds_t tv_usec; //微妙 long int
     * };
     */
    struct timeval now;
    struct timeval res;
    //获取时间值
    gettimeofday(&now, NULL);
    //用now - start,将值存于res
    timersub(&now, &start, &res);
    return (res.tv_sec)*1000 + (res.tv_usec) / 1000;
}

/* 本地函数声明 */

static void MQTTClient_terminate(void);

static void MQTTClient_emptyMessageQueue(Clients* client);

static int MQTTClient_deliverMessage(
        int rc, MQTTClients* m,
        char** topicName, int* topicLen,
        MQTTClient_message** message);

static int clientSockCompare(void* a, void* b);

static void* connectionLost_call(void* context);

static void* MQTTClient_run(void* n);

static void MQTTClient_stop(void);

static void MQTTClient_closeSession(Clients* client);

static int MQTTClient_cleanSession(Clients* client);

static int MQTTClient_connectURIVersion(
    MQTTClient handle, MQTTClient_connectOptions* options,
    const char* serverURI, int mqttVersion,
    struct timeval start, long milliSecsTimeout);

static int MQTTClient_connectURI(MQTTClient handle, MQTTClient_connectOptions* options, const char* serverURI);

static int MQTTClient_disconnect1(MQTTClient handle, int timeout, int internal, int stop);

static int MQTTClient_disconnect_internal(MQTTClient handle, int timeout);

static void MQTTClient_retry(void);

static MQTTPacket* MQTTClient_cycle(int* sock, unsigned long timeout, int* rc);

static MQTTPacket* MQTTClient_waitFor(MQTTClient handle, int packet_type, int* rc, long timeout);

static int pubCompare(void* a, void* b);

static void MQTTProtocol_checkPendingWrites(void);

static void MQTTClient_writeComplete(int socket);


/* 函数实现 */

int MQTTClient_create(MQTTClient* handle, const char* serverURI, const char* clientId,
                      int persistenceType, void* persistenceContext) {
    int rc = 0;
    MQTTClients *m = NULL;

    rc = Thread_lock_mutex(mqttClientMutex);
    //确保serverURI和clientId不为null
    if (serverURI == NULL || clientId == NULL) {
        rc = MQTT_CLIENT_NULL_PARAMETER;
        goto exit;
    }
    //确保clientId为合法utf-8字符串
    if (!UTF8_validateString(clientId)) {
        rc = MQTT_CLIENT_BAD_UTF8_STRING;
        goto exit;
    }

    if (!initialized) {
        Heap_initialize();
        bState->clients = ListInitialize();
        Socket_outInitialize();
        Socket_setWriteCompleteCallback(MQTTClient_writeComplete);
        handles = ListInitialize();
#ifdef OPENSSL
        SSLSocket_initialize();
#endif
        initialized = 1;
    }

    m = malloc(sizeof(MQTTClients));
    /* handle是一个void**指针，用handle存放MQTTClients的内存地址*/
    *handle = m;
    memset(m, '0', sizeof(MQTTClients));

    if (strncmp(URI_TCP, serverURI, strlen(URI_TCP)) == 0) {
        serverURI += strlen(URI_TCP);
    }
#ifdef OPENSSL
    else if (strncmp(URI_SSL, serverURI, strlen(URI_SSL)) == 0) {
        serverURI += strlen(URI_SSL);
        m->ssl = 1;
    }
#endif
    //复制serverURI,复制的serverURI是通过malloc分配的内存
    m->serverURI = MQTTStrdup(serverURI);
    ListAppend(handles, m, sizeof(MQTTClients));

    m->c = malloc(sizeof(Clients));
    memset(m->c, '\0', sizeof(Clients));

    m->c->context = m;
    m->c->outboundMsgs = ListInitialize();
    m->c->inboundMsgs = ListInitialize();
    m->c->publishMessageQueue = ListInitialize();
    m->c->clientID = MQTTStrdup(clientId);
    m->connect_sem = Thread_create_sem();
    m->connack_sem = Thread_create_sem();
    m->suback_sem = Thread_create_sem();
    m->unsuback_sem = Thread_create_sem();

#ifndef NO_PERSISTENCE
    rc = MQTTPersistence_create(&(m->c->persistence), persistenceType, persistenceContext);
    if (rc == 0) {
        rc = MQTTPersistence_initialize(m->c, m->serverURI);
        if (rc == 0) {
            MQTTPersistence_restoreMessageQueue(m->c);
        }
    }
#endif

    ListAppend(bState->clients, m->c, sizeof(Clients) + 3 * sizeof(List));


exit:
    Thread_unlock_mutex(mqttClientMutex);
    return rc;
}

static void MQTTClient_terminate(void) {
    MQTTClient_stop();
    if (initialized) {
        ListFree(bState->clients);
        ListFree(handles);
        handles = NULL;
        Socket_outTerminate();
#ifdef OPENSSL
        SSLSocket_terminate();
#endif
        Heap_terminate();
        initialized = 0;
    }
}

/**
 * 清空消息队列
 * @param client
 */
static void MQTTClient_emptyMessageQueue(Clients* client) {
    if (client->publishMessageQueue->count > 0) {
        ListElement* current = NULL;
        while (ListNextElement(client->publishMessageQueue, &current)) {
            qEntry* qe = (qEntry*)(current->content);
            free(qe->topicName);
            free(qe->msg->payload);
            free(qe->msg);
        }
        ListEmpty(client->publishMessageQueue);
    }
}

/**
 * 销毁MQTTClient相关内存
 * @param handle
 */
void MQTTClient_destroy(MQTTClient* handle) {
    MQTTClients* m = *handle;
    Thread_lock_mutex(mqttClientMutex);

    if (m == NULL) {
        goto exit;
    }

    if (m->c) {
        char* saved_clientId = MQTTStrdup(m->c->clientID);
#ifndef NO_PERSISTENCE
        MQTTPersistence_close(m->c);
#endif
        MQTTClient_emptyMessageQueue(m->c);
        MQTTProtocol_freeClient(m->c);
        if (!ListRemove(bState->clients, m->c)) {
            LOG("Can not remove client %s from list in %s-%d", saved_clientId, __FILE__, __LINE__);
        } else {
            LOG("Success to remove client %s ", saved_clientId);
        }
        free(saved_clientId);
    }
    if (m->serverURI) {
        free(m->serverURI);
    }

    Thread_destroy_sem(m->connect_sem);
    Thread_destroy_sem(m->connack_sem);
    Thread_destroy_sem(m->suback_sem);
    Thread_destroy_sem(m->unsuback_sem);

    if (!ListRemove(handles, m)) {
        LOG("Success to remove client");
    }
    *handle = NULL;
    if (bState->clients->count == 0) {
        MQTTClient_terminate();
    }

exit:
    Thread_unlock_mutex(mqttClientMutex);
}


/**
 * 释放Message相关内存
 */
void MQTTClient_freeMessage(MQTTClient_message** message) {
    free((*message)->payload);
    free(*message);
    *message = NULL;
}

/**
 * 释放内存
 */
void MQTTClient_free(void* memory) {
    free(memory);
}


/**
 * 传送消息
 */
static int MQTTClient_deliverMessage(int rc, MQTTClients* m, char** topicName, int* topicLen, MQTTClient_message** message) {
    qEntry* qe = (qEntry*)(m->c->publishMessageQueue->first->content);

    *message = qe->msg;
    *topicName = qe->topicName;
    *topicLen = qe->topicLen;

    if (strlen(*topicName) != *topicLen) {
        rc = MQTT_CLIENT_TOPIC_NAME_TRUNCATED;
    }
#ifndef NO_PERSISTENCE
    if (m->c->persistence) {
        MQTTPersistence_unpersistQueueEntry(m->c, (MQTTPersistence_qEntry*)qe);
    }
#endif
    ListRemove(m->c->publishMessageQueue, m->c->publishMessageQueue->first->content);
    return rc;
}

/**
 * 比较两个client是否相等
 */
static int clientSockCompare(void* a, void* b) {
    MQTTClients* m = (MQTTClients*)a;
    return m->c->net.socket == *(int*)b;
}


/**
 * 包装函数，在一个独立的线程上调用connectionLost回调
 * @param context
 * @return
 */
static void* connectionLost_call(void* context) {
    MQTTClients* m = (MQTTClients*)context;
    (*(m->cl))(m->context, NULL);
    return 0;
}

/**
 * 不斷地从socket中接收消息,如果有设置回调函数，
 * 则该函数的作用是在一个独立的线程中去掉用这些回调函数
 * @param n
 * @return
 */
static void* MQTTClient_run(void* n) {
    long timeout = 10L;

    running = 1;
    runId = Thread_getId();

    Thread_lock_mutex(mqttClientMutex);
    while (!toStop) {
        int rc = SOCKET_ERROR;
        int sock = -1;

        MQTTClients* m = NULL;
        //只有头部的packet
        MQTTPacket* packet = NULL;

        Thread_unlock_mutex(mqttClientMutex);
        //从已就绪的socket中读取一个packet,并处理
        packet = MQTTClient_cycle(&sock, timeout, &rc);
        Thread_lock_mutex(mqttClientMutex);

        if (toStop) {
            break;
        }

        if (ListFindItem(handles, &sock, clientSockCompare) == NULL) {
            continue;
        }

        m = (MQTTClient)(handles->current->content);
        if (m == NULL) {
            continue;
        }

        if (rc == SOCKET_ERROR) {
            if (m->c->connected) {
                //断开socket连接
                MQTTClient_disconnect_internal(m, 0);
            } else {
                if (m->c->connectState == CONNECT_STATE_WAIT_FOR_SSL_COMPLETE && !Thread_check_sem(m->connect_sem)) {
                    //如果信号量小于0同时SSL正在等待连接
                    LOG("Posting connect semaphore for client %s", m->c->clientID);
                    Thread_post_sem(m->connect_sem);
                }
                if (m->c->connectState == CONNECT_STATE_WAIT_FOR_CONNACK && !Thread_check_sem(m->connack_sem)) {
                    //如果信号量小于0且连接还未完成,等待CONNACK
                    LOG("Posting connack semaphore for client %s", m->c->clientID);
                    Thread_post_sem(m->connack_sem);
                }
            }
        } else {
            //从消息队列中读取消息并回调messageArrived
            if (m->c->publishMessageQueue->count > 0) {
                qEntry* qe = (qEntry*)(m->c->publishMessageQueue->first->content);
                int topicLen = qe->topicLen;

                if (strlen(qe->topicName) == topicLen) {
                    topicLen = 0;
                }

                Thread_unlock_mutex(mqttClientMutex);
                //回调messageArrived
                rc = (*(m->ma))(m->context, qe->topicName, topicLen, qe->msg);
                Thread_lock_mutex(mqttClientMutex);
                /* 如果回调函数返回0，代表处理失败，先不从publishMessageQueue中删除消息，之后会再次重试*/
                if (rc) {
#ifndef NO_PERSISTENCE
                    if (m->c->persistence) {
                        MQTTPersistence_unpersistQueueEntry(m->c, (MQTTPersistence_qEntry*)qe);
                    }
#endif
                    ListRemove(m->c->publishMessageQueue, qe);
                } else {
                    LOG("False return from messageArrived for client %s, message remains on queue", m->c->clientID);
                }
            }
            //处理Packet
            if (packet) {
                unsigned int type = packet->header.bits.type;
                if (type == CONNACK && !Thread_check_sem(m->connack_sem)) {
                    m->pack = packet;
                    LOG("Posting connack semaphore for clent %s", m->c->clientID);
                    Thread_post_sem(m->connack_sem);
                } else if (type == SUBACK) {
                    m->pack = packet;
                    LOG("Posting suback semaphore for client %s", m->c->clientID);
                    Thread_post_sem(m->suback_sem);
                } else if (type == UNSUBACK) {
                    m->pack = packet;
                    LOG("Posting unsuback semaphore for client %s", m->c->clientID);
                    Thread_post_sem(m->unsuback_sem);
                }
            } else if (m->c->connectState == CONNECT_STATE_WAIT_FOR_TCP_COMPLETE
                       && !Thread_check_sem(m->connect_sem)) {
                /* 如果TCP正在连接，且connect_sem小于0, 获取socket相关选项并提交connect_sem*/
                int error;
                socklen_t len = sizeof(error);
                //获取Socket相关的选项
                if ((m->rc == getsockopt(m->c->net.socket, SOL_SOCKET, SO_ERROR, (char *)&error, &len)) == 0) {
                    m->rc = error;
                }
                LOG("Posting connect semaphore for SSL client %s rc %d", m->c->clientID, m->rc);
                Thread_post_sem(m->connect_sem);
            }
#ifdef OPENSSL
            else if (m->c->connectState == 2 && !Thread_check_sem(m->connect_sem)) {
                rc = SSLSocket_connect(m->c->net.ssl, m->c->net.socket);
                if (rc == 1 || rc == SSL_FATAL) {
                    if (rc == 1 && !m->c->cleanSession && m->c->session == NULL) {
                        m->c->session = SSL_get1_session(m->c->net.ssl);
                    }
                    m->rc = rc;
                    LOG("Posting connect semaphore for SSL client %s rc %d", m->c->clientID, m->rc);
                    Thread_post_sem(m->connect_sem);
                }
            }
#endif
        }
    }
    runId = 0;
    running = toStop = 0;
    Thread_unlock_mutex(mqttClientMutex);
    return 0;
}




static void MQTTClient_stop(void) {
    int rc = 0;
    if (running == 1 && toStop == 0) {
        int conn_count =0;
        ListElement* current = NULL;

        if (handles != NULL) {
            while (ListNextElement(handles, &current)) {
                if (((MQTTClients*)(current->content))->c->connectState > CONNECT_STATE_NONE ||
                        ((MQTTClients*)(current->content))->c->connected) {
                    ++conn_count;
                }
            }
        }
        if (conn_count == 0) {
            int count = 0;
            toStop = 1;
            if (Thread_getId() != runId) {
                while (running && ++count < 100) {
                    Thread_unlock_mutex(mqttClientMutex);
                    MQTTClient_sleep(100L);
                    Thread_lock_mutex(mqttClientMutex);
                }
            }
            rc = 1;
        }
    }
}

/**
 * 设置回调函数
 * @param handle
 * @param context
 * @param cl
 * @param ma
 * @param dc
 * @return
 */
int MQTTClient_setCallbacks(MQTTClient handle, void* context, MQTTClient_connectionLost* cl,
                                MQTTClient_messageArrived* ma, MQTTClient_deliveryComplete* dc) {
    int rc = MQTT_CLIENT_SUCCESS;
    MQTTClients* m = handle;

    Thread_lock_mutex(mqttClientMutex);
    if (m == NULL || ma == NULL || m->c->connectState != CONNECT_STATE_NONE) {
        rc = MQTT_CLIENT_FAILURE;
    } else {
        m->context = context;
        m->cl = cl;
        m->ma = ma;
        m->dc = dc;
    }
    Thread_unlock_mutex(mqttClientMutex);
    return rc;
}


static void MQTTClient_closeSession(Clients* client) {
    client->good = 0;
    client->pingOutStanding = 0;
    if (client->net.socket > 0) {
        if (client->connected) {
            MQTTPacket_send_disconnect(&client->net, client->clientID);
        }
        Thread_lock_mutex(socketMutex);
#ifdef OPENSSL
        SSLSocket_close(&client->net);
#endif
        Socket_close(client->net.socket);
        Thread_unlock_mutex(socketMutex);
        client->net.socket = 0;
#ifdef OPENSSL
        client->net.ssl = NULL;
#endif
    }
    client->connected = 0;
    client->connectState = CONNECT_STATE_NONE;

    if (client->cleanSession) {
        MQTTClient_cleanSession(client);
    }
}



static int MQTTClient_cleanSession(Clients* client) {
    int rc = 0;
#ifndef NO_PERSISTENCE
    rc = MQTTPersistence_clear(client);
#endif
    MQTTProtocol_emptyMessageList(client->inboundMsgs);
    MQTTProtocol_emptyMessageList(client->outboundMsgs);
    MQTTClient_emptyMessageQueue(client);
    client->msgID = 0;
    return rc;
}

void Protocol_processPublication(Publish* publish, Clients* client) {
    qEntry* qe = NULL;
    MQTTClient_message* mm = NULL;

    qe = malloc(sizeof(qEntry));
    mm = malloc(sizeof(MQTTClient_message));

    qe->msg = mm;

    qe->topicName = publish->topic;
    qe->topicLen = publish->topicLen;
    publish->topic = NULL;

    /* 如果qos = 2, 存储payload*/
    if (publish->header.bits.qos == 2) {
        mm->payload = publish->payload;
    } else {
        mm->payload = malloc(publish->payloadLen);
        memcpy(mm->payload, publish->payload, publish->payloadLen);
    }

    mm->payloadLen = publish->payloadLen;
    mm->qos = publish->header.bits.qos;
    mm->retained = publish->header.bits.retain;

    if (publish->header.bits.qos == 2) {
        /* 确保qos=2时， dup不能为1*/
        mm->dup = 0;
    } else {
        mm->dup = publish->header.bits.dup;
    }
    mm->msgId = publish->msgId;

    ListAppend(client->publishMessageQueue, qe, sizeof(qe) + sizeof(mm) + mm->payloadLen + strlen(qe->topicName) + 1);
#ifndef NO_PERSISTENCE
    if (client->persistence) {
        MQTTPersistence_persistQueueEntry(client, (MQTTPersistence_qEntry*)qe);
    }
#endif
}


static int MQTTClient_connectURIVersion(MQTTClient handle, MQTTClient_connectOptions* options, const char* serverURI, int mqttVersion,
                                    struct timeval start, long milliSecsTimeout) {

    MQTTClients* m = handle;
    int rc = SOCKET_ERROR;
    int sessionPresent = 0;

    if (m->ma && !running) {
        Thread_start(MQTTClient_run, handle);
        if (MQTTClient_elapsed(start) >= milliSecsTimeout) {
            rc = SOCKET_ERROR;
            goto exit;
        }
        MQTTClient_sleep(100L);
    }

    LOG("Connecting to serverURI %s with MQTT version %d", serverURI, mqttVersion);

    /* 连接server, 发送CONNECT请求*/
#ifdef OPENSSL
    rc = MQTTProtocol_connect(serverURI, m->c, m->ssl, mqttVersion);
#else
    rc = MQTTProtocol_connect(serverURI, m->c, mqttVersion);
#endif

    if (rc == SOCKET_ERROR) {
        goto exit;
    }

    if (m->c->connectState == CONNECT_STATE_NONE) {
        rc = SOCKET_ERROR;
        goto exit;
    }

    if (m->c->connectState == CONNECT_STATE_WAIT_FOR_TCP_COMPLETE) {
        /* 如果是等待TCP连接完成，证明错误是EINPROGRESS或EWOULDBLOCK，重新发出CONNECT请求*/
        Thread_unlock_mutex(mqttClientMutex);
        /* 递减connect_sem信号量，如果递减不成功，当前线程不会被阻塞，因为调用的是sem_trywait*/
        MQTTClient_waitFor(handle, CONNECT, &rc, milliSecsTimeout - MQTTClient_elapsed(start));
        Thread_lock_mutex(mqttClientMutex);
        if (rc != 0) {
            rc = SOCKET_ERROR;
            goto exit;
        }

#ifdef OPENSSL
        if (m->ssl) {
            int port;
            char* hostName;
            int setSocketForSSLrc = 0;

            //拼接hostname和port
            hostName = MQTTProtocol_addressPort(m->serverURI, &port);
            //给Socket设置SSL
            setSocketForSSLrc = SSLSocket_setSocketForSSL(&m->c->net, m->c->sslopts, hostName);

            if (hostName != m->serverURI) {
                free(hostName);
            }

            if (setSocketForSSLrc != MQTTClient_SUCCESS) {
                /* 如果没有成功给Socket设置SSL*/
                if (m->c->session != NULL && (rc = SSL_set_session(m->c->net.ssl, m->c->session)) != 1) {
                    LOG("Failed to set SSL session with stored data, non critical in %s-%d", __FILE__, __LINE__);
                }
                //进行SSL的连接
                rc = SSLSocket_connect(m->c->net.ssl, m->c->net.socket);
                if (rc == TCPSOCKET_INTERRUPTED) {
                    m->c->connectState = CONNECT_STATE_WAIT_FOR_SSL_COMPLETE;
                } else if (rc == SSL_FATAL) {
                    rc = SOCKET_ERROR;
                    goto exit;
                } else if (rc == 1) {
                    rc = MQTTClient_SUCCESS;
                    m->c->connectState = CONNECT_STATE_WAIT_FOR_CONNACK;
                    //发送CONNECT数据包
                    if (MQTTPacket_send_connect(m->c, mqttVersion) == SOCKET_ERROR) {
                        rc = SOCKET_ERROR;
                        goto exit;
                    }
                    if (!m->c->cleanSession && m->c->session == NULL) {
                        m->c->session = SSL_get1_session(m->c->net.ssl);
                    }
                }
            } else {
                rc = SOCKET_ERROR;
                goto exit;
            }
        } else {
#endif
            /* 发送CONNECT*/
            m->c->connectState = CONNECT_STATE_WAIT_FOR_CONNACK;
            if (MQTTPacket_send_connect(m->c, mqttVersion) == SOCKET_ERROR) {
                rc = SOCKET_ERROR;
                goto exit;
            }
#ifdef OPENSSL
        }
#endif
    }

#ifdef OPENSSL
    if (m->c->connectState == CONNECT_STATE_WAIT_FOR_SSL_COMPLETE) {
        /* 如果SSL连接还未完成,证明SSL_connect()发生错误*/
        Thread_unlock_mutex(mqttClientMutex);
        MQTTClient_waitFor(handle, CONNECT, &rc, milliSecsTimeout - MQTTClient_elapsed(start));
        Thread_lock_mutex(mqttClientMutex);

        if (rc != 1) {
            rc = SOCKET_ERROR;
            goto exit;
        }

        if (!m->c->cleanSession && m->c->session == NULL) {
            m->c->session = SSL_get1_session(m->c->net.ssl);
        }
        m->c->connectState = CONNECT_STATE_WAIT_FOR_CONNACK;
        if (MQTTPacket_send_connect(m->c, mqttVersion) == SOCKET_ERROR) {
            rc = SOCKET_ERROR;
            goto exit;
        }
    }
#endif

    if (m->c->connectState == CONNECT_STATE_WAIT_FOR_CONNACK) {
        MQTTPacket* pack = NULL;
        Thread_unlock_mutex(mqttClientMutex);
        /* 递减connack_sem信号量*/
        pack = MQTTClient_waitFor(handle, CONNACK, &rc, milliSecsTimeout - MQTTClient_elapsed(start));
        Thread_lock_mutex(mqttClientMutex);

        if (pack == NULL) {
            rc = SOCKET_ERROR;
        } else {
            Connack* connack = (Connack*)pack;
            if ((rc = connack->rc) == MQTT_CLIENT_SUCCESS) {
                m->c->connected = 1;
                m->c->good= 1;
                m->c->connectState = CONNECT_STATE_NONE;

                if (mqttVersion == 4) {
                    sessionPresent = connack->flags.bits.sessionPresent;
                }

                if (m->c->cleanSession) {
                    rc = MQTTClient_cleanSession(m->c);
                }

                if (m->c->outboundMsgs->count > 0) {
                    ListElement* outCurrent = NULL;

                    while (ListNextElement(m->c->outboundMsgs, &outCurrent)) {
                        Messages* m = (Messages*)(outCurrent->content);
                        m->lastTouch = 0;
                    }
                    MQTTProtocol_retry((time_t)0, 1, 1);
                    if (m->c->connected != 1) {
                        rc = MQTT_CLIENT_DISCONNECTED;
                    }
                }
            }
            free(connack);
            m->pack = NULL;
        }
    }

exit:
    if (rc == MQTT_CLIENT_SUCCESS) {
        if (options->structVersion == 4) {
            options->returned.serverURI = serverURI;
            options->returned.mqttVersion = mqttVersion;
            options->returned.sessionPresent = sessionPresent;
        }
    } else {
        MQTTClient_disconnect1(handle, 0, 0, (mqttVersion == 3));
    }
    return rc;

}

static int MQTTClient_connectURI(MQTTClient handle, MQTTClient_connectOptions* options, const char* serverURI) {
    MQTTClients* m = handle;
    struct timeval start;
    long milliSecsTimeout = 30000L;
    int rc = SOCKET_ERROR;
    int mqttVersion = 0;

    milliSecsTimeout = options->connectTimeout * 1000;
    start = MQTTClient_start_clock();

    m->c->keepAliveInterval = options->keepAliveInterval;
    m->c->cleanSession = options->cleanSession;
    m->c->maxInFlightMessages = (options->reliable) ? 1 : 10;

    if (m->c->will) {
        free(m->c->will->msg);
        free(m->c->will->topic);
        free(m->c->will);
        m->c->will = NULL;
    }

    if (options->will && options->will->structVersion == 0) {
        m->c->will = malloc(sizeof(WillMessages));
        m->c->will->msg = MQTTStrdup(options->will->message);
        m->c->will->qos = options->will->qos;
        m->c->will->retained = options->will->retained;
        m->c->will->topic = MQTTStrdup(options->will->topicName);
    }

#ifdef OPENSSL
    if (m->c->sslopts){
        if (m->c->sslopts->trustStore)
            free((void*)m->c->sslopts->trustStore);
        if (m->c->sslopts->keyStore)
            free((void*)m->c->sslopts->keyStore);
        if (m->c->sslopts->privateKey)
            free((void*)m->c->sslopts->privateKey);
        if (m->c->sslopts->privateKeyPassword)
            free((void*)m->c->sslopts->privateKeyPassword);
        if (m->c->sslopts->enabledCipherSuites)
            free((void*)m->c->sslopts->enabledCipherSuites);
        free(m->c->sslopts);
        m->c->sslopts = NULL;
    }

    if (options->struct_version != 0 && options->ssl){
        m->c->sslopts = malloc(sizeof(MQTTClient_SSLOptions));
        memset(m->c->sslopts, '\0', sizeof(MQTTClient_SSLOptions));
        if (options->ssl->trustStore)
            m->c->sslopts->trustStore = MQTTStrdup(options->ssl->trustStore);
        if (options->ssl->keyStore)
            m->c->sslopts->keyStore = MQTTStrdup(options->ssl->keyStore);
        if (options->ssl->privateKey)
            m->c->sslopts->privateKey = MQTTStrdup(options->ssl->privateKey);
        if (options->ssl->privateKeyPassword)
            m->c->sslopts->privateKeyPassword = MQTTStrdup(options->ssl->privateKeyPassword);
        if (options->ssl->enabledCipherSuites)
            m->c->sslopts->enabledCipherSuites = MQTTStrdup(options->ssl->enabledCipherSuites);
        m->c->sslopts->enableServerCertAuth = options->ssl->enableServerCertAuth;
    }
#endif

    m->c->username = options->username;
    m->c->password = options->password;
    m->c->retryInterval = options->retryInterval;

    if (options->structVersion >= 3) {
        mqttVersion = options->mqttVersion;
    } else {
        mqttVersion = MQTT_VERSION_DEFAULT;
    }

    if (mqttVersion == MQTT_VERSION_DEFAULT) {
        if ((rc = MQTTClient_connectURIVersion(handle, options, serverURI, 4, start, milliSecsTimeout)) != MQTT_CLIENT_SUCCESS) {
            rc = MQTTClient_connectURIVersion(handle, options, serverURI, 3, start, milliSecsTimeout);
        }
    } else {
        rc = MQTTClient_connectURIVersion(handle, options, serverURI, mqttVersion, start, milliSecsTimeout);
    }

    return rc;
}


/**
 * 发送CONNECT数据包
 * @param handle
 * @param options
 * @return
 */
int MQTTClient_connect(MQTTClient handle, MQTTClient_connectOptions* options) {
    //声明一个MQTTClients指针指向handle,handle里面存放的是创建好的MQTTClients地址
    MQTTClients* m = handle;
    int rc = SOCKET_ERROR;

    Thread_lock_mutex(connectMutex);
    Thread_lock_mutex(mqttClientMutex);

    //确保有连接选项
    if (options == NULL) {
        rc = MQTT_CLIENT_NULL_PARAMETER;
        goto exit;
    }

    if (strncmp(options->structId, "MQTC", 4) != 0 ||
            (options->structVersion != 0 && options->structVersion != 1 &&
            options->structVersion != 2 && options->structVersion != 3 &&
                    options->structVersion != 4)) {

        rc = MQTT_CLIENT_BAD_STRUCTURE;
        goto exit;
    }


    if (options->will) {
        if (strncmp(options->will->structId, "MQTW", 4) != 0 || options->will->structVersion != 0) {
            rc = MQTT_CLIENT_BAD_STRUCTURE;
            goto exit;
        }
    }

#ifdef OPENSSL
    //structVersion = 0表明没有ssl
    if (options->structVersion != 0 && options->ssl) {
        if (strncmp(options->ssl->structId, "MQTS", 4) != 0 || options->ssl->structVersion != 0) {
            rc = MQTT_CLIENT_BAD_STRUCTURE;
            goto exit;
        }
    }
#endif

    if ((options->username && !UTF8_validateString(options->username)) ||
            (options->password && !UTF8_validateString(options->password))) {
        rc = MQTT_CLIENT_BAD_UTF8_STRING;
        goto exit;
    }

    if (options->structVersion < 2 || options->serverURICount == 0) {
        //如果在选项中没有指定serverURI,则使用MQTTClients中的serverURI
        rc = MQTTClient_connectURI(handle, options, m->serverURI);
    } else {
        int i;
        for (i = 0; i < options->serverURICount; ++i) {
            char* serverURI = options->serverURIs[i];
            if (strncmp(URI_TCP, serverURI, strlen(URI_TCP)) == 0) {
                serverURI += strlen(URI_TCP);
            }
#ifdef OPENSSL
            else if (strncmp(URI_SSL, serverURI, strlen(URI_SSL)) == 0) {
                serverURI += strlen(URI_SSL);
                m->ssl = 1;
            }
#endif
            if ((rc = MQTTClient_connectURI(handle, options, serverURI)) == MQTT_CLIENT_SUCCESS) {
                break;
            }
        }
    }


exit:
    if (m->c->will) {
        free(m->c->will);
        m->c->will = NULL;
    }
    Thread_unlock_mutex(mqttClientMutex);
    Thread_unlock_mutex(connectMutex);
    return rc;
}


/**
 * 如果在多线程中调用该函数，mqttClientMutex必须上锁
 * @return
 */
static int MQTTClient_disconnect1(MQTTClient handle, int timeout, int call_connect_lost, int stop) {
    MQTTClients* m = handle;
    struct timeval start;

    int rc = MQTT_CLIENT_SUCCESS;
    int wasConnected = 0;


    if (m == NULL || m->c == NULL) {
        rc = MQTT_CLIENT_FAILURE;
        goto exit;
    }

    if (m->c->connected == 0 && m->c->connectState == CONNECT_STATE_NONE) {
        rc = MQTT_CLIENT_DISCONNECTED;
        goto exit;
    }

    wasConnected = m->c->connected;
    if (m->c->connected != 0) {
        start = MQTTClient_start_clock();
        m->c->connectState = CONNECT_STATE_DISCONNECTED;
        while (m->c->inboundMsgs->count > 0 || m->c->outboundMsgs->count > 0) {
            if (MQTTClient_elapsed(start) >= timeout) {
                break;
            }
            //等待所有in-flight消息完成，这期间保持连接
            Thread_unlock_mutex(mqttClientMutex);
            MQTTClient_yield();
            Thread_lock_mutex(mqttClientMutex);
        }
    }

    MQTTClient_closeSession(m->c);

    //持续递减connect_sem信号量直到为0
    while (Thread_check_sem(m->connect_sem)) {
        Thread_wait_sem(m->connect_sem, 100);
    }

    while (Thread_check_sem(m->connack_sem)) {
        Thread_wait_sem(m->connack_sem, 100);
    }

    while (Thread_check_sem(m->suback_sem)) {
        Thread_wait_sem(m->suback_sem, 100);
    }

    while (Thread_check_sem(m->unsuback_sem)) {
        Thread_wait_sem(m->unsuback_sem, 100);
    }


exit:
    if (stop) {
        MQTTClient_stop();
    }
    /* 新开一个线程，回调connectionLost*/
    if (call_connect_lost && m->cl && wasConnected) {
        Thread_start(connectionLost_call, m);
    }
    return rc;
}

static int MQTTClient_disconnect_internal(MQTTClient handle, int timeout) {
    return MQTTClient_disconnect1(handle, timeout, 1, 1);
}


void MQTTProtocol_closeSession(Clients* c, int sendWill) {
    MQTTClient_disconnect_internal((MQTTClient)c->context, 0);
}



int MQTTClient_disconnect(MQTTClient handle, int timeout) {
    int rc = 0;

    Thread_lock_mutex(mqttClientMutex);
    rc = MQTTClient_disconnect1(handle, timeout, 0, 1);
    Thread_unlock_mutex(mqttClientMutex);
    return rc;
}


int MQTTClient_isConnected(MQTTClient handle) {
    MQTTClients* m = handle;
    int rc = 0;

    Thread_lock_mutex(mqttClientMutex);
    if (m && m->c) {
        rc = m->c->connected;
    }
    Thread_unlock_mutex(mqttClientMutex);

    return rc;
}

/**
 *
 * @param handle
 * @param count
 * @param topic
 * @param qos
 * @return
 */
int MQTTClient_subscribeMany(MQTTClient handle, int count , char* const* topic, int* qos) {
    MQTTClients* m = handle;

    List* topics = NULL;
    List* qoss = NULL;
    int i = 0;
    int rc = MQTT_CLIENT_FAILURE;
    int msgId = 0;

    Thread_lock_mutex(subscribeMutex);
    Thread_lock_mutex(mqttClientMutex);

    if (m == NULL || m->c == NULL) {
        rc = MQTT_CLIENT_FAILURE;
        goto exit;
    }

    if (m->c->connected == 0) {
        rc = MQTT_CLIENT_DISCONNECTED;
        goto exit;
    }

    //逐个验证topic，qos
    for (i = 0; i < count; i++) {
        if (!UTF8_validateString(topic[i])) {
            rc = MQTT_CLIENT_BAD_UTF8_STRING;
            goto exit;
        }

        if (qos[i] < 0 || qos[i] > 2) {
            rc = MQTT_CLIENT_BAD_QOS;
            goto exit;
        }
    }

    if ((msgId = MQTTProtocol_assignMsgId(m->c)) == 0) {
        rc = MQTT_CLIENT_MAX_MESSAGES_INFLIGHT;
        goto exit;
    }

    topics = ListInitialize();
    qoss = ListInitialize();

    for (i = 0; i < count; i++) {
        ListAppend(topics, topic[i], strlen(topic[i]));
        ListAppend(qoss, &qos[i], sizeof(int));
    }

    rc = MQTTProtocol_subscribe(m->c, topics, qoss, msgId);
    ListFreeNoContent(topics);
    ListFreeNoContent(qoss);

    if (rc == TCPSOCKET_COMPLETE) {
        MQTTPacket* packet = NULL;

        Thread_unlock_mutex(mqttClientMutex);
        //递减SUBACK信号量，如果信号量不为0，直到可以递减
        packet = MQTTClient_waitFor(handle, SUBACK, &rc, 10000L);
        Thread_lock_mutex(mqttClientMutex);

        if (packet != NULL) {
            Suback* sub = (Suback*)packet;
            ListElement* current = NULL;
            i = 0;
            //修改传入的qos数组的值
            while (ListNextElement(sub->qoss, &current)) {
                int* reqQos = (int*)(current->content);
                qos[i++] = *reqQos;
            }
            rc= MQTTProtocol_handleSubacks(packet, m->c->net.socket);
            m->pack = NULL;
        } else {
            rc  = SOCKET_ERROR;
        }
    }

    if (rc == SOCKET_ERROR) {
        MQTTClient_disconnect_internal(handle, 0);
    } else if (rc == TCPSOCKET_COMPLETE) {
        rc = MQTT_CLIENT_SUCCESS;
    }


exit:
    Thread_unlock_mutex(mqttClientMutex);
    Thread_unlock_mutex(subscribeMutex);
    return rc;
}


int MQTTClient_subscribe(MQTTClient handle, const char* topic, int qos) {
    int rc = 0;
    char* const topics[] = {(char*)topic};

    rc = MQTTClient_subscribeMany(handle, 1, topics, &qos);
    if (qos == MQTT_BAD_SUBSCRIBE) {
        rc = MQTT_CLIENT_BAD_STRUCTURE;
    }
    return rc;
}

int MQTTClient_unsubscribeMany(MQTTClient handle, int count, char* const* topic) {
    MQTTClients* m = handle;
    List* topics = NULL;
    int i = 0;
    int rc = SOCKET_ERROR;
    int msgId = 0;

    Thread_lock_mutex(unsubscribeMutex);
    Thread_lock_mutex(mqttClientMutex);

    if (m == NULL || m->c == NULL) {
        rc = MQTT_CLIENT_FAILURE;
        goto exit;
    }

    if (m->c->connected == 0) {
        rc = MQTT_CLIENT_DISCONNECTED;
        goto exit;
    }

    for (i = 0; i < count; i++) {
        if (!UTF8_validateString(topic[i])) {
            rc = MQTT_CLIENT_BAD_UTF8_STRING;
            goto exit;
        }
    }

    if ((msgId = MQTTProtocol_assignMsgId(m->c)) == 0) {
        rc = MQTT_CLIENT_MAX_MESSAGES_INFLIGHT;
        goto exit;
    }

    topics = ListInitialize();
    for (i = 0; i < count; i++) {
        ListAppend(topics, topic[i], strlen(topic[i]));
    }
    rc = MQTTProtocol_unsubscribe(m->c, topics, msgId);
    ListFreeNoContent(topics);

    if (rc == TCPSOCKET_COMPLETE) {
        MQTTPacket* packet = NULL;

        Thread_unlock_mutex(mqttClientMutex);
        packet = MQTTClient_waitFor(handle, UNSUBACK, &rc, 10000L);
        Thread_lock_mutex(mqttClientMutex);

        if (packet != NULL) {
            rc = MQTTProtocol_handleUnsubacks(packet, m->c->net.socket);
            m->pack = NULL;
        } else {
            rc = SOCKET_ERROR;
        }
    }
    if (rc == SOCKET_ERROR) {
        MQTTClient_disconnect_internal(handle, 0);
    }

exit:
    Thread_unlock_mutex(mqttClientMutex);
    Thread_unlock_mutex(unsubscribeMutex);
    return rc;

}

int MQTTClient_unsubscribe(MQTTClient handle, const char* topic) {
    int rc = 0;
    char* const topics[] = {(char*)topic};
    rc = MQTTClient_unsubscribeMany(handle, 1, topics);
    return rc;
}


int MQTTClient_publish(MQTTClient handle, const char* topicName, int payloadLen, void* payload,
                       int qos, int retained, MQTTClient_deliveryToken* deliveryToken) {
    int rc = MQTT_CLIENT_SUCCESS;
    MQTTClients* m = handle;
    Messages* msg = NULL;
    Publish* publish = NULL;
    int blocked = 0;
    int msgId = 0;

    Thread_lock_mutex(mqttClientMutex);

    if (m == NULL || m->c == NULL) {
        rc = MQTT_CLIENT_FAILURE;
        goto exit;
    }
    if (m->c->connected == 0) {
        rc = MQTT_CLIENT_DISCONNECTED;
        goto exit;
    }
    if (!UTF8_validateString(topicName)) {
        rc = MQTT_CLIENT_BAD_UTF8_STRING;
        goto exit;
    }

    /* if outbound queue is full, block until it is not*/
    while (m->c->outboundMsgs->count >= m->c->maxInFlightMessages ||
            Socket_noPendingWrites(m->c->net.socket) == 0) {
        if (blocked == 0) {
            blocked = 1;
            LOG("Blocking publish on queue full for client %s", m->c->clientID);
        }
        Thread_unlock_mutex(mqttClientMutex);
        MQTTClient_yield();
        Thread_lock_mutex(mqttClientMutex);
        if (m->c->connected == 0) {
            rc = MQTT_CLIENT_FAILURE;
            goto exit;
        }

    }
    if (blocked == 1) {
        LOG("Resume publish now queue not full for client %s", m->c->clientID);
    }

    if (qos > 0 && (msgId == MQTTProtocol_assignMsgId(m->c)) == 0) {
        rc = MQTT_CLIENT_MAX_MESSAGES_INFLIGHT;
        goto exit;
    }

    publish = malloc(sizeof(Publish));

    publish->payload = payload;
    publish->payloadLen = payloadLen;
    publish->topic = (char*)topicName;
    publish->msgId = msgId;

    rc = MQTTProtocol_startPublish(m->c, publish, qos, retained, &msg);

    /** 如果数据有一部分已经写入了socket, 等待期结束，但是如果这期间连接中断且qos != 0，
     * 仍然返回成功，将数据写入persistence, 当再次连接成功时，发送剩余数据*/
    if (rc == TCPSOCKET_INTERRUPTED) {
        while (m->c->connected == CONNECT_STATE_WAIT_FOR_TCP_COMPLETE && SocketBuffer_getWrite(m->c->net.socket)) {
            Thread_unlock_mutex(mqttClientMutex);
            MQTTClient_yield();
            Thread_lock_mutex(mqttClientMutex);
        }
        rc = (qos > 0 || m->c->connected == 1) ? MQTT_CLIENT_SUCCESS : MQTT_CLIENT_FAILURE;
    }

    if (deliveryToken && qos > 0) {
        *deliveryToken = msg->msgId;
    }

    free(publish);

    if (rc == SOCKET_ERROR) {
        MQTTClient_disconnect_internal(handle, 0);
        rc = (qos > 0) ? MQTT_CLIENT_SUCCESS : MQTT_CLIENT_FAILURE;
    }

exit:
    Thread_unlock_mutex(mqttClientMutex);
    return rc;
}


int MQTTClient_publishMessage(MQTTClient handle, const char* topicName, MQTTClient_message* message,
                              MQTTClient_deliveryToken * deliveryToken) {
    int rc = MQTT_CLIENT_SUCCESS;

    if (message == NULL) {
        rc = MQTT_CLIENT_NULL_PARAMETER;
        goto exit;
    }

    if (strncmp(message->structId, "MQTM", 4) != 0 || message->structVersion != 0) {
        rc = MQTT_CLIENT_BAD_STRUCTURE;
        goto exit;
    }

    rc = MQTTClient_publish(handle, topicName, message->payloadLen, message->payload, message->qos,
                            message->retained, deliveryToken);

exit:
    return rc;

}


static void MQTTClient_retry(void) {
    time_t now;

    time(&(now));
    if (difftime(now, last) > 5) {
        time(&(last));
        MQTTProtocol_keepAlive(now);
        MQTTProtocol_retry(now, 1, 0);
    } else {
        MQTTProtocol_retry(now, 0, 0);
    }
}

/**
 * 获取一个就绪的Socket,然后从Socket中读取一个Packet
 * @param sock
 * @param timeout
 * @param rc
 * @return
 */
static MQTTPacket* MQTTClient_cycle(int* sock, unsigned long timeout, int* rc) {
    struct timeval tp = {0L,0L};
    static Ack ack;
    MQTTPacket* packet = NULL;

    if (timeout > 0L) {
        tp.tv_sec = timeout / 1000;
        tp.tv_usec = (timeout % 1000) * 1000;
    }

#ifdef OPENSSL
    if ((*sock = SSLSocket_getPendingRead()) == -1) {
    /* 0 indicates no work to do, -1 == error, but can happen normally*/
#endif
    Thread_lock_mutex(socketMutex);
    *sock = Socket_getReadySocket(0, &tp);
    Thread_unlock_mutex(socketMutex);
#ifdef OPENSSL
    }
#endif

    Thread_lock_mutex(mqttClientMutex);

    if (*sock > 0) {
        MQTTClients* m = NULL;
        if (ListFindItem(handles, sock, clientSocketCompare) != NULL) {
            m = (MQTTClient)(handles->current->content);
        }

        if (m != NULL) {
            if (m->c->connectState == CONNECT_STATE_WAIT_FOR_TCP_COMPLETE
                || m->c->connectState == CONNECT_STATE_WAIT_FOR_SSL_COMPLETE) {
                /* 如果当前仍然处于等待连接完成的状态(tcp连接发出/SSL连接发出)*/
                *rc = 0;
            } else {
                //从socket中读取一个packet, packet可能是publish,subscribe...
                packet = MQTTPacket_Factory(&m->c->net, rc);
                if (*rc == TCPSOCKET_INTERRUPTED) {
                    *rc = 0;
                }
            }
        }

        if (packet != NULL) {
            int freed = 1;
            unsigned int type  = packet->header.bits.type;


            if (type == PUBLISH) {
                //处理接收到的Publish消息
                *rc = MQTTProtocol_handlePublishes(packet, *sock);
            } else if (type == PUBACK || type == PUBCOMP) {
                int msgId;
                ack = (type == PUBCOMP) ? *(Pubcomp*)packet : *(Puback*)packet;
                msgId = ack.msgId;
                *rc = (type == PUBCOMP) ? MQTTProtocol_handlePubcomps(packet, *sock) :
                      MQTTProtocol_handlePubacks(packet, *sock);
                if (m && m->dc) {
                    //如果接收到PUBACK或PUBCOMP，则回调deliveryComplete函数
                    (*(m->dc))(m->context, msgId);
                }
            } else if (type == PUBREC) {
                *rc = MQTTProtocol_handlePubrecs(packet, *sock);
            } else if (type == PUBREL) {
                *rc = MQTTProtocol_handlePubrels(packet, *sock);
            } else if (type == PINGRESP) {
                *rc = MQTTProtocol_handlePingresps(packet, *sock);
            } else {
                freed = 0;
            }

            if (freed) {
                packet = NULL;
            }
        }
    }
    MQTTClient_retry();
    Thread_unlock_mutex(mqttClientMutex);
    return packet;
}

static MQTTPacket* MQTTClient_waitFor(MQTTClient handle, int packetType, int* rc, long timeout) {
    MQTTPacket* packet = NULL;
    MQTTClients* m = handle;

    struct timeval start = MQTTClient_start_clock();
    if (m == NULL) {
        *rc = MQTT_CLIENT_FAILURE;
        goto exit;
    }

    if (running) {
        /* 当MQTTClient_run函数执行时，running = 1，
         * 即设置messageArrived回调新开启了一个线程来处理消息的接收*/
        if (packetType == CONNECT) {
            if ((*rc = Thread_wait_sem(m->connect_sem, timeout)) == 0) {
                *rc = m->rc;
            }
        } else if (packetType == CONNACK) {
            *rc = Thread_wait_sem(m->connack_sem, timeout);
        } else if (packetType == SUBACK) {
            *rc = Thread_wait_sem(m->unsuback_sem, timeout);
        }

        if (*rc == 0 && packetType != CONNECT && m->pack == NULL) {
            LOG("WaitFor unexpectedly is NULL for client %s, packetType %d, timeout %ld",
                m->c->clientID, packetType, timeout);
        }
        packet = m->pack;
    } else {
        *rc = TCPSOCKET_COMPLETE;
        while (1) {
            int sock = -1;
            packet = MQTTClient_cycle(&sock, 100L, rc);
            if (sock == m->c->net.socket) {
                if (*rc == SOCKET_ERROR) {
                    break;
                }
                if (packet && (packet->header.bits.type == packetType)) {
                    break;
                }

                if (m->c->connectState == CONNECT_STATE_WAIT_FOR_TCP_COMPLETE) {
                    int error;
                    socklen_t len = sizeof(error);

                    if ((*rc = getsockopt(m->c->net.socket, SOL_SOCKET, SO_ERROR, (char*)&error, &len)) == 0) {
                        *rc = error;
                    }
                    break;
                }
#ifdef OPENSSL
                else if (m->c->connectState == CONNECT_STATE_WAIT_FOR_SSL_COMPLETE) {
                    *rc = SSLSocket_connect(m->c->net.ssl, sock);
                    if (*rc == SSL_FATAL) {
                        break;
                    } else if (*rc == 1) {
                        // rc == 1 代表SSL连接完成
                        if (!m->c->cleanSession && m->c->session == NULL) {
                            m->c->session = SSL_get1_session(m->c->net.ssl);
                        }
                        break;
                    }
                }
#endif
                else if (m->c->connectState == CONNECT_STATE_WAIT_FOR_CONNACK) {
                    int error;
                    socklen_t len = sizeof(error);

                    if (getsockopt(m->c->net.socket, SOL_SOCKET, SO_ERROR, (char*)&error, &len) == 0) {
                        if (error) {
                            *rc = error;
                            break;
                        }
                    }
                }
            }
            if (MQTTClient_elapsed(start) > timeout) {
                packet = NULL;
                break;
            }
        }
    }

exit:
    return packet;

}


/**
 * 接收消息
 * @param handle
 * @param topicName
 * @param topicLen
 * @param message
 * @param timeout
 * @return
 */
int MQTTClient_receive(MQTTClient handle, char** topicName, int* topicLen, MQTTClient_message** message, unsigned long timeout) {
    int rc = TCPSOCKET_COMPLETE;
    struct timeval start = MQTTClient_start_clock();
    unsigned long elapsed = 0L;
    MQTTClients* m = handle;

    if (m == NULL || m->c == NULL || running) {
        rc = MQTT_CLIENT_FAILURE;
        goto exit;
    }

    if (m->c->connected == 0) {
        rc = MQTT_CLIENT_DISCONNECTED;
        goto exit;
    }

    *topicName = NULL;
    *message = NULL;

    if (m->c->publishMessageQueue->count > 0) {
        timeout = 0L;
    }

    elapsed = MQTTClient_elapsed(start);
    do {
        int sock = 0;
        //读取一个packet
        MQTTClient_cycle(&sock, (timeout > elapsed) ? timeout - elapsed : 0L, &rc);

        if (rc == SOCKET_ERROR) {
            if (ListFindItem(handles, &sock, clientSockCompare) &&
                    (MQTTClient)(handles->current->content) == handle) {
                break;
            }
        }
        elapsed = MQTTClient_elapsed(start);
    } while (elapsed < timeout && m->c->publishMessageQueue->count == 0);

    if (m->c->publishMessageQueue->count > 0) {
        rc = MQTTClient_deliverMessage(rc, m, topicName, topicLen, message);
    }

    if (rc == SOCKET_ERROR) {
        MQTTClient_disconnect_internal(handle, 0);
    }

exit:
    return rc;
}


void MQTTClient_yield() {
    struct timeval start = MQTTClient_start_clock();
    unsigned long elapsed = 0L;
    unsigned long timeout = 100L;
    int rc = 0;

    if (running) { //yield 不能在多线程环境中使用
        MQTTClient_sleep(timeout);
        return;
    }

    elapsed = MQTTClient_elapsed(start);
    do {
        int sock = -1;
        MQTTClient_cycle(&sock, (timeout > elapsed) ? timeout - elapsed : 0L, &rc);
        Thread_lock_mutex(mqttClientMutex);
        if (rc == SOCKET_ERROR && ListFindItem(handles, &sock, clientSockCompare)) {
            MQTTClients* m = (MQTTClient)(handles->current->content);
            if (m->c->connectState != CONNECT_STATE_DISCONNECTED) {
                MQTTClient_disconnect_internal(m, 0);
            }
        }
        Thread_unlock_mutex(mqttClientMutex);
        elapsed = MQTTClient_elapsed(start);
    } while (elapsed < timeout);

}

static int pubCompare(void* a, void* b) {
    Messages* msg = (Messages*)a;
    return msg->publish == (Publications*)b;
}


int MQTTClient_waitForCompletion(MQTTClient handle, MQTTClient_deliveryToken mdt, unsigned long timeout) {
    int rc = MQTT_CLIENT_FAILURE;
    struct timeval start = MQTTClient_start_clock();
    unsigned long elapsed = 0L;
    MQTTClients* m = handle;
    
    Thread_lock_mutex(mqttClientMutex);

    if (m == NULL || m->c == NULL) {
        rc = MQTT_CLIENT_FAILURE;
        goto exit;
    }

    if (m->c->connected == 0) {
        rc = MQTT_CLIENT_DISCONNECTED;
        goto exit;
    }

    if (ListFindItem(m->c->outboundMsgs, &mdt, messageIDCompare) == NULL) {
        rc = MQTT_CLIENT_SUCCESS;
        goto exit;
    }

    elapsed = MQTTClient_elapsed(start);
    while (elapsed < timeout) {
        Thread_unlock_mutex(mqttClientMutex);
        MQTTClient_yield();
        Thread_lock_mutex(mqttClientMutex);
        if (ListFindItem(m->c->outboundMsgs, &mdt, messageIDCompare) == NULL) {
            rc = MQTT_CLIENT_SUCCESS;
            goto exit;
        }
        elapsed = MQTTClient_elapsed(start);
    }


exit:
    Thread_unlock_mutex(mqttClientMutex);
    return rc;
}

int MQTTClient_getPendingDeliveryTokens(MQTTClient handle, MQTTClient_deliveryToken **tokens)
{
    int rc = MQTT_CLIENT_SUCCESS;
    MQTTClients* m = handle;
    *tokens = NULL;

    Thread_lock_mutex(mqttClientMutex);

    if (m == NULL) {
        rc = MQTT_CLIENT_FAILURE;
        goto exit;
    }

    if (m->c && m->c->outboundMsgs->count > 0)
    {
        ListElement* current = NULL;
        int count = 0;

        *tokens = malloc(sizeof(MQTTClient_deliveryToken) * (m->c->outboundMsgs->count + 1));

        while (ListNextElement(m->c->outboundMsgs, &current)) {
            Messages* m = (Messages*)(current->content);
            (*tokens)[count++] = m->msgId;
        }
        (*tokens)[count] = -1;
    }

exit:
    Thread_unlock_mutex(mqttClientMutex);
    return rc;
}

/**
 * 检查是否有pending writes已经完成，如果有删除存储的publication
 */
static void MQTTProtocol_checkPendingWrites(void) {
    if (state.pendingWritePubList.count > 0) {
        ListElement* element = state.pendingWritePubList.first;
        while (element) {
            if (Socket_noPendingWrites(((PendingWritePublication*)(element->content))->socket)) {
                MQTTProtocol_removePublication(((PendingWritePublication*)(element->content))->p);
                state.pendingWritePubList.current = element;
                ListRemove(&(state.pendingWritePubList), element->content); /* does NextElement itself */
                element = state.pendingWritePubList.current;
            }
            else {
                ListNextElement(&(state.pendingWritePubList), &element);
            }
        }
    }
}


static void MQTTClient_writeComplete(int socket) {
    ListElement* found = NULL;

    /* a partial write is now complete for a socket - this will be on a publish*/
    MQTTProtocol_checkPendingWrites();

    /* find the client using this socket */
    if ((found = ListFindItem(handles, &socket, clientSockCompare)) != NULL) {
        MQTTClients* m = (MQTTClients*)(found->content);
        time(&(m->c->net.lastSent));
    }
}




// end