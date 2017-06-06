
#ifndef DIM_MQTT_CLIENT_PERSISTENCE_H
#define DIM_MQTT_CLIENT_PERSISTENCE_H


/**
 * 指明使用默认的基于文件的persistence机制
 */
#define MQTT_CLIENT_PERSISTENCE_DEFAULT 0


/**
 * 指明不使用persistence机制
 */
#define MQTT_CLIENT_PERSISTENCE_NONE 1

/**
 * 指明使用应用指定的persistence机制
 */
#define MQTT_CLIENT_PERSISTENCE_USER 2

/**
 * 指明基于内存的persistence
 */
#define MQTT_CLIENT_PERSISTENCE_MEMORY 3

/**
 * 当应用的persistence函数出现错误时，返回此错误码
 */
#define MQTT_CLIENT_PERSISTENCE_ERROR -2


/**
 * 初始化persistence
 * @return 0: 成功
 */
typedef int (*Persistence_open)(void** handle, const char* clientID, const char* serverURI, void* context);


/**
 * 关闭persistence
 * @return 0: 成功
 */
typedef int (*Persistence_close)(void* handle);

/**
 * 存放指定的数据到persistence
 * @return 0: 成功
 */
typedef int (*Persistence_put)(void* handle, char* key, int bufCount, char* buffers[], int bufLens[]);

/**
 * 从persistence获取数据
 * @param handle
 * @param key
 * @param buffer 如果成功获取到key对应的数据，存储到buffer中
 */
typedef int (*Persistence_get)(void* handle, char* key, char** buffer, int* bufLen);

/**
 * 从persistence中删除数据
 */
typedef int (*Persistence_remove)(void* handle, char* key);

/**
 * 返回persistence存储的所有key
 * @param keys 指向所有key
 * @param nKeys 所有key的数量
 */
typedef int (*Persistence_keys)(void* handle, char*** keys, int* nkeys);

/**
 * 清空persistence存储
 */
typedef int (*Persistence_clear)(void* handle);

/**
 * 查看是否存在指定的key对应的数据
 */
typedef int (*Persistence_containsKey)(void* handle, char* key);

typedef struct {
    void*               context;
    Persistence_open    pOpen;
    Persistence_close   pClose;
    Persistence_put     pPut;
    Persistence_get     pGet;
    Persistence_remove  pRemove;
    Persistence_keys    pKeys;
    Persistence_clear   pClear;
    Persistence_containsKey pContainsKey;
} MQTTClientPersistence;


#endif //DIM_MQTT_CLIENT_PERSISTENCE_H
