#include "MQTTPacket.h"
#include "Log.h"

#ifndef NO_PERSISTENCE
#include "MQTTPersistence.h"
#endif


#ifndef min
#define min(A,B) ((A) < (B) ? (A):(B))
#endif


static const char* packet_names[] = {
  "RESERVED",
  "CONNECT",
  "CONNACK",
  "PUBLISH",
  "PUBACK",
  "PUBREC",
  "PUBREL",
  "PUBCOMP",
  "SUBSCRIBE",
  "SUBACK",
  "UNSUBSCRIBE",
  "UNSUBACK",
  "PINGREQ",
  "PINGRESP",
  "DISCONNECT"
};


const char** MQTTClient_packet_names = packet_names;

const char* MQTTPacket_name(int ptype) {
    return (ptype >= 0 && ptype <= DISCONNECT) ? packet_names[ptype] : "UNKNOWN";
}

/* 函数数组， 对应不同类型的*/
pf new_packets[] =
{
	NULL,	                /**< reserved */
	NULL,	                /**< MQTTPacket_connect*/
	MQTTPacket_connack,     /**< CONNACK */
	MQTTPacket_publish,	    /**< PUBLISH */
	MQTTPacket_ack,         /**< PUBACK */
	MQTTPacket_ack,         /**< PUBREC */
	MQTTPacket_ack,         /**< PUBREL */
	MQTTPacket_ack,         /**< PUBCOMP */
	NULL,                   /**< MQTTPacket_subscribe*/
	MQTTPacket_suback,      /**< SUBACK */
	NULL,                   /**< MQTTPacket_unsubscribe*/
	MQTTPacket_ack,         /**< UNSUBACK */
	MQTTPacket_header_only, /**< PINGREQ */
	MQTTPacket_header_only, /**< PINGRESP */
	MQTTPacket_header_only  /**< DISCONNECT */
};


static char* readUTFLen(char** pptr, char* end_data, int* len);
static int MQTTPacket_send_ack(int type, int msgId, int dup, NetworkHandles *net);

/**
 * 从socket中读取一个MQTT数据包
 */
void* MQTTPacket_Factory(NetworkHandles* net, int* error) {
    char* data = NULL;
    static Header header;
    size_t remaining_length;
    int packetType;
    void* pack = NULL;
    size_t actual_len = 0;

    *error = SOCKET_ERROR;

    /* 先从socket中读取一个字节, MQTT消息有一个字节的固定头部信息*/
#ifdef OPENSSL
    *error = (net->ssl) ? SSLSocket_getChar(net->ssl, net->socket, &header.byte) : Socket_getChar(net->socket, &header.byte);
#else
    *error = Socket_getChar(net->socket, &header.byte);
#endif

    if (*error != TCPSOCKET_COMPLETE) {
        goto exit;
    }
    //获取remaining_length从而了解到还有多少字节需要读取
    if ((*error = MQTTPacket_decode(net, &remaining_length)) != TCPSOCKET_COMPLETE) {
        goto exit;
    }

    //再读取剩余的数据
#ifdef OPENSSL
    data = (net->ssl) ? SSLSocket_getData(net->ssl, net->socket, remaining_length, &actual_len) :
                          Socket_getData(net->socket, remaining_length, &actual_len);
#else
    data = Socket_getData(net->socket, remaining_length, &actual_len);
#endif

    if (data == NULL) {
        *error = SOCKET_ERROR;
        goto exit;
    }

    if (actual_len != remaining_length) {
        *error = TCPSOCKET_INTERRUPTED;
    } else {
        //从头部信息中获取当前数据包类型
        packetType = header.bits.type;
        if (packetType < CONNECT || packetType > DISCONNECT || new_packets[packetType] == NULL) {
            LOG("packet type is %d", packetType);
        } else {
            //根据数据包类型调用对应的回调函数
            if ((pack = (*new_packets[packetType])(header.byte, data, remaining_length)) == NULL) {
                *error = BAD_MQTT_PACKET;
            }
#ifndef NO_PERSISTENCE
            else if (header.bits.type == PUBLISH && header.bits.qos == 2) {
                int buf0len;
                char* buf = malloc(10);
                buf[0] = header.byte;
                buf0len = 1 + MQTTPacket_encode(&buf[1], remaining_length);
                *error = MQTTPersistence_put(net->socket, buf, buf0len, 1, &data,
                    &remaining_length, header.bits.type, ((Publish*)pack)->msgId, 1);
                free(buf);
            }
#endif
        }
    }
    if (pack) {
        time(&(net->lastReceived));
    }

exit:
    return pack;

}


/**
 * 发送一个MQTT数据包
 * @param socket 目标socket
 * @param header MQTT头部信息
 * @param buffer 要发送的内容
 * @param bufLen 发送内容的长度
 * @return 结果码
 */
int MQTTPacket_send(NetworkHandles* net, Header header, char* buffer, size_t bufLen, int freed) {
    int rc;
    size_t buf0len;
    char* buf;

    buf = malloc(10);
    buf[0] = header.byte;
    buf0len = 1 + MQTTPacket_encode(&buf[1], bufLen);

#ifndef NO_PERSISTENCE
    if (header.bits.type == PUBREL) {
        char* ptr_aux = buffer;
        int msgId = readInt(&ptr_aux);
        rc = MQTTPersistence_put(net->socket, buf, buf0len, 1, &buffer,
                                 &bufLen, header.bits.type, msgId, 0);
    }
#endif

    /* 写入头部和剩余数据，这里buffer数据就一个*/
#ifdef OPENSSL
    if (net->ssl) {
        rc = SSLSocket_putDatas(net->ssl, net->socket, buf, buf0len, 1, &buffer, &bufLen, &freed);
    } else
#endif
    {
        rc = Socket_putDatas(net->socket,buf, buf0len, 1, &buffer, &bufLen, &freed);
    }

    if (rc == TCPSOCKET_COMPLETE) {
        time(&(net->lastSent));
    }

    if (rc != TCPSOCKET_INTERRUPTED) {
        free(buf);
    }

    return rc;
}


/**
 * 将多个缓冲区内容作为一个MQTT数据包发送
 * @param socket 目标socket
 * @param header 头部信息
 * @param count buffer数量
 * @param buffers buffer数组
 * @param bufLens 包含每一个buffer长度的数组
 * @return
 */
int MQTTPacket_sends(NetworkHandles* net, Header header, int count, char** buffers, size_t* bufLens, int* frees) {
    int i, rc;
    size_t buf0len, total = 0;
    char* buf;

    buf = malloc(10);
    buf[0] = header.byte;
    for (i = 0; i < count; i++) {
        total += bufLens[i];
    }
    buf0len = 1 + MQTTPacket_encode(&buf[1], total);

#ifndef NO_PERSISTENCE
    if (header.bits.type == PUBLISH && header.bits.qos != 0) {
        char* ptr_aux = buffers[2];
        int msgId = readInt(&ptr_aux);
        rc = MQTTPersistence_put(net->socket, buf, buf0len, count, buffers, bufLens,
          header.bits.type, msgId, 0);
    }
#endif

#ifdef OPENSSL
    if (net->ssl) {
        rc = SSLSocket_putDatas(net->ssl, net->socket, buf, buf0len, count, buffers, bufLens, frees);
    } else
#endif
    {
        rc = Socket_putDatas(net->socket, buf, buf0len, count, buffers, bufLens, frees);
    }

    if (rc == TCPSOCKET_COMPLETE) {
        time(&(net->lastSent));
    }

    if (rc != TCPSOCKET_INTERRUPTED) {
        free(buf);
    }
    return rc;
}


/**
 * 根据MQTT协议对传入的内容进行编码
 */
int MQTTPacket_encode(char* buf, size_t length) {
    int rc = 0;
    do {
        char d = length % 128;
        length /= 128;
        if (length > 0) {
            d |= 0x80;
        }
        buf[rc++] = d;
    } while (length > 0);

    return rc;
}

/**
 * 根据MQTT协议对传入的字符串进行解码
 */
int MQTTPacket_decode(NetworkHandles* net, size_t* value) {
    int rc = SOCKET_ERROR;
    char c;
    int multiplier = 1;
    int len = 0;
#define MAX_NO_OF_REMAINING_LENGTH_BYTES 4

    *value = 0;
    do {
        if (++len > MAX_NO_OF_REMAINING_LENGTH_BYTES) {
            rc = SOCKET_ERROR;
            goto exit;
        }
#ifdef OPENSSL
        rc = (net->ssl) ? SSLSocket_getChar(net->ssl, net->socket, &c) : Socket_getChar(net->socket, &c);
#else
        rc = Socket_getChar(net->socket, &c);
#endif

        if (rc != TCPSOCKET_COMPLETE) {
            goto exit;
        }
        *value += (c & 127) * multiplier;
        multiplier *= 128;
    } while ((c & 128) != 0);

exit:
    return rc;
}

/**
 * 从输入的buffer中将两个字节转换为一个int值
 */
int readInt(char** pptr) {
    char* ptr = *pptr;
    int len = 256 * ((unsigned char)(*ptr)) + (unsigned char)(*(ptr + 1));
    *pptr += 2;
    return len;
}

/**
 * 从buffer中读取一个UTF字符串
 */
static char* readUTFLen(char** pptr, char* end_data, int* len) {
    char* string = NULL;
    if (end_data - (*pptr) > 1) {
        *len = readInt(pptr);
        if (&(*pptr)[*len] <= end_data) {
            string = malloc(*len + 1);
            memcpy(string, *pptr, *len);
            string[*len] = '\0';
            *pptr += *len;
        }
    }
    return string;
}


char* readUTF(char**  pptr, char* end_data) {
    int len;
    return readUTFLen(pptr, end_data, &len);
}

/**
 * 从buffer中读取一个字节
 */
unsigned char readChar(char** pptr) {
    unsigned char c = **pptr;
    (*pptr)++;
    return c;
}

/**
 * 向buffer中写入一个字节
 */
void writeChar(char** pptr, char c) {
    **pptr = c;
    (*pptr)++;
}


/**
 * 向buffer写入两个字节的int值
 */
void writeInt(char** pptr, int anInt) {
    **pptr = (char)(anInt / 256);
    (*pptr)++;
    **pptr = (char)(anInt %256);
    (*pptr)++;
}

/**
 * 向buffer中写入一个UTF字符串
 */
void writeUTF(char** pptr, const char* string) {
    size_t len = strlen(string);
    writeInt(pptr, (int)len);
    memcpy(*pptr, string, len);
    *pptr += len;
}

void* MQTTPacket_header_only(unsigned char aHeader, char* data, size_t datalen){
	static unsigned char header = 0;
	header = aHeader;
	return &header;
}


/**
 * 发送disconnect数据包
 */
int MQTTPacket_send_disconnect(NetworkHandles* net, const char* clientId) {
    Header header;
    int rc = 0;

    header.byte = 0;
    header.bits.type = DISCONNECT;
    rc = MQTTPacket_send(net, header, NULL, 0, 0);
    LOG("send disconnect client id is %s", clientId);
    return rc;
}


/**
 * 创建publish数据包
 * @param aHeader MQTT Header
 * @param data 数据包内容
 * @param data_len 数据报内容长度
 * @return 指向生成的packet结构体的指针
 */
void* MQTTPacket_publish(unsigned char aHeader, char* data, size_t dataLen) {
    Publish* pack = malloc(sizeof(Publish));
    char* curData = data;
    char* endData = &data[dataLen];
    pack->header.byte = aHeader;
    if ((pack->topic = readUTFLen(&curData, endData, &pack->topicLen)) == NULL) {
        free(pack);
        pack = NULL;
        goto exit;
    }
    if (pack->header.bits.qos > 0) {
        pack->msgId = readInt(&curData);
    } else {
        pack->msgId = 0;
    }

    pack->payload = curData;
    pack->payloadLen = (int)(dataLen - (curData - data));

exit:
    return pack;
}

void MQTTPacket_freePublish(Publish* pack) {
    if (pack->topic != NULL) {
        free(pack->topic);
    }
    free(pack);
}


/**
 * 发送一个MQTT acknowledgement数据包
 * @param type 数据包类型，如 SUBACK
 * @param msgId
 * @param dup boolean - MQTT DUP flag
 * @param net
 * @return
 */
static int MQTTPacket_send_ack(int type, int msgId, int dup, NetworkHandles* net) {
    Header header;
    int rc;
    char* buf = malloc(2);
    char* ptr = buf;

    header.byte = 0;
    header.bits.type = type;
    header.bits.dup = dup;

    if (type == PUBREL) {
        header.bits.qos = 1;
    }
    writeInt(&ptr, msgId);
    if ((rc = MQTTPacket_send(net, header, buf, 2, 1)) != TCPSOCKET_INTERRUPTED) {
        free(buf);
    }
    return rc;
}

/**
 * 发送PUBACK
 */
int MQTTPacket_send_puback(int msgId, NetworkHandles* net, const char* clientId) {
    int rc = 0;

    rc = MQTTPacket_send_ack(PUBACK, msgId, 0, net);
    LOG("send puback clientId is %s, msgId is %d", clientId, msgId);
    return rc;
}

void MQTTPacket_freeSuback(Suback* pack) {
    if (pack->qoss != NULL) {
        ListFree(pack->qoss);
    }
    free(pack);
}


/**
 * 发送PUBREC
 */
int MQTTPacket_send_pubrec(int msgId, NetworkHandles* net, const char* clientId) {
    int rc = 0;
    rc = MQTTPacket_send_ack(PUBREC, msgId, 0, net);
    LOG("send pubrec clientId is %s, msgId is %d", clientId, msgId);
    return rc;
}

/**
 * 发送PUBREL
 */
int MQTTPacket_send_pubrel(int msgId, int dup, NetworkHandles* net, const char* clientId) {
    int rc = 0;
    rc = MQTTPacket_send_ack(PUBREL, msgId, dup, net);
    LOG("send pubrel clientId is %s, msgId is %d", clientId, msgId);
    return rc;
}


/**
 * 发送PUBCOMP
 */
int MQTTPacket_send_pubcomp(int msgId, NetworkHandles* net, const char* clientId) {
    int rc = 0;
    rc = MQTTPacket_send_ack(PUBCOMP, msgId, 0, net);
    LOG("send pubcomp clientId is %s, msgId is %d", clientId, msgId);
    return rc;
}


/**
 * 传建一个Acknowledgement packet
 */
void* MQTTPacket_ack(unsigned char aHeader, char* data, size_t dataLen) {
    Ack* pack = malloc(sizeof(Ack));
    char* cur_data = data;

    pack->header.byte = aHeader;
    pack->msgId = readInt(&cur_data);
    return pack;
}


/**
 * 发送一个MQTT Publish数据包
 */
int MQTTPacket_send_publish(Publish* pack, int dup, int qos, int retained, NetworkHandles* net, const char* clientId) {
    Header header;
    char* topicLen;
    int rc = -1;

    topicLen = malloc(2);

    header.bits.type = PUBLISH;
    header.bits.dup = dup;
    header.bits.qos = qos;
    header.bits.retain = retained;

    if (qos > 0) {
        /* 如果qos > 0, 在topic后也要加两个字节, 这两个字节是msgId*/
        char* buf = malloc(2);
        char* ptr = buf;
        char* buffers[4] = {
                topicLen,
                pack->topic,
                buf,
                pack->payload
        };
        size_t lens[4] = {2, strlen(pack->topic), 2, pack->payloadLen};
        int frees[4] = {1, 0, 1, 0};

        /* 设置buf, buf为msgId*/
        writeInt(&ptr, pack->msgId);
        ptr = topicLen;
        /* 设置topicLen, lens[1]对应的就是topic*/
        writeInt(&ptr, (int)lens[1]);
        rc = MQTTPacket_sends(net, header, 4, buffers, lens, frees);
        if (rc != TCPSOCKET_INTERRUPTED) {
            free(buf);
        }
    } else {
        char* ptr = topicLen;
        char* buffers[3] = {
                topicLen,
                pack->topic,
                pack->payload
        };
        size_t lens[3] = {2, strlen(pack->topic), pack->payloadLen};
        int frees[3] = {1, 0, 0};

        /* 设置topicLen, lens[1]对应的就是topic*/
        writeInt(&ptr, (int)lens[1]);
        rc = MQTTPacket_sends(net, header, 3, buffers, lens, frees);
    }

    if (rc != TCPSOCKET_INTERRUPTED) {
        free(topicLen);
    }
    return rc;
}


/**
 * 释放一个MQTT packet的内存
 * @param pack
 */
void MQTTPacket_free_packet(MQTTPacket* pack) {
    if (pack->header.bits.type == PUBLISH) {
        MQTTPacket_freePublish((Publish*)pack);
    } else {
        free(pack);
    }
}



// end
