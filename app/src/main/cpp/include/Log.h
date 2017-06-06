
#ifndef DIM_LOG_H
#define DIM_LOG_H

#include <android/log.h>

#define TAG "MQTT"
#define DEBUG

#ifdef __cplusplus
extern "C" {
#endif


#ifdef DEBUG

#define LOG(...) \
    __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#else
#define LOG(...) \
    do {} while (0)
#endif



#ifdef __cplusplus
}
#endif

#endif //DIM_LOG_H
