
#ifndef DIM_MQTT_PROTOCOL_H_
#define DIM_MQTT_PROTOCOL_H_

#include "LinkedList.h"
#include "MQTTPacket.h"
#include "Clients.h"

#define MAX_MSG_ID 65535
#define MAX_CLIENTID_LEN 65535

typedef struct {
	int socket;
	Publications* p;
} PendingWritePublication;


typedef struct {
	List publications;
	unsigned int msgs_received;
	unsigned int msgs_sent;
	//List<PendingWritePublication>
	List pendingWritePubList; /* for qos 0 writes not complete */
} MQTTProtocol;


#include "MQTTProtocolOut.h"

#endif //DIM_MQTT_PROTOCOL_H_
