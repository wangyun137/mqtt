#include <stdlib.h>
#include <sys/time.h>
#include <stdbool.h>

#ifndef NO_PERSISTENCE
#include "MQTTPersistence.h"
#endif

#include "MQTTAsync.h"
#include "utf-8.h"
#include "MQTTProtocol.h"
#include "Thread.h"
#include "Log.h"
#include "Heap.h"

#ifdef OPENSSL
#include "SSLSocket.h"
#endif

#define URI_TCP "tcp://"

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

static ClientStates ClientState = {
        "1.1.0", //version
        NULL //client list
};

ClientStates* bState = &ClientState;

MQTTProtocol state;

/**
 * 定义线程状态
 */
enum AsyncThreadStates {
    STOPPED, STARTING, RUNNING, STOPPING
};

/**
 * 定义两个线程状态
 */

//初始化发送线程的状态
enum AsyncThreadStates sendThreadState = STOPPED;
//初始化接收线程的状态
enum AsyncThreadStates receiveThreadState = STOPPED;


static pthread_t sendThreadId = 0;
static pthread_t receiveThreadId = 0;

static pthread_mutex_t mqttAsyncMutexStore = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t* mqttAsyncMutex = &mqttAsyncMutexStore;

static pthread_mutex_t socketMutexStore = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t* socketMutex = &socketMutexStore;

static pthread_mutex_t mqttCommandMutexStore = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t* mqttCommandMutex = &mqttCommandMutexStore;

static cond_type_struct sendCondStore = { PTHREAD_COND_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
static cond_type sendCond = &sendCondStore;

/**
 * 初始化mutex和condition
 */
void MQTTAsync_init(void) {
    pthread_mutexattr_t  attr;
    int rc;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);

    if ((rc = pthread_mutex_init(mqttAsyncMutex, &attr)) != 0) {
        LOG("MQTTAsync error %d initializing async mutex", rc);
    }

    if ((rc = pthread_mutex_init(mqttCommandMutex, &attr)) != 0) {
        LOG("MQTTAsync error %d initializing command mutex", rc);
    }

    if ((rc = pthread_mutex_init(socketMutex, &attr)) != 0) {
        LOG("MQTTAsync error %d initializing socket mutex", rc);
    }

    if ((rc = pthread_cond_init(&sendCond->cond, NULL)) != 0) {
        LOG("MQTTAsync error %d initializing send cond ", rc);
    }

    if ((rc = pthread_mutex_init(&sendCond->mutex, &attr)) != 0) {
        LOG("MQTTAsync error %d initializing send cond mutex", rc);
    }

}

/* 表明是否完成初始化*/
static volatile bool initialized = 0;
/* MQTTAsyncClient列表, 存储MQTTAsyncClient指针*/
static List* handles = NULL; /* List<MQTTAsyncClient*> */
static bool toStop = false;
/* 存储所有Command的列表, List<MQTTAsyncQueued_command*> */
static List* commands = NULL;

/**
 * 获取当前时间
 * @return
 */
struct timeval MQTTAsync_start_clock(void) {
    static struct timeval start;
    gettimeofday(&start, NULL);
    return start;
}

/**
 * 获取当前时间与启动时间的差值
 * @param start
 * @return
 */
long MQTTAsync_elapsed(struct timeval start) {
    struct timeval now;
    struct timeval res;

    gettimeofday(&now, NULL);
    timersub(&now, &start, &res);
    return (res.tv_sec) * 1000 + (res.tv_usec) / 1000;
}

/**
 * 表示一个完整的消息结构, MQTTAsyncMessage中没有topicName,
 * seqNo是序列号
 */
typedef struct {
    MQTTAsyncMessage*   msg;
    char*               topicName;
    int                 topicLen;
    unsigned int        seqNo;
} qEntry;

/**
 * 定义command数据结构
 */
typedef struct {
    int                     type;       /* CONNECT, CONNACK, ...*/
    MQTTAsync_onSuccess*    onSuccess;  /* 成功回调函数*/
    MQTTAsync_onFailure*    onFailure;  /* 失败回调函数*/
    MQTTAsyncToken          token;      /* token值，int*/
    void*                   context;
    struct timeval          startTime;  /* 创建command的时间*/
    union {
        struct {
            int     count;
            char**  topics; /*topic数组*/
            int*    qoss;   /*qos数组*/
        } sub;
        struct {
            int     count;
            char**  topics;
        } unsub;
        struct {
            char*   destinationName;
            int     payloadLen;
            void*   payload;
            int     qos;
            int     retained;
        } pub;
        struct {
            bool    internal;
            int     timeout;
        } dis;
        struct {
            int     currentURI;
            int     mqttVersion;
        } conn;
    } details;

} AsyncCommand;


typedef struct MQTTAsync_struct {
    char*                           serverURI;  /* 服务端地址 */
    bool                            ssl;        /* 是否是ssl连接*/
    Clients*                        c;          /* client数据结构*/

    MQTTAsync_onConnectionLost*     cl;         /* 连接中断回调函数*/
    MQTTAsync_onMessageArrived*     ma;         /* 消息到达回调函数*/
    MQTTAsync_onDeliveryComplete*   dc;         /* 确认发送消息的已到达回调函数*/
    void*                           context;

    MQTTAsync_onConnected*          connected;  /* Connect成功时的回调函数*/
    void*                           connectedContext;

    AsyncCommand                    connectCommand;
    AsyncCommand                    disconnectCommand;
    AsyncCommand*                   pendingWriteCommandList;   /* 延迟命令*/

    List*                           responses;      /* 响应列表, List<MQTTAsync_queuedCommand>*/
    unsigned int                    commandSeqNo;
    MQTTPacket*                     pack;

    MQTTAsyncCreateOptions*         createOptions;
    bool                            shouldBeConnected;

    bool                            automaticReconnect; /* 指明是否应该自动重连*/
    int                             minRetryInterval;
    int                             maxRetryInterval;
    int                             serverURICount;
    char**                          serverURIs; /* 服务端地址数组*/
    int                             connectTimeout; /* connect的超时时长*/

    int                             currentInterval;
    struct timeval                  lastConnectionFailedTime;
    bool                            retrying;
    bool                            reconnectNow;

} MQTTAsyncClient;


typedef struct {
    AsyncCommand            command;
    MQTTAsyncClient*        client;
    unsigned int            seqNo;
} MQTTAsync_queuedCommand;


static int clientSockCompare(void* a, void* b);

static void MQTTAsync_lock_mutex(pthread_mutex_t* aMutex);

static void MQTTAsync_unlock_mutex(pthread_mutex_t* aMutex);

static int MQTTAsync_checkConn(AsyncCommand* command, MQTTAsyncClient* client);

static void MQTTAsync_terminate(void);


#ifndef NO_PERSISTENCE
static int MQTTAsync_unPersistCommand(MQTTAsync_queuedCommand* qCmd);

static int MQTTAsync_persistCommand(MQTTAsync_queuedCommand* qCmd);

static MQTTAsync_queuedCommand* MQTTAsync_restoreCommand(char* buffer, int bufLen);

static void MQTTAsync_insertInOrder(List* list, void* content, int size);

static int MQTTAsync_restoreCommands(MQTTAsyncClient* client);
#endif


static int MQTTAsync_addCommand(MQTTAsync_queuedCommand* command, int command_size);

static void MQTTAsync_startConnectRetry(MQTTAsyncClient* m);

static void MQTTAsync_checkDisconnect(MQTTAsync handle, AsyncCommand* command);

static void MQTTProtocol_checkPendingWrites(void);

static void MQTTAsync_freeServerURIs(MQTTAsyncClient* m);

static void MQTTAsync_freeCommandInternal(MQTTAsync_queuedCommand *command);

static void MQTTAsync_freeCommand(MQTTAsync_queuedCommand *command);

static void MQTTAsync_writeComplete(int socket);

static int MQTTAsync_processCommand(void);

static void MQTTAsync_checkTimeouts(void);

static void* MQTTAsync_sendThread(void* n);

static void MQTTAsync_emptyMessageQueue(Clients* client);

static void MQTTAsync_removeResponsesAndCommands(MQTTAsyncClient* m);

static int MQTTAsync_completeConnection(MQTTAsyncClient* m, MQTTPacket* pack);

static void* MQTTAsync_receiveThread(void* n);

static void MQTTAsync_stop(void);

static void MQTTAsync_closeOnly(Clients* client);

static void MQTTAsync_closeSession(Clients* client);

static int clientStructCompare(void* a, void* b);

static int MQTTAsync_cleanSession(Clients* client);

static int MQTTAsync_deliverMessage(MQTTAsyncClient* m, char* topicName, size_t topicLen, MQTTAsyncMessage* mm);

static int MQTTAsync_disconnect1(MQTTAsync handle, const MQTTAsyncDisconnectOptions* options, int internal);

static int MQTTAsync_disconnect_internal(MQTTAsync handle, int timeout);

static int cmdMessageIDCompare(void* a, void* b);

static int MQTTAsync_assignMsgId(MQTTAsyncClient* m);

static int MQTTAsync_countBufferedMessages(MQTTAsyncClient* m);

static void MQTTAsync_retry(void);

static int MQTTAsync_connecting(MQTTAsyncClient* m);

static MQTTPacket* MQTTAsync_cycle(int* sock, unsigned long timeout, int* rc);

static int pubCompare(void* a, void* b);


/**
 * 线程休眠
 * @param milliSeconds
 */
void MQTTAsync_sleep(long milliSeconds) {
    usleep(milliSeconds * 1000);
}

/**
 * 根据传入的socket文件描述符比较MQTTAsyncClient是否相等
 * @param a
 * @param b
 * @return
 */
static int clientSockCompare(void* a, void* b) {
    MQTTAsyncClient* m = (MQTTAsyncClient*)a;
    return m->c->net.socket == *(int*)b;
}

static void MQTTAsync_lock_mutex(pthread_mutex_t* aMutex) {
    int rc = Thread_lock_mutex(aMutex);
    if (rc != 0)
        LOG("MQTTAsync lock mutex Error %s locking mutex", strerror(rc));
}

static void MQTTAsync_unlock_mutex(pthread_mutex_t* aMutex) {
    int rc = Thread_unlock_mutex(aMutex);
    if (rc != 0)
        LOG("MQTTAsync unlock mutex Error %s unlocking mutex", strerror(rc));
}

/**
 * 检查当前command的的连接选项配置是否正确
 * @param command
 * @param client
 * @return 1 - 正确; 0 - 错误
 */
static int MQTTAsync_checkConn(AsyncCommand* command, MQTTAsyncClient* client) {
    int rc;
    rc = command->details.conn.currentURI < client->serverURICount ||
            (command->details.conn.mqttVersion == 4 && client->c->mqttVersion == MQTT_VERSION_DEFAULT);
    return rc;

}

/**
 *
 * @param handle
 * @param serverURI
 * @param clientId
 * @param persistenceType
 * @param persistenceContext
 * @param options
 * @return
 */
int MQTTAsync_createWithOptions(MQTTAsync* handle, const char* serverURI, const char* clientId,
                                int persistenceType, void* persistenceContext, MQTTAsyncCreateOptions* options) {

    int rc = MQTT_ASYNC_SUCCESS;
    MQTTAsyncClient* m = NULL;

    MQTTAsync_lock_mutex(mqttAsyncMutex);

    if (serverURI == NULL || clientId == NULL) {
        rc = MQTT_ASYNC_NULL_PARAMETER;
        goto exit;
    }

    //验证clientID是否是合法的UTF-8字符串
    if (!UTF8_validateString(clientId)) {
        rc = MQTT_ASYNC_BAD_UTF8_STRING;
        goto exit;
    }

    //验证options结构是否正确
    if (options && (strncmp(options->structId, "MQCO", 4) != 0 || options->structVersion != 0)) {
        rc = MQTT_ASYNC_BAD_STRUCTURE;
        goto exit;
    }

    if (!initialized) {
        Heap_initialize();
        bState->clients = ListInitialize();
        //初始化Sockets结构
        Socket_outInitialize();
        Socket_setWriteCompleteCallback(MQTTAsync_writeComplete);
        handles = ListInitialize();
        commands = ListInitialize();
#ifdef OPENSSL
        SSLSocket_initialize();
#endif
        initialized = true;

    }

    m = malloc(sizeof(MQTTAsyncClient));
    /* MQTTAsync本身就是void*, 传入的handle是MQTTAsync*, 所以handle是void***/
    *handle = m;
    memset(m, 0x00, sizeof(MQTTAsyncClient));

    /**
     * 判断serverURI是tcp还是ssl
     */
    if (strncmp(URI_TCP, serverURI, strlen(URI_TCP)) == 0)
        serverURI += strlen(URI_TCP);
#ifdef OPENSSL
    else if (strncmp(URI_SSL, serverURI, strlen(URI_SSL)) == 0) {
        serverURI += strlen(URI_SSL);
        m->ssl = 1;
    }
#endif

    m->serverURI = MQTTStrdup(serverURI);
    m->responses = ListInitialize();
    /* 将新创建的MQTTAsyncClient添加至handles列表*/
    ListAppend(handles, m, sizeof(MQTTAsync));

    m->c = malloc(sizeof(Clients));
    memset(m->c, 0x00, sizeof(Clients));
    m->c->context = m;
    m->c->outboundMsgs = ListInitialize();
    m->c->inboundMsgs = ListInitialize();
    m->c->publishMessageQueue = ListInitialize();
    m->c->clientID = MQTTStrdup(clientId);
    m->shouldBeConnected = false;

    if (options) {
        m->createOptions = malloc(sizeof(MQTTAsyncCreateOptions));
        memcpy(m->createOptions, options, sizeof(MQTTAsyncCreateOptions));
    }

    /* 初始化Persistence*/
#ifndef NO_PERSISTENCE
    rc = MQTTPersistence_create(&(m->c->persistence), persistenceType, persistenceContext);
    if (rc == MQTT_ASYNC_SUCCESS) {
        rc = MQTTPersistence_initialize(m->c, m->serverURI);
        if (rc == MQTT_ASYNC_SUCCESS) {
            MQTTAsync_restoreCommands(m);
            MQTTPersistence_restoreMessageQueue(m->c);
        }
    }
#endif

    ListAppend(bState->clients, m->c, sizeof(Clients) + 3 * sizeof(List));

exit:
    MQTTAsync_unlock_mutex(mqttAsyncMutex);
    return rc;
}


/**
 * 创建MQTT通信所需的数据结构
 * @param handle
 * @param serverURI
 * @param clientId
 * @param persistenceType
 * @param persistenceContext
 * @return
 */
int MQTTAsync_create(MQTTAsync* handle, const char* serverURI, const char* clientId,
                     int persistenceType, void* persistenceContext) {
    return MQTTAsync_createWithOptions(handle, serverURI, clientId, persistenceType, persistenceContext, NULL);
}

/**
 * 释放相应的内存
 */
static void MQTTAsync_terminate(void) {
    MQTTAsync_stop();
    if (initialized) {
        ListElement* elem = NULL;
        ListFree(bState->clients);
        ListFree(handles);

        while (ListNextElement(commands, &elem)) {
            MQTTAsync_freeCommandInternal((MQTTAsync_queuedCommand*)(elem->content));
        }

        ListFree(commands);
        handles = NULL;
        Socket_outTerminate();
#ifdef OPENSSL
        SSLSocket_terminate();
#endif
        Heap_terminate();
        initialized = false;
    }
}


#ifndef NO_PERSISTENCE

/**
 * 从Persistence中删除内容
 * @param qCmd
 * @return 
 */
static int MQTTAsync_unPersistCommand(MQTTAsync_queuedCommand* qCmd) {
    int rc = MQTT_ASYNC_SUCCESS;
    char key[PERSISTENCE_MAX_KEY_LENGTH + 1];

    sprintf(key, "%s%d", PERSISTENCE_COMMAND_KEY, qCmd->seqNo);
    if ((rc = qCmd->client->c->persistence->pRemove(qCmd->client->c->pHandle, key)) != 0)
        LOG("MQTTAsync_unPersistCommand Error %d removing command from persistence", rc);
    
    return rc;
}

/**
 * 根据Command创建相应的key并将command存储至persistence中
 * @param qCmd
 * @return
 */
static int MQTTAsync_persistCommand(MQTTAsync_queuedCommand* qCmd) {
    int rc = MQTT_ASYNC_SUCCESS;
    MQTTAsyncClient* aClient = qCmd->client;
    AsyncCommand* command = &qCmd->command;
    int* lens = NULL;
    void** bufs = NULL;
    int bufIndex = 0;
    int i;
    int nBufs = 0;
    char key[PERSISTENCE_MAX_KEY_LENGTH + 1];

    switch (command->type) {
        case SUBSCRIBE:
            nBufs = 3 + (command->details.sub.count * 2);

            lens = (int*)malloc(nBufs * sizeof(int));
            bufs = malloc(nBufs * sizeof(char *));

            //index = 0 存放type
            bufs[bufIndex] = &command->type;
            lens[bufIndex++] = sizeof(command->type);

            //index = 1 存放token
            bufs[bufIndex] = &command->token;
            lens[bufIndex++] = sizeof(command->token);

            //index = 2 存放count
            bufs[bufIndex] = &command->details.sub.count;
            lens[bufIndex++] = sizeof(command->details.sub.count);

            //后续的位置依次存放topic, qos
            for (i = 0; i < command->details.sub.count; ++i) {
                bufs[bufIndex] = command->details.sub.topics[i];
                lens[bufIndex++] = (int)strlen(command->details.sub.topics[i]) + 1;
                bufs[bufIndex] = &command->details.sub.qoss[i];
                lens[bufIndex++] = sizeof(command->details.sub.qoss[i]);
            }
            sprintf(key, "%s%d", PERSISTENCE_COMMAND_KEY, ++aClient->commandSeqNo);
            break;
        case UNSUBSCRIBE:
            nBufs = 3 + command->details.unsub.count;

            lens = (int*)malloc(nBufs * sizeof(int));
            bufs = malloc(nBufs * sizeof(char *));

            bufs[bufIndex] = &command->type;
            lens[bufIndex++] = sizeof(command->type);


            bufs[bufIndex] = &command->token;
            lens[bufIndex++] = sizeof(command->token);

            bufs[bufIndex] = &command->details.unsub.count;
            lens[bufIndex++] = sizeof(command->details.unsub.count);

            for (i = 0; i < command->details.unsub.count; i++) {
                bufs[bufIndex] = command->details.unsub.topics[i];
                lens[bufIndex++] = (int)strlen(command->details.unsub.topics[i]) + 1;
            }

            sprintf(key, "%s%d", PERSISTENCE_COMMAND_KEY, ++aClient->commandSeqNo);

            break;
        case PUBLISH:
            nBufs = 7;

            lens = (int*)malloc(nBufs * sizeof(int));
            bufs = malloc(nBufs * sizeof(char *));

            bufs[bufIndex] = &command->type;
            lens[bufIndex++] = sizeof(command->type);


            bufs[bufIndex] = &command->token;
            lens[bufIndex++] = sizeof(command->token);

            bufs[bufIndex] = command->details.pub.destinationName;
            lens[bufIndex++] = (int)strlen(command->details.pub.destinationName) + 1;

            bufs[bufIndex] = &command->details.pub.payloadLen;
            lens[bufIndex++] = sizeof(command->details.pub.payloadLen);


            bufs[bufIndex] = command->details.pub.payload;
            lens[bufIndex++] = command->details.pub.payloadLen;

            bufs[bufIndex] = &command->details.pub.qos;
            lens[bufIndex++] = sizeof(command->details.pub.qos);

            bufs[bufIndex] = &command->details.pub.retained;
            lens[bufIndex++] = sizeof(command->details.pub.retained);

            sprintf(key, "%s%d", PERSISTENCE_COMMAND_KEY, ++aClient->commandSeqNo);
            break;
    }

    if (nBufs > 0 ) {
        if ((rc = aClient->c->persistence->pPut(aClient->c->pHandle, key, nBufs, (char**)bufs, lens)) != 0)
            LOG("ERROR persisting command, rc %d", rc);
        qCmd->seqNo = aClient->commandSeqNo;
    }
    
    if (lens)
        free(lens);

    if (bufs)
        free(bufs);

    return rc;
}

/**
 * 从字符串中恢复为Command
 * @param buffer
 * @param bufLen
 * @return
 */
static MQTTAsync_queuedCommand* MQTTAsync_restoreCommand(char* buffer, int bufLen) {
    AsyncCommand* command = NULL;
    MQTTAsync_queuedCommand* qCommand = NULL;
    char* ptr = buffer;
    int i;
    size_t dataSize;

    qCommand = malloc(sizeof(MQTTAsync_queuedCommand));
    memset(qCommand, 0x00, sizeof(MQTTAsync_queuedCommand));
    command = &qCommand->command;


    command->type = *(int*)ptr;
    ptr += sizeof(int);

    command->token = *(MQTTAsyncToken*)ptr;
    ptr += sizeof(MQTTAsyncToken);

    switch (command->type) {
        case SUBSCRIBE:
            command->details.sub.count = *(int*)ptr;
            ptr += sizeof(int);

            for (i = 0; i < command->details.sub.count; ++i) {
                dataSize = strlen(ptr) + 1;
                command->details.sub.topics[i] = malloc(dataSize);
                strcpy(command->details.sub.topics[i], ptr);
                ptr += dataSize;

                command->details.sub.qoss[i] = *(int*)ptr;
                ptr += sizeof(int);
            }
            break;
        case UNSUBSCRIBE:
            command->details.sub.count = *(int*)ptr;
            ptr += sizeof(int);
            for (i = 0; i< command->details.unsub.count; ++i) {
                size_t dataSize = strlen(ptr) + 1;

                command->details.unsub.topics[i] = malloc(dataSize);
                strcpy(command->details.unsub.topics[i], ptr);
                ptr += dataSize;
            }
            break;
        case PUBLISH:
            dataSize = strlen(ptr) + 1;
            command->details.pub.destinationName = malloc(dataSize);
            strcpy(command->details.pub.destinationName, ptr);
            ptr += dataSize;

            command->details.pub.payloadLen = *(int*)ptr;
            ptr += sizeof(int);

            dataSize = command->details.pub.payloadLen;
            command->details.pub.payload = malloc(dataSize);
            memcpy(command->details.pub.payload, ptr, dataSize);
            ptr += dataSize;

            command->details.pub.qos = *(int*)ptr;
            ptr += sizeof(int);

            command->details.pub.retained = *(int*)ptr;
            ptr += sizeof(int);
            break;
        default:
            free(qCommand);
            qCommand = NULL;
            break;
    }

    return qCommand;
}

/**
 * 按seqNo大小顺序向List中插入Command
 * @param list
 * @param content
 * @param size
 */
static void MQTTAsync_insertInOrder(List* list, void* content, int size) {
    ListElement* index = NULL;
    ListElement* current = NULL;

    while (ListNextElement(list, &current) != NULL && index == NULL) {
        if (((MQTTAsync_queuedCommand*)content)->seqNo < ((MQTTAsync_queuedCommand*)current->content)->seqNo) 
            index = current;
    }
    ListInsert(list, content, size, index);
}

/**
 * 
 * @param client 
 * @return 成功: MQTT_ASYNC_SUCCESS
 */
static int MQTTAsync_restoreCommands(MQTTAsyncClient* client) {
    int rc = MQTT_ASYNC_SUCCESS;
    char** msgKeys;
    int nKeys;
    int i = 0;
    Clients* clients = client->c;
    int commandsRestored = 0;

    if (clients->persistence && (rc = clients->persistence->pKeys(clients->pHandle, &msgKeys, &nKeys)) == MQTT_ASYNC_SUCCESS) {
        /* 先从persistence中获取所有的key*/
        while (rc == 0 && i < nKeys) {
            char* buffer = NULL;
            int bufLen;

            if (strncmp(msgKeys[i], PERSISTENCE_COMMAND_KEY, strlen(PERSISTENCE_COMMAND_KEY)) != MQTT_ASYNC_SUCCESS) {
                LOG("restore commands that msgKey %s is not command key", msgKeys[i]);
            }

            if ((rc = clients->persistence->pGet(clients->pHandle, msgKeys[i], &buffer, &bufLen)) == MQTT_ASYNC_SUCCESS) {
                /* 从persistence中恢复数据*/
                MQTTAsync_queuedCommand* cmd = MQTTAsync_restoreCommand(buffer, bufLen);

                if (cmd) {
                    cmd->client = client;
                    cmd->seqNo = atoi(msgKeys[i] + 2);
                    /* 将恢复后的command按seqNo插入commands列表中*/
                    MQTTPersistence_insertInOrder(commands, cmd, sizeof(MQTTAsync_queuedCommand));
                    free(buffer);
                    client->commandSeqNo = max(client->commandSeqNo, cmd->seqNo);
                    commandsRestored++;
                }
            }

            /* 恢复command后，释放其对应key的内存*/
            if (msgKeys[i]) {
                free(msgKeys[i]);
            }
            i++;
        }
        if (msgKeys != NULL) {
            free(msgKeys);
        }
    }
    LOG("%d commands restore for client %s", commandsRestored, clients->clientID);
    return rc;
}

#endif

/**
 * 添加命令
 * @param command
 * @param commandSize
 * @return
 */
static int MQTTAsync_addCommand(MQTTAsync_queuedCommand* command, int commandSize) {
    int rc = MQTT_ASYNC_SUCCESS;
    MQTTAsync_lock_mutex(mqttCommandMutex);

    //设置command的起始时间
    command->command.startTime = MQTTAsync_start_clock();

    if (command->command.type == CONNECT ||
            (command->command.type == DISCONNECT && command->command.details.dis.internal)) {
        /* 如果command是connect或disconnect, 则查看命令队列中的第一个命令
         * 是否跟要添加的命令类型一致，如果一致，不添加命令，如果不一致，则将命令
         * 插入队列头部*/
        MQTTAsync_queuedCommand* head = NULL;
        if (commands->first) {
            head = (MQTTAsync_queuedCommand*)(commands->first->content);
        }

        if (head != NULL && head->client == command->client &&
                head->command.type == command->command.type) {
            MQTTAsync_freeCommand(command);
        } else {
            ListInsert(commands, command, commandSize, commands->first);
        }
    } else {
        /**
         * 其他命令直接添加至队列，并存储
         */
        ListAppend(commands, command, commandSize);
#ifndef NO_PERSISTENCE
        if (command->client->c->persistence) {
            MQTTAsync_persistCommand(command);
        }
#endif
    }

    MQTTAsync_unlock_mutex(mqttCommandMutex);
    rc = Thread_signal_cond(sendCond);
    if (rc != 0) {
        LOG("MQTTAsync_addCommand error %d signal cond", rc);
    }

    return rc;
}

/**
 * 更新跟重新连接有关的变量
 * @param m
 */
static void MQTTAsync_startConnectRetry(MQTTAsyncClient* m) {
   if (m->automaticReconnect && m->shouldBeConnected) {
       //设置最近一次连接失败的时间
       m->lastConnectionFailedTime = MQTTAsync_start_clock();
       if (m->retrying) {
           m->currentInterval = min(m->currentInterval * 2, m->maxRetryInterval);
       } else {
           m->currentInterval = m->minRetryInterval;
           m->retrying = true;
       }
   }
}


/**
 * 重新进行连接
 * 如果设置了自动重新连接，则更新响应变量; 如果没有设置，则新建一个CONNECT命令放入队列
 * @param handle
 * @return
 */
int MQTTAsync_reconnect(MQTTAsync handle) {
    int rc = MQTT_ASYNC_FAILURE;
    MQTTAsyncClient* m = handle;

    MQTTAsync_lock_mutex(mqttAsyncMutex);

    if (m->automaticReconnect && m->shouldBeConnected) {
        m->reconnectNow = true;
        if (m->retrying == false) {
            m->currentInterval = m->minRetryInterval;
            m->retrying = true;
        }
        rc = MQTT_ASYNC_SUCCESS;
    } else {
        /* 如果设置是不进行自动重连， 新建一个CONNECT命令，放入队列*/
        MQTTAsync_queuedCommand* conn = malloc(sizeof(MQTTAsync_queuedCommand));
        memset(conn, 0x0, sizeof(MQTTAsync_queuedCommand));
        conn->client = m;
        conn->command = m->connectCommand;
        if (m->c->mqttVersion == MQTT_VERSION_DEFAULT) {
            conn->command.details.conn.mqttVersion = 0;
        }
        MQTTAsync_addCommand(conn, sizeof(m->connectCommand));
        rc = MQTT_ASYNC_SUCCESS;
    }
    MQTTAsync_unlock_mutex(mqttAsyncMutex);
    return rc;
}

/**
 * Disconnect连接，并回调相应的函数
 * @param handle
 * @param command
 */
static void MQTTAsync_checkDisconnect(MQTTAsync handle, AsyncCommand* command) {
    MQTTAsyncClient* m = handle;
    /* 如果没有in-flight消息，或者消息还没超时，关闭Session,*/
    if (m->c->outboundMsgs->count == 0 || MQTTAsync_elapsed(command->startTime) >= command->details.dis.timeout) {
        int wasConnected = m->c->connected;
        MQTTAsync_closeSession(m->c);
        if (command->details.dis.internal) {
            if (m->cl && wasConnected) {
                LOG("Calling connectionLost for client %s", m->c->clientID);
                /* 回调connectionLost*/
                (*(m->cl))(m->context, NULL);
            }
            //设置disconnect相关参数
            MQTTAsync_startConnectRetry(m);
        } else if (command->onSuccess) {
            LOG("Calling disconnect complete for client %s", m->c->clientID);
            /* 回调onSuccess*/
            (*(command->onSuccess))(command->context, NULL);
        }
    }
}

/**
 * 检查pendingWritePubList中是否
 */
static void MQTTProtocol_checkPendingWrites() {
    if (state.pendingWritePubList.count > 0) {
        /* element的content指向的是pending_write*/
        ListElement* element = state.pendingWritePubList.first;
        while (element) {
            if (Socket_noPendingWrites(((PendingWritePublication*)(element->content))->socket)) {
                /* 如果pendingWrite中的socket并不处于写操作等待列表中，
                 * 从pendingWritePubList列表中删除Publication*/
                MQTTProtocol_removePublication(((PendingWritePublication*)(element->content))->p);
                state.pendingWritePubList.current = element;
                ListRemove(&(state.pendingWritePubList), element->content);
                //element指向下一个节点
                element = state.pendingWritePubList.current;
            } else
                ListNextElement(&(state.pendingWritePubList), &element);

        }
    }
}

/**
 * 释放serverURI内存
 * @param m
 */
static void MQTTAsync_freeServerURIs(MQTTAsyncClient* m) {
    int i;

    for (i = 0; i < m->serverURICount; ++i)
        free(m->serverURIs[i]);
    if (m->serverURIs)
        free(m->serverURIs);
}

/**
 * 根据command的类型，释放相应的内存
 * @param command
 */
static void MQTTAsync_freeCommandInternal(MQTTAsync_queuedCommand *command) {
    if (command->command.type == SUBSCRIBE) {
        int i;

        for (i = 0; i < command->command.details.sub.count; i++)
            free(command->command.details.sub.topics[i]);

        free(command->command.details.sub.topics);
        free(command->command.details.sub.qoss);
    }
    else if (command->command.type == UNSUBSCRIBE) {
        int i;

        for (i = 0; i < command->command.details.unsub.count; i++)
            free(command->command.details.unsub.topics[i]);

        free(command->command.details.unsub.topics);
    }
    else if (command->command.type == PUBLISH) {
        /* qos 1 and 2 topics are freed in the protocol code when the flows are completed */
        if (command->command.details.pub.destinationName)
            free(command->command.details.pub.destinationName);
        free(command->command.details.pub.payload);
    }
}

static void MQTTAsync_freeCommand(MQTTAsync_queuedCommand *command) {
    MQTTAsync_freeCommandInternal(command);
    free(command);
}


/**
 * 回调onSuccess函数
 * @param socket
 */
static void MQTTAsync_writeComplete(int socket) {
    ListElement* found = NULL;

    MQTTProtocol_checkPendingWrites();

    if ((found = ListFindItem(handles, &socket, clientSockCompare)) != NULL) {
        /* 从handles列表中根据socket文件描述符找到对应的MQTTAsyncClient结构*/
        MQTTAsyncClient* m = (MQTTAsyncClient*)(found->content);
        //更新lastSent时间
        time(&(m->c->net.lastSent));

        if (m->pendingWriteCommandList) {
            ListElement* curResponse = NULL;
            AsyncCommand* command = m->pendingWriteCommandList;
            MQTTAsync_queuedCommand* queuedCommand = NULL;

            /* 遍历response列表，找到列表中和m->pendingWrite一致的元素*/
            while (ListNextElement(m->responses, &curResponse)) {
                queuedCommand = (MQTTAsync_queuedCommand*)(curResponse->content);
                if (queuedCommand->client->pendingWriteCommandList == m->pendingWriteCommandList) {
                    break;
                }
            }

            /* 回调onSuccess函数*/
            if (curResponse && command->onSuccess) {
                MQTTAsyncSuccessData data;
                data.token = command->token;
                data.alt.pub.destinationName = command->details.pub.destinationName;
                data.alt.pub.message.payload = command->details.pub.payload;
                data.alt.pub.message.payloadLen = command->details.pub.payloadLen;
                data.alt.pub.message.qos = command->details.pub.qos;
                data.alt.pub.message.retained = command->details.pub.retained;
                LOG("Calling publish success for client %s", m->c->clientID);
                (*(command->onSuccess))(command->context, &data);
            }
            m->pendingWriteCommandList = NULL;
            //从response中删除对应的节点，但是并不释放queuedCommand内存
            ListDetach(m->responses, queuedCommand);
            //释放queueCommand内存
            MQTTAsync_freeCommand(queuedCommand);
        }
    }
}


/**
 * 处理Command
 * @return
 */
static int MQTTAsync_processCommand() {
    int rc = MQTT_ASYNC_SUCCESS;

    MQTTAsync_queuedCommand* queuedCommand = NULL;
    ListElement* curCommand = NULL;
    List* ignoredClients = NULL;


    MQTTAsync_lock_mutex(mqttAsyncMutex);
    MQTTAsync_lock_mutex(mqttCommandMutex);

    /* 队列中只有第一个command会被处理，如果跳过一个command，必须跳过余下的所有command
     * 用ignoredClients来对这些command进行跟踪*/
    ignoredClients = ListInitialize();

    while (ListNextElement(commands, &curCommand)) {
        /* 从commands队列中取出第一个queueCommand*/
        MQTTAsync_queuedCommand* cmd = (MQTTAsync_queuedCommand*)(curCommand->content);
        /* 如果该command在ignoreClients中，忽略该command,继续获取下一个command*/
        if (ListFind(ignoredClients, cmd->client)) {
            continue;
        }

        /**
         * command必须是connect，disconnect,或者command对应的socket不在写操作等待列表中
         * 否则忽略这个command
         */
        int commandType = cmd->command.type;
        bool noPendingWrite = (cmd->client->c->connected && cmd->client->c->connectState == CONNECT_STATE_NONE &&
                               Socket_noPendingWrites(cmd->client->c->net.socket));
        if (commandType == CONNECT || commandType == DISCONNECT || noPendingWrite) {
            if ((cmd->command.type == PUBLISH || cmd->command.type == SUBSCRIBE || cmd->command.type == UNSUBSCRIBE)
                    && cmd->client->c->outboundMsgs->count >= MAX_MSG_ID - 1) {
                /* 如果messageId已经达到最大，则把该消息加入ignoredClients列表*/
                LOG("No more message ids available");
            } else {
                queuedCommand = cmd;
                break;
            }
        }
        ListAppend(ignoredClients, cmd->client, sizeof(cmd->client));
    }

    /* 释放ignoreClient自身的内存，但不释放其存储的command的内存*/
    ListFreeNoContent(ignoredClients);

    /* 从commands队列中删除command, 并从persistence中删除相应的缓存*/
    if (queuedCommand) {
        ListDetach(commands, queuedCommand);
#ifndef NO_PERSISTENCE
        if (queuedCommand->client->c->persistence) {
            MQTTAsync_unPersistCommand(queuedCommand);
        }
#endif
    }
    MQTTAsync_unlock_mutex(mqttCommandMutex);

    if (!queuedCommand) {
        goto exit;
    }

    if (queuedCommand->command.type == CONNECT) {
        if (queuedCommand->client->c->connectState != CONNECT_STATE_NONE || queuedCommand->client->c->connected) {
            /* 如果command是connect，但目前连接状态是已连接或等待连接完成，不处理*/
            rc = 0;
        } else {
            char* serverURI = queuedCommand->client->serverURI;
            if (queuedCommand->client->serverURICount > 0) {
                if (queuedCommand->client->c->mqttVersion == MQTT_VERSION_DEFAULT) {
                    if (queuedCommand->command.details.conn.mqttVersion == MQTT_VERSION_3_1) {
                        queuedCommand->command.details.conn.currentURI++;
                        queuedCommand->command.details.conn.mqttVersion = MQTT_VERSION_DEFAULT;
                    }
                } else {
                    queuedCommand->command.details.conn.currentURI++;
                }

                serverURI = queuedCommand->client->serverURIs[queuedCommand->command.details.conn.currentURI];

                /**
                 * 获取tcp://或ssl://后真正的server uri
                 */
                if (strncmp(URI_TCP, serverURI, strlen(URI_TCP)) == 0) {
                    serverURI += strlen(URI_TCP);
                }
#ifdef OPENSSL
                else if (strncmp(URI_SSL, serverURI, strlen(URI_SSL)) == 0) {
                    serverURI += strlen(URI_SSL);
                    queuedCommand->client->ssl = 1;
                }
#endif
            }
            /* 设置MQTT版本号*/
            if (queuedCommand->client->c->mqttVersion == MQTT_VERSION_DEFAULT) {
                if (queuedCommand->command.details.conn.mqttVersion == MQTT_VERSION_DEFAULT) {
                    queuedCommand->command.details.conn.mqttVersion = MQTT_VERSION_3_1_1;
                } else if (queuedCommand->command.details.conn.mqttVersion == MQTT_VERSION_3_1_1) {
                    queuedCommand->command.details.conn.mqttVersion = MQTT_VERSION_3_1;
                }
            } else {
                queuedCommand->command.details.conn.mqttVersion = queuedCommand->client->c->mqttVersion;
            }

            LOG("Connecting to serverURI %s with MQTT version %d", serverURI, queuedCommand->command.details.conn.mqttVersion);
            /* 连接Server, 发送CONNECT数据包*/
#ifdef OPENSSL
            rc = MQTTProtocol_connect(serverURI, queuedCommand->client->c, queuedCommand->client->ssl, queuedCommand->command.details.conn.mqttVersion);
#else
            rc = MQTTProtocol_connect(serverURI, queuedCommand->client->c, queuedCommand->command.details.conn.mqttVersion);
#endif

            if (queuedCommand->client->c->connectState == CONNECT_STATE_NONE)
                rc = SOCKET_ERROR;

            /* 如果返回系统错误EINPROGRESS, 则将当前socket添加至写操作等待列表中*/
            if (rc == EINPROGRESS)
                Socket_addPendingWrite(queuedCommand->client->c->net.socket);

        }
    } else if (queuedCommand->command.type == SUBSCRIBE) {
        List* topics = ListInitialize();
        List* qoss = ListInitialize();
        int i;

        /* 添加topic和qos*/
        for (i = 0; i < queuedCommand->command.details.sub.count; i++) {
            ListAppend(topics, queuedCommand->command.details.sub.topics[i], strlen(queuedCommand->command.details.sub.topics[i]));
            ListAppend(qoss, &queuedCommand->command.details.sub.qoss[i], sizeof(int));
        }
        rc = MQTTProtocol_subscribe(queuedCommand->client->c, topics, qoss, queuedCommand->command.token);
        ListFreeNoContent(topics);
        ListFreeNoContent(qoss);
    } else if (queuedCommand->command.type == UNSUBSCRIBE) {
        List* topics = ListInitialize();
        int i;

        for (i = 0; i < queuedCommand->command.details.unsub.count; i++) {
            ListAppend(topics, queuedCommand->command.details.unsub.topics[i], strlen(queuedCommand->command.details.unsub.topics[i]));
        }

        rc = MQTTProtocol_unsubscribe(queuedCommand->client->c, topics, queuedCommand->command.token);
        ListFreeNoContent(topics);
    } else if (queuedCommand->command.type == PUBLISH) {
        Messages* msg = NULL;
        Publish* publish = NULL;

        publish = malloc(sizeof(Publish));

        publish->payload = queuedCommand->command.details.pub.payload;
        publish->payloadLen = queuedCommand->command.details.pub.payloadLen;
        publish->topic = queuedCommand->command.details.pub.destinationName;
        publish->msgId = queuedCommand->command.token;

        /* 发送PUBLISH消息*/
        rc = MQTTProtocol_startPublish(queuedCommand->client->c, publish,
                                       queuedCommand->command.details.pub.qos, queuedCommand->command.details.pub.retained, &msg);

        if (queuedCommand->command.details.pub.qos == 0) {
            /* 如果qos = 0且发送完成，回调onSuccess*/
            if (rc == TCPSOCKET_COMPLETE) {
                if (queuedCommand->command.onSuccess) {
                    MQTTAsyncSuccessData data;

                    data.token = queuedCommand->command.token;
                    data.alt.pub.destinationName = queuedCommand->command.details.pub.destinationName;
                    data.alt.pub.message.payload = queuedCommand->command.details.pub.payload;
                    data.alt.pub.message.payloadLen = queuedCommand->command.details.pub.payloadLen;
                    data.alt.pub.message.qos = queuedCommand->command.details.pub.qos;
                    data.alt.pub.message.retained = queuedCommand->command.details.pub.retained;
                    LOG("Calling publish success for client %s", queuedCommand->client->c->clientID);
                    (*(queuedCommand->command.onSuccess))(queuedCommand->command.context, &data);
                }
            } else {
                /* 将command设为pendingWrite*/
                queuedCommand->command.details.pub.destinationName = NULL;
                queuedCommand->client->pendingWriteCommandList = &queuedCommand->command;
            }
        } else {
            queuedCommand->command.details.pub.destinationName = NULL;
        }

        free(publish);
    } else if (queuedCommand->command.type == DISCONNECT) {
        if (queuedCommand->client->c->connectState != CONNECT_STATE_NONE || queuedCommand->client->c->connected != 0) {
            queuedCommand->client->c->connectState = CONNECT_STATE_DISCONNECTED;
            //发送DISCONECT数据包
            MQTTAsync_checkDisconnect(queuedCommand->client, &queuedCommand->command);
        }
    }

    /* 发送完相应数据包，释放响应内存或处理错误*/

    if (queuedCommand->command.type == CONNECT && rc != SOCKET_ERROR && rc != MQTT_ASYNC_PERSISTENCE_ERROR) {
        queuedCommand->client->connectCommand = queuedCommand->command;
        MQTTAsync_freeCommand(queuedCommand);
    }
    else if (queuedCommand->command.type == DISCONNECT) {
        queuedCommand->client->disconnectCommand = queuedCommand->command;
        MQTTAsync_freeCommand(queuedCommand);
    }
    else if (queuedCommand->command.type == PUBLISH && queuedCommand->command.details.pub.qos == 0) {
        /* 如果是INTERRUPTED, 则将command添加至response中*/
        if (rc == TCPSOCKET_INTERRUPTED)
            /* 如果command是publish且qos = 0, 当被中断时， 将command添加至response*/
            ListAppend(queuedCommand->client->responses, queuedCommand, sizeof(queuedCommand));
        else
            MQTTAsync_freeCommand(queuedCommand);
    }
    else if (rc == SOCKET_ERROR || rc == MQTT_ASYNC_PERSISTENCE_ERROR) {
        if (queuedCommand->command.type == CONNECT) {
            MQTTAsyncDisconnectOptions opts = MQTTAsyncDisconnectOptions_initializer;
            /* 断开连接，这里并不回调connectionLost,同时设置shouldBeConnected = true*/
            MQTTAsync_disconnect(queuedCommand->client, &opts);
            queuedCommand->client->shouldBeConnected = true;
        } else {
            /* 断开连接，同时回调connectionLost*/
            MQTTAsync_disconnect_internal(queuedCommand->client, 0);
        }

        if (queuedCommand->command.type == CONNECT && MQTTAsync_checkConn(&queuedCommand->command, queuedCommand->client)) {
            LOG("Connect failed, more to try");
            /* 重新将CONNECT的command放入队列*/
            rc = MQTTAsync_addCommand(queuedCommand, sizeof(queuedCommand->command.details.conn));
        } else {
            /* 对于其他类型数据包， 回调onFailure并释放内存*/
            if (queuedCommand->command.onFailure) {
                LOG("Calling command failure for client %s", queuedCommand->client->c->clientID);
                (*(queuedCommand->command.onFailure))(queuedCommand->command.context, NULL);
            }
            MQTTAsync_freeCommand(queuedCommand);
        }
    }
    else {
        /* 将subscribe, unsubscribe等command放入response*/
        ListAppend(queuedCommand->client->responses, queuedCommand, sizeof(queuedCommand));
    }


exit:
    MQTTAsync_unlock_mutex(mqttAsyncMutex);
    rc = (queuedCommand != NULL);
    return rc;
}


static void MQTTAsync_checkTimeouts() {
    ListElement* current = NULL;
    static time_t last = 0L;
    time_t now;

    time(&(now));
    if (difftime(now, last) < 3)
        return;


    MQTTAsync_lock_mutex(mqttAsyncMutex);
    last = now;

    /* 遍历handles列表，获取MQTTAsyncClient*/
    while (ListNextElement(handles, &current)) {
        ListElement* curResponse = NULL;
        int i;
        int timedOutCount = 0;

        MQTTAsyncClient* m = (MQTTAsyncClient*)(current->content);
        bool isTimeout = MQTTAsync_elapsed(m->connectCommand.startTime) > (m->connectTimeout * 1000);

        if (m->c->connectState == CONNECT_STATE_DISCONNECTED) {
            MQTTAsync_checkDisconnect(m ,&m->disconnectCommand);
        }
        else if (m->c->connectState != CONNECT_STATE_NONE && isTimeout) {
            /* 如果超时了或还未连接成功, 如果连接参数正确，重新将command添加至队列即进行重连*/
            if (MQTTAsync_checkConn(&m->connectCommand, m)) {
                MQTTAsync_queuedCommand* conn;
                //断开连接
                MQTTAsync_closeOnly(m->c);
                //重新创建CONNECT的command并添加至command队列
                conn = malloc(sizeof(MQTTAsync_queuedCommand));
                memset(conn, 0x00, sizeof(MQTTAsync_queuedCommand));
                conn->client = m;
                conn->command = m->connectCommand;
                LOG("Connect failed with timeout, more to try");
                MQTTAsync_addCommand(conn, sizeof(m->connectCommand));
            } else {
                /* 回调onFailure*/
                MQTTAsync_closeSession(m->c);
                if (m->connectCommand.onFailure) {
                    MQTTAsyncFailureData data;

                    data.token = 0;
                    data.code = MQTT_ASYNC_FAILURE;
                    data.message = "TCP connect timeout";
                    LOG("Calling connect failure for client %s", m->c->clientID);
                    (*(m->connectCommand.onFailure))(m->connectCommand.context, &data);
                }
                MQTTAsync_startConnectRetry(m);
            }
            continue;
        }

        timedOutCount = 0;

        while (ListNextElement(m->responses, &curResponse)) {
            MQTTAsync_queuedCommand* queuedCommand = (MQTTAsync_queuedCommand*)(curResponse->content);

            if (1 /*MQTTAsync_elapsed(com->command.start_time) < 120000*/)
                break; /* command has not timed out */
            else {
                if (queuedCommand->command.onFailure) {
                    LOG("Calling %s failure for client %s",
                        MQTTPacket_name(queuedCommand->command.type), m->c->clientID);
                    (*(queuedCommand->command.onFailure))(queuedCommand->command.context, NULL);
                }
                timedOutCount++;
            }
        }

        for (i = 0; i < timedOutCount; ++i) {
            ListRemoveHead(m->responses);
        }

        if (m->automaticReconnect && m->retrying) {
            if (m->reconnectNow || MQTTAsync_elapsed(m->lastConnectionFailedTime) > (m->currentInterval * 1000)) {
                MQTTAsync_queuedCommand* conn = malloc(sizeof(MQTTAsync_queuedCommand));
                memset(conn, 0x0, sizeof(MQTTAsync_queuedCommand));
                conn->client = m;
                conn->command = m->connectCommand;

                if (m->c->mqttVersion == MQTT_VERSION_DEFAULT) {
                    conn->command.details.conn.mqttVersion = 0;
                }
                LOG("Automatically attempting to reconnect");
                MQTTAsync_addCommand(conn, sizeof(m->connectCommand));
                m->reconnectNow = false;
            }
        }
    }
    MQTTAsync_unlock_mutex(mqttAsyncMutex);
}

/**
 * 定义发送线程的执行逻辑
 * @param n
 * @return
 */
static void* MQTTAsync_sendThread(void* n) {
    MQTTAsync_lock_mutex(mqttAsyncMutex);
    sendThreadState = RUNNING;
    sendThreadId = Thread_getId();
    MQTTAsync_unlock_mutex(mqttAsyncMutex);
    /**
     * 只要没有设置toStop = 1，无限循环发送数据
     */
    while (!toStop) {
        int rc;
        while (commands->count > 0) {
            if (MQTTAsync_processCommand() == 0) {
                break;
            }
        }

        if ((rc = Thread_wait_cond(sendCond, 1)) != 0 && rc != ETIMEDOUT) {
            LOG("Error %d waiting for condition variable", rc);
        }

        MQTTAsync_checkTimeouts();
    }
    sendThreadState = STOPPING;
    MQTTAsync_lock_mutex(mqttAsyncMutex);
    sendThreadState = STOPPED;
    sendThreadId = 0;
    MQTTAsync_unlock_mutex(mqttAsyncMutex);
    return 0;
}

/**
 * 清空消息队列,并释放相应的内存
 * @param client
 */
static void MQTTAsync_emptyMessageQueue(Clients* client) {
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



static void MQTTAsync_removeResponsesAndCommands(MQTTAsyncClient* m) {
    int count = 0;
    ListElement* current = NULL;
    ListElement* next = NULL;

    if (m->responses) {
        ListElement* curResponse = NULL;
        /* 循环response队列，并回调对应的onFailure函数, 同时释放command相应的内存*/
        while (ListNextElement(m->responses, &curResponse)) {
            MQTTAsync_queuedCommand* command = (MQTTAsync_queuedCommand*)(curResponse->content);
            if (command->command.onFailure) {
                MQTTAsyncFailureData data;
                data.token = command->command.token;
                data.code = MQTT_ASYNC_OPERATION_INCOMPLETE;
                data.message = NULL;

                LOG("Calling %s failure for client %s", MQTTPacket_name(command->command.type),
                    m->c->clientID);
                (*(command->command.onFailure))(command->command.context, &data);
            }

            MQTTAsync_freeCommandInternal(command);
            count++;
        }
    }
    ListEmpty(m->responses);
    LOG("%d responses removed for client %s", count ,m->c->clientID);

    count = 0;
    current = ListNextElement(commands, &next);
    ListNextElement(commands, &next);

    while (current) {
        MQTTAsync_queuedCommand* command = (MQTTAsync_queuedCommand*)(current->content);
        if (command->client == m) {
            ListDetach(commands, command);

            if (command->command.onFailure) {
                MQTTAsyncFailureData data;

                data.token = command->command.token;
                data.code = MQTT_ASYNC_OPERATION_INCOMPLETE;
                data.message = NULL;

                LOG("Calling %s failure for client %s", MQTTPacket_name(command->command.type), m->c->clientID);
                (*(command->command.onFailure))(command->command.context, &data);
            }

            MQTTAsync_freeCommand(command);
            count++;
        }
        current = next;
        ListNextElement(commands, &next);
    }
    LOG("%d commands removed for client %s", count , m->c->clientID);
}


void MQTTAsync_destroy(MQTTAsync* handle) {
    MQTTAsyncClient* m = *handle;

    MQTTAsync_lock_mutex(mqttAsyncMutex);

    if (m == NULL) {
        goto exit;
    }

    MQTTAsync_removeResponsesAndCommands(m);
    ListFree(m->responses);

    if (m->c) {
        int savedSocket = m->c->net.socket;
        char* savedClientId = MQTTStrdup(m->c->clientID);
#ifndef NO_PERSISTENCE
        MQTTPersistence_close(m->c);
#endif
        MQTTAsync_emptyMessageQueue(m->c);
        MQTTProtocol_freeClient(m->c);

        if (!ListRemove(bState->clients, m->c)) {
            LOG("Failed to remove clients at %s-%d", __FILE__, __LINE__);
        } else {
            LOG("Success to remove clients for socket %d at %s-%d", savedSocket, __FILE__, __LINE__);
        }
        free(savedClientId);
    }

    if (m->serverURI) {
        free(m->serverURI);
    }
    if (m->createOptions) {
        free(m->createOptions);
    }
    MQTTAsync_freeServerURIs(m);

    if (!ListRemove(handles, m)) {
        LOG("Free error at %s-%d", __FILE__, __LINE__);
    }
    *handle = NULL;
    if (bState->clients->count == 0) {
        MQTTAsync_terminate();
    }

exit:
    MQTTAsync_unlock_mutex(mqttAsyncMutex);
}



void MQTTAsync_freeMessage(MQTTAsyncMessage** message) {
    free((*message)->payload);
    free(*message);
    *message = NULL;
}


void MQTTAsync_free(void* memory) {
    free(memory);
}


static int MQTTAsync_completeConnection(MQTTAsyncClient* m, MQTTPacket* packet) {
    int rc = MQTT_ASYNC_FAILURE;

    if (m->c->connectState == CONNECT_STATE_WAIT_FOR_CONNACK) {
        Connack* connack = (Connack*)packet;
        if ((rc = connack->rc) == MQTT_ASYNC_SUCCESS) {
            m->retrying = false;
            m->c->connected = 1;
            m->c->good = 1;
            m->c->connectState = CONNECT_STATE_NONE;

            if (m->c->cleanSession)
                rc = MQTTAsync_cleanSession(m->c);


            if (m->c->outboundMsgs->count > 0) {
                ListElement* outCurrent = NULL;
                while (ListNextElement(m->c->outboundMsgs, &outCurrent)) {
                    Messages* m = (Messages*)(outCurrent->content);
                    m->lastTouch = 0;
                }
                MQTTProtocol_retry((time_t)0, 1, 1);
                if (m->c->connected != 1) {
                    rc = MQTT_ASYNC_DISCONNECTED;
                }
            }
        }
        free(connack);
        m->pack = NULL;
        Thread_signal_cond(sendCond);
    }

    return rc;
}

/**
 * 接收线程的逻辑
 * @param n
 * @return
 */
static void* MQTTAsync_receiveThread(void* n) {
    long timeout = 10L;

    MQTTAsync_lock_mutex(mqttAsyncMutex);
    receiveThreadState = RUNNING;
    receiveThreadId = Thread_getId();
    while (!toStop) {
        int rc = SOCKET_ERROR;
        int sock = -1;
        MQTTAsyncClient* m = NULL;
        MQTTPacket* packet = NULL;

        MQTTAsync_unlock_mutex(mqttAsyncMutex);
        /* 从socket中读取一个packet,并处理*/
        packet = MQTTAsync_cycle(&sock, timeout, &rc);
        MQTTAsync_lock_mutex(mqttAsyncMutex);

        if (toStop) {
            break;
        }
        timeout = 1000L;

        if (sock == 0) {
            continue;
        }

        if (ListFindItem(handles, &sock, clientSockCompare) == NULL) {
            LOG("Could not find client corresponding to socket %d", sock);
            continue;
        }

        m = (MQTTAsyncClient*)(handles->current->content);
        if (m == NULL) {
            LOG("Client structure was NULL for socket %d - removing socket", sock);
            Socket_close(sock);
            continue;
        }

        if (rc == SOCKET_ERROR) {
            LOG("Error from MQTTAsync_cycle() - removing socket %d", sock);
            if (m->c->connected == 1) {
                MQTTAsync_unlock_mutex(mqttAsyncMutex);
                MQTTAsync_disconnect_internal(m, 0);
                MQTTAsync_lock_mutex(mqttAsyncMutex);
            } else
                MQTTAsync_closeOnly(m->c);

        } else {
            if (m->c->publishMessageQueue->count > 0) {
                qEntry* qe = (qEntry*)(m->c->publishMessageQueue->first->content);
                int topicLen = qe->topicLen;

                if (strlen(qe->topicName) == topicLen)
                    topicLen = 0;


                if (m->ma)
                    /* 回调messageArrived*/
                    rc = MQTTAsync_deliverMessage(m, qe->topicName, topicLen, qe->msg);
                else
                    rc = 1;


                if (rc) {
                    ListRemove(m->c->publishMessageQueue, qe);
#ifndef NO_PERSISTENCE
                    if (m->c->persistence)
                        MQTTPersistence_unpersistQueueEntry(m->c, (MQTTPersistence_qEntry*)qe);
#endif
                } else
                    LOG("False returned from messageArrived for client %s, message remains on queue", m->c->clientID);
            }


            if (packet == NULL) {
                continue;
            }

            if (packet->header.bits.type == CONNACK) {
                int sessionPresent = ((Connack*)packet)->flags.bits.sessionPresent;
                int rc = MQTTAsync_completeConnection(m, packet);

                if (rc == MQTT_ASYNC_SUCCESS) {
                    int onSuccess = 0;
                    if (m->serverURICount > 0) {
                        LOG("Connect succeeded to %s", m->serverURIs[m->connectCommand.details.conn.currentURI]);
                    }
                    onSuccess = (m->connectCommand.onSuccess != NULL) ;
                    if (m->connectCommand.onSuccess) {
                        MQTTAsyncSuccessData data;
                        memset(&data, 0x00, sizeof(data));
                        LOG("Calling connect success for client %s", m->c->clientID);
                        if (m->serverURICount > 0) {
                            data.alt.connect.serverURI = m->serverURIs[m->connectCommand.details.conn.currentURI];
                        } else {
                            data.alt.connect.serverURI = m->serverURI;
                        }

                        data.alt.connect.mqttVersion = m->connectCommand.details.conn.mqttVersion;
                        data.alt.connect.sessionPresent = sessionPresent;
                        (*(m->connectCommand.onSuccess))(m->connectCommand.context, &data);
                        m->connectCommand.onSuccess = NULL;
                    }

                    if (m->connected) {
                        char* reason = (onSuccess) ? "connect onSuccess called" : "automatic reconnect";
                        LOG("Calling connected for client %s", m->c->clientID);
                        (*(m->connected))(m->connectedContext, reason);
                    }
                } else {
                    if (MQTTAsync_checkConn(&m->connectCommand, m)) {
                        MQTTAsync_queuedCommand* conn;

                        MQTTAsync_closeOnly(m->c);
                        /* put the connect command back to the head of the command queue, using the next serverURI */
                        conn = malloc(sizeof(MQTTAsync_queuedCommand));
                        memset(conn, '\0', sizeof(MQTTAsync_queuedCommand));
                        conn->client = m;
                        conn->command = m->connectCommand;
                        LOG( "Connect failed, more to try");
                        MQTTAsync_addCommand(conn, sizeof(m->connectCommand));
                    } else {
                        MQTTAsync_closeSession(m->c);
                        if (m->connectCommand.onFailure) {
                            MQTTAsyncFailureData data;

                            data.token = 0;
                            data.code = rc;
                            data.message = "CONNACK return code";
                            LOG("Calling connect failure for client %s", m->c->clientID);
                            (*(m->connectCommand.onFailure))(m->connectCommand.context, &data);
                        }
                        MQTTAsync_startConnectRetry(m);
                    }
                }
            }
            else if (packet->header.bits.type == SUBACK) {
                ListElement* current = NULL;

                while (ListNextElement(m->responses, &current)) {
                    MQTTAsync_queuedCommand* command = (MQTTAsync_queuedCommand*)(current->content);
                    if (command->command.token == ((Suback*)packet)->msgId) {
                        Suback* sub = (Suback*)packet;
                        if (!ListDetach(m->responses, command)) /* remove the response from the list */
                            LOG("Subscribe command not removed from command list");

                        if (sub->qoss->count == 1 && *(int*)(sub->qoss->first->content) == MQTT_BAD_SUBSCRIBE) {
                            if (command->command.onFailure) {
                                MQTTAsyncFailureData data;

                                data.token = command->command.token;
                                data.code = *(int*)(sub->qoss->first->content);
                                LOG("Calling subscribe failure for client %s", m->c->clientID);
                                (*(command->command.onFailure))(command->command.context, &data);
                            }
                        } else if (command->command.onSuccess) {
                            MQTTAsyncSuccessData data;
                            int* array = NULL;

                            if (sub->qoss->count == 1)
                                data.alt.qos = *(int*)(sub->qoss->first->content);
                            else if (sub->qoss->count > 1) {
                                ListElement* cur_qos = NULL;
                                int* element = array = data.alt.qosList = malloc(sub->qoss->count * sizeof(int));
                                while (ListNextElement(sub->qoss, &cur_qos))
                                    *element++ = *(int*)(cur_qos->content);
                            }
                            data.token = command->command.token;
                            LOG("Calling subscribe success for client %s", m->c->clientID);
                            (*(command->command.onSuccess))(command->command.context, &data);
                            if (array)
                                free(array);
                        }
                        MQTTAsync_freeCommand(command);
                        break;
                    }
                }
                rc = MQTTProtocol_handleSubacks(packet, m->c->net.socket);
            }
            else if (packet->header.bits.type == UNSUBACK) {
                ListElement* current = NULL;
                int handleCalled = 0;

                while (ListNextElement(m->responses, &current)) {
                    MQTTAsync_queuedCommand* command = (MQTTAsync_queuedCommand*)(current->content);
                    if (command->command.token == ((Unsuback*)packet)->msgId) {
                        if (!ListDetach(m->responses, command)) /* remove the response from the list */
                            LOG("Unsubscribe command not removed from command list");
                        if (command->command.onSuccess) {
                            rc = MQTTProtocol_handleUnsubacks(packet, m->c->net.socket);
                            handleCalled = 1;
                            LOG( "Calling unsubscribe success for client %s", m->c->clientID);
                            (*(command->command.onSuccess))(command->command.context, NULL);
                        }
                        MQTTAsync_freeCommand(command);
                        break;
                    }
                }
                if (!handleCalled)
                    rc = MQTTProtocol_handleUnsubacks(packet, m->c->net.socket);
            }
        }
    }

    receiveThreadState = STOPPED;
    receiveThreadId = 0;
    MQTTAsync_unlock_mutex(mqttAsyncMutex);
    if (sendThreadState != STOPPED) {
        Thread_signal_cond(sendCond);
    }

    return 0;
}


static void MQTTAsync_stop(void) {
    int rc = 0;

    if (sendThreadState != STOPPED || receiveThreadState != STOPPED) {
        int connCount = 0;
        ListElement* current = NULL;

        if (handles != NULL)
        {
            /* find out how many handles are still connected */
            while (ListNextElement(handles, &current))
            {
                if (((MQTTAsyncClient*)(current->content))->c->connectState > CONNECT_STATE_NONE ||
                    ((MQTTAsyncClient*)(current->content))->c->connected)
                    ++connCount;
            }
        }
        LOG( "Conn count is %d", connCount);
        /* stop the background thread, if we are the last one to be using it */
        if (connCount == 0)
        {
            int count = 0;
            toStop = true;
            while ((sendThreadState != STOPPED || receiveThreadState != STOPPED) && ++count < 100)
            {
                MQTTAsync_unlock_mutex(mqttAsyncMutex);
                MQTTAsync_sleep(100L);
                MQTTAsync_lock_mutex(mqttAsyncMutex);
            }
            rc = 1;
            toStop = false;
        }
    }
}


int MQTTAsync_setCallbacks(MQTTAsync handle, void* context,
                           MQTTAsync_onConnectionLost* cl,
                           MQTTAsync_onMessageArrived* ma,
                           MQTTAsync_onDeliveryComplete* dc) {
    int rc = MQTT_ASYNC_SUCCESS;
    MQTTAsyncClient* m = handle;

    MQTTAsync_lock_mutex(mqttAsyncMutex);

    if (m == NULL || ma == NULL || m->c->connectState != 0)
        rc = MQTT_ASYNC_FAILURE;
    else {
        m->context = context;
        m->cl = cl;
        m->ma = ma;
        m->dc = dc;
    }

    MQTTAsync_unlock_mutex(mqttAsyncMutex);
    return rc;
}


int MQTTAsync_setConnected(MQTTAsync handle, void* context, MQTTAsync_onConnected* connected) {
    int rc = MQTT_ASYNC_SUCCESS;
    MQTTAsyncClient *m = handle;
    MQTTAsync_lock_mutex(mqttAsyncMutex);

    if (m == NULL || m->c->connectState != 0)
        rc = MQTT_ASYNC_FAILURE;
    else {
        m->connectedContext = context;
        m->connected = connected;
    }

    MQTTAsync_unlock_mutex(mqttAsyncMutex);
    return rc;
}



static void MQTTAsync_closeOnly(Clients* client) {
    client->good = 0;
    client->pingOutStanding = 0;
    if (client->net.socket > 0) {
        if (client->connected)
            MQTTPacket_send_disconnect(&client->net, client->clientID);
        Thread_lock_mutex(socketMutex);
#if defined(OPENSSL)
        SSLSocket_close(&client->net);
#endif
        Socket_close(client->net.socket);
        Thread_unlock_mutex(socketMutex);
        client->net.socket = 0;
#if defined(OPENSSL)
        client->net.ssl = NULL;
#endif
    }
    client->connected = 0;
    client->connectState = 0;
}


static void MQTTAsync_closeSession(Clients* client) {
    MQTTAsync_closeOnly(client);

    if (client->cleanSession)
        MQTTAsync_cleanSession(client);
}


static int clientStructCompare(void* a, void* b) {
    MQTTAsyncClient* m = (MQTTAsyncClient*)a;
    return m->c == (Clients*)b;
}


static int MQTTAsync_cleanSession(Clients* client) {
    int rc = 0;
    ListElement* found = NULL;

#if !defined(NO_PERSISTENCE)
    rc = MQTTPersistence_clear(client);
#endif
    MQTTProtocol_emptyMessageList(client->inboundMsgs);
    MQTTProtocol_emptyMessageList(client->outboundMsgs);
    MQTTAsync_emptyMessageQueue(client);
    client->msgID = 0;

    if ((found = ListFindItem(handles, client, clientStructCompare)) != NULL)
    {
        MQTTAsyncClient* m = (MQTTAsyncClient*)(found->content);
        MQTTAsync_removeResponsesAndCommands(m);
    }
    else
        LOG("cleanSession: did not find client structure in handles list");
    return rc;
}

static int MQTTAsync_deliverMessage(MQTTAsyncClient* m, char* topicName, size_t topicLen, MQTTAsyncMessage* mm) {
    int rc;

    LOG("Calling messageArrived for client %s, queue depth %d",
        m->c->clientID, m->c->publishMessageQueue->count);
    rc = (*(m->ma))(m->context, topicName, (int)topicLen, mm);
    /* if 0 (false) is returned by the callback then it failed, so we don't remove the message from
     * the queue, and it will be retried later.  If 1 is returned then the message data may have been freed,
     * so we must be careful how we use it.
     */
    return rc;
}



void Protocol_processPublication(Publish* publish, Clients* client) {
    MQTTAsyncMessage* mm = NULL;
    int rc = 0;

    mm = malloc(sizeof(MQTTAsyncMessage));

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
        mm->dup = 0;
    } else {
        mm->dup = publish->header.bits.dup;
    }
    mm->msgId = publish->msgId;

    if (client->publishMessageQueue->count == 0 && client->connected) {
        ListElement* found = NULL;

        if ((found = ListFindItem(handles, client, clientStructCompare)) == NULL)
            LOG("processPublication: did not find client structure in handles list");
        else {
            MQTTAsyncClient* m = (MQTTAsyncClient*)(found->content);

            if (m->ma)
                rc = MQTTAsync_deliverMessage(m, publish->topic, publish->topicLen, mm);
        }
    }

    /* if message was not delivered, queue it up */
    if (rc == 0)  {
        qEntry* qe = malloc(sizeof(qEntry));
        qe->msg = mm;
        qe->topicName = publish->topic;
        qe->topicLen = publish->topicLen;
        ListAppend(client->publishMessageQueue, qe, sizeof(qe) + sizeof(mm) + mm->payloadLen + strlen(qe->topicName)+1);
#if !defined(NO_PERSISTENCE)
        if (client->persistence)
            MQTTPersistence_persistQueueEntry(client, (MQTTPersistence_qEntry*)qe);
#endif
    }
    publish->topic = NULL;
}


int MQTTAsync_connect(MQTTAsync handle, const MQTTAsyncConnectOptions* options) {
    MQTTAsyncClient* m = handle;
    int rc = MQTT_ASYNC_SUCCESS;
    MQTTAsync_queuedCommand* conn;

    if (options == NULL) {
        rc = MQTT_ASYNC_NULL_PARAMETER;
        goto exit;
    }

    if (strncmp(options->structId, "MQTC", 4) != 0 || options->structVersion < 0 || options->structVersion > 4) {
        rc = MQTT_ASYNC_BAD_STRUCTURE;
        goto exit;
    }

    if (options->will) {
        if (strncmp(options->will->structId, "MQTW", 4) != 0 || options->will->structVersion != 0) {
            rc = MQTT_ASYNC_BAD_STRUCTURE;
            goto exit;
        }

        if (options->will->qos < 0 || options->will->qos > 2) {
            rc = MQTT_ASYNC_BAD_QOS;
            goto exit;
        }
    }

    if (options->structVersion != 0 && options->ssl) {
        if (strncmp(options->ssl->structId, "MQTS", 4) != 0 || options->ssl->structVersion != 0) {
            rc = MQTT_ASYNC_BAD_STRUCTURE;
            goto exit;
        }
    }

    if ((options->username && !UTF8_validateString(options->username)) ||
            (options->password && !UTF8_validateString(options->password))) {
        rc = MQTT_ASYNC_BAD_UTF8_STRING;
        goto exit;
    }

    m->connectCommand.onSuccess = options->onSuccess;
    m->connectCommand.onFailure = options->onFailure;
    m->connectCommand.context = options->context;
    m->connectTimeout = options->connectTimeout;

    toStop = false;
    /* 开启发送消息线程*/
    if (sendThreadState != STARTING && sendThreadState != RUNNING) {
        MQTTAsync_lock_mutex(mqttAsyncMutex);
        sendThreadState = STARTING;
        Thread_start(MQTTAsync_sendThread, NULL);
        MQTTAsync_unlock_mutex(mqttAsyncMutex);
    }

    /* 开启接收消息线程*/
    if (receiveThreadState != STARTING && receiveThreadState != RUNNING) {
        MQTTAsync_lock_mutex(mqttAsyncMutex);
        receiveThreadState = STARTING;
        Thread_start(MQTTAsync_receiveThread, handle);
        MQTTAsync_unlock_mutex(mqttAsyncMutex);
    }

    m->c->keepAliveInterval = options->keepAliveInterval;
    m->c->cleanSession = options->cleanSession;
    m->c->maxInFlightMessages = options->maxInFlight;

    if (options->structVersion >= 3) {
        m->c->mqttVersion = options->mqttVersion;
    } else {
        m->c->mqttVersion = 0;
    }

    if (options->structVersion >= 4) {
        m->automaticReconnect = options->automaticReconnect;
        m->minRetryInterval = options->minRetryInterval;
        m->maxRetryInterval = options->maxRetryInterval;
    }

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
    if (m->c->sslopts) {
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

        free((void*)m->c->sslopts);
        m->c->sslopts = NULL;
    }

    if (options->structVersion != 0 && options->ssl) {
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
    m->shouldBeConnected = true;

    m->connectTimeout = options->connectTimeout;


    MQTTAsync_freeServerURIs(m);
    if (options->structVersion >= 2 && options->serverURICount > 0) {
        int i;

        m->serverURICount = options->serverURICount;
        m->serverURIs = malloc(options->serverURICount * sizeof(char*));
        for (i = 0; i < options->serverURICount; ++i)
            m->serverURIs[i] = MQTTStrdup(options->serverURIs[i]);
    }

    /* Add connect request to operation queue */
    conn = malloc(sizeof(MQTTAsync_queuedCommand));
    memset(conn, '\0', sizeof(MQTTAsync_queuedCommand));
    conn->client = m;
    if (options) {
        conn->command.onSuccess = options->onSuccess;
        conn->command.onFailure = options->onFailure;
        conn->command.context = options->context;
    }
    conn->command.type = CONNECT;
    conn->command.details.conn.currentURI = 0;
    rc = MQTTAsync_addCommand(conn, sizeof(conn));

exit:
    return rc;
}


static int MQTTAsync_disconnect1(MQTTAsync handle, const MQTTAsyncDisconnectOptions* options, int internal) {
    MQTTAsyncClient* m = handle;
    int rc = MQTT_ASYNC_SUCCESS;
    MQTTAsync_queuedCommand* dis;

    if (m == NULL || m->c == NULL) {
        rc = MQTT_ASYNC_FAILURE;
        goto exit;
    }
    if (!internal)
        m->shouldBeConnected = false;

    if (m->c->connected == 0) {
        rc = MQTT_ASYNC_DISCONNECTED;
        goto exit;
    }

    /* Add disconnect request to operation queue */
    dis = malloc(sizeof(MQTTAsync_queuedCommand));
    memset(dis, '\0', sizeof(MQTTAsync_queuedCommand));
    dis->client = m;
    if (options) {
        dis->command.onSuccess = options->onSuccess;
        dis->command.onFailure = options->onFailure;
        dis->command.context = options->context;
        dis->command.details.dis.timeout = options->timeout;
    }
    dis->command.type = DISCONNECT;
    dis->command.details.dis.internal = internal;
    rc = MQTTAsync_addCommand(dis, sizeof(dis));

exit:
    return rc;
}

static int MQTTAsync_disconnect_internal(MQTTAsync handle, int timeout) {
    MQTTAsyncDisconnectOptions options = MQTTAsyncDisconnectOptions_initializer;

    options.timeout = timeout;
    return MQTTAsync_disconnect1(handle, &options, 1);
}


void MQTTProtocol_closeSession(Clients* c, int sendwill) {
    MQTTAsync_disconnect_internal((MQTTAsync)c->context, 0);
}

int MQTTAsync_disconnect(MQTTAsync handle, const MQTTAsyncDisconnectOptions* options) {
    return MQTTAsync_disconnect1(handle, options, 0);
}

int MQTTAsync_isConnected(MQTTAsync handle) {
    MQTTAsyncClient* m = handle;
    int rc = 0;

    MQTTAsync_lock_mutex(mqttAsyncMutex);
    if (m && m->c)
        rc = m->c->connected;
    MQTTAsync_unlock_mutex(mqttAsyncMutex);
    return rc;
}


static int cmdMessageIDCompare(void* a, void* b) {
    MQTTAsync_queuedCommand* cmd = (MQTTAsync_queuedCommand*)a;
    return cmd->command.token == *(int*)b;
}

static int MQTTAsync_assignMsgId(MQTTAsyncClient* m) {
    int startMsgId = m->c->msgID;
    int msgId = startMsgId;
    pthread_t thread_id = 0;
    int locked = 0;

    /* We might be called in a callback. In which case, this mutex will be already locked. */
    thread_id = Thread_getId();
    if (thread_id != sendThreadId && thread_id != receiveThreadId)
    {
        MQTTAsync_lock_mutex(mqttAsyncMutex);
        locked = 1;
    }

    msgId = (msgId == MAX_MSG_ID) ? 1 : msgId + 1;
    while (ListFindItem(commands, &msgId, cmdMessageIDCompare) ||
           ListFindItem(m->responses, &msgId, cmdMessageIDCompare))
    {
        msgId = (msgId == MAX_MSG_ID) ? 1 : msgId + 1;
        if (msgId == startMsgId)
        { /* we've tried them all - none free */
            msgId = 0;
            break;
        }
    }
    if (msgId != 0)
        m->c->msgID = msgId;
    if (locked)
        MQTTAsync_unlock_mutex(mqttAsyncMutex);

    return msgId;
}

int MQTTAsync_subscribeMany(MQTTAsync handle, int count, char* const* topic, int* qos, MQTTAsyncResponseOptions* response) {
    MQTTAsyncClient* m = handle;
    int i = 0;
    int rc = MQTT_ASYNC_FAILURE;
    MQTTAsync_queuedCommand* sub;
    int msgId = 0;

    if (m == NULL || m->c == NULL) {
        rc = MQTT_ASYNC_FAILURE;
        goto exit;
    }

    if (m->c->connected == 0) {
        rc = MQTT_ASYNC_DISCONNECTED;
        goto exit;
    }

    for (i = 0; i < count; i++) {
        if (!UTF8_validateString(topic[i])) {
            rc = MQTT_ASYNC_BAD_UTF8_STRING;
            goto exit;
        }

        if (qos[i] < 0 || qos[i] > 2) {
            rc = MQTT_ASYNC_BAD_QOS;
            goto exit;
        }
    }

    if ((msgId = MQTTAsync_assignMsgId(m)) == 0) {
        rc = MQTT_ASYNC_NO_MORE_MSGIDS;
        goto exit;
    }


    sub = malloc(sizeof(MQTTAsync_queuedCommand));
    memset(sub, 0x0, sizeof(MQTTAsync_queuedCommand));
    sub->client = m;
    sub->command.token = msgId;

    if (response) {
        sub->command.onSuccess = response->onSuccess;
        sub->command.onFailure = response->onFailure;
        sub->command.context = response->context;
        response->token = sub->command.token;
    }

    sub->command.type = SUBSCRIBE;
    sub->command.details.sub.count = count;
    sub->command.details.sub.topics = malloc(sizeof(char*) * count);
    sub->command.details.sub.qoss = malloc(sizeof(int) * count);

    for (i = 0; i < count; i++) {
        sub->command.details.sub.topics[i] = MQTTStrdup(topic[i]);
        sub->command.details.sub.qoss[i] = qos[i];
    }

    rc = MQTTAsync_addCommand(sub, sizeof(sub));

exit:
    return rc;
}


int MQTTAsync_subscribe(MQTTAsync handle, const char* topic, int qos, MQTTAsyncResponseOptions* response) {
    int rc = 0;
    char* const topics[] = {(char*)topic};
    rc = MQTTAsync_subscribeMany(handle, 1, topics, &qos, response);
    return rc;
}


int MQTTAsync_unsubscribeMany(MQTTAsync handle, int count, char* const* topic, MQTTAsyncResponseOptions* response) {
    MQTTAsyncClient* m = handle;
    int i = 0;
    int rc = SOCKET_ERROR;
    MQTTAsync_queuedCommand* unsub;
    int msgId = 0;

    if (m == NULL || m->c == NULL) {
        rc = MQTT_ASYNC_FAILURE;
        goto exit;
    }

    if (m->c->connected == 0) {
        rc = MQTT_ASYNC_DISCONNECTED;
        goto exit;
    }

    for (i = 0; i < count; i++) {
        if (!UTF8_validateString(topic[i])) {
            rc = MQTT_ASYNC_BAD_UTF8_STRING;
            goto exit;
        }
    }

    if ((msgId = MQTTAsync_assignMsgId(m)) == 0) {
        rc = MQTT_ASYNC_NO_MORE_MSGIDS;
        goto exit;
    }

    unsub = malloc(sizeof(MQTTAsync_queuedCommand));
    memset(unsub, 0x0, sizeof(MQTTAsync_queuedCommand));
    unsub->client = m;
    unsub->command.type = UNSUBSCRIBE;
    unsub->command.token = msgId;
    if (response) {
        unsub->command.onSuccess = response->onSuccess;
        unsub->command.onFailure = response->onFailure;
        unsub->command.context = response->context;
        response->token = unsub->command.token;
    }
    unsub->command.details.unsub.count = count;
    unsub->command.details.unsub.topics = malloc(sizeof(char*) * count);

    for (i = 0; i < count; i++) {
        unsub->command.details.unsub.topics[i] = MQTTStrdup(topic[i]);
    }
    rc = MQTTAsync_addCommand(unsub, sizeof(unsub));

exit:
    return rc;

}


int MQTTAsync_unsubscribe(MQTTAsync handle, const char* topic, MQTTAsyncResponseOptions* response) {
    int rc = 0;
    char *const topics[] = {(char*)topic};
    rc = MQTTAsync_unsubscribeMany(handle, 1, topics, response);
    return rc;
}


static int MQTTAsync_countBufferedMessages(MQTTAsyncClient* m) {
    ListElement* current = NULL;
    int count = 0;

    while (ListNextElement(commands, &current)) {
        MQTTAsync_queuedCommand* cmd = (MQTTAsync_queuedCommand*)(current->content);

        if (cmd->client == m && cmd->command.type == PUBLISH)
            count++;
    }
    return count;
}


int MQTTAsync_send(MQTTAsync handle, const char* destinationName, int payloadlen, void* payload,
                   int qos, int retained, MQTTAsyncResponseOptions* response) {
    int rc = MQTT_ASYNC_SUCCESS;
    MQTTAsyncClient* m = handle;
    MQTTAsync_queuedCommand* pub;
    int msgid = 0;

    if (m == NULL || m->c == NULL)
        rc = MQTT_ASYNC_FAILURE;
    else if (m->c->connected == 0 &&
            (m->createOptions == NULL || m->createOptions->sendWhileDisconnected == 0 ||
                    m->shouldBeConnected == 0))
        rc = MQTT_ASYNC_DISCONNECTED;
    else if (!UTF8_validateString(destinationName))
        rc = MQTT_ASYNC_BAD_UTF8_STRING;
    else if (qos < 0 || qos > 2)
        rc = MQTT_ASYNC_BAD_QOS;
    else if (qos > 0 && (msgid = MQTTAsync_assignMsgId(m)) == 0)
        rc = MQTT_ASYNC_NO_MORE_MSGIDS;
    else if (m->createOptions && (MQTTAsync_countBufferedMessages(m) >= m->createOptions->maxBufferedMessages))
        rc = MQTT_ASYNC_MAX_BUFFERED_MESSAGES;

    if (rc != MQTT_ASYNC_SUCCESS)
        goto exit;

    /* Add publish request to operation queue */
    pub = malloc(sizeof(MQTTAsync_queuedCommand));
    memset(pub, '\0', sizeof(MQTTAsync_queuedCommand));
    pub->client = m;
    pub->command.type = PUBLISH;
    pub->command.token = msgid;
    if (response) {
        pub->command.onSuccess = response->onSuccess;
        pub->command.onFailure = response->onFailure;
        pub->command.context = response->context;
        response->token = pub->command.token;
    }
    pub->command.details.pub.destinationName = MQTTStrdup(destinationName);
    pub->command.details.pub.payloadLen = payloadlen;
    pub->command.details.pub.payload = malloc(payloadlen);
    memcpy(pub->command.details.pub.payload, payload, payloadlen);
    pub->command.details.pub.qos = qos;
    pub->command.details.pub.retained = retained;
    rc = MQTTAsync_addCommand(pub, sizeof(pub));

    exit:
    return rc;
}


int MQTTAsync_sendMessage(MQTTAsync handle, const char* destinationName, const MQTTAsyncMessage* message,
                          MQTTAsyncResponseOptions* response) {
    int rc = MQTT_ASYNC_SUCCESS;

    if (message == NULL) {
        rc = MQTT_ASYNC_NULL_PARAMETER;
        goto exit;
    }

    if (strncmp(message->structId, "MQTM", 4) != 0 || message->structVersion != 0) {
        rc = MQTT_ASYNC_BAD_STRUCTURE;
        goto exit;
    }

    rc = MQTTAsync_send(handle, destinationName, message->payloadLen, message->payload,
                        message->qos, message->retained, response);
exit:
    return rc;
}


static void MQTTAsync_retry(void) {
    static time_t last = 0L;
    time_t now;

    time(&(now));
    if (difftime(now, last) > 5) {
        time(&(last));
        MQTTProtocol_keepAlive(now);
        MQTTProtocol_retry(now, 1, 0);
    }
    else {
        MQTTProtocol_retry(now, 0, 0);
    }
}


static int MQTTAsync_connecting(MQTTAsyncClient* m) {
    int rc = -1;
    if (m->c->connectState == CONNECT_STATE_WAIT_FOR_TCP_COMPLETE) {
        int error;
        socklen_t len = sizeof(error);

        if ((rc = getsockopt(m->c->net.socket, SOL_SOCKET, SO_ERROR, (char*)&error, &len)) == 0) {
            rc = error;
        }

        if (rc != 0) {
            goto exit;
        }

        Socket_clearPendingWrite(m->c->net.socket);

#ifdef OPENSSL
       if (m->ssl) {
           int port;
           char* hostName;
           int setSocketForSSLRc = 0;

           hostName = MQTTProtocol_addressPort(m->serverURI, &port);
           setSocketForSSLRc = SSLSocket_setSocketForSSL(&m->c->net, m->c->sslopts, hostName);
           if (hostName != m->serverURI) {
               free(hostName);
           }

           if (setSocketForSSLRc != MQTT_ASYNC_SUCCESS) {
               if (m->c->session != NULL && ((rc = SSL_set_session(m->c->net.ssl, m->c->session)) != 1)) {
                   LOG("Failed to set SSL session with stored data, non critical");
               }
               rc = SSLSocket_connect(m->c->net.ssl, m->c->net.socket);

               if (rc == TCPSOCKET_INTERRUPTED) {
                   rc = MQTT_CLIENT_SUCCESS;
                   m->c->connectState = CONNECT_STATE_WAIT_FOR_SSL_COMPLETE;
               } else if (rc == SSL_FATAL) {
                   rc = SOCKET_ERROR;
                   goto exit;
               } else if (rc == 1) {
                   rc = MQTT_CLIENT_SUCCESS;
                   m->c->connectState = CONNECT_STATE_WAIT_FOR_CONNACK;

                   if (MQTTPacket_send_connect(m->c, m->connectCommand.details.conn.mqttVersion) == SOCKET_ERROR) {
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

           m->c->connectState = CONNECT_STATE_WAIT_FOR_CONNACK;
           if ((rc = MQTTPacket_send_connect(m->c, m->connectCommand.details.conn.mqttVersion)) ==
               SOCKET_ERROR) {
               goto exit;
           }
#ifdef OPENSSL
       }
#endif
    }

#ifdef OPENSSL
    else if (m->c->connectState == CONNECT_STATE_WAIT_FOR_SSL_COMPLETE) {
        if ((rc = SSLSocket_connect(m->c->net.ssl, m->c->net.socket)) != CONNECT_STATE_WAIT_FOR_TCP_COMPLETE) {
            goto exit;
        }

        if (!m->c->cleanSession && m->c->session == NULL) {
            m->c->session = SSL_get1_session(m->c->net.ssl);
        }
        m->c->connectState = CONNECT_STATE_WAIT_FOR_CONNACK;
        if ((rc = MQTTPacket_send_connect(m->c, m->connectCommand.details.conn.mqttVersion)) == SOCKET_ERROR) {
            goto exit;
        }
    }
#endif

exit:

    if ((rc != 0 && rc != TCPSOCKET_INTERRUPTED && m->c->connectState != CONNECT_STATE_WAIT_FOR_SSL_COMPLETE)
            || (rc == SSL_FATAL)) {
        if (MQTTAsync_checkConn(&m->connectCommand, m)) {
            MQTTAsync_queuedCommand* conn;
            MQTTAsync_closeOnly(m->c);

            conn = malloc(sizeof(MQTTAsync_queuedCommand));
            memset(conn, 0x00, sizeof(MQTTAsync_queuedCommand));
            conn->client = m;
            conn->command = m->connectCommand;
            LOG("Connect failed, more to try");
            MQTTAsync_addCommand(conn, sizeof(m->connectCommand));
        } else {
            MQTTAsync_closeSession(m->c);
            if (m->connectCommand.onFailure) {
                MQTTAsyncFailureData data;

                data.token = 0;
                data.code = MQTT_ASYNC_FAILURE;
                data.message = "TCP?TLS connect failure";
                LOG("Calling connect failure for client %s", m->c->clientID);
                (*(m->connectCommand.onFailure))(m->connectCommand.context, &data);
            }
            MQTTAsync_startConnectRetry(m);
        }
    }

    return rc;
}

static MQTTPacket* MQTTAsync_cycle(int* sock, unsigned long timeout, int* rc) {
    struct timeval tp = {0L, 0L};
    static Ack ack;
    MQTTPacket* packet = NULL;

    if (timeout > 0L) {
        tp.tv_sec = timeout / 1000;
        tp.tv_usec = (timeout % 1000) * 1000;
    }

#ifdef OPENSSL
    if ((*sock = SSLSocket_getPendingRead()) == -1) {
#endif
        Thread_lock_mutex(socketMutex);
        *sock = Socket_getReadySocket(0, &tp);
        Thread_unlock_mutex(socketMutex);
        if (!toStop && *sock == 0 && (tp.tv_sec > 0L || tp.tv_usec > 0L))
            MQTTAsync_sleep(100L);

#ifdef OPENSSL
    }
#endif
    MQTTAsync_lock_mutex(mqttAsyncMutex);
    if (*sock > 0) {
        MQTTAsyncClient* m = NULL;
        if (ListFindItem(handles, sock, clientSockCompare) != NULL)
            m = (MQTTAsyncClient*)(handles->current->content);

        if (m != NULL) {
            if (m->c->connectState == CONNECT_STATE_WAIT_FOR_TCP_COMPLETE ||
                    m->c->connectState == CONNECT_STATE_WAIT_FOR_SSL_COMPLETE) {
                *rc = MQTTAsync_connecting(m);
            } else {
                //从socket中读取一个MQTTPacket
                packet = MQTTPacket_Factory(&m->c->net, rc);
            }

            if (m->c->connectState == CONNECT_STATE_WAIT_FOR_CONNACK && *rc == SOCKET_ERROR) {
                LOG("CONNECT send but MQTTPacket_factory has return SOCKET_ERROR");
                if (MQTTAsync_checkConn(&m->connectCommand, m)) {
                    MQTTAsync_queuedCommand* conn;

                    MQTTAsync_closeOnly(m->c);
                    conn = malloc(sizeof(MQTTAsync_queuedCommand));
                    memset(conn, 0x00, sizeof(MQTTAsync_queuedCommand));
                    conn->client = m;
                    conn->command = m->connectCommand;
                    LOG("Connect failed, more to try");
                    MQTTAsync_addCommand(conn, sizeof(m->connectCommand));
                } else {
                    MQTTAsync_closeSession(m->c);
                    if (m->connectCommand.onFailure) {
                        MQTTAsyncFailureData data;
                        data.token = 0;
                        data.code = MQTT_ASYNC_FAILURE;
                        data.message = "TCP connect completion failure";
                        LOG("Calling connect failure for client %s", m->c->clientID);
                        (*(m->connectCommand.onFailure))(m->connectCommand.context, &data);
                    }
                    MQTTAsync_startConnectRetry(m);
                }
            }
        }

        if (packet) {
            int freed = 1;

            if (packet->header.bits.type == PUBLISH) {
                *rc = MQTTProtocol_handlePublishes(packet, *sock);
            }//PUBACK or PUBCOMP
            else if (packet->header.bits.type == PUBACK || packet->header.bits.type == PUBCOMP) {
                int msgId;

                ack = (packet->header.bits.type == PUBCOMP) ? *(Pubcomp*)packet : *(Puback*)packet;
                msgId = ack.msgId;

                *rc = (packet->header.bits.type == PUBCOMP) ?
                      MQTTProtocol_handlePubcomps(packet, *sock) : MQTTProtocol_handlePubacks(packet, *sock);

                if (!m)
                    LOG("PUBCOMP or PUBACK received for no client, msgId %d", msgId);
                else {
                    ListElement* current = NULL;
                    if (m->dc) {
                        LOG("Calling deliveryComplete for client %s, msgId %d", m->c->clientID, msgId);
                        (*(m->dc))(m->context, msgId);
                    }

                    while (ListNextElement(m->responses, &current)) {
                        MQTTAsync_queuedCommand* command = (MQTTAsync_queuedCommand*)(current->content);
                        if (command->command.token == msgId) {
                            if (!ListDetach(m->responses, command))
                                LOG("Publish command not removed from command list");

                            if (command->command.onSuccess) {
                                MQTTAsyncSuccessData data;

                                data.token = command->command.token;
                                data.alt.pub.destinationName = command->command.details.pub.destinationName;
                                data.alt.pub.message.payload = command->command.details.pub.payload;
                                data.alt.pub.message.payloadLen = command->command.details.pub.payloadLen;
                                data.alt.pub.message.qos = command->command.details.pub.qos;
                                data.alt.pub.message.retained = command->command.details.pub.retained;
                                LOG("Calling publish success for client %s", m->c->clientID);
                                (*(command->command.onSuccess))(command->command.context, &data);
                            }

                            MQTTAsync_freeCommand(command);
                            break;
                        }
                    }
                }
            } //PUBREC
            else if (packet->header.bits.type == PUBREC) {
                *rc = MQTTProtocol_handlePubrecs(packet, *sock);
            }
            // PUBREL
            else if (packet->header.bits.type == PUBREL) {
                *rc = MQTTProtocol_handlePubrels(packet, *sock);
            }
            // PINGRESP
            else if (packet->header.bits.type == PINGRESP) {
                *rc = MQTTProtocol_handlePingresps(packet, *sock);
            }
            else
                freed = 0;

            if (freed)
                packet = NULL;
        }
    }
    MQTTAsync_retry();
    MQTTAsync_unlock_mutex(mqttAsyncMutex);
    return packet;
}


static int pubCompare(void* a, void* b) {
    Messages* msg = (Messages*)a;
    return msg->publish == (Publications*)b;
}

int MQTTAsync_getPendingTokens(MQTTAsync handle, MQTTAsyncToken **tokens) {
    int rc = MQTT_ASYNC_SUCCESS;
    MQTTAsyncClient* m = handle;
    ListElement* current = NULL;
    int count = 0;

    MQTTAsync_lock_mutex(mqttAsyncMutex);
    *tokens = NULL;

    if (m == NULL)
    {
        rc = MQTT_ASYNC_FAILURE;
        goto exit;
    }

    /* calculate the number of pending tokens - commands plus inflight */
    while (ListNextElement(commands, &current))
    {
        MQTTAsync_queuedCommand* cmd = (MQTTAsync_queuedCommand*)(current->content);

        if (cmd->client == m)
            count++;
    }
    if (m->c)
        count += m->c->outboundMsgs->count;
    if (count == 0)
        goto exit; /* no tokens to return */
    *tokens = malloc(sizeof(MQTTAsyncToken) * (count + 1));  /* add space for sentinel at end of list */

    /* First add the unprocessed commands to the pending tokens */
    current = NULL;
    count = 0;
    while (ListNextElement(commands, &current))
    {
        MQTTAsync_queuedCommand* cmd = (MQTTAsync_queuedCommand*)(current->content);

        if (cmd->client == m)
            (*tokens)[count++] = cmd->command.token;
    }

    /* Now add the in-flight messages */
    if (m->c && m->c->outboundMsgs->count > 0)
    {
        current = NULL;
        while (ListNextElement(m->c->outboundMsgs, &current))
        {
            Messages* m = (Messages*)(current->content);
            (*tokens)[count++] = m->msgId;
        }
    }
    (*tokens)[count] = -1; /* indicate end of list */

    exit:
    MQTTAsync_unlock_mutex(mqttAsyncMutex);
    return rc;
}


/**
 * 查看是否完成
 * @param handle
 * @param dt
 * @return
 */
int MQTTAsync_isComplete(MQTTAsync handle, MQTTAsyncToken dt) {
    int rc = MQTT_ASYNC_SUCCESS;
    MQTTAsyncClient* m = handle;
    ListElement* current = NULL;

    MQTTAsync_lock_mutex(mqttAsyncMutex);

    if (m == NULL) {
        rc = MQTT_ASYNC_FAILURE;
        goto exit;
    }

    /* First check unprocessed commands */
    current = NULL;
    while (ListNextElement(commands, &current)) {
        MQTTAsync_queuedCommand* cmd = (MQTTAsync_queuedCommand*)(current->content);

        if (cmd->client == m && cmd->command.token == dt) {
            goto exit;
        }
    }

    /* Now check the in-flight messages */
    if (m->c && m->c->outboundMsgs->count > 0) {
        current = NULL;
        while (ListNextElement(m->c->outboundMsgs, &current)) {
            Messages* m = (Messages*)(current->content);
            if (m->msgId == dt) {
                goto exit;
            }
        }
    }
    rc = MQTT_ASYNC_TRUE; /* Can't find it, so it must be complete */

    exit:
    MQTTAsync_unlock_mutex(mqttAsyncMutex);
    return rc;
}


/**
 * 等待所有操作完成
 * @param handle
 * @param dt
 * @param timeout
 * @return
 */
int MQTTAsync_waitForCompletion(MQTTAsync handle, MQTTAsyncToken dt, unsigned long timeout) {
    int rc = MQTT_ASYNC_FAILURE;
    struct timeval start = MQTTAsync_start_clock();
    unsigned long elapsed = 0L;
    MQTTAsyncClient* m = handle;

    MQTTAsync_lock_mutex(mqttAsyncMutex);

    if (m == NULL || m->c == NULL) {
        rc = MQTT_ASYNC_FAILURE;
        goto exit;
    }
    if (m->c->connected == 0) {
        rc = MQTT_ASYNC_DISCONNECTED;
        goto exit;
    }
    MQTTAsync_unlock_mutex(mqttAsyncMutex);

    if (MQTTAsync_isComplete(handle, dt) == 1) {
        rc = MQTT_ASYNC_SUCCESS; /* well we couldn't find it */
        goto exit;
    }

    elapsed = MQTTAsync_elapsed(start);
    while (elapsed < timeout) {
        MQTTAsync_sleep(100);
        if (MQTTAsync_isComplete(handle, dt) == 1) {
            rc = MQTT_ASYNC_SUCCESS; /* well we couldn't find it */
            goto exit;
        }
        elapsed = MQTTAsync_elapsed(start);
    }

exit:
    return rc;
}
