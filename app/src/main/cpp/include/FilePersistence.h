
#ifndef DIM_PERSISTENCE_DEFAULT_H
#define DIM_PERSISTENCE_DEFAULT_H

#define MESSAGE_FILENAME_LENGTH 8
#define MESSAGE_FILENAME_EXTENSION ".msg"

int pstOpen(void** handle, const char* clientID, const char* serverURI, void* context);

int pstClose(void* handle);

int pstPut(void* handle, char* key, int bufCount, char* buffers[], int bufLens[]);

int pstGet(void* handle, char* key, char** buffer, int* bufLen);

int pstRemove(void* handle, char* key);

int pstKeys(void* handle, char*** keys, int* nKeys);

int pstClear(void* handle);

int pstContainsKey(void* handle, char* key);

int pstMkdir(char *pPathname);

#endif //DIM_PERSISTENCE_DEFAULT_H
