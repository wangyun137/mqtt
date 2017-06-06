
#ifndef DIM_MQTT_PERSISTENCE_H
#define DIM_MQTT_PERSISTENCE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "Clients.h"


#define PERSISTENCE_MAX_KEY_LENGTH 8

#define PERSISTENCE_COMMAND_KEY "c-"

#define PERSISTENCE_QUEUE_KEY "q-"

#define PERSISTENCE_PUBLISH_RECEIVED "r-"

#define PERSISTENCE_PUBREL "sc-"

#define PERSISTENCE_PUBLISH_SENT "s-"


int MQTTPersistence_create(MQTTClientPersistence** per, int type, void* context);

int MQTTPersistence_initialize(Clients* c, const char* serverURI);

int MQTTPersistence_close(Clients* c);

int MQTTPersistence_clear(Clients* c);

int MQTTPersistence_restore(Clients* c);

void* MQTTPersistence_restorePacket(char* buffer, size_t bufLen);

void MQTTPersistence_insertInOrder(List* list, void* content, size_t size);

int MQTTPersistence_put(int socket, char* buf0, size_t buf0len, int count,
                        char** buffers, size_t* bufLens, int hType, int msgId, int scr);

int MQTTPersistence_remove(Clients* c, char* type, int qos, int msgId);

void MQTTPersistence_wrapMsgID(Clients *c);

typedef struct {
    char    structId[4];
    int     structVersion;
    int     payloadLen;
    void*   payload;
    int     qos;
    int     retained;
    int     dup;
    int     msgId;
} MQTTPersistence_message;

typedef struct {
    MQTTPersistence_message*    msg;
    char*                       topicName;
    int                         topicLen;
    unsigned int                seqNo; /* only used on restore */
} MQTTPersistence_qEntry;

int MQTTPersistence_unpersistQueueEntry(Clients* client, MQTTPersistence_qEntry* qe);

int MQTTPersistence_persistQueueEntry(Clients* client, MQTTPersistence_qEntry* qe);

int MQTTPersistence_restoreMessageQueue(Clients* c);

#ifdef __cplusplus
}
#endif


#endif //DIM_MQTT_PERSISTENCE_H
