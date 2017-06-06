#include "Thread.h"

#include <unistd.h>
#include <stdlib.h>


void*  thread_store_get( thread_store_t* store) {
    if (!store->hasTls) {
        return NULL;
    }

    return pthread_getspecific(store->tls);
}

void thread_store_set(thread_store_t* store,
                             void*  value,
                             thread_store_destruct_t destroy) {

    pthread_mutex_lock( &store->lock);
    if (!store->hasTls) {
        if (pthread_key_create( &store->tls, destroy) != 0) {
            pthread_mutex_unlock(&store->lock);
            return;
        }
        store->hasTls = 1;
    }
    pthread_mutex_unlock(&store->lock);
    pthread_setspecific( store->tls, value);
}


/**
* 开启一个新线程
*/
pthread_t Thread_start(thread_fn fn, void* parameter) {
    pthread_t thread = 0;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, fn, parameter) != 0)
        thread = 0;
    pthread_attr_destroy(&attr);
    return thread;
}

/**
* 创建一个mutex
*/
pthread_mutex_t* Thread_create_mutex(void) {
    pthread_mutex_t* mutex = NULL;
    int rc = 0;

    mutex = malloc(sizeof(pthread_mutex_t));
    rc = pthread_mutex_init(mutex, NULL);

    return mutex;
}

/**
* 锁定mutex
*/
int Thread_lock_mutex(pthread_mutex_t* mutex) {
    int rc = -1;
    rc = pthread_mutex_lock(mutex);

    return rc;
}

/**
* 解锁mutex
*/
int Thread_unlock_mutex(pthread_mutex_t* mutex) {
    int rc = -1;
    rc = pthread_mutex_unlock(mutex);
    return rc;
}

/**
* 释放mutex内存
*/
void Thread_destroy_mutex(pthread_mutex_t* mutex) {
    int rc = 0;
    rc = pthread_mutex_destroy(mutex);
    free(mutex);
}

/**
* 获取当前线程id
*/
pthread_t Thread_getId(void) {
    return pthread_self();
}

/**
* 创建一个信号量
*/
sem_type Thread_create_sem(void) {
    sem_type sem = NULL;
    int rc = 0;

    sem = malloc(sizeof(sem_t));
    rc = sem_init(sem, 0, 0);

    return sem;
}

/**
 * 循环递减信号量，如果递减不成功，休眠10秒钟，之后再次尝试递减
 * sem_trywait是非阻塞函数，如果信号量等于0,则无法递减，直接返回EAGAIN错误
 */
int Thread_wait_sem(sem_type sem, int timeout) {
    int rc = -1;
#define USE_TRYWAIT
#ifdef USE_TRYWAIT
    int i = 0;
    int interval = 10000;
    int count = (1000 * timeout) / interval;
#else
    struct timespec ts;
#endif

#ifdef USE_TRYWAIT
    while (++i < count && (rc = sem_trywait(sem)) != 0) {
        if (rc == -1 && ((rc = errno) != EAGAIN)) {
            rc = 0;
            break;
        }
        usleep(interval);
    }
#else
    if (clock_gettime(CLOCK_REALTIME, &ts) != -1) {
        ts.tv_sec += timeout;
        rc = sem_timedwait(sem, &ts);
    }
#endif

    return rc;
}


/**
* 检查传入的信号量是否在没有超时的情况下已被提交，信号量是否大于0
*/
int Thread_check_sem(sem_type sem) {
    int semval = -1;
    sem_getvalue(sem, &semval);
    return semval > 0;
}

/**
* 提交信号量
*/
int Thread_post_sem(sem_type sem) {
    int rc = 0;
    if (sem_post(sem) == -1) {
        rc = errno;
    }
    return rc;
}

/**
* 释放已创建的信号量
*/
int Thread_destroy_sem(sem_type sem) {
    int rc = 0;
    rc = sem_destroy(sem);
    free(sem);
    return rc;
}

/**
* 创建一个Condition
*/
cond_type Thread_create_cond(void) {
    cond_type condVar = NULL;
    int rc = 0;

    condVar = malloc(sizeof(cond_type_struct));
    rc = pthread_cond_init(&condVar->cond, NULL);
    rc = pthread_mutex_init(&condVar->mutex, NULL);

    return condVar;
}

int Thread_signal_cond(cond_type condVar) {
    int rc = 0;
    pthread_mutex_lock(&condVar->mutex);
    rc = pthread_cond_signal(&condVar->cond);
    pthread_mutex_unlock(&condVar->mutex);

    return rc;
}

/**
* 等待一个Condition的超时
*/
int Thread_wait_cond(cond_type condVar, int timeout) {
    int rc = 0;
    struct timespec cond_timeout;
    struct timeval cur_time;

    gettimeofday(&cur_time, NULL);

    cond_timeout.tv_sec = cur_time.tv_sec + timeout;
    cond_timeout.tv_nsec = cur_time.tv_usec * 1000;

    pthread_mutex_lock(&condVar->mutex);
    rc = pthread_cond_timedwait(&condVar->cond, &condVar->mutex, &cond_timeout);
    pthread_mutex_unlock(&condVar->mutex);

    return rc;
}

/**
* 释放Condition
*/
int Thread_destroy_cond(cond_type condVar) {
    int rc = 0;

    rc = pthread_mutex_destroy(&condVar->mutex);
    rc = pthread_cond_destroy(&condVar->cond);
    free(condVar);

    return rc;
}
