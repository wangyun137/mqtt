
#ifndef DIM_HASHMAP_H
#define DIM_HASHMAP_H


#include <stdbool.h>
#include <stdlib.h>


#ifdef __cplusplus
extern "C" {
#endif

typedef struct Hashmap Hashmap;

/**
 * 创建一个新的Hashmap, 如果内存分配失败返回NULL
 * @param initialCapacity 初始容量
 * @param hash 计算hash值得函数指针
 * @param equal 判断值相等的equal函数指针
 * @return
 */
Hashmap* hashmapCreate(size_t initialCapacity, int(*hash)(void* key), bool (*equal)(void* keyA, void* keyB));

/**
 * 释放Hashmap相关内存
 * @param map
 */
void hashmapFree(Hashmap* map);


/**
 * 计算由key指向的内存的hash值
 * @param key
 * @param keySize
 * @return
 */
int hashmapHash(void* key, size_t keySize);


/**
 * 将value根据指定的key放入map中
 * @param map
 * @param key
 * @param value
 * @return
 */
void* hashmapPut(Hashmap* map, void* key, void* value);


/**
 * 从map中根据key获取value
 * @param map
 * @param key
 * @return
 */
void* hashmapGet(Hashmap* map, void* key);


/**
 * 判断map中是否包含指定key值得value
 * @param map
 * @param key
 * @return
 */
bool hashmapContainsKey(Hashmap* map, void* key);


/**
 * 获取指定key值得value, 如果value不存在，会返回一个值，同时会通过给定的callback创建一个entry
 * 如果内存分配失败，不会调用回调函数，函数返回NULL, errno置为ENOMEM
 * @param map
 * @param key
 * @param initialValue
 * @param context
 * @return
 */
void* hashmapMemorize(Hashmap* map, void* key, void* (*initialValue)(void* key, void* context), void* context);


/**
 * 从map中删除一个entry
 * @param map
 * @param key
 * @return
 */
void* hashmapRemove(Hashmap* map, void* key);

/**
 * 返回map中entry的数量
 * @param map
 * @return
 */
size_t hashmapSize(Hashmap* map);

/**
 * 遍历map, 在每一个entry上回调传入的callback
 */
void hashmapForEach(Hashmap* map, bool (*callback)(void* key, void* value, void* context), void* context);

/**
 * 多线程上锁
 * @param map
 */
void hashmapLock(Hashmap* map);

/**
 * 多线程解锁
 * @param map
 */
void hashmapUnlock(Hashmap* map);

/**
 * 计算int类型key的hash值
 * @param key
 * @return
 */
int hashmapIntHash(void* key);

/**
 * 比较两个int型key值是否相等
 * @param keyA
 * @param keyB
 * @return
 */
bool hashmapIntEqual(void* keyA, void* keyB);

/**
 * 获取当前容量
 * @param map
 * @return
 */
size_t hashmapCurrentCappacity(Hashmap* map);

/**
 * 返回冲突entry的数量
 * @param map
 * @return
 */
size_t hashmapCountCollisions(Hashmap* map);


void hashmapCollectKeys(Hashmap* map, char*** keys);


#ifdef __cplusplus
}
#endif


#endif //DIM_HASHMAP_H
