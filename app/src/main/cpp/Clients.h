#ifndef DIM_MQTT_CLIENTS_H_
#define DIM_MQTT_CLIENTS_H_

#include <time.h>
#ifdef OPENSSL
#include <openssl/ssl.h>
#endif

#include "MQTTClient.h"
#include "LinkedList.h"
#include "MQTTClientPersistence.h"


/* 定义连接状态*/
#define CONNECT_STATE_DISCONNECTED -2
#define CONNECT_STATE_NONE 0
#define CONNECT_STATE_WAIT_FOR_TCP_COMPLETE 1
#define CONNECT_STATE_WAIT_FOR_SSL_COMPLETE 2
#define CONNECT_STATE_WAIT_FOR_CONNACK 3


/**
* 用来缓存publish消息对应的基本数据结构
*/
typedef struct{
	char*	topic;
	int 	topicLen;
	char* 	payload;
	int 	payloadLen;
	int 	refCount;
} Publications;

/**
* publish动作对应的消息的数据结构
*/
typedef struct{
	int 			qos;
	int 			retain;
	int 			msgId;
	Publications*	publish;
	time_t 			lastTouch;		/**> used for retry and expiry */
	char 			nextMessageType;	/**> PUBREC, PUBREL, PUBCOMP */
	int 			len;				/**> length of the whole structure+data */
} Messages;

/**
 * LWT Message
 */
typedef struct
{
	char*	topic;
	char*	msg;
	int 	retained;
	int 	qos;
} WillMessages;

/**
* 持有网络请求必要的数据结构，socket文件描述符, 最后发送时间和最近接收时间
*/
typedef struct{
	int socket; //socket文件描述符
	time_t lastSent;
	time_t lastReceived;
#if defined(OPENSSL)
	SSL* ssl;
	SSL_CTX* ctx;
#endif
} NetworkHandles;

/**
 * 持有客户端必要的数据
 */
typedef struct{
	char* 					clientID;					/** client 唯一ID*/
	const char* 			username;					/** MQTT v3.1 user name */
	const char* 			password;					/** MQTT v3.1 password */
	unsigned int 			cleanSession : 1;			/** MQTT clean session flag, 只占1bit */
	unsigned int 			connected : 1;				/** 指明客户端当前是否处于连接状态, 只占1bit */
	unsigned int 			good : 1; 					/** if we have an error on the socket we turn this off */
	unsigned int 			pingOutStanding : 1;
	int 					connectState : 4; 			//占4bit
	NetworkHandles 			net; 						//网络数据结构
	int 					msgID;
	int 					keepAliveInterval; 			//keep alive时间
	int 					retryInterval; 				//重试时间
	int 					maxInFlightMessages; 		//in-flight message的最大数
	WillMessages* 			will; 						//LWT message
	List* 					inboundMsgs;
	List* 					outboundMsgs;				/** in flight */
	List* 					publishMessageQueue; 		//消息队列 List<qEntry*>
	unsigned int 			qEntrySeqNo;
	void* 					pHandle;  					/* the persistence handle */
	MQTTClientPersistence* persistence; 				/* a persistence implementation */
	void* 					context; 					/* calling context - used when calling disconnect_internal */
	int 					mqttVersion;
#if defined(OPENSSL)
	MQTTClient_SSLOptions *sslopts;
	SSL_SESSION* session;    /***< SSL session pointer for fast handhake */
#endif
} Clients;

int clientIdCompare(void* a, void* b);
int clientSocketCompare(void* a, void* b);

/**
 * 所有Client的集合
 */
typedef struct{
	const char* 		version;
	List* 				clients;
} ClientStates;

#endif
