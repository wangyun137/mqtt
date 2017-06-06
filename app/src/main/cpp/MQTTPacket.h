#ifndef DIM_MQTT_PACKET_H_
#define DIM_MQTT_PACKET_H_

#include <stdbool.h>
#include "Socket.h"
#if defined(OPENSSL)
#include "SSLSocket.h"
#endif
#include "LinkedList.h"
#include "Clients.h"

typedef void* (*pf)(unsigned char, char*, size_t);

#define BAD_MQTT_PACKET -4

enum msgTypes
{
	CONNECT = 1, CONNACK, PUBLISH, PUBACK, PUBREC, PUBREL,
	PUBCOMP, SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK,
	PINGREQ, PINGRESP, DISCONNECT
};


/**
 * MQTT数据包的头部信息
 */
typedef union{
	/*unsigned*/ char byte;	/**< the whole byte */
#if defined(REVERSED)
	struct{
		unsigned int 	type : 4;	/**< message type nibble */
		bool 			dup : 1;			/**< DUP flag bit */
		unsigned int 	qos : 2;	/**< QoS value, 0, 1 or 2 */
		bool 			retain : 1;		/**< retained flag bit */
	} bits;
#else
	struct {
		bool 			retain : 1;		/**< retained flag bit */
		unsigned int 	qos : 2;	/**< QoS value, 0, 1 or 2 */
		bool 			dup : 1;			/**< DUP flag bit */
		unsigned int 	type : 4;	/**< message type nibble */
	} bits;
#endif
} Header;


/**
 * Connect时的数据包
 */
typedef struct {
	Header header;	/**< MQTT header byte */
	union {
		unsigned char all;	/**< all connect flags */
#if defined(REVERSED)
		struct {
			bool username : 1;			/**< 3.1 user name */
			bool password : 1; 			/**< 3.1 password */
			bool willRetain : 1;		/**< will retain setting */
			unsigned int willQoS : 2;	/**< will QoS value */
			bool will : 1;				/**< will flag */
			bool cleanstart : 1;		/**< cleansession flag */
			int : 1;					/**< unused */
		} bits;
#else
		struct {
			int : 1;					/**< unused */
			bool cleanStart : 1;		/**< cleanSession flag */
			bool will : 1;				/**< will flag */
			unsigned int willQoS : 2;	/**< will QoS value */
			bool willRetain : 1;		/**< will retain setting */
			bool password : 1; 			/**< 3.1 password */
			bool username : 1;			/**< 3.1 user name */
		} bits;
#endif
	} flags;							/**< connect flags byte */

	char *Protocol; /**< MQTT protocol name */
	char *clientID;	/**< string client id */
  	char *willTopic;	/**< will topic */
  	char *willMsg;	/**< will payload */

	int keepAliveTimer;		/**< keep alive timeout value in seconds */
	unsigned char version;	/**< MQTT version number */
} Connect;


/**
 * CONNACK 数据包.
 */
typedef struct{
	Header header; /**< MQTT header byte */
	union{
		unsigned char all;	/**< all connack flags */
#if defined(REVERSED)
		struct{
			unsigned int reserved : 7;	/**< message type nibble */
			bool sessionPresent : 1;    /**< was a session found on the server? */
		} bits;
#else
		struct {
			bool sessionPresent : 1;    /**< was a session found on the server? */
			unsigned int reserved : 7;	/**< message type nibble */
		} bits;
#endif
	} flags;	 /**< connack flags byte */
	char rc; /**< connack return code */
} Connack;


/**
 * 只有头部信息的MQTT数据包.
 */
typedef struct
{
	Header header;	/**< MQTT header byte */
} MQTTPacket;


/**
 * Subscribe数据包.
 */
typedef struct
{
	Header 	header;		/**< MQTT header byte */
	int 	msgId;		/**< MQTT message id */
	List* 	topics;		/**< list of topic strings */
	List* 	qoss;		/**< list of corresponding QoSs */
	int 	noTopics;	/**< topic and qos count */
} Subscribe;


/**
 * Suback数据包
 */
typedef struct
{
	Header 	header;		/**< MQTT header byte */
	int 	msgId;		/**< MQTT message id */
	List* 	qoss;		/**< list of granted QoSs */
} Suback;


/**
 * Unsubscribe数据包.
 */
typedef struct
{
	Header 	header;		/**< MQTT header byte */
	int 	msgId;		/**< MQTT message id */
	List* 	topics;		/**< list of topic strings */
	int 	noTopics;	/**< topic count */
} Unsubscribe;


/**
 * Publish数据包
 */
typedef struct
{
	Header 	header;		/**< MQTT header byte */
	char* 	topic;		/**< topic string */
	int 	topicLen;
	int 	msgId;		/**< MQTT message id */
	char* 	payload;	/**< binary payload, length delimited */
	int 	payloadLen;	/**< payload length */
} Publish;


/**
 * ACK数据包.
 */
typedef struct
{
	Header header;	/**< MQTT header byte */
	int msgId;		/**< MQTT message id */
} Ack;

typedef Ack Puback;
typedef Ack Pubrec;
typedef Ack Pubrel;
typedef Ack Pubcomp;
typedef Ack Unsuback;

int MQTTPacket_encode(char* buf, size_t length);

int MQTTPacket_decode(NetworkHandles* net, size_t* value);

int readInt(char** pptr);

char* readUTF(char** pptr, char* endData);

unsigned char readChar(char** pptr);

void writeChar(char** pptr, char c);

void writeInt(char** pptr, int anInt);

void writeUTF(char** pptr, const char* string);

const char* MQTTPacket_name(int packetType);

void* MQTTPacket_Factory(NetworkHandles* net, int* error);

int MQTTPacket_send(NetworkHandles* net, Header header, char* buffer, size_t bufLen, int free);

int MQTTPacket_sends(NetworkHandles* net, Header header, int count, char** buffers, size_t* bufLens, int* frees);

void* MQTTPacket_header_only(unsigned char aHeader, char* data, size_t dataLen);

int MQTTPacket_send_disconnect(NetworkHandles* net, const char* clientId);

void* MQTTPacket_publish(unsigned char aHeader, char* data, size_t dataLen);

void MQTTPacket_freePublish(Publish* pack);

int MQTTPacket_send_publish(Publish* pack, int dup, int qos, int retained, NetworkHandles* net, const char* clientID);

int MQTTPacket_send_puback(int msgId, NetworkHandles* net, const char* clientID);

void* MQTTPacket_ack(unsigned char aHeader, char* data, size_t dataLen);

void MQTTPacket_freeSuback(Suback* pack);

int MQTTPacket_send_pubrec(int msgId, NetworkHandles* net, const char* clientID);

int MQTTPacket_send_pubrel(int msgId, int dup, NetworkHandles* net, const char* clientID);

int MQTTPacket_send_pubcomp(int msgId, NetworkHandles* net, const char* clientID);

void MQTTPacket_free_packet(MQTTPacket* pack);

#ifndef NO_BRIDGE
	#include "MQTTPacketOut.h"
#endif

#endif
