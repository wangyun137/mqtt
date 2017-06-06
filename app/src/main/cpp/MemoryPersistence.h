
#ifndef DIM_MEMORY_PERSISTENCE_H
#define DIM_MEMORY_PERSISTENCE_H

int meOpen(void** handle, const char* clientId, const char* serverURI, void* context);

int meClose(void* handle);

int mePut(void* handle, char* key, int bufCount, char* buffers[], int bufLens[]);

int meGet(void* handle, char* key, char** buffer, int* bufLen);

int meRemove(void* handle, char* key);

int meKeys(void* handle, char*** keys, int* nKeys);

int meClear(void* handle);

int meContainsKey(void* handle, char* key);


#endif //DIM_MEMORY_PERSISTENCE_H
