#include <memory.h>
#include "Hashmap.h"
#include "MQTTClientPersistence.h"

#ifndef NO_PERSISTENCE


static int str_hash(void* key);

static bool str_icase_equals(void* keyA, void* keyB);


int meOpen(void** handle, const char* clientId, const char* serverURI, void* context) {
    int rc = 0;
    Hashmap* map = hashmapCreate(256, str_hash, str_icase_equals);
    *handle = map;
    return rc;
}

static int str_hash(void *key) {
    return hashmapHash(key, strlen(key));
}

/** Test if two string keys are equal ignoring case */
static bool str_icase_equals(void *keyA, void *keyB) {
    return strcasecmp(keyA, keyB) == 0;
}

int mePut(void* handle, char* key, int bufCount, char* buffers[], int bufLens[]) {
    int rc = 0;
    Hashmap* map = handle;
    int i;
    int valueLen;
    char* value;

    if (map == NULL) {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        goto exit;
    }

    for (i = 0 ; i < bufCount; i++) {
        valueLen += bufLens[i];
    }

    value = (char *)malloc(valueLen + 1);
    if (value == NULL) {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        goto exit;
    }

    memset(value, '\0', valueLen + 1);
    for (i = 0; i < bufCount; i++) {
        memcpy(value, buffers[i], bufLens[i]);
        value += bufLens[i];
    }

    char* keyDup = strdup(key);
    hashmapPut(map, keyDup, value);

exit:
    return rc;
}


int meGet(void* handle, char* key, char** buffer, int* bufLen) {
    int rc = 0;
    Hashmap* map = handle;
    char* value;

    if (map == NULL) {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        goto exit;
    }

    value = hashmapGet(map, key);
    if (value != NULL) {
        *buffer = value;
        *bufLen = strlen(value);
    } else {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
    }

exit:
    return rc;
}


int meRemove(void* handle, char* key) {
    int rc = 0;
    Hashmap* map = handle;

    if (map == NULL) {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        goto exit;
    }

    char* value = hashmapRemove(map, key);
    if (value == NULL) {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
    }

exit:
    return rc;

}


static bool removeStrToStr(void* key, void* value, void* context) {
    Hashmap* map = context;
    hashmapRemove(map, key);
    free(key);
    free(value);
    return true;
}

int meClose(void* handle) {
    int rc = 0;
    Hashmap* map = handle;

    if (map == NULL) {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        goto exit;
    }

    hashmapForEach(map, removeStrToStr, map);
    free(map);

exit:
    return rc;
}


int meContainsKey(void* handle, char* key) {
    int rc = 0;
    Hashmap* map = handle;

    if (map == NULL) {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        goto exit;
    }

    if (hashmapContainsKey(map, key)) {
        rc = 1;
    }

exit:
    return rc;
}


int meClear(void* handle) {
    int rc = 0;
    Hashmap* map = handle;

    if (map == NULL) {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        goto exit;
    }

    //just clear key-value
    hashmapForEach(map, removeStrToStr, map);

exit:
    return rc;
}

int meKeys(void* handle, char*** keys, int* nKeys) {
    int rc = 0;
    Hashmap* map = handle;

    hashmapCollectKeys(map, keys);
    *nKeys = hashmapSize(map);

    return rc;
}


#endif
