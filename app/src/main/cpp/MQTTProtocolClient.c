#include <stdlib.h>

#include "MQTTProtocolClient.h"
#ifndef NO_PERSISTENCE
#include "MQTTPersistence.h"
#endif

#include "SocketBuffer.h"
#include "Heap.h"
#include "Log.h"

#ifndef min
#define min(A,B) ((A) < (B) ? (A):(B))
#endif

extern MQTTProtocol state;
extern ClientStates* bState;

static void MQTTProtocol_storeQoS0(Clients* pubClient, Publish* publish);

static int MQTTProtocol_startPublishCommon(Clients* pubClient, Publish* publish, int qos, int retained);

static void MQTTProtocol_retries(time_t now, Clients* client, int regardless);

/**
* 回调函数，比较两个message是否相等
*/
int messageIDCompare(void* a, void* b) {
    Messages* msg = (Messages*)a;
    return msg->msgId = *(int*)b;
}

/**
* 给client分配一个新的msgId
*/
int MQTTProtocol_assignMsgId(Clients* client) {
    int startMsgId = client->msgID;
    int msgId = startMsgId;

    msgId = (msgId == MAX_MSG_ID) ? 1 : msgId + 1;
    while (ListFindItem(client->outboundMsgs, &msgId, messageIDCompare) != NULL) {
        msgId = (msgId == MAX_MSG_ID) ? 1 : msgId + 1;
        if (msgId == startMsgId) {
            msgId = 0;
            break;
        }
    }
    if (msgId != 0) {
        client->msgID = msgId;
    }

    return msgId;
}

/**
 * 存储QoS为0的Publication
 * @param pubClient
 * @param publish
 */
static void MQTTProtocol_storeQoS0(Clients* pubClient, Publish* publish) {
    int len;
    PendingWritePublication* pw = NULL;
    pw = malloc(sizeof(PendingWritePublication));

    pw->p = MQTTProtocol_storePublication(publish, &len);
    pw->socket = pubClient->net.socket;
    ListAppend(&(state.pendingWritePubList), pw, sizeof(pendingWriteBuf) + len);
    if (SocketBuffer_updateWrite(pw->socket, pw->p->topic, pw->p->payload) == NULL) {
        LOG("Error updating write at file %s line %d", __FILE__, __LINE__);
    }
}

/**
* Publish消息
*/
static int MQTTProtocol_startPublishCommon(Clients* pubClient, Publish* publish, int qos, int retained) {
    int rc = TCPSOCKET_COMPLETE;

    rc = MQTTPacket_send_publish(publish, 0, qos, retained, &pubClient->net, pubClient->clientID);
    if (qos == 0 && rc == TCPSOCKET_INTERRUPTED) {
        MQTTProtocol_storeQoS0(pubClient, publish);
    }
    return rc;
}

/**
* publish新消息，存储所有必需的状态同时如果有错误尝试重发数据包
*/
int MQTTProtocol_startPublish(Clients* pubClient, Publish* publish, int qos, int retained, Messages** mm) {
    Publish p = *publish;
    int rc = 0;

    if (qos > 0) {
        *mm = MQTTProtocol_createMessage(publish, mm, qos, retained);
        ListAppend(pubClient->outboundMsgs, *mm, (*mm)->len);
        p.payload = (*mm)->publish->payload;
        p.topic = (*mm)->publish->topic;
    }
    rc = MQTTProtocol_startPublishCommon(pubClient, &p, qos, retained);
    return rc;
}

/**
* 复制并存储message
*/
Messages* MQTTProtocol_createMessage(Publish* publish, Messages **mm, int qos, int retained) {
    Messages* m = malloc(sizeof(Messages));

    m->len = sizeof(Messages);
    if (*mm == NULL || (*mm)->publish == NULL) {
        int len1;
        *mm = m;
        m->publish = MQTTProtocol_storePublication(publish, &len1);
        m->len += len1;
    } else {
        ++(((*mm)->publish)->refCount);
        m->publish = (*mm)->publish;
    }
    m->msgId = publish->msgId;
    m->qos = qos;
    m->retain = retained;
    time(&(m->lastTouch));
    if (qos == 2) {
        m->nextMessageType = PUBREC;
    }
    return m;
}


/**
* 缓存publish message
*/
Publications* MQTTProtocol_storePublication(Publish* publish, int* len) {
    Publications* p = malloc(sizeof(Publications));

    p->refCount = 1;

    *len = (int)strlen(publish->topic) + 1;
    /* 在堆中查找对应的topic,如果没有分配新内存，并复制topic到新内存*/
    if (Heap_findItem(publish->topic)) {
        p->topic = publish->topic;
    } else {
        p->topic = malloc(*len);
        strcpy(p->topic, publish->topic);
    }
    *len += sizeof(Publications);

    p->topicLen = publish->topicLen;
    p->payloadLen = publish->payloadLen;
    p->payload = malloc(publish->payloadLen);
    memcpy(p->payload, publish->payload, p->payloadLen);
    *len += publish->payloadLen;
    //将Publication添加至list中
    ListAppend(&(state.publications), p, *len);
    return p;
}

/**
* 删除缓存的publication消息
*/
void MQTTProtocol_removePublication(Publications* p) {
    if (--(p->refCount) == 0) {
        free(p->payload);
        free(p->topic);
        ListRemove(&(state.publications), p);
    }
}


/**
* 处理收到的Publish消息
*/
int MQTTProtocol_handlePublishes(void* pack, int sock) {
    Publish* publish = (Publish*)pack;
    Clients* client = NULL;
    char* clientId = NULL;
    int rc = TCPSOCKET_COMPLETE;

    //在clients列表中根据传入的socket查找对应的Clients结构
    client = (Clients*)(ListFindItem(bState->clients, &sock, clientSocketCompare)->content);
    clientId = client->clientID;
    LOG("handlePublishes clientId is %s, msgId is %d, qos is %d, retain is %d in %s-%d",
        clientId, publish->msgId, publish->header.bits.qos, publish->header.bits.retain, __FILE__, __LINE__);

    if (publish->header.bits.qos == 0) {
        //如果qos = 0，直接处理该消息即可
        Protocol_processPublication(publish, client);
    } else if (publish->header.bits.qos == 1) {
        /* 如果qos = 1, 首先发送puback(发布确认)响应给对方, 之后再处理消息
        这里先发送响应的原因是，可能会有很多publications消息填满socket的缓冲区*/
        rc = MQTTPacket_send_puback(publish->msgId, &client->net, client->clientID);
        //如果在发送ACK时发生错误，是否应该忽略这条publications?
        Protocol_processPublication(publish, client);
    } else if (publish->header.bits.qos == 2) {
        int len;
        ListElement* listElem = NULL;
        Messages* m = malloc(sizeof(Messages));
        /* 如果qos = 2, 缓存消息*/
        Publications* p = MQTTProtocol_storePublication(publish, &len);
        m->publish = p;
        m->msgId = publish->msgId;
        m->qos = publish->header.bits.qos;
        m->retain = publish->header.bits.retain;
        m->nextMessageType = PUBREL;
        if ((listElem = ListFindItem(client->inboundMsgs, &(m->msgId), messageIDCompare)) != NULL) {
            /* 如果在inboundMsgs中有相同msgId的Message,删除原先的Message，将新收到的Message插入的inboundMsgs中*/
            Messages* msg = (Messages*)(listElem->content);
            MQTTProtocol_removePublication(msg->publish);
            ListInsert(client->inboundMsgs, m, sizeof(Messages) + len, listElem);
            ListRemove(client->inboundMsgs, msg);
        } else {
            //将新收到的消息插入到inboundMsgs
            ListAppend(client->inboundMsgs, m, sizeof(Messages) + len);
        }
        //发送PUBREC(发布收到)
        rc = MQTTPacket_send_pubrec(publish->msgId, &client->net, client->clientID);
        publish->topic = NULL;
    }
    MQTTPacket_freePublish(publish);
    return rc;
}


/**
* 处理收到的PUBACK
*/
int MQTTProtocol_handlePubacks(void* pack, int sock) {
    Puback* puback = (Puback*)pack;
    Clients* client = NULL;
    int rc = TCPSOCKET_COMPLETE;
    //查找socket对应的client
    client = (Clients*)(ListFindItem(bState->clients, &sock, clientSocketCompare)->content);
    if (ListFindItem(client->outboundMsgs, &(puback->msgId), messageIDCompare) == NULL) {
        /* 如果相同msgId的PUBACK在outboundMsgs中，啥也不干，直接等该消息发出去就可以了*/
        LOG("handle PUBACK clientId is %s, msgId is %d in %s-%d",
            client->clientID, puback->msgId, __FILE__, __LINE__);
    } else {
        Messages* m = (Messages*)(client->outboundMsgs->current->content);
        if (m->qos == 1) {
            #ifndef NO_PERSISTENCE
            rc = MQTTPersistence_remove(client, PERSISTENCE_PUBLISH_SENT, m->qos, puback->msgId);
            #endif
            //删除缓存的publication
            MQTTProtocol_removePublication(m->publish);
            ListRemove(client->outboundMsgs, m);
        }
    }

    free(pack);
    return rc;
}


/**
* 处理收到的pubrec响应
*/
int MQTTProtocol_handlePubrecs(void* pack, int sock) {
    Pubrec* pubrec = (Pubrec*)pack;
    Clients* client = NULL;
    int rc = TCPSOCKET_COMPLETE;

    client = (Clients*)(ListFindItem(bState->clients, &sock, clientSocketCompare)->content);

    client->outboundMsgs->current = NULL;
    if (ListFindItem(client->outboundMsgs, &(pubrec->msgId), messageIDCompare) == NULL) {
        if (pubrec->header.bits.dup == 0) {
            LOG("handle PUBREC clientId is %s, msgId is %d in %s-%d",
                client->clientID, pubrec->msgId, __FILE__, __LINE__);
        }
    } else {
        //在outboundMsgs中找到了对应的Publish消息
        Messages* m = (Messages*)(client->outboundMsgs->current->content);
        if (m->qos != 2) {
            if (pubrec->header.bits.dup == 0) {
                LOG("handle PUBREC clientId is %s, msgId is %d, qos is %d in %s-%d",
                    client->clientID, pubrec->msgId, m->qos, __FILE__, __LINE__);
            }
        } else if (m->nextMessageType != PUBREC) {
            if (pubrec->header.bits.dup == 0) {
                LOG("handle PUBREC clientId is %s, msgId is %d in %s-%d",
                    client->clientID, pubrec->msgId, __FILE__, __LINE__);
            }
        } else {
            rc = MQTTPacket_send_pubrel(pubrec->msgId, 0, &client->net, client->clientID);
            m->nextMessageType = PUBCOMP;
            time(&(m->lastTouch));
        }
    }
    free(pack);
    return rc;
}

/**
* 处理接收到的pubrel消息
*/
int MQTTProtocol_handlePubrels(void* pack, int sock) {
    Pubrel* pubrel = (Pubrel*)pack;
    Clients* client = NULL;
    int rc = TCPSOCKET_COMPLETE;

    client = (Clients*)(ListFindItem(bState->clients, &sock, clientSocketCompare)->content);
    if (ListFindItem(client->inboundMsgs, &(pubrel->msgId), messageIDCompare) == NULL) {
        //如果在inboundMsgs中没有找到相同msgId的pubrel，则根据dup来判断是否需要发送PUBCOMP
        if (pubrel->header.bits.dup == 0) {
            //dup != 1时不用回复确认
            LOG("handle PUBREL clientId is %s, msgId is %d in %s-%d",
                client->clientID, pubrel->msgId, __FILE__, __LINE__);
        } else {
            //发送PUBCOMP
            rc = MQTTPacket_send_pubcomp(pubrel->msgId, &client->net, client->clientID);
        }
    } else {
        //如果找到了相同msgId的pubrel
        Messages* m = (Messages*)(client->inboundMsgs->current->content);

        if (m->qos != 2) {
            LOG("handle PUBREL clientId is %s, msgId is %d, qos is %d in %s-%d",
                client->clientID, pubrel->msgId, m->qos, __FILE__, __LINE__);
        } else if (m->nextMessageType != PUBREL) {
            LOG("handle PUBREL clientId is %s, msgId is %d in %s-%d",
                client->clientID, pubrel->msgId, __FILE__, __LINE__);
        } else {
            Publish publish;
            rc = MQTTPacket_send_pubcomp(pubrel->msgId, &client->net, client->clientID);
            publish.header.bits.qos = m->qos;
            publish.header.bits.retain = m->retain;
            publish.msgId = m->msgId;
            publish.topic = m->publish->topic;
            publish.topicLen = m->publish->topicLen;
            publish.payload = m->publish->payload;
            publish.payloadLen = m->publish->payloadLen;
            Protocol_processPublication(&publish, client);
#ifndef NO_PERSISTENCE
            rc += MQTTPersistence_remove(client, PERSISTENCE_PUBLISH_RECEIVED, m->qos, pubrel->msgId);
#endif
            ListRemove(&(state.publications), m->publish);
            ListRemove(client->inboundMsgs, m);
            ++(state.msgs_received);
        }
    }
    free(pack);
    return rc;
}

/**
* 处理接收到的pubcomp消息
*/
int MQTTProtocol_handlePubcomps(void* pack, int sock) {
    Pubcomp* pubcomp = (Pubcomp*)pack;
    Clients* client = NULL;
    int rc = TCPSOCKET_COMPLETE;

    client = (Clients*)(ListFindItem(bState->clients, &sock, clientSocketCompare)->content);

    if (ListFindItem(client->outboundMsgs, &(pubcomp->msgId), messageIDCompare) == NULL) {
        if (pubcomp->header.bits.dup == 0) {
            LOG("handle PUBREL clientId is %s, msgId is %d in %s-%d",
                client->clientID, pubcomp->msgId, __FILE__, __LINE__);
        }
    } else {
        Messages* m = (Messages*)(client->outboundMsgs->current->content);
        if (m->qos != 2) {
            LOG("handle PUBREL clientId is %s, msgId is %d, qos is %d in %s-%d",
                client->clientID, pubcomp->msgId, m->qos, __FILE__, __LINE__);
        } else {
            if (m->nextMessageType != PUBCOMP) {
                LOG("handle PUBREL clientId is %s, msgId is %d in %s-%d",
                    client->clientID, pubcomp->msgId, __FILE__, __LINE__);
            } else {
#ifndef NO_PERSISTENCE
                rc = MQTTPersistence_remove(client, PERSISTENCE_PUBLISH_SENT, m->qos, pubcomp->msgId);
#endif
                MQTTProtocol_removePublication(m->publish);
                ListRemove(client->outboundMsgs, m);
                (++state.msgs_sent);
            }
        }
    }
    free(pack);
    return rc;
}


/**
* 保持MQTT长连接，发送PINGREQ心跳包
*/
void MQTTProtocol_keepAlive(time_t now) {
    ListElement* current = NULL;
    ListNextElement(bState->clients, &current);
    while (current) {
        Clients* client = (Clients*)(current->content);
        ListNextElement(bState->clients, &current);
        if (client->connected && client->keepAliveInterval > 0 &&
              (difftime(now, client->net.lastSent) >= client->keepAliveInterval ||
              difftime(now, client->net.lastReceived) >= client->keepAliveInterval)) {

            if (client->pingOutStanding == 0) {
                if (Socket_noPendingWrites(client->net.socket)) {
                    if (MQTTPacket_send_pingreq(&client->net, client->clientID) != TCPSOCKET_COMPLETE) {
                        LOG("Error to send PINREQ for client %s in %s-%d", client->clientID, __FILE__, __LINE__);
                        MQTTProtocol_closeSession(client, 1);
                    } else {
                        client->net.lastSent = now;
                        client->pingOutStanding = 1;
                    }
                }
            } else {
                MQTTProtocol_closeSession(client, 1);
            }
        }
    }
}

/**
* 尝试重发消息
*/
static void MQTTProtocol_retries(time_t now, Clients* client, int regardless) {
    ListElement* outCurrent = NULL;
    if (!regardless && client->retryInterval <= 0) {
        return;
    }

    while (client && ListNextElement(client->outboundMsgs, &outCurrent) &&
            client->connected && client->good && Socket_noPendingWrites(client->net.socket)) {
        Messages* m = (Messages*)(outCurrent->content);
        if (regardless || difftime(now, m->lastTouch) > max(client->retryInterval, 10)) {
            if (m->qos == 1 || (m->qos == 2 && m->nextMessageType == PUBREC)) {
                /* 两种情况下再次发送消息
                   1. qos = 1，即消息至少发送一次
                   2. qos = 2, 且下一个消息是PUBREC，如果按正常流程，只要qos=2,nextMessageType就是PUBREC
                */
                Publish publish;
                int rc;
                publish.msgId = m->msgId;
                publish.topic = m->publish->topic;
                publish.payload = m->publish->payload;
                publish.payloadLen = m->publish->payloadLen;
                //publish一个消息
                rc = MQTTPacket_send_publish(&publish, 1, m->qos, m->retain, &client->net, client->clientID);
                if (rc == SOCKET_ERROR) {
                    //如果发布失败，则断开连接
                    client->good = 0;
                    MQTTProtocol_closeSession(client, 1);
                    client = NULL;
                } else {
                    //如果publish消息被打断，则缓存该条消息
                    if (m->qos == 0 && rc == TCPSOCKET_INTERRUPTED) {
                        MQTTProtocol_storeQoS0(client, &publish);
                    }
                    //更新lastTouch值
                    time(&(m->lastTouch));
                }
            } else if (m->qos && m->nextMessageType == PUBCOMP){
                /* 当收到PUBREC消息以后, nextMessageType会变为PUBCOMP*/
                if (MQTTPacket_send_pubrel(m->msgId, 0, &client->net, client->clientID) != TCPSOCKET_COMPLETE) {
                    client->good = 0;
                    MQTTProtocol_closeSession(client, 1);
                    client = NULL;
                } else {
                    time(&(m->lastTouch));
                }
            }
        }
    }

}


void MQTTProtocol_retry(time_t now, int doRetry, int regardless) {
    ListElement* current = NULL;
    ListNextElement(bState->clients, &current);

    while (current) {
        Clients* client = (Clients*)(current->content);
        ListNextElement(bState->clients, &current);
        if (client->connected == 0) {
            continue;
        }
        if (client->good == 0) {
            MQTTProtocol_closeSession(client, 1);
            continue;
        }
        if (Socket_noPendingWrites(client->net.socket) == 0) {
            continue;
        }
        if (doRetry) {
            MQTTProtocol_retries(now, client, regardless);
        }
    }
}


void MQTTProtocol_freeClient(Clients* client) {
    MQTTProtocol_freeMessageList(client->outboundMsgs);
    MQTTProtocol_freeMessageList(client->inboundMsgs);
    ListFree(client->publishMessageQueue);
    free(client->clientID);
    if (client->will) {
        free(client->will->msg);
        free(client->will->topic);
        free(client->will);
    }


#ifdef OPENSSL
    if (client->sslopts) {
        if (client->sslopts->trustStore) {
            free((void*)client->sslopts->trustStore);
        }

        if (client->sslopts->keyStore) {
            free((void*)client->sslopts->keyStore);
        }

        if (client->sslopts->privateKey) {
			      free((void*)client->sslopts->privateKey);
        }

        if (client->sslopts->privateKeyPassword) {
			      free((void*)client->sslopts->privateKeyPassword);
        }

		    if (client->sslopts->enabledCipherSuites) {
			     free((void*)client->sslopts->enabledCipherSuites);
        }

       free(client->sslopts);
    }
#endif

}


/**
 * Empty a message list, leaving it able to accept new messages
 * @param msgList the message list to empty
 */
void MQTTProtocol_emptyMessageList(List* msgList){
	ListElement* current = NULL;

	while (ListNextElement(msgList, &current)){
		Messages* m = (Messages*)(current->content);
		MQTTProtocol_removePublication(m->publish);
	}
	ListEmpty(msgList);
}



void MQTTProtocol_freeMessageList(List* msgList){
	MQTTProtocol_emptyMessageList(msgList);
	ListFree(msgList);
}



char* MQTTStrncpy(char *dest, const char *src, size_t dest_size){
    size_t count = dest_size;
    char *temp = dest;

    if (dest_size < strlen(src)) {
          LOG("MQTTStrncpy error: the src string is truncated %s-%d", __FILE__, __LINE__);
    }

      /* We must copy only the first (dest_size - 1) bytes */
    while (count > 1 && (*temp++ = *src++))
        count--;

    *temp = '\0';

    return dest;
}


/**
* 复制一个字符串
*/
char* MQTTStrdup(const char* src) {
	size_t mLen = strlen(src) + 1;
	char* temp = malloc(mLen);
	MQTTStrncpy(temp, src, mLen);
	return temp;
}
