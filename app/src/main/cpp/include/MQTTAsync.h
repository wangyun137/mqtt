#ifndef DIM_MQTT_ASYNC_H_
#define DIM_MQTT_ASYNC_H_

#if defined(__cplusplus)
  extern "C" {
#endif

#include <stdio.h>

/**
* 返回值, 指明关于MQTT client的操作成功
*/
#define MQTT_ASYNC_SUCCESS 0

/**
* 返回值, 指明关于MQTT client的操作失败
*/
#define MQTT_ASYNC_FAILURE -1

/**
* MQTT persistence error
*/
#define MQTT_ASYNC_PERSISTENCE_ERROR -2

/**
* 返回值, 指明client断开连接
*/
#define MQTT_ASYNC_DISCONNECTED -3

/**
* 返回值, 指明已经到达同时请求的最大允许数
*/
#define MQTT_ASYNC_MAX_MESSAGES_INFLIGHT -4

/**
* 返回值, 检测到无效的utf-8字符串
*/
#define MQTT_ASYNC_BAD_UTF8_STRING -5

/**
* 返回值, 传递了一个空参数
*/
#define MQTT_ASYNC_NULL_PARAMETER -6

/**
* 返回值, topic不完整，服务端的string函数无法完全获取topic
*/
#define MQTT_ASYNC_TOPICNAME_TRUNCATED -7

/**
* 返回值, 错误的结构体参数, 结构体中的struct_id和structVersion不对
*/
#define MQTT_ASYNC_BAD_STRUCTURE -8

/**
* 返回值, qos的值不是0, 1, 2
*/
#define MQTT_ASYNC_BAD_QOS -9

/**
* 返回值, 所有65535个msgIds都被使用了
*/
#define MQTT_ASYNC_NO_MORE_MSGIDS -10

/**
* 返回值, 请求在没完成之前就被废弃
*/
#define MQTT_ASYNC_OPERATION_INCOMPLETE -11

/**
* 返回值, 没有更多的message可以被缓存了
*/
#define MQTT_ASYNC_MAX_BUFFERED_MESSAGES -12

/**
* 默认的MQTT版本号
*/
#define MQTT_VERSION_DEFAULT 0

/**
* MQTT 3.1 版本
*/
#define MQTT_VERSION_3_1 3

/**
* MQTT 3.1.1 版本
*/
#define MQTT_VERSION_3_1_1 4

/**
* 3.1.1版本中subscribe的错误返回值
*/
#define MQTT_BAD_SUBSCRIBE 0x80


/**
* MQTT客户端的句柄, 当MQTTAsync_create()成功之后会有一个有效的句柄
*/
typedef void* MQTTAsync;

/**
* 当客户端publish一个消息后会返回一个token，可以用这个token检查message是否成功传送
*/
typedef int MQTTAsyncToken;

/**
* 定义Message结构体，包含payload以及其他属性， topic属性并不包含在这个结构体中
*/
typedef struct {
    /** 标识符, 必须是MQTM*/
    char structId[4];
    /** 结构体的版本号，必须是0*/
    int structVersion;
    /** payload的字节长度*/
    int payloadLen;
    /** 指向payload的指针*/
    void* payload;
    /**
    * QoS值,可以有以下三种取值
    * QoS0 - 消息可能不会被传送
    * QoS1 - 消息至少被传送一次，但也可能会被传送多次
    */
    int qos;
    /**
    * retained = true
    * 当publish消息时，指明MQTT服务端应当retain一份message的拷贝, 消息将会被传输至匹配的sunscribers
    * 当subscribe消息时，表明收到的消息并不是新的，MQTT服务器已经retain了该消息
    *
    * retained = false
    * publish消息时，指明MQTT服务器不应该retain该消息
    * subscribe时，指明这是一条普通消息，即publisher发布到服务端的消息
    */
    int retained;
    /**
    * 指明该消息是否是复制的，只有在QoS1时才有意义，当设为true时，客户端需要考虑如何正确处理
    * 这条复制消息
    */
    int dup;
    /**
    * 消息标识
    */
    int msgId;
} MQTTAsyncMessage;

#define MQTTAsyncMessage_initializer { {'M', 'Q', 'T', 'M'}, 0, 0, NULL, 0, 0, 0, 0}


/**
* 消息到达时的回调函数, 客户端通过MQTTAsync_setCallbacks()设置该回调函数，该函数在一个独立的线程中执行
* @param context 指向在MQTTAsync_setCallbacks中传入的context
* @param topicName 到达消息的topicName
* @param topicLen topicName的长度，除非有多个NULL字符在topicName中，否则topicLen = 0, 如果topicLen = 0,
*                 可以用strlen(topicName)来获取可信任的topicName长度
* @param message 以MQTTAsyncMessage结构体表示的消息
* @return 返回true or false, 指明消息是否已安全地被客户端程序接收, 返回true, 表明消息已经被成功接收并处理，
*         返回false, 表明遇到问题, 此时会试图重新调用MQTTAsyncMessageArrived函数来重新传输消息
*/
typedef int MQTTAsync_onMessageArrived(void* context, char* topicName, int topicLen, MQTTAsyncMessage* message);


/**
* 客户端publish消息后当消息传输完成的回调, 通过MQTTAsync_setCallbacks()设置，该函数在一个独立的线程中执行
* 该函数表明了通信必须的握手响应操作已经完成
* @param context 指向在MQTTAsync_setCallbacks中传入的context
* @param token MQTTAsyncToken同published消息绑定，客户端可以通过比对在MQTTAsync_send()以及MQTTAsync_sendMessage()
*              中传入的token和在该函数中收到的token是否一致来检查消息是否正确
*/
typedef void MQTTAsync_onDeliveryComplete(void* context, MQTTAsyncToken token);


/**
* 连接中断时的回调函数, 通过MQTTAsync_setCallbacks()设置，该函数在一个独立的线程执行
* @param context 指向在MQTTAsync_setCallbacks中传入的context
* @param cause 连接中断的原因，目前版本，一直设为NULL
*/
typedef void MQTTAsync_onConnectionLost(void* context, char* cause);

/**
* 当MQTTAsync_connect连接成功时的回调，因为有onSuccess函数，该函数的功能有点多余，不过
* 如果允许自动重连，且重连成功时，可以通过该函数获取连接成功的通知
* @param context 指向在MQTTAsync_setCallbacks中传入的context
* @param cause 断连的原因，目前版本，一直设为NULL
*/
typedef void MQTTAsync_onConnected(void* context, char* cause);

/** onFailure中返回的失败请求数据结构*/
typedef struct {
    /** 标识失败请求的token*/
    MQTTAsyncToken   token;
    /** 错误码*/
    int               code;
    /** 可选的错误原因*/
    const char*       message;
} MQTTAsyncFailureData;

/** onSuccess中返回的成功请求的数据结构*/
typedef struct {
    /** 成功请求的token */
    MQTTAsyncToken token;
    /** subscribe, unsubscribe, publish共用的联合体, 可以用来表示不同类型请求的不同数据*/
    union {
        /** subscribe操作， 服务端返回的QoS*/
        int     qos;
        /** subscribeMany, 服务端返回的QoS列表*/
        int*    qosList;
        /** publish操作, 发给服务端的消息*/
        struct {
            MQTTAsyncMessage   message;
            char*               destinationName;
        } pub;
        /** connect操作*/
        struct {
            char* serverURI;
            int mqttVersion;
            int sessionPresent;
        } connect;
    } alt;
} MQTTAsyncSuccessData;

/**
* 任何请求成功的回调函数，在MQTTAsyncResponseOptions中设置该函数
* @param context 指向在MQTTAsyncResponseOptions中传入的context
* @param response MQTTAsyncSuccessData类型数据，包含操作成功以后的数据
*/
typedef void MQTTAsync_onSuccess(void* context, MQTTAsyncSuccessData* response);


/**
* 任何请求失败的回调函数, 在MQTTAsyncResponseOptions中设置该函数
* @param context 指向在MQTTAsyncResponseOptions中传入的context
* @param response MQTTAsync_failureData类型数据, 包含操作失败以后的数据
*/
typedef void MQTTAsync_onFailure(void* context, MQTTAsyncFailureData* response);

typedef struct {
    /** 当前结构体的标识符, 必须是MQTR*/
    char   structId[4];
    /** 当前结构体的版本号, 必须是0*/
    int struct_version;
    /** 请求成功时的回调函数*/
    MQTTAsync_onSuccess* onSuccess;
    /** 请求失败时的回调函数*/
    MQTTAsync_onFailure* onFailure;
    /** 指向客户端提供的context*/
    void* context;
    MQTTAsyncToken token;
} MQTTAsyncResponseOptions;


#define MQTTAsyncResponseOptions_initializer {{'M', 'Q', 'T', 'R'}, 0, NULL, NULL, 0 , 0}


/**
* 设置全局回调函数, 如果不需要某个特定的回调函数，可以设置为NULL
* @param handle 通过MQTTAsync_create创建的handle
* @param context 指向应用程序指定的context
* @param cl 指向MQTTAsync_connectionLost函数
* @param ma 指向MQTTAsync_messageArrived函数
* @param dc 指向MQTTAsync_deliveryComplete函数
* @return MQTT_ASYNC_SUCCESS: callback设置成功
*         MQTT_ASYNC_FAILURE: 错误发生
*/
int MQTTAsync_setCallbacks(MQTTAsync handle, void* context, MQTTAsync_onConnectionLost* cl,
                                      MQTTAsync_onMessageArrived* ma, MQTTAsync_onDeliveryComplete* dc);


/**
* 设置MQTTAsync_connected回调函数
* @param handle 通过MQTTAsync_create创建的handle
* @param context 指向应用程序指定的context
* @param co 指向MQTTAsync_connected函数
* @return MQTT_ASYNC_SUCCESS: callback设置成功
*         MQTT_ASYNC_FAILURE: 错误发生
*/
int MQTTAsync_setConnected(MQTTAsync handle, void* context, MQTTAsync_onConnected* co);

/**
* 使用之前的连接选项重新连接
* @param handle 通过MQTTAsync_create创建的handle
* @return MQTT_ASYNC_SUCCESS: callback设置成功
*         MQTT_ASYNC_FAILURE: 错误发生
*/
int MQTTAsync_reconnect(MQTTAsync handle);

/**
* 根据指定的服务端地址，指定的Persistent创建一个MQTT client
* @param 要创建的MQTTAsync
* @param serverURI 服务端地址，标准格式为protocol://host:port, protocol必须是tcp或者ss,
*                  host可以是ip或者域名
* @param clientId  客户端标识符，必须是utf-8格式的字符串
* @param persistence_type 客户端使用的persistent类型
*                  MQTT_CLIENT_PERSISTENCE_NONE: 内存存储，如果设备关闭，当前正在传输的消息将丢失且部分QoS1,QoS2的
*                  消息不会被传输;
*                  MQTT_CLIENT_PERSISTENCE_DEFAULT: 使用文件存储，对消息提供保护机制
*                  MQTT_CLIENT_PERSISTENCE_USER: 使用应用指定的persistence实现，应用必须实现MQTTClent_persistence接口
* @param persistence_context 如果persistence_type设为MQTTCLIENT_PERSISTENCE_DEFAULT, 该参数需要设为persistence目录地址,
*                              如果设为NULL，则默认为当前工作目录
*                  如果persistence_type设为MQTTCLIENT_PERSISTENCE_USER,则该参数指向一个MQTTClent_persistence结构体
* @return MQTT_ASYNC_SUCCESS: 创建成功
*         MQTT_ASYNC_FAILURE: 错误发生
*/
int MQTTAsync_create(MQTTAsync* handle, const char* serverURI, const char* clientId,
                            int persistence_type, void* persistence_context);

typedef struct {
    /** 标识符, 必须是MQCO*/
    const char structId[4];
    /** 结构体的版本号，必须是0*/
    int structVersion;
    /** 当并未连接时是否允许发送消息*/
    int sendWhileDisconnected;
    /** 当并未连接时允许的最大缓存消息数*/
    int maxBufferedMessages;
} MQTTAsyncCreateOptions;

#define MQTTAsyncCreateOptions_initializer { {'M', 'Q', 'C', 'O'}, 0, 0, 100}

int MQTTAsync_createWithOptions(MQTTAsync* handle, const char* serverURI, const char* clientID, int persistence_type,
                                    void* persistence_context, MQTTAsyncCreateOptions* options);


/**
* Last Will and Testament 遗言机制对应的结构体, client在连接server时，如果指定了LWT，
* 当client同server的连接在不正常的情况下断开时，server会publish LWT消息给订阅了LWT topic的client
* 从而让其他client知道这个client已经断连
*/
typedef struct {
    /** 结构体标识符, 必须是 MQTW*/
    const char structId[4];
    /** 结构体版本号， 必须是0*/
    int structVersion;
    /** LWT topic*/
    const char* topicName;
    /** LWT payload*/
    const char* message;
    /** retain flag*/
    int retained;
    /** LWT qos*/
    int qos;
} MQTTAsyncWillOptions;

#define MQTTAsyncWillOptions_initializer {{'M', 'Q', 'T', 'W'}, 0 , NULL, NULL, 0, 0 }

/**
* 定义SSL/TLS相关的选项，包含以下方面内容:
* - 服务端认证: 客户端需要验证服务端数字证书
* - 相互认证: 在SSL握手过程中客户端和服务端相互认证
* - 匿名连接: 客户端和服务端在进行SSL连接时都不需要进行认证
*/
typedef struct {
    /** 结构体标识符，必须是MQTS*/
    const char structId[4];
    /** 结构体版本号， 必须是0*/
    int structVersion;
    /** 存放客户端信任的数字证书的PEM格式文件*/
    const char* trustStore;
    /** 存放客户端证书链的PEM格式文件, 同样可以包含客户端private key*/
    const char* keyStore;
    /** 存放客户端private key的PEM格式文件*/
    const char* privateKey;
    /** 如果private key被加密，解密的密钥*/
    const char* privateKeyPassword;
    /** SSL握手期间，客户端需要呈现给服务端的cipher suites,
    * 如果被设置，则默认值为"ALL"-即所有cipher suites，除了不提供加密功能的
    * 对于匿名连接，也可以被设置为"aNULL"
    */
    const char* enabledCipherSuites;

    int enableServerCertAuth;
} MQTTAsyncSSLOptions;

#define MQTTAsyncSSLOptions_initializer {{'M', 'Q', 'T', 'S'}, 0, NULL, NULL, NULL, NULL, NULL, 1}

/**
* MQTTAsyncConnectOptions定义了同server连接时一些设置
*/
typedef struct {
    /** 结构体标识符, 必须是MQTC*/
    const char structId[4];
    /** 结构体版本号，必须是0,1,2,3,4
    * 0 - 没有SSL配置，没有serverURIs
    * 1 - 没有serverURIs
    * 2 - 没有mqttVersion
    * 3 - 没有自动重连选项
    */
    int structVersion;
    /**
    * 秒为单位, 当server超过该值设置的时间还没收到client消息，则server设为
    * 与client之间断连. client在每个keepAliveInterval周期内都会确保至少
    * 有一条消息到达服务端
    * 如果不想有keep alive，则设为0
    */
    int keepAliveInterval;
    /** boolean值(0或1), client和server同时持有session状态信息，这个信息用来
    * 确保"at least once", "exactly once"传送， 以及"exactly once"接收
    * 当设为true时， 在连接以及断连时，session状态信息被被废弃; 设为false时,
    * session状态信息会被保留.
    * 当client使用MQTTAsync_connect()连接server时，server会检查是否有之前连接
    * 的session信息，如果存在, 其cleansession = true, 之前的session信息会被清除
    * 如果cleansession = false, 则保留之前的session信息，如果没有之前的session信息
    * 新创建一个session信息
    */
    int cleanSession;
    /** 同时发送消息的最大值*/
    int maxInFlight;
    /** LWT选项*/
    MQTTAsyncWillOptions* will;
    /** username参数*/
    const char* username;
    /** password参数*/
    const char* password;
    /** connect超时时长*/
    int connectTimeout;
    /** 重试的间隔时长*/
    int retryInterval;
    /** SSL选项*/
    MQTTAsyncSSLOptions* ssl;
    /** 连接成功后的回调函数*/
    MQTTAsync_onSuccess* onSuccess;
    /** 连接失败后的回调函数*/
    MQTTAsync_onFailure* onFailure;
    /** 应用上下文*/
    void* context;
    /** serverURIs数组的长度*/
    int serverURICount;
    /** server地址数组*/
    char* const* serverURIs;
    /** 设置connect时MQTT协议的版本号
    * MQTT_VERSION_DEFAULT(0): 使用3.1.1，如果失败，回滚到3.1
    * MQTT_VERSION_3_1(3): 只使用3.1版本
    * MQTT_VERSION_3_1_1(4):只使用3.1.1版本
    */
    int mqttVersion;
    /** 当连接断开时是否自动重连*/
    int automaticReconnect;
    /** 最小重试间隔，秒为单位*/
    int minRetryInterval;
    /** 最大重试间隔，秒为单位*/
    int maxRetryInterval;
} MQTTAsyncConnectOptions;

#define MQTTAsyncConnectOptions_initializer { {'M', 'Q', 'T', 'C'}, 4, 60, 1, 10, NULL, NULL, NULL, 30, 0,\
NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 1, 60}


/**
* 将通过MQTTAsync_create创建的client同server进行连接，如果享受到异步消息，
* 必须在MQTTAsync_connect之前调用MQTTAsync_setCallbacks
* @param handle 通过MQTTAsync_create创建的client
* @param options MQTTAsyncConnectOptions指针
* @return MQTT_ASYNC_SUCCESS: 如果连接请求被接受
*         如果client不能够连接server，则会回调onFailure()
* 错误码:
* 1: 错误的protocol
* 2: Identifier refused
* 3: server不可用
* 4: 错误的username或password
* 5: 未授权
*/
int MQTTAsync_connect(MQTTAsync handle, const MQTTAsyncConnectOptions* options);

/**
* 断开连接时的选项参数
*/
typedef struct {
    /** 结构体标识符, 必须是MQTD*/
    const char structId[4];
    /** 结构体版本号, 必须是0或1， 0 - 没有SSL选项*/
    int structVersion;
    /** 微秒为单位，client会在大于等于这个时间内确保当前in-flight消息完成后再断开连接*/
    int timeout;
    /** 成功回调函数*/
    MQTTAsync_onSuccess* onSuccess;
    /** 失败回调函数*/
    MQTTAsync_onFailure* onFailure;
    /** 应用上下文*/
    void* context;
} MQTTAsyncDisconnectOptions;

#define MQTTAsyncDisconnectOptions_initializer { {'M', 'Q', 'T', 'D'}, 0, 0, NULL, NULL, NULL }

/**
* 断开client同server之间的连接, 为了确保在断开连接之前所有in-flight消息能够完成，会指定一个timeout,
* 当超过这个timeout时间以后，即使还有message响应client都会断开连接, 当下次client和同一个server再次进行连接时
* 无论QoS1还是QoS2的未完成的消息都会很具cleansession值来确定是否重试
* @param handle 通过MQTTAsync_create创建的client
* @param options MQTTAsyncDisconnectOptions指针
*/
int MQTTAsync_disconnect(MQTTAsync handle, const MQTTAsyncDisconnectOptions* options);

/**
* 判断client是否连接
* @param handle 通过MQTTAsync_create创建的client
*/
int MQTTAsync_isConnected(MQTTAsync handle);

/**
* subscribe操作
* @param handle 通过MQTTAsync_create创建的client
* @param topic 主题，其中可以包含通配符
* @param qos qos值
* @param response MQTTAsyncResponseOptions指针
* @return MQTT_ASYNC_SUCCESS: subcribe请求成功
*         错误码: 请求失败
*/
int MQTTAsync_subscribe(MQTTAsync handle, const char* topic, int qos, MQTTAsyncResponseOptions* response);

/**
* subscribe多个topic
* @param handle 通过MQTTAsync_create创建的client
* @param count 主题数量
* @param qos qos数组, qos[n]对应topic[n]
* @param response MQTTAsyncResponseOptions指针
* @return MQTT_ASYNC_SUCCESS: subscribe操作成功
*         错误码: 请求失败
*/
int MQTTAsync_subscribeMany(MQTTAsync handle, int count, char* const* topic, int* qos, MQTTAsyncResponseOptions* response);


/**
* unsubscribe操作
* @param handle 通过MQTTAsync_create创建的client
* @param topic 主题
* @param response MQTTAsyncResponseOptions指针
* @param response MQTTAsyncResponseOptions指针
* @return MQTT_ASYNC_SUCCESS: unsubscribe操作成功
*         错误码: 请求失败
*/
int MQTTAsync_unsubscribe(MQTTAsync handle, const char* topic, MQTTAsyncResponseOptions* response);


/**
* unsubscribe多条topic
* @param handle 通过MQTTAsync_create创建的client
* @param count 主题数量
* @param topic topic数组
* @return MQTT_ASYNC_SUCCESS: unsubscribe操作成功
*         错误码: 请求失败
*/
int MQTTAsync_unsubscribeMany(MQTTAsync handle, int count, char* const* topic, MQTTAsyncResponseOptions* options);

/**
* publish消息
* 如果client需要检查消息是否传输成功，需要设置MQTTAsync_onSuccess和MQTTAsync_deliveryComplete
* 两个回调函数
* @param handle 通过MQTTAsync_create创建的client
* @param destinationName topic主题
* @param payloadLen payload的长度
* @param payload byte array指针
* @param qos QoS值
* @param retained
* @param response MQTTAsyncResponseOptions指针
* @return MQTT_ASYNC_SUCCESS: unsubscribe操作成功
*         错误码: 请求失败
*/
int MQTTAsync_send(MQTTAsync handle, const char* destinationName, int payloadLen, void* payload, int qos, int retained,
                                MQTTAsyncResponseOptions* response);

/**
* publish消息
* 如果client需要检查消息是否传输成功，需要设置MQTTAsync_onSuccess和MQTTAsync_deliveryComplete回调
* @param handle 通过MQTTAsync_create创建的client
* @param destinationName topic主题
* @param msg MQTTAsync_message指针
* @param response MQTTAsyncResponseOptions指针
* @return MQTT_ASYNC_SUCCESS: unsubscribe操作成功
*         错误码: 请求失败
*/
int MQTTAsync_sendMessage(MQTTAsync handle, const char* destinationName, const MQTTAsyncMessage* msg,
                                MQTTAsyncResponseOptions* response);


/**
* 获取正在传输中的message集合
* NOTE: tokens是通过malloc()分配的, 客户端应当在不使用时释放内存
* @param handle 通过MQTTAsync_create创建的client
* @param tokens 当函数成功执行，该指针会指向正在传输中的message的token数组
* @return MQTT_ASYNC_SUCCESS: unsubscribe操作成功
*         错误码: 请求失败
*/
int MQTTAsync_getPendingTokens(MQTTAsync handle, MQTTAsyncToken **tokens);

/**
* token相对应的请求是否已经完成
* @param handle 通过MQTTAsync_create创建的client
* @param token 请求相关的token
*/
#define MQTT_ASYNC_TRUE 1
int MQTTAsync_isComplete(MQTTAsync handle, MQTTAsyncToken token);

/**
* 一直等token相关的请求完成
* @param handle 通过MQTTAsync_create创建的client
* @param token 请求相关的token
* @param timeout 微秒为单位, 等待的超时时长
*/
int MQTTAsync_waitForCompletion(MQTTAsync handle, MQTTAsyncToken token, unsigned long timeout);


/**
* 释放message的内存
* NOTE: 该函数并不是放topic string的内存, 应用程序应当使用MQTTAsync_free()来释放
* @param msg 要释放的MQTTAsync_message
*/
void MQTTAsync_freeMessage(MQTTAsyncMessage** msg);

/**
* 释放内存，尤其是topic string
*/
void MQTTAsync_free(void* ptr);

/**
* 释放由MQTTAsync_create()分配的所有内存
* @param handle 通过MQTTAsync_create创建的client
*/
void MQTTAsync_destroy(MQTTAsync* handle);


typedef struct {
	const char* name;
	const char* value;
} MQTTAsync_nameValue;


#ifdef __cplusplus
    }
#endif

#endif //DIM_MQTT_ASYNC_H_
