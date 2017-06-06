#ifdef __cplusplus
extern "C" {
#endif

#ifndef DIM_MQTT_CLIENT_H_
#define DIM_MQTT_CLIENT_H_

#include <stdio.h>

#ifndef NO_PERSISTENCE
//#include "MQTTPersistence.h"
#endif

/* 表明MQTT client相关操作成功*/
#define MQTT_CLIENT_SUCCESS 0

/* 表明MQTT client相关操作失败*/
#define MQTT_CLIENT_FAILURE -1

/* 表明客户端是断开连接的*/
#define MQTT_CLIENT_DISCONNECTED -3

/* 表明当前以达到in-flight最大上限*/
#define MQTT_CLIENT_MAX_MESSAGES_INFLIGHT -4


/* 表明有错误格式的UTF-8字符串*/
#define MQTT_CLIENT_BAD_UTF8_STRING -5

/* 表明有值为空的参数*/
#define MQTT_CLIENT_NULL_PARAMETER -6

/* 指明topic字符串有错误，字符串函数无法完整的获取topic*/
#define MQTT_CLIENT_TOPIC_NAME_TRUNCATED -7

/* 指明结构体参数的eyeCatcher或版本号错误*/
#define MQTT_CLIENT_BAD_STRUCTURE -8

/* 指明QoS值不在正确的范围内(0,1,2)*/
#define MQTT_CLIENT_BAD_QOS -9

/* 默认的MQTT版本号*/
#define MQTT_VERSION_DEFAULT 0

/* 3.1版本*/
#define MQTT_VERSION_3_1 3

/* 3.1.1版本*/
#define MQTT_VERSION_3_1_1 4

/* 由3.1.1版本文档指定的SUBSCRIBE返回的错误码 */
#define MQTT_BAD_SUBSCRIBE 0x80


typedef void* MQTTClient;

/* 当一个消息被published以后会返回一个delivery token，可以用这个token检查消息是否到达*/
typedef int MQTTClient_deliveryToken;

typedef int MQTTClient_token;

/**
 * 定义了MQTT消息数据结构
 */
typedef struct
{
    /** 结构体标识符. 必须是MQTM. */
    char structId[4];
    /** 结构体版本号. 必须是0 */
    int structVersion;
    /** payload长度 */
    int payloadLen;
    /** 指向payload的指针 */
    void* payload;
    /**
     * QoS取值有以下三种
     * QoS0:不确保消息一定会传输到
     * QoS1:消息至少传输一次，有可能会多次
     * QoS2:只有一次，确保消息的传输接收只有一次
     */
    int qos;
    /**
     * 如果消息用于publish，当retained = true时, 指明server应当保留一份消息的副本, 该消息
     * 会被传送到订阅该主题的订阅者那里
     * 如果是Subscribers接收到的消息，当retained = true时，指明该消息是server保留的拷贝
     */
    int retained;
    /**
      * 只有在QoS=1时才起作用，指明该消息是否是副本
      */
    int dup;
    /**
      * 消息标识符
      */
    int msgId;
} MQTTClient_message;

#define MQTTClient_message_initializer { {'M', 'Q', 'T', 'M'}, 0, 0, NULL, 0, 0, 0, 0 }

/**
 * 消息到达时的回调函数，该函数在一个独立的线程中执行
 */
typedef int MQTTClient_messageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message);

/**
 * 消息到达时的回调函数，当成功publish消息到server时，会回调这个函数，表明由qos指定的握手行为已经完成
 */
typedef void MQTTClient_deliveryComplete(void* context, MQTTClient_deliveryToken dt);

/**
 * 连接中断时的回调函数,这个函数在一个独立的线程中执行
 */
typedef void MQTTClient_connectionLost(void* context, char* cause);

/**
 * 注册回调函数
 */
int MQTTClient_setCallbacks(MQTTClient handle, void* context, MQTTClient_connectionLost* cl,
                            MQTTClient_messageArrived* ma, MQTTClient_deliveryComplete* dc);

/**
 * 创建一个等待连接的client
 */
int MQTTClient_create(MQTTClient* handle, const char* serverURI, const char* clientId,
                      int persistence_type, void* persistence_context);

/**
 * LWT消息的配置参数
 */
typedef struct {
    /** 结构体标识符.  必须是MQTW. */
    const char structId[4];
    /** 结构体版本号.  必须是0 */
    int structVersion;
    /** LWT topic . */
    const char* topicName;
    /** LWT payload. */
    const char* message;
    /** LWT message retained flag.*/
    int retained;
    /** qos值 */
    int qos;
} MQTTClient_willOptions;

#define MQTTClient_willOptions_initializer { {'M', 'Q', 'T', 'W'}, 0, NULL, NULL, 0, 0 }

/**
 * SSL相关配置参数
 */
typedef struct {
    /** 结构体标识符.  必须是MQTS */
    const char structId[4];
    /** 结构体版本号.  必须是0 */
    int structVersion;

    /** PEM格式的数字证书. */
    const char* trustStore;

    /** PEM格式的数字证书链.*/
    const char* keyStore;

    /** 包含私钥的文件 */
    const char* privateKey;
    /** 如果私钥被加密，解密私钥的密码*/
    const char* privateKeyPassword;

    /** 加密套件, 默认为ALL */
    const char* enabledCipherSuites;

    /** True/False 是否允许验证服务端证书**/
    int enableServerCertAuth;

} MQTTClient_SSLOptions;

#define MQTTClient_SSLOptions_initializer { {'M', 'Q', 'T', 'S'}, 0, NULL, NULL, NULL, NULL, NULL, 1 }


/**
 * 连接参数
 */
typedef struct {
    /** 结构体标识符. 必须是MQTC. */
    const char structId[4];
    /** 结构体版本号. 必须是0, 1, 2, 3 or 4.
     * 0 no SSL options and no serverURIs
     * 1 no serverURIs
     * 2 no mqttVersion
     * 3 no returned values
     */
    int structVersion;
    /** 秒为单位，客户单需要保证在改时间内至少有一条消息到达服务端 */
    int keepAliveInterval;
    /**
     * boolean值，cleanSession = true时，连接的会话状态信息会被丢弃；cleanSession = false时
     * 会话状态会被保存, 当client连接到server时，server会检查是否有该client之前的连接会话，如果
     * cleanSession=true,则之前的会话状态会被清除，如果为false,则之前的会话状态恢复，如果没有之前的
     * 会话，新创建一个会话
     */
    int cleanSession;
    /**
     * reliable = true，指明在当前publish的消息没有完成钱不可以发送下一条消息
     * reliable = false, 允许同时发出10条消息.
     */
    int reliable;
    /** LWT指针 */
    MQTTClient_willOptions* will;
    /** MQTT server认证用到的username*/
    const char* username;
    /** MQTT server用到的password */
    const char* password;
    /** 连接的超时时间 */
    int connectTimeout;
    /** retry 时间间隔 */
    int retryInterval;
    /** SSL指针 */
    MQTTClient_SSLOptions* ssl;
    /** 服务端URI数组的长度 */
    int serverURICount;
    /** 服务端URI数组，可以有多个server URI*/
    char* const* serverURIs;
    /**
     * MQTT协议版本号.
     * MQTT_VERSION_DEFAULT (0) = default: start with 3.1.1, and if that fails, fall back to 3.1
     * MQTT_VERSION_3_1 (3) = only try version 3.1
     * MQTT_VERSION_3_1_1 (4) = only try version 3.1.1
     */
    int mqttVersion;
    /**
     * Returned from the connect when the MQTT version used to connect is 3.1.1
     */
    struct {
        const char* serverURI;     /**< the serverURI connected to */
        int mqttVersion;     /**< the MQTT version used to connect with */
        int sessionPresent;  /**< if the MQTT version is 3.1.1, the value of sessionPresent returned in the connack */
    } returned;
} MQTTClient_connectOptions;

#define MQTTClient_connectOptions_initializer { {'M', 'Q', 'T', 'C'}, 4, 60, 1, 1, NULL, NULL, NULL, 30, 20, NULL, 0, NULL, 0, {NULL, 0, 0} }


typedef struct
{
    const char* name;
    const char* value;
} MQTTClient_nameValue;

/**
 * 获取版本信息
 * @return
 */
MQTTClient_nameValue* MQTTClient_getVersionInfo(void);

/**
 * 连接MQTT server
 * @param handle
 * @param options
 * @return MQTT_CLIENT_SUCCESS 成功连接到server,如果连接失败返回以下错误码:
 * 1: Connection refused: Unacceptable protocol version
 * 2: Connection refused: Identifier rejected
 * 3: Connection refused: Server unavailable
 * 4: Connection refused: Bad user name or password
 * 5: Connection refused: Not authorized
 */
int MQTTClient_connect(MQTTClient handle, MQTTClient_connectOptions* options);

/**
 * 断开连接
 * @param handle
 * @param timeout
 * @return MQTT_CLIENT_SUCCESS 断开成功; 断开失败返回错误码
 */
int MQTTClient_disconnect(MQTTClient handle, int timeout);

/**
 * 判断是否连接
 * @param handle
 * @return
 */
int MQTTClient_isConnected(MQTTClient handle);

/**
 * 订阅一个主题
 * @param handle
 * @param topic
 * @param qos
 * @return MQTT_CLIENT_SUCCESS,订阅成功;订阅失败，返回错误码
 */
int MQTTClient_subscribe(MQTTClient handle, const char* topic, int qos);

/**
 * 订阅多个主题
 * @param handle
 * @param count
 * @param topic
 * @param qos
 * @return
 */
int MQTTClient_subscribeMany(MQTTClient handle, int count, char* const* topic, int* qos);

/**
 * 取消订阅
 * @param handle
 * @param topic
 * @return
 */
int MQTTClient_unsubscribe(MQTTClient handle, const char* topic);

/**
 * 取消订阅多个主题
 * @param handle
 * @param count
 * @param topic
 * @return
 */
int MQTTClient_unsubscribeMany(MQTTClient handle, int count, char* const* topic);

/**
 * publish消息
 * @param handle
 * @param topicName
 * @param payloadLen
 * @param payload
 * @param qos
 * @param retained
 * @param dt
 * @return
 */
int MQTTClient_publish(MQTTClient handle, const char* topicName, int payloadLen, void* payload, int qos, int retained,
                       MQTTClient_deliveryToken* dt);

/**
 * publish消息
 * @param handle
 * @param topicName
 * @param msg
 * @param dt
 * @return
 */
int MQTTClient_publishMessage(MQTTClient handle, const char* topicName, MQTTClient_message* msg, MQTTClient_deliveryToken* dt);

/**
 * 如果被调用，线程将一直阻塞直到消息被成功传递或超时
 * @param handle
 * @param dt
 * @param timeout
 * @return
 */
int MQTTClient_waitForCompletion(MQTTClient handle, MQTTClient_deliveryToken dt, unsigned long timeout);

/**
 * 获取当前in-flight的delivery tokens
 * @param handle
 * @param tokens
 * @return
 */
int MQTTClient_getPendingDeliveryTokens(MQTTClient handle, MQTTClient_deliveryToken **tokens);

/**
 * 如果是单线程，周期性的调用该方法允许消息重发以及发送ping包,如果调用了MQTTClient_receive(),则
 * 可以不用调用这个方法
 */
void MQTTClient_yield(void);

/**
 * 同步的接收消息,只有在应用没有调用MQTTClient_setCallbacks设置回调时，用这个方法接收消息，当调用该方法后
 * 线程会被阻塞直到有消息到来或超时
 * @param handle
 * @param topicName
 * @param topicLen
 * @param message
 * @param timeout
 * @return
 */
int MQTTClient_receive(MQTTClient handle, char** topicName, int* topicLen, MQTTClient_message** message,
                       unsigned long timeout);

/**
 * 释放内存
 * @param msg
 */
void MQTTClient_freeMessage(MQTTClient_message** msg);

/**
 * 释放所有内存
 * @param ptr
 */
void MQTTClient_free(void* ptr);

/**
 * 释放内存
 * @param handle
 */
void MQTTClient_destroy(MQTTClient* handle);


#endif // DIM_MQTT_CLIENT_H_

#ifdef __cplusplus
}
#endif