#ifndef DIM_MQTT_PACKET_OUT_H_
#define DIM_MQTT_PACKET_OUT_H_

#include "MQTTPacket.h"

int MQTTPacket_send_connect(Clients* client, int mqttVersion);

void* MQTTPacket_connack(unsigned char aHeader, char* data, size_t dataLen);

int MQTTPacket_send_pingreq(NetworkHandles* net, const char* clientID);

int MQTTPacket_send_subscribe(List* topics, List* qoss, int msgId, int dup,
                              NetworkHandles* net, const char* clientId);

void* MQTTPacket_suback(unsigned char aHeader, char* data, size_t dataLen);

int MQTTPacket_send_unsubscribe(List* topics, int msgId, int dup, NetworkHandles* net, const char* clientId);

#endif
