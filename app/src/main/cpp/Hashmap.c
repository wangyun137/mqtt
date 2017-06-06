#include "Hashmap.h"
#include "Thread.h"
#include <assert.h>


typedef struct Entry {
    void*           key; //key值
    int             hash; //hash值
    void*           value; //对应的value
    struct Entry*   next;
} Entry;

struct Hashmap {
    Entry**         buckets;
    size_t          bucketCount;
    int             (*hash)(void* key);
    bool            (*equals)(void* keyA, void* keyB);
    pthread_mutex_t lock;
    size_t          size;
};

Hashmap* hashmapCreate(size_t initialCapacity, int (*hash)(void* key), bool (*equals)(void* keyA, void* keyB)) {

    assert(hash != NULL);
    assert(equals != NULL);

    Hashmap* map = malloc(sizeof(Hashmap));
    if (map == NULL) {
        return NULL;
    }

    //0.75 load factor
    size_t minimumBucketCount = initialCapacity * 4 / 3;
    map->bucketCount = 1;
    while (map->bucketCount <= minimumBucketCount) {
        // Bucket数量必须是2的n次方
        map->bucketCount <<= 1;
    }
    //calloc分配的内存都是经过初始化的，全是0
    map->buckets = calloc(map->bucketCount, sizeof(Entry*));
    if (map->buckets == NULL) {
        free(map);
        return NULL;
    }

    map->size = 0;
    map->hash = hash;
    map->equals = equals;

    pthread_mutex_init(&map->lock, NULL);

    return map;
}


/**
 * 计算给定key值得hash值
 * @param map
 * @param key
 * @return
 */
static inline int hashKey(Hashmap* map, void* key) {
    int h = map->hash(key);

    h += ~(h << 9);
    h ^= (((unsigned int) h) >> 14);
    h += (h << 4);
    h ^= (((unsigned int) h) >> 10);

    return h;
}

size_t hashmapSize(Hashmap* map) {
    return map->size;
}

/**
 * 根据给定的hash值，计算索引位置
 * @param bucketCount
 * @param hash
 * @return
 */
static inline size_t calculateIndex(size_t bucketCount, int hash) {
    return ((size_t) hash) & (bucketCount - 1);
}


static void expandIfNecessary(Hashmap* map) {
    //扩容因子是0.75
    if (map->size > (map->bucketCount * 3 / 4)) {
        //扩容两倍
        size_t newBucketCount = map->bucketCount << 1;
        Entry** newBuckets = calloc(newBucketCount, sizeof(Entry*));
        if (newBuckets == NULL) {
            return;
        }

        size_t i;
        for (i = 0; i < map->bucketCount; i++) {
            /**
             * 遍历原有的buckets,
             */
            Entry* entry = map->buckets[i];
            while (entry != NULL) {
                Entry* next = entry->next;
                size_t index = calculateIndex(newBucketCount, entry->hash);
                entry->next = newBuckets[i];
                newBuckets[index] = entry;
                entry = next;
            }
        }

        free(map->buckets);
        map->buckets = newBuckets;
        map->bucketCount = newBucketCount;
    }
}


void hashmapLock(Hashmap* map) {
    pthread_mutex_lock(&map->lock);
}


void hashmapUnlock(Hashmap* map) {
    pthread_mutex_unlock(&map->lock);
}


void hashmapFree(Hashmap* map) {
    size_t i;
    for (i = 0; i < map->bucketCount; i++) {
        Entry* entry = map->buckets[i];
        while (entry != NULL) {
            Entry* next = entry->next;
            free(entry);
            entry = next;
        }
    }
    free(map->buckets);
    pthread_mutex_destroy(&map->lock);
    free(map);
}

int hashmapHash(void* key, size_t keySize) {
    int h = keySize;
    char* data = (char*) key;
    size_t i;
    for (i = 0; i < keySize; i++) {
        h = h * 31 + *data;
        data++;
    }
    return h;
}

static Entry* createEntry(void* key, int hash, void* value) {
    Entry* entry = malloc(sizeof(Entry));
    if (entry == NULL) {
        return NULL;
    }

    entry->key = key;
    entry->hash = hash;
    entry->value = value;
    entry->next = NULL;
    return entry;
}


static inline bool equalKeys(void* keyA, int hashA, void* keyB, int hashB, bool (*equals)(void*, void*)) {
    if (keyA == keyB) {
        return true;
    }

    if (hashA != hashB) {
        return false;
    }

    return equals(keyA, keyB);
}

/**
 * 如果已经有有对应key的value, 则替换旧value，并返回旧value
 * @param map
 * @param key
 * @param value
 * @return
 */
void* hashmapPut(Hashmap* map, void* key, void* value) {
    //首先计算key的hash值
    int hash = hashKey(map, key);
    size_t index = calculateIndex(map->bucketCount, hash);

    Entry** p = &(map->buckets[index]);
    while (true) {
        Entry* current = *p;

        //添加一个新Entry
        if (current == NULL) {
            *p = createEntry(key, hash, value);
            if (*p == NULL) {
                errno = ENOMEM;
                return NULL;
            }
            map->size++;
            expandIfNecessary(map);
            return NULL;
        }

        //替换已有entry, 只有在key值完全相同时才会替换，否则不替换，应当视为hash冲突
        if (equalKeys(current->key, current->hash, key, hash, map->equals)) {
            void* oldValue = current->value;
            current->value = value;
            return oldValue;
        }

        p = &current->next;
    }
}

void* hashmapGet(Hashmap* map, void* key) {
    int hash = hashKey(map, key);
    size_t index = calculateIndex(map->bucketCount, hash);

    Entry* entry = map->buckets[index];
    while (entry != NULL) {
        if (equalKeys(entry->key, entry->hash, key, hash, map->equals)) {
            return entry->value;
        }
        entry = entry->next;
    }
    return NULL;
}

bool hashmapContainsKey(Hashmap* map, void* key) {
    int hash = hashKey(map, key);
    size_t index = calculateIndex(map->bucketCount, hash);

    Entry* entry = map->buckets[index];
    while (entry != NULL) {
        if (equalKeys(entry->key, entry->hash, key, hash, map->equals)) {
            return true;
        }
        entry = entry->next;
    }
    return false;
}

void* hashmapMemorize(Hashmap* map, void* key, void* (*initialValue)(void* key, void* context), void* context) {
    int hash = hashKey(map, key);
    size_t index = calculateIndex(map->bucketCount, hash);

    Entry** p = &(map->buckets[index]);
    while (true) {
        Entry* current = *p;

        //添加一个新entry
        if (current == NULL) {
            *p = createEntry(key, hash, NULL);
            if (*p == NULL) {
                errno = ENOMEM;
                return NULL;
            }

            void* value = initialValue(key, context);
            (*p)->value = value;
            map->size++;
            expandIfNecessary(map);
            return value;
        }

        //返回已存在的value
        if (equalKeys(current->key, current->hash, key, hash, map->equals)) {
            return current->value;
        }

        p = &current->next;
    }
}

void* hashmapRemove(Hashmap* map, void* key) {
    int hash = hashKey(map, key);
    size_t index = calculateIndex(map->bucketCount, hash);

    Entry** p = &(map->buckets[index]);
    Entry* current;
    while ((current = *p) != NULL) {
        if (equalKeys(current->key, current->hash, key, hash, map->equals)) {
            void* value = current->value;
            *p = current->next;
            free(current);
            map->size--;
            return value;
        }

        p = &current->next;
    }

    return NULL;
}



void hashmapForEach(Hashmap* map, bool (*callback)(void* key, void* value, void* context), void* context) {
    size_t i;
    for (i = 0; i < map->bucketCount; i++) {
        Entry* entry = map->buckets[i];
        while (entry != NULL) {
            Entry* next = entry->next;
            if (!callback(entry->key, entry->value, context)) {
                return;
            }
            entry = next;
        }
    }
}

size_t hashmapCurrentCappacity(Hashmap* map) {
    size_t bucketCount = map->bucketCount;
    return bucketCount * 3 / 4;
}


size_t hashmapCountCollosions(Hashmap* map) {
    size_t collisions = 0;
    size_t i;
    for (i = 0; i < map->bucketCount; i++) {
        Entry* entry = map->buckets[i];
        while (entry != NULL) {
            if (entry->next != NULL) {
                collisions++;
            }
            entry = entry->next;
        }
    }
    return collisions;
}


int hashmapIntHash(void* key) {
    return *((int*) key);
}


bool hashmapInEquals(void* keyA, void* keyB) {
    int a = *((int*) keyA);
    int b = *((int*) keyB);
    return a == b;
}


void hashmapCollectKeys(Hashmap* map, char*** keys) {
    size_t i;
    size_t j = 0;

    char** tempKeys = (char **)malloc(map->size * sizeof(char *));

    for (i = 0; i< map->bucketCount; i++) {
        Entry* entry = map->buckets[i];
        while (entry != NULL) {
            Entry* next = entry->next;
            tempKeys[j] = strdup(entry->key);
            entry = next;
            j++;
        }
    }

    *keys = tempKeys;
}
























