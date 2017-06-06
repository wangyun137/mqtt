#ifndef DIM_MQTT_THREAD_H_
#define DIM_MQTT_THREAD_H_

#include "MQTTClient.h"

#include <pthread.h>

typedef void* (*thread_fn)(void*);
typedef struct { pthread_cond_t cond; pthread_mutex_t mutex; } cond_type_struct;
typedef cond_type_struct* cond_type;

#include <semaphore.h>
typedef sem_t* sem_type;

typedef struct {
    pthread_mutex_t     lock;
    int                 hasTls;
    pthread_key_t       tls;
} thread_store_t;

#define THREAD_STORE_INITIALIZER { PTHREAD_MUTEX_INITIALIZER, 0, 0};

typedef void (*thread_store_destruct_t)(void* value);

extern void* thread_store_get(thread_store_t* store);

extern void  thread_store_set(thread_store_t* store,
                                void*  value,
                                thread_store_destruct_t destroy);


cond_type Thread_create_cond(void);

int Thread_signal_cond(cond_type);

int Thread_wait_cond(cond_type condVar, int timeout);

int Thread_destroy_cond(cond_type);

pthread_t Thread_start(thread_fn, void*);

pthread_mutex_t* Thread_create_mutex();

int Thread_lock_mutex(pthread_mutex_t*);

int Thread_unlock_mutex(pthread_mutex_t*);

void Thread_destroy_mutex(pthread_mutex_t*);

pthread_t Thread_getId();

sem_type Thread_create_sem(void);

int Thread_wait_sem(sem_type sem, int timeout);

int Thread_check_sem(sem_type sem);

int Thread_post_sem(sem_type sem);

int Thread_destroy_sem(sem_type sem);


#endif //DIM_MQTT_THREAD_H_
