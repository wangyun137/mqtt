#ifndef DIM_MQTT_STACK_TRACE_H_
#define DIM_MQTT_STACK_TRACE_H_

#include <stdio.h>
#include "Thread.h"

void StackTrace_entry(const char* name, int line);
void StackTrace_exit(const char* name, int line, void* return_value);

void StackTrace_printStack(FILE* dest);
char* StackTrace_get(pthread_t);

#endif //DIM_MQTT_STACK_TRACE_H_
