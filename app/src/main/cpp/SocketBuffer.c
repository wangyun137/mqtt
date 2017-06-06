/**
* 提供socket的缓存相关方法
*/

#include "SocketBuffer.h"
#include "LinkedList.h"
#include "StackTrace.h"


/** 默认的输入缓存数据结构*/
static socketQueue* gDefaultQueue;

/** 输入缓存队列 List<socketQueue*>*/
static List* gSocketQueues;

/** write缓存队列 List<pendingWriteBuf*>*/
static List writes;

int socketCompare(void* a, void* b);
void SocketBuffer_newDefQ(void);
void SocketBuffer_freeDefQ(void);
int pending_socketCompare(void* a, void* b);

/**
* 比较两个socket是否相等
*/
int socketCompare(void* a, void* b) {
    return ((socketQueue*)a)->socket == *(int*)b;
}

/**
* 创建并初始化默认的输入缓存队列
*/
void SocketBuffer_newDefQ(void) {
    gDefaultQueue = malloc(sizeof(socketQueue));
    gDefaultQueue->bufLen = 100;
    gDefaultQueue->buf = malloc(gDefaultQueue->bufLen);
    gDefaultQueue->socket = gDefaultQueue->index = 0;
    gDefaultQueue->bufLen = gDefaultQueue->dataLen = 0;
}

/**
* 初始化socketBuffer
*/
void SocketBuffer_initialize(void) {
    SocketBuffer_newDefQ();
    //创建gSocketQueues,并初始化
    gSocketQueues = ListInitialize();
    //将writes初始化为空
    ListZero(&writes);
}

/**
* 释放默认的缓存队列内存
*/
void SocketBuffer_freeDefQ(void) {
    free(gDefaultQueue->buf);
    free(gDefaultQueue);
}

/**
* 释放SocketBuffer的内存
*/
void SocketBuffer_terminate(void) {
    ListElement* cur = NULL;
    ListEmpty(&writes);

    while (ListNextElement(gSocketQueues, &cur)) {
        //遍历gSocketQueues,逐个释放内存
        free(((socketQueue*)(cur->content))->buf);
    }
    ListFree(gSocketQueues);
    SocketBuffer_freeDefQ();
}

/**
* 清空指定socket的所有buffer数据
*/
void SocketBuffer_cleanup(int socket) {
    if (ListFindItem(gSocketQueues, &socket, socketCompare)) {
        //如果在queue中找到了相应的socket节点
        free(((socketQueue*)(gSocketQueues->current->content))->buf);
        ListRemove(gSocketQueues, gSocketQueues->current->content);
    }
    if (gDefaultQueue->socket == socket) {
        gDefaultQueue->socket = gDefaultQueue->index = 0;
        gDefaultQueue->headerLen = gDefaultQueue->dataLen = 0;
    }
}

/**
* 获取指定的socket的queue data
*/
char* SocketBuffer_getQueuedData(int socket, size_t bytes, size_t* actual_len) {
    socketQueue* queue = NULL;
    if (ListFindItem(gSocketQueues, &socket, socketCompare)) {
        /* gSocketQueues中每一个item的content都是socketQueue指针*/
        queue = (socketQueue*)(gSocketQueues->current->content);
        *actual_len = queue->dataLen;
    } else {
        //使用默认的socketQueue
        *actual_len = 0;
        queue = gDefaultQueue;
    }
    if (bytes > queue->dataLen) {
        if (queue->dataLen > 0) {
            /* 重新分配一块bytes大小的内存，将buf中的内容复制到新内存中，
            释放原有buf内存，将buf指向新内存 */
            void* newMem = malloc(bytes);
            memcpy(newMem, queue->buf, queue->dataLen);
            free(queue->buf);
            queue->buf = newMem;
        } else {
            //将buf的内存大小修改为bytes
            queue->buf = realloc(queue->buf, bytes);
        }
        queue->bufLen = bytes;
    }
    return queue->buf;
}

/**
* 从指定的socket中获取一个字符
*/
int SocketBuffer_getQueuedChar(int socket, char* c) {
    int rc = SOCKET_BUFFER_INTERRUPTED;

    if (ListFindItem(gSocketQueues, &socket, socketCompare)) {
        socketQueue* queue = (socketQueue*)(gSocketQueues->current->content);
        if (queue->index < queue->headerLen) {
            *c = queue->fixed_header[(queue->index)++];
            rc = SOCKET_BUFFER_COMPLETE;
            goto exit;
        } else if (queue->index > 4) {
            rc = SOCKET_ERROR;
            goto exit;
        }
    }
exit:
    return rc;
}

/*
* socket的读操作被打断时缓存数据
* @param socket 目标socket
* @param actual_len 实际要读数据的长度
*/
void SocketBuffer_interrupted(int socket, size_t actual_len) {
    socketQueue* queue = NULL;
    if (ListFindItem(gSocketQueues, &socket, socketCompare)) {
        queue = (socketQueue*)(gSocketQueues->current->content);
    } else {
        queue = gDefaultQueue;
        ListAppend(gSocketQueues, gDefaultQueue, sizeof(socketQueue) + gDefaultQueue->bufLen);
        //重新创建默认socketQueue
        SocketBuffer_newDefQ();
    }
    queue->index = 0;
    queue->dataLen = actual_len;
}

/**
* socket的读操作完成，释放queue内存
*/
char* SocketBuffer_complete(int socket) {
    if (ListFindItem(gSocketQueues, &socket, socketCompare)) {
        socketQueue* queue = (socketQueue*)(gSocketQueues->current->content);
        SocketBuffer_freeDefQ();
        gDefaultQueue = queue;
        ListDetach(gSocketQueues, queue);
    }
    gDefaultQueue->socket = gDefaultQueue->index = 0;
    gDefaultQueue->headerLen = gDefaultQueue->dataLen = 0;
    return gDefaultQueue->buf;
}

void SocketBuffer_queueChar(int socket, char c) {
    int error = 0;
    socketQueue* cur_queue = gDefaultQueue;
    if (ListFindItem(gSocketQueues, &socket, socketCompare)) {
        cur_queue = (socketQueue*)(gSocketQueues->current->content);
    } else if (gDefaultQueue->socket == 0) {
        gDefaultQueue->socket = socket;
        gDefaultQueue->index = 0;
        gDefaultQueue->dataLen = 0;
    } else if (gDefaultQueue->socket != socket) {
        error = 1;
    }

    if (cur_queue->index > 4) {
        error = 1;
    }

    if (!error) {
        cur_queue->fixed_header[(cur_queue->index)++] = c;
        cur_queue->headerLen = cur_queue->index;
    }
}

/*
* socket的写操作被中断，需要存储数据
*/
#ifdef OPENSSL
void SocketBuffer_pendingWrite(int socket, SSL* ssl, int count, iobuf* iovecs, int* frees, size_t total, size_t bytes)
#else
void SocketBuffer_pendingWrite(int socket, int count, iobuf* iovecs, int* frees, size_t total, size_t bytes)
#endif
{
    int i = 0;
    pendingWriteBuf* pw = NULL;

    pw = malloc(sizeof(pendingWriteBuf));
    pw->socket = socket;
#ifdef OPENSSL
    pw->ssl = ssl;
#endif
    pw->bytes = bytes;
    pw->total = total;
    pw->count = count;
    for (i = 0; i < count; i++) {
        pw->iovecs[i] = iovecs[i];
        pw->frees[i] = frees[i];
    }
    ListAppend(&writes, pw, sizeof(pw) + total);
}

int pending_socketCompare(void* a, void* b){
	 return ((pendingWriteBuf*)a)->socket == *(int*)b;
}

/**
* 获取socket的writes队列中的数据
*/
pendingWriteBuf* SocketBuffer_getWrite(int socket){
    ListElement* le = ListFindItem(&writes, &socket, pending_socketCompare);
	  return (le) ? (pendingWriteBuf*)(le->content) : NULL;
}

/**
* socket的写操作全部完成,移除writes队列
*/
int SocketBuffer_writeComplete(int socket){
	 return ListRemoveItem(&writes, &socket, pending_socketCompare);
}

/**
* 更新QoS 0消息的queued write data
*/
pendingWriteBuf* SocketBuffer_updateWrite(int socket, char* topic, char* payload){
	 pendingWriteBuf* pw = NULL;
	 ListElement* le = NULL;

	 if ((le = ListFindItem(&writes, &socket, pending_socketCompare)) != NULL){
         pw = (pendingWriteBuf*)(le->content);
         if (pw->count == 4){
             pw->iovecs[2].iov_base = topic;
             pw->iovecs[3].iov_base = payload;
         }
	}
	return pw;
}
