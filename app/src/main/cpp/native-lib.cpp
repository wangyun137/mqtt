#include <jni.h>
#include <string>

#include <openssl/aes.h>

extern "C"
JNIEXPORT jstring JNICALL
Java_com_le_ledim_MainActivity_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {

    AES_KEY ak;
    unsigned char data[16];
    int i = 0;
    for (i = 0; i < 16; i++) {
        data[i] = 32 + i;
    }
    AES_set_encrypt_key(data, 128, &ak);

    std::string hello = "Hello from C++";
    return env->NewStringUTF(hello.c_str());
}
