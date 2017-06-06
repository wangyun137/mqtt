
#include <stdlib.h>

#include "MQTTProtocolOut.h"
#include "Heap.h"
#include "Log.h"
#include "Clients.h"

extern ClientStates* bState;


/**
* 拼接host和port到一个字符串中
*/
char* MQTTProtocol_addressPort(const char* uri, int* port) {
    //查找:在uri中从左开始最后出现的位置，如果有返回:及之后的字符
    char* colon_pos = strrchr(uri, ':');
    char* buf = (char*)uri;
    size_t len;

    if (uri[0] == '[') {
        //ip v6
        if (colon_pos < strrchr(uri, ']')) {
            //意味着这是一个ipv6,并不是典型的host:port
            colon_pos = NULL;
        }
    }

    if (colon_pos) {
        //指向同一个数组的两个不同元素指针可以相减
        //colon_pos - uri等于host的长度
        size_t addr_len = colon_pos - uri;
        buf = malloc(addr_len + 1);
        *port = atoi(colon_pos + 1);
        MQTTStrncpy(buf, uri, addr_len + 1);
    } else {
        *port = DEFAULT_PORT;
    }

    len = strlen(buf);
    if (buf[len - 1] == ']') {
        buf[len - 1] = '\0';
    }

    return buf;
}


/**
* 连接MQTT
*/
#ifdef OPENSSL
int MQTTProtocol_connect(const char* ip_address, Clients* aClient, int ssl, int mqttVersion)
#else
int MQTTProtocol_connect(const char* ip_address, Clients* aClient, int mqttVersion)
#endif
{
    int rc, port;
    char* addr;

    aClient->good = 1;
    addr = MQTTProtocol_addressPort(ip_address, &port);
    rc = Socket_new(addr, port, &(aClient->net.socket));

    if (rc == EINPROGRESS || rc == EWOULDBLOCK) {
        aClient->connectState = CONNECT_STATE_WAIT_FOR_TCP_COMPLETE;
    } else if (rc == 0) {
        #ifdef OPENSSL
        if (ssl) {
            //绑定socket到SSL中
            if (SSLSocket_setSocketForSSL(&aClient->net, aClient->sslopts, addr) == 1) {
                //连接SSL
                rc = SSLSocket_connect(aClient->net.ssl);
                if (rc == -1) {
                    aClient->connectState = CONNECT_STATE_WAIT_FOR_SSL_COMPLETE;//SSL connect被调用，等待完成
                }
            }
        }
        #endif

        if (rc == 0) {
            //正式发送MQTT connect数据包
            if ((rc = MQTTPacket_send_connect(aClient, mqttVersion)) == 0) {
                aClient->connectState = CONNECT_STATE_WAIT_FOR_CONNACK; //MQTT connect已发送，等待CONNACK响应
            } else {
                aClient->connectState = CONNECT_STATE_NONE;
            }
        }
    }
    //判断是不是同一个指针, 不是同一个指针，则释放addr
    if (addr != ip_address) {
        free(addr);
    }

    return rc;
}


/**
* 处理接收到的ping响应
*/
int MQTTProtocol_handlePingresps(void* pack, int sock) {
    Clients* client = NULL;
    int rc = TCPSOCKET_COMPLETE;

    client = (Clients*)(ListFindItem(bState->clients, &sock, clientSocketCompare)->content);
    LOG("handle ping resp client is %s in %s-%d", client->clientID, __FILE__, __LINE__);
    client->pingOutStanding = 0;
    return rc;
}


/**
* 发送subscribe请求
*/
int MQTTProtocol_subscribe(Clients* client, List* topics, List* qoss, int msgID) {
    int rc = 0;
    rc = MQTTPacket_send_subscribe(topics, qoss, msgID, 0, &client->net, client->clientID);
    return rc;
}

/**
* 处理接收到的suback响应
*/
int MQTTProtocol_handleSubacks(void* pack, int sock) {
    Suback* suback = (Suback*)pack;
    Clients* client = NULL;
    int rc = TCPSOCKET_COMPLETE;

    client = (Clients*)(ListFindItem(bState->clients, &sock, clientSocketCompare)->content);
    LOG("handle sub ack client is %s in %s-%d", client->clientID, __FILE__, __LINE__);
    MQTTPacket_freeSuback(suback);
    return rc;
}

/**
* 发送unsubscribe请求
*/
int MQTTProtocol_unsubscribe(Clients* client, List* topics, int msgID) {
    int rc = 0;

    rc = MQTTPacket_send_unsubscribe(topics, msgID, 0, &client->net, client->clientID);
    return rc;
}

/**
* 处理unsuback响应
*/
int MQTTProtocol_handleUnsubacks(void* pack, int sock) {
    Unsuback* unsuback =(Unsuback*)pack;
    Clients* client = NULL;
    int rc = TCPSOCKET_COMPLETE;

    client = (Clients*)(ListFindItem(bState->clients, &sock, clientSocketCompare)->content);
    LOG("handle unsub ack client is %s in %s-%d", client->clientID, __FILE__, __LINE__);
    free(unsuback);
    return rc;
}
