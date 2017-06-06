
#include "MQTTPacketOut.h"
#include "Log.h"


/**
 * 发送CONNECT数据包
 */
int MQTTPacket_send_connect(Clients* client, int mqttVersion) {
    char* buf;
    char* ptr;
    int rc = -1;
    int len;
    Connect packet;

    packet.header.byte = 0;
    packet.header.bits.type = CONNECT;


    len = ((mqttVersion == 3) ? 12 : 10) + (int)strlen(client->clientID) + 2;
    if (client->will) {
        len += (int)strlen(client->will->topic) + 2 + (int)strlen(client->will->msg) + 2;
    }

    if (client->username) {
        len += (int)strlen(client->username) + 2;
    }

    if (client->password) {
        len += (int)strlen(client->password) + 2;
    }

    ptr = buf = malloc(len);
    if (mqttVersion == 3) {
        writeUTF(&ptr, "MQIsdp");
        writeChar(&ptr, (char)3);
    } else if (mqttVersion == 4) {
        writeUTF(&ptr, "MQTT");
        writeChar(&ptr, (char)4);
    } else {
        goto exit;
    }


    packet.flags.all = 0;
    packet.flags.bits.cleanStart = client->cleanSession;
    packet.flags.bits.will = (client->will) ? 1 : 0;

    if (packet.flags.bits.will) {
        packet.flags.bits.willQoS = client->will->qos;
        packet.flags.bits.willRetain = client->will->retained;
    }

    if (client->username) {
        packet.flags.bits.username = 1;
    }

    if (client->password) {
        packet.flags.bits.password = 1;
    }
    //写入all
    writeChar(&ptr, packet.flags.all);
    //写入keepAliveInterval
    writeInt(&ptr, client->keepAliveInterval);
    //写入clientID
    writeUTF(&ptr, client->clientID);

    //写入LWT
    if (client->will) {
        writeUTF(&ptr, client->will->topic);
        writeUTF(&ptr, client->will->msg);
    }

    //写入username和password
    if (client->username) {
        writeUTF(&ptr, client->username);
    }
    if (client->password) {
        writeUTF(&ptr, client->password);
    }

    rc = MQTTPacket_send(&client->net, packet.header, buf, len, 1);

exit:
    if (rc != TCPSOCKET_INTERRUPTED) {
        free(buf);
    }
    return rc;
}

/**
 * 生成一个新的CONNACK数据包
 */
void* MQTTPacket_connack(unsigned char aHeader, char* data, size_t dataLen) {
    Connack* pack = malloc(sizeof(Connack));
    char* curData = data;

    pack->header.byte = aHeader;
    pack->flags.all = readChar(&curData);
    pack->rc = readChar(&curData);
    return pack;
}

/**
 * 发送MQTT PINGREQ
 */
int MQTTPacket_send_pingreq(NetworkHandles* net, const char* clientID) {
    Header header;
    int rc = 0;
    size_t bufLen = 0;

    header.byte = 0;
    header.bits.type = PINGREQ;
    rc = MQTTPacket_send(net, header, NULL, bufLen, 0);
    return rc;
}

/**
 * 发送Subscribe数据包
 * @param topics 要订阅的主题列表
 * @param qoss  主题列表对应的qos列表
 * @param msgId
 * @param dup
 * @param net
 * @param clientID
 * @return
 */
int MQTTPacket_send_subscribe(List* topics, List* qoss, int msgId, int dup, NetworkHandles* net, const char* clientID) {
    Header header;
    char* data, *ptr;
    int rc = -1;
    ListElement* element = NULL;
    ListElement* qosElement = NULL;
    int dataLen;

    header.bits.type = SUBSCRIBE;
    header.bits.dup = dup;
    header.bits.qos = 1;
    header.bits.retain = 0;

    dataLen = 2 + topics->count * 3; //每一个utf字符+qos(char)的长度是3
    while (ListNextElement(topics, &element)) {
        dataLen += (int)strlen((char*)(element->content));
    }
    ptr = data = malloc(dataLen);

    writeInt(&ptr, msgId);
    element = NULL;
    while (ListNextElement(topics, &element)) {
        ListNextElement(qoss, &qosElement);
        writeUTF(&ptr, (char*)(element->content));
        writeChar(&ptr, *(int*)(qosElement->content));
    }

    rc = MQTTPacket_send(net, header, data, dataLen, 1);
    LOG("send subscribe socket is %d, clientId is %s, msgId is %d in %s-%d",
        net->socket, clientID, msgId, __FILE__, __LINE__);
    if (rc != TCPSOCKET_INTERRUPTED) {
        free(data);
    }
    return rc;
}


/**
 * 生成一个SUBACK数据包
 * @param aHeader
 * @param data
 * @param dataLen
 * @return
 */
void* MQTTPacket_suback(unsigned char aHeader, char* data, size_t dataLen) {
    Suback* pack = malloc(sizeof(Suback));
    char* curData = data;

    pack->header.byte = aHeader;
    pack->msgId = readInt(&curData);
    pack->qoss = ListInitialize();

    while ((size_t)(curData - data) < dataLen) {
        int* newInt;
        newInt = malloc(sizeof(int));
        *newInt = (int)readChar(&curData);
        ListAppend(pack->qoss, newInt, sizeof(int));
    }
    return pack;
}

/**
 * 发送一个Unsubscribe数据包
 * @param topics
 * @param msgId
 * @param dup
 * @param net
 * @param clientId
 * @return
 */
int MQTTPacket_send_unsubscribe(List* topics, int msgId, int dup, NetworkHandles* net, const char* clientId) {
    Header header;
    char* data, *ptr;
    int rc = -1;
    ListElement* element = NULL;
    int dataLen;

    header.bits.type = UNSUBSCRIBE;
    header.bits.dup = dup;
    header.bits.qos = 1;
    header.bits.retain = 0;

    dataLen = 2 + topics->count * 2;//utf字符长度 = 2
    while (ListNextElement(topics, &element)) {
        dataLen += (int)strlen((char*)(element->content));
    }
    ptr = data = malloc(dataLen);

    writeInt(&ptr, msgId);
    element = NULL;
    while (ListNextElement(topics, &element)) {
        writeUTF(&ptr, (char *)(element->content));
    }

    rc = MQTTPacket_send(net, header, data, dataLen, 1);
    LOG("send unsubscribe socket is %d, clientId is %s, msgId is %d in %s-%d",
        net->socket, clientId, msgId, __FILE__, __LINE__);
    if (rc != TCPSOCKET_INTERRUPTED) {
        free(data);
    }
    return rc;
}

