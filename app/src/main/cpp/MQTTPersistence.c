#include <stdio.h>
#include <string.h>

#include "MQTTPersistence.h"
#include "FilePersistence.h"
#include "MemoryPersistence.h"
#include "MQTTPacket.h"
#include "Log.h"
#include "MQTTProtocolClient.h"
#include "Clients.h"

static MQTTPersistence_qEntry* MQTTPersistence_restoreQueueEntry(char* buffer, size_t bufLen);
static void MQTTPersistence_insertInSeqOrder(List* list, MQTTPersistence_qEntry* qEntry, size_t size);

int MQTTPersistence_create(MQTTClientPersistence** persistence, int type, void* context) {
    int rc = 0;
    MQTTClientPersistence* per = NULL;

#ifndef NO_PERSISTENCE
    switch (type) {
        case MQTT_CLIENT_PERSISTENCE_NONE:
            per = NULL;
            break;
        case MQTT_CLIENT_PERSISTENCE_DEFAULT:
            per = malloc(sizeof(MQTTClientPersistence));
            if (per != NULL) {
                if (context != NULL) {
                    per->context = malloc(strlen(context) + 1);
                    strcpy(per->context, context);
                } else {
                    //设置为当前工作目录
                    per->context = ".";
                }

                per->pOpen = pstOpen;
                per->pClose = pstClose;
                per->pPut = pstPut;
                per->pGet = pstGet;
                per->pRemove = pstRemove;
                per->pKeys = pstKeys;
                per->pClear = pstClear;
                per->pContainsKey = pstContainsKey;
            } else {
                rc = MQTT_CLIENT_PERSISTENCE_ERROR;
            }
            break;
        case MQTT_CLIENT_PERSISTENCE_USER:
            per = (MQTTClientPersistence*)context;
            if (per == NULL || (per != NULL && (per->context == NULL || per->pClear == NULL ||
                                               per->pClose == NULL || per->pContainsKey == NULL ||
                                               per->pGet == NULL || per->pKeys == NULL ||
                                               per->pOpen == NULL || per->pPut == NULL || per->pRemove == NULL)))
                rc = MQTT_CLIENT_PERSISTENCE_ERROR;
            break;
        case MQTT_CLIENT_PERSISTENCE_MEMORY:
            per = malloc(sizeof(MQTTClientPersistence));
            if (per != NULL) {
                per->pOpen = meOpen;
                per->pClose = meClose;
                per->pPut = mePut;
                per->pGet = meGet;
                per->pRemove = meRemove;
                per->pKeys = meKeys;
                per->pClear = meClear;
                per->pContainsKey = meContainsKey;
            } else {
                rc = MQTT_CLIENT_PERSISTENCE_ERROR;
            }
            break;
        default:
            rc = MQTT_CLIENT_PERSISTENCE_ERROR;
            break;
    }
#endif

    *persistence = per;
    return rc;
}

int MQTTPersistence_initialize(Clients* c, const char* serverURI) {
    int rc = 0;
    if (c->persistence != NULL) {
        rc = c->persistence->pOpen(&(c->pHandle), c->clientID, serverURI, c->persistence->context);
        if (rc == 0) {
            rc = MQTTPersistence_restore(c);
        }
    }
    return rc;
}

int MQTTPersistence_close(Clients* c) {
    int rc = 0;
    if (c->persistence != NULL) {
        rc = c->persistence->pClose(c->pHandle);
        c->pHandle = NULL;
#ifndef NO_PERSISTENCE
        if (c->persistence->pOpen == pstOpen)
            free(c->persistence);
#endif
        c->persistence = NULL;
    }

    return rc;
}

int MQTTPersistence_clear(Clients* c) {
    int rc = 0;
    if (c->persistence != NULL) {
        rc = c->persistence->pClear(c->pHandle);
    }
    return rc;
}

int MQTTPersistence_restore(Clients* c) {
    int rc = 0;
    char** msgKeys = NULL;
    char* buffer = NULL;

    int nKeys;
    int bufLen;
    int i = 0;
    int msgsSent = 0;
    int msgsRecv = 0;

    if (c->persistence && (rc = c->persistence->pKeys(c->pHandle, &msgKeys, &nKeys)) == 0) {
        while (rc ==0 && i < nKeys) {
            if ((rc = c->persistence->pGet(c->pHandle, msgKeys[i], &buffer, &bufLen)) == 0) {
                MQTTPacket* packet = MQTTPersistence_restorePacket(buffer, bufLen);
                if (packet == NULL) {
                    rc = c->persistence->pRemove(c->pHandle, msgKeys[i]);
                } else {
                    if (strstr(msgKeys[i], PERSISTENCE_PUBLISH_RECEIVED) != NULL) {
                        Publish* publish = (Publish*)packet;
                        Messages* msg = NULL;
                        msg = MQTTProtocol_createMessage(publish, &msg, publish->header.bits.qos, publish->header.bits.retain);
                        ListAppend(c->inboundMsgs, msg, msg->len);
                        publish->topic = NULL;
                        MQTTPacket_freePublish(publish);
                        msgsRecv++;
                    }
                    else if (strstr(msgKeys[i], PERSISTENCE_PUBLISH_SENT) != NULL) {
                        Publish* publish = (Publish*)packet;
                        Messages* msg = NULL;
                        char* key = malloc(MESSAGE_FILENAME_LENGTH + 1);
                        sprintf(key, "%s%d", PERSISTENCE_PUBREL, publish->msgId);
                        msg = MQTTProtocol_createMessage(publish, &msg, publish->header.bits.qos, publish->header.bits.retain);
                        if (c->persistence->pContainsKey(c->pHandle, key) == 0)
                            msg->nextMessageType = PUBCOMP;

                        msg->lastTouch = 0;
                        MQTTPersistence_insertInOrder(c->outboundMsgs, msg, msg->len);
                        publish->topic = NULL;
                        free(key);
                        msgsSent++;
                    }
                    else if (strstr(msgKeys[i], PERSISTENCE_PUBREL) != NULL) {
                        Pubrel* pubrel = (Pubrel*)packet;
                        char* key = malloc(MESSAGE_FILENAME_LENGTH + 1);
                        sprintf(key, "%s%d", PERSISTENCE_PUBLISH_SENT, pubrel->msgId);
                        if (c->persistence->pContainsKey(c->pHandle, key) != 0)
                            rc = c->persistence->pRemove(c->pHandle, msgKeys[i]);

                        free(pubrel);
                        free(key);
                    }
                }
            }
            if (buffer) {
                free(buffer);
                buffer = NULL;
            }
            if (msgKeys[i])
                free(msgKeys[i]);

            i++;
        }

        if (msgKeys)
            free(msgKeys);
    }


    LOG("%d send messages and %d received messages restored for client %s\n", msgsSent, msgsRecv, c->clientID);
    MQTTPersistence_wrapMsgID(c);
    return rc;
}

void* MQTTPersistence_restorePacket(char* buffer, size_t bufLen) {
    void* packet = NULL;
    Header header;

    int fixedHeaderLength = 1;
    int pType;
    int remainingLength = 0;
    char c;
    int multiplier = 1;
    extern pf new_packets[];

    header.byte = buffer[0];
    do {
        c = *(++buffer);
        remainingLength += (c & 127) * multiplier;
        multiplier *= 128;
        fixedHeaderLength++;
    } while ((c & 128) != 0);

    if ((fixedHeaderLength + remainingLength) == bufLen) {
        pType = header.bits.type;
        if (pType >= CONNECT && pType <= DISCONNECT && new_packets[pType] != NULL)
            packet = (*new_packets[pType])(header.byte, ++buffer, remainingLength);
    }

    return packet;

}

void MQTTPersistence_insertInOrder(List* list, void* content, size_t size) {
    ListElement* index = NULL;
    ListElement* current = NULL;

    while (ListNextElement(list, &current) != NULL && index == NULL) {
        if (((Messages*)content)->msgId < ((Messages*)current->content)->msgId)
            index = current;
    }

    ListInsert(list, content, size, index);
}

int MQTTPersistence_put(int socket, char* buf0, size_t buf0Len, int count, char** buffers,
                                size_t* bufLens, int hType, int msgId, int scr) {
    int rc = 0;
    extern ClientStates* bState;
    int nBufs, i;
    int* lens = NULL;
    char** bufs = NULL;
    char *key;
    Clients* client = NULL;

    client = (Clients*)(ListFindItem(bState->clients, &socket, clientSocketCompare)->content);
    if (client->persistence != NULL) {
        key = malloc(MESSAGE_FILENAME_LENGTH + 1);
        nBufs = 1 + count;
        lens = (int *)malloc(nBufs * sizeof(int));
        bufs = (char **)malloc(nBufs * sizeof(char *));
        lens[0] = (int)buf0Len;
        bufs[0] = buf0;
        for (i = 0; i < count; i++) {
            lens[i+1] = (int)bufLens[i];
            bufs[i+1] = buffers[i];
        }

        if ( scr == 0 ) {  /* sending */
            if (hType == PUBLISH)   /* PUBLISH QoS1 and QoS2*/
                sprintf(key, "%s%d", PERSISTENCE_PUBLISH_SENT, msgId);
            if (hType == PUBREL)  /* PUBREL */
                sprintf(key, "%s%d", PERSISTENCE_PUBREL, msgId);
        }

        if ( scr == 1 )  /* receiving PUBLISH QoS2 */
            sprintf(key, "%s%d", PERSISTENCE_PUBLISH_RECEIVED, msgId);

        rc = client->persistence->pPut(client->pHandle, key, nBufs, bufs, lens);

        free(key);
        free(lens);
        free(bufs);
    }

    return rc;
}

int MQTTPersistence_remove(Clients* c, char *type, int qos, int msgId) {
    int rc = 0;

    if (c->persistence != NULL) {
        char *key = malloc(MESSAGE_FILENAME_LENGTH + 1);
        if ( (strcmp(type,PERSISTENCE_PUBLISH_SENT) == 0) && qos == 2 ) {
            sprintf(key, "%s%d", PERSISTENCE_PUBLISH_SENT, msgId) ;
            rc = c->persistence->pRemove(c->pHandle, key);
            sprintf(key, "%s%d", PERSISTENCE_PUBREL, msgId) ;
            rc = c->persistence->pRemove(c->pHandle, key);
        }
        else /* PERSISTENCE_PUBLISH_SENT && qos == 1 */{    /* or PERSISTENCE_PUBLISH_RECEIVED */
            sprintf(key, "%s%d", type, msgId) ;
            rc = c->persistence->pRemove(c->pHandle, key);
        }
        free(key);
    }

    return rc;
}

void MQTTPersistence_wrapMsgID(Clients *client) {
    ListElement* wrapel = NULL;
    ListElement* current = NULL;

    if ( client->outboundMsgs->count > 0 ) {
        int firstMsgID = ((Messages*)client->outboundMsgs->first->content)->msgId;
        int lastMsgID = ((Messages*)client->outboundMsgs->last->content)->msgId;
        int gap = MAX_MSG_ID - lastMsgID + firstMsgID;
        current = ListNextElement(client->outboundMsgs, &current);

        while(ListNextElement(client->outboundMsgs, &current) != NULL) {
            int curMsgID = ((Messages*)current->content)->msgId;
            int curPrevMsgID = ((Messages*)current->prev->content)->msgId;
            int curgap = curMsgID - curPrevMsgID;
            if ( curgap > gap ) {
                gap = curgap;
                wrapel = current;
            }
        }
    }

    if ( wrapel != NULL ) {
        /* put wrapel at the beginning of the queue */
        client->outboundMsgs->first->prev = client->outboundMsgs->last;
        client->outboundMsgs->last->next = client->outboundMsgs->first;
        client->outboundMsgs->first = wrapel;
        client->outboundMsgs->last = wrapel->prev;
        client->outboundMsgs->first->prev = NULL;
        client->outboundMsgs->last->next = NULL;
    }
}


#ifndef NO_PERSISTENCE

int MQTTPersistence_unpersistQueueEntry(Clients* client, MQTTPersistence_qEntry* qe) {
    int rc = 0;
    char key[PERSISTENCE_MAX_KEY_LENGTH + 1];

    sprintf(key, "%s%d", PERSISTENCE_QUEUE_KEY, qe->seqNo);
    if ((rc = client->persistence->pRemove(client->pHandle, key)) != 0)
        LOG( "Error %d removing qEntry from persistence", rc);
    return rc;
}

int MQTTPersistence_persistQueueEntry(Clients* aclient, MQTTPersistence_qEntry* qe) {
    int rc = 0;
    int nbufs = 8;
    int bufindex = 0;
    char key[PERSISTENCE_MAX_KEY_LENGTH + 1];
    int* lens = NULL;
    void** bufs = NULL;

    lens = (int*)malloc(nbufs * sizeof(int));
    bufs = malloc(nbufs * sizeof(char *));

    bufs[bufindex] = &qe->msg->payloadLen;
    lens[bufindex++] = sizeof(qe->msg->payloadLen);

    bufs[bufindex] = qe->msg->payload;
    lens[bufindex++] = qe->msg->payloadLen;

    bufs[bufindex] = &qe->msg->qos;
    lens[bufindex++] = sizeof(qe->msg->qos);

    bufs[bufindex] = &qe->msg->retained;
    lens[bufindex++] = sizeof(qe->msg->retained);

    bufs[bufindex] = &qe->msg->dup;
    lens[bufindex++] = sizeof(qe->msg->dup);

    bufs[bufindex] = &qe->msg->msgId;
    lens[bufindex++] = sizeof(qe->msg->msgId);

    bufs[bufindex] = qe->topicName;
    lens[bufindex++] = (int)strlen(qe->topicName) + 1;

    bufs[bufindex] = &qe->topicLen;
    lens[bufindex++] = sizeof(qe->topicLen);

    sprintf(key, "%s%d", PERSISTENCE_QUEUE_KEY, ++aclient->qEntrySeqNo);
    qe->seqNo = aclient->qEntrySeqNo;

    if ((rc = aclient->persistence->pPut(aclient->pHandle, key, nbufs, (char**)bufs, lens)) != 0)
        LOG("Error persisting queue entry, rc %d", rc);

    free(lens);
    free(bufs);

    return rc;
}

static MQTTPersistence_qEntry* MQTTPersistence_restoreQueueEntry(char* buffer, size_t buflen) {
    MQTTPersistence_qEntry* qe = NULL;
    char* ptr = buffer;
    int data_size;

    qe = malloc(sizeof(MQTTPersistence_qEntry));
    memset(qe, '\0', sizeof(MQTTPersistence_qEntry));

    qe->msg = malloc(sizeof(MQTTPersistence_message));
    memset(qe->msg, '\0', sizeof(MQTTPersistence_message));

    qe->msg->payloadLen = *(int*)ptr;
    ptr += sizeof(int);

    data_size = qe->msg->payloadLen;
    qe->msg->payload = malloc(data_size);
    memcpy(qe->msg->payload, ptr, data_size);
    ptr += data_size;

    qe->msg->qos = *(int*)ptr;
    ptr += sizeof(int);

    qe->msg->retained = *(int*)ptr;
    ptr += sizeof(int);

    qe->msg->dup = *(int*)ptr;
    ptr += sizeof(int);

    qe->msg->msgId = *(int*)ptr;
    ptr += sizeof(int);

    data_size = (int)strlen(ptr) + 1;
    qe->topicName = malloc(data_size);
    strcpy(qe->topicName, ptr);
    ptr += data_size;

    qe->topicLen = *(int*)ptr;
    ptr += sizeof(int);

    return qe;
}


static void MQTTPersistence_insertInSeqOrder(List* list, MQTTPersistence_qEntry* qEntry, size_t size) {
    ListElement* index = NULL;
    ListElement* current = NULL;

    while (ListNextElement(list, &current) != NULL && index == NULL) {
        if (qEntry->seqNo < ((MQTTPersistence_qEntry*)current->content)->seqNo)
            index = current;
    }
    ListInsert(list, qEntry, size, index);
}

int MQTTPersistence_restoreMessageQueue(Clients* c) {
    int rc = 0;
    char **msgKeys;
    int nKeys;
    int i = 0;
    int entries_restored = 0;

    if (c->persistence && (rc = c->persistence->pKeys(c->pHandle, &msgKeys, &nKeys)) == 0) {
        while (rc == 0 && i < nKeys) {
            char *buffer = NULL;
            int bufLen;

            if ((rc = c->persistence->pGet(c->pHandle, msgKeys[i], &buffer, &bufLen)) == 0) {
                MQTTPersistence_qEntry* qe = MQTTPersistence_restoreQueueEntry(buffer, bufLen);

                if (qe) {
                    qe->seqNo = atoi(msgKeys[i]+2);
                    MQTTPersistence_insertInSeqOrder(c->publishMessageQueue, qe, sizeof(MQTTPersistence_qEntry));
                    free(buffer);
                    c->qEntrySeqNo = max(c->qEntrySeqNo, qe->seqNo);
                    entries_restored++;
                }
            }
            if (msgKeys[i])
                free(msgKeys[i]);
            i++;
        }
        if (msgKeys != NULL)
            free(msgKeys);
    }
    LOG("%d queued messages restored for client %s", entries_restored, c->clientID);
    return rc;
}

#endif