#ifndef DIM_MQTT_PROTOCOL_OUT_H_
#define DIM_MQTT_PROTOCOL_OUT_H_

#include "LinkedList.h"
#include "MQTTPacket.h"
#include "Clients.h"
#include "MQTTProtocol.h"
#include "MQTTProtocolClient.h"

#define DEFAULT_PORT 1883

char* MQTTProtocol_addressPort(const char* uri, int* port);
void MQTTProtocol_reconnect(const char* ip_address, Clients* client);
#if defined(OPENSSL)
int MQTTProtocol_connect(const char* ip_address, Clients* acClients, int ssl, int mqttVersion);
#else
int MQTTProtocol_connect(const char* ip_address, Clients* acClients, int mqttVersion);
#endif
int MQTTProtocol_handlePingresps(void* pack, int sock);
int MQTTProtocol_subscribe(Clients* client, List* topics, List* qoss, int msgID);
int MQTTProtocol_handleSubacks(void* pack, int sock);
int MQTTProtocol_unsubscribe(Clients* client, List* topics, int msgID);
int MQTTProtocol_handleUnsubacks(void* pack, int sock);

#endif //DIM_MQTT_PROTOCOL_OUT_H_
