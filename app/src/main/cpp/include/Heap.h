#ifndef DIM_MQTT_HEAP_H_
#define DIM_MQTT_HEAP_H_



#include <stdio.h>
#include <memory.h>
#include <stdlib.h>


#ifdef NO_HEAP_TRACKING

#define malloc(x) myMalloc(__FILE__, __LINE__, x)

#define realloc(a, b) myRealloc(__FILE__, __LINE__, a, b)

#define free(x) myFree(__FILE__, __LINE__, x)

#endif // NO_HEAP_TRACKING


typedef struct {
    size_t current_size;
    size_t max_size;
} heap_info;

void* myMalloc(char*, int, size_t size);

void* myRealloc(char*, int, void* p, size_t size);

void myFree(char*, int, void* p);

void Heap_scan(FILE* file);

int Heap_initialize(void);

void Heap_terminate(void);

heap_info*  Heap_get_info(void);

int HeapDump(FILE* file);

int HeapDumpString(FILE* file, char* str);

void* Heap_findItem(void* p);

void Heap_unlink(char* file, int line, void* p);


#endif //DIM_MQTT_HEAP_H_
