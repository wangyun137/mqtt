#ifdef OPENSSL

#include "SocketBuffer.h"
#include "MQTTClient.h"
#include "SSLSocket.h"
#include "Socket.h"
#include "Log.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/crypto.h>


extern Sockets s;

/* 函数声明 */
int     SSLSocket_error(char* aString, SSL* ssl, int sock, int rc);
char*   SSL_get_verify_result_string(int rc);
void    SSL_CTX_info_callback(const SSL* ssl, int where, int ret);
char*   SSLSocket_get_version_string(int version);
void    SSL_CTX_msg_callback(
		    int write_p,
		    int version,
		    int content_type,
		    const void* buf, size_t len,
		    SSL* ssl, void* arg);
int     pem_passwd_cb(char* buf, int size, int rwflag, void* userdata);
int     SSL_create_mutex(ssl_mutex_type* mutex);
int     SSL_lock_mutex(ssl_mutex_type* mutex);
int     SSL_unlock_mutex(ssl_mutex_type* mutex);
void    SSL_destroy_mutex(ssl_mutex_type* mutex);

#if (OPENSSL_VERSION_NUMBER >= 0x010000000)
extern void SSLThread_id(CRYPTO_THREADID *id);
#else
extern unsigned long SSLThread_id(void);
#endif

extern void SSLLocks_callback(int mode, int n, const char* file, int line);
int     SSLSocket_createContext(NetworkHandles* net, MQTTClient_SSLOptions* opts);
void    SSLSocket_destroyContext(NetworkHandles* net);
void    SSLSocket_addPendingRead(int sock);

static ssl_mutex_type* sslLocks = NULL;
static ssl_mutex_type sslCoreMutex;


/* 函数实现 */

/**
* 获取具体的错误
* @param aString 错误发生时的错误字符串
* @param ssl SSL结构体指针
* @param sock 发生错误的soket的文件标识符
* @param rc
* @return 具体的TCP错误码
*/
int SSLSocket_error(char* aString, SSL* ssl, int sock, int rc) {
    int error;
    if (ssl) {
        error = SSL_get_error(ssl, rc);
    } else {
        error = ERR_get_error();
    }

    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        LOG("SSLSocket error WANT_READ/WANT_WRITE");
    } else {
        static char buf[120];
        if (strcmp(aString, "shutdown") != 0) {
            LOG("SSLSocket error %s %d in %s for socket %d rc %d errno %d %s",
                buf, error, aString, sock, rc, errno, strerror(errno));
        }
        ERR_print_erros_fp(stderr);
        if (error == SSL_ERROR_SSL || error == SSL_ERROR_SYSCALL) {
            error = SSL_FATAL;
        }
    }
    return error;
}

static struct {
    int code;
    char* string;
} X509_message_table[] =
{
  { X509_V_OK, "X509_V_OK" },
  { X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT, "X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT" },
  { X509_V_ERR_UNABLE_TO_GET_CRL, "X509_V_ERR_UNABLE_TO_GET_CRL" },
  { X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE, "X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE" },
  { X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE, "X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE" },
  { X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY, "X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY" },
  { X509_V_ERR_CERT_SIGNATURE_FAILURE, "X509_V_ERR_CERT_SIGNATURE_FAILURE" },
  { X509_V_ERR_CRL_SIGNATURE_FAILURE, "X509_V_ERR_CRL_SIGNATURE_FAILURE" },
  { X509_V_ERR_CERT_NOT_YET_VALID, "X509_V_ERR_CERT_NOT_YET_VALID" },
  { X509_V_ERR_CERT_HAS_EXPIRED, "X509_V_ERR_CERT_HAS_EXPIRED" },
  { X509_V_ERR_CRL_NOT_YET_VALID, "X509_V_ERR_CRL_NOT_YET_VALID" },
  { X509_V_ERR_CRL_HAS_EXPIRED, "X509_V_ERR_CRL_HAS_EXPIRED" },
  { X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD, "X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD" },
  { X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD, "X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD" },
  { X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD, "X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD" },
  { X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD, "X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD" },
  { X509_V_ERR_OUT_OF_MEM, "X509_V_ERR_OUT_OF_MEM" },
  { X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT, "X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT" },
  { X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN, "X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN" },
  { X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY, "X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY" },
  { X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE, "X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE" },
  { X509_V_ERR_CERT_CHAIN_TOO_LONG, "X509_V_ERR_CERT_CHAIN_TOO_LONG" },
  { X509_V_ERR_CERT_REVOKED, "X509_V_ERR_CERT_REVOKED" },
  { X509_V_ERR_INVALID_CA, "X509_V_ERR_INVALID_CA" },
  { X509_V_ERR_PATH_LENGTH_EXCEEDED, "X509_V_ERR_PATH_LENGTH_EXCEEDED" },
  { X509_V_ERR_INVALID_PURPOSE, "X509_V_ERR_INVALID_PURPOSE" },
  { X509_V_ERR_CERT_UNTRUSTED, "X509_V_ERR_CERT_UNTRUSTED" },
  { X509_V_ERR_CERT_REJECTED, "X509_V_ERR_CERT_REJECTED" },
  { X509_V_ERR_SUBJECT_ISSUER_MISMATCH, "X509_V_ERR_SUBJECT_ISSUER_MISMATCH" },
  { X509_V_ERR_AKID_SKID_MISMATCH, "X509_V_ERR_AKID_SKID_MISMATCH" },
  { X509_V_ERR_AKID_ISSUER_SERIAL_MISMATCH, "X509_V_ERR_AKID_ISSUER_SERIAL_MISMATCH" },
  { X509_V_ERR_KEYUSAGE_NO_CERTSIGN, "X509_V_ERR_KEYUSAGE_NO_CERTSIGN" },
  { X509_V_ERR_UNABLE_TO_GET_CRL_ISSUER, "X509_V_ERR_UNABLE_TO_GET_CRL_ISSUER" },
  { X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION, "X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION" },
  { X509_V_ERR_KEYUSAGE_NO_CRL_SIGN, "X509_V_ERR_KEYUSAGE_NO_CRL_SIGN" },
  { X509_V_ERR_UNHANDLED_CRITICAL_CRL_EXTENSION, "X509_V_ERR_UNHANDLED_CRITICAL_CRL_EXTENSION" },
  { X509_V_ERR_INVALID_NON_CA, "X509_V_ERR_INVALID_NON_CA" },
  { X509_V_ERR_PROXY_PATH_LENGTH_EXCEEDED, "X509_V_ERR_PROXY_PATH_LENGTH_EXCEEDED" },
  { X509_V_ERR_KEYUSAGE_NO_DIGITAL_SIGNATURE, "X509_V_ERR_KEYUSAGE_NO_DIGITAL_SIGNATURE" },
  { X509_V_ERR_PROXY_CERTIFICATES_NOT_ALLOWED, "X509_V_ERR_PROXY_CERTIFICATES_NOT_ALLOWED" },
  { X509_V_ERR_INVALID_EXTENSION, "X509_V_ERR_INVALID_EXTENSION" },
  { X509_V_ERR_INVALID_POLICY_EXTENSION, "X509_V_ERR_INVALID_POLICY_EXTENSION" },
  { X509_V_ERR_NO_EXPLICIT_POLICY, "X509_V_ERR_NO_EXPLICIT_POLICY" },
  { X509_V_ERR_UNNESTED_RESOURCE, "X509_V_ERR_UNNESTED_RESOURCE" },
#if defined(X509_V_ERR_DIFFERENT_CRL_SCOPE)
  { X509_V_ERR_DIFFERENT_CRL_SCOPE, "X509_V_ERR_DIFFERENT_CRL_SCOPE" },
  { X509_V_ERR_UNSUPPORTED_EXTENSION_FEATURE, "X509_V_ERR_UNSUPPORTED_EXTENSION_FEATURE" },
  { X509_V_ERR_PERMITTED_VIOLATION, "X509_V_ERR_PERMITTED_VIOLATION" },
  { X509_V_ERR_EXCLUDED_VIOLATION, "X509_V_ERR_EXCLUDED_VIOLATION" },
  { X509_V_ERR_SUBTREE_MINMAX, "X509_V_ERR_SUBTREE_MINMAX" },
  { X509_V_ERR_UNSUPPORTED_CONSTRAINT_TYPE, "X509_V_ERR_UNSUPPORTED_CONSTRAINT_TYPE" },
  { X509_V_ERR_UNSUPPORTED_CONSTRAINT_SYNTAX, "X509_V_ERR_UNSUPPORTED_CONSTRAINT_SYNTAX" },
  { X509_V_ERR_UNSUPPORTED_NAME_SYNTAX, "X509_V_ERR_UNSUPPORTED_NAME_SYNTAX" },
#endif
};

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#endif

/**
* 获取ssl校验结果
*/
char* SSL_get_verify_result_string(int rc) {
    int i;
    char* result = "undef";

    for (i = 0; i < ARRAY_SIZE(X509_message_table); ++i) {
        if (X509_message_table[i].code == rc) {
            result = X509_message_table[i].string;
            break;
        }
    }
    return result;
}


void SSL_CTX_info_callback(const SSL* ssl, int where, int ret) {
    if (where & SSL_CB_LOOP) {
        LOG("SSL state %s:%s:%s", (where & SSL_ST_CONNECT) ? "connect" : (where & SSL_ST_CONNECT) ? "accept" : "undef",
            SSL_state_string_long(ssl), SSL_get_cipher_name(ssl));
    } else if (where & SSL_CB_EXIT) {
        LOG("SSL  %s:%s", (where & SSL_ST_CONNECT) ? "connect" : (where & SSL_ST_CONNECT) ? "accept" : "undef",
            SSL_state_string_long(ssl));
    } else if (where & SSL_CB_ALERT) {
        LOG("SSL alert %s:%s:%s", (where & SSL_CB_READ) ? "read" : "write",
            SSL_alert_type_string_long(ret), SSL_alert_desc_String_long(ret));
    } else if (where & SSL_CB_HANDSHAKE_START) {
        LOG("SSL handshake started %s:%s:%s", (where & SSL_CB_READ) ? "read" : "write",
            SSL_alert_type_string_long(ret), SSL_alert_desc_String_long(ret));
    } else if (where & SSL_CB_HANDSHAKE_DONE) {
        LOG("SSL handshake done %s:%s:%s", (where & SSL_CB_READ) ? "read" : "write",
            SSL_alert_type_string_long(ret), SSL_alert_desc_String_long(ret));
        LOG("SSL certification verification %s", SSL_get_verify_result_string(SSL_get_verify_result(ssl)));
    } else  {
        LOG("SSL state %s:%s:%s", SSL_state_string_long(ssl), SSL_alert_type_string_long(ret), SSL_alert_desc_string_long(ret));
    }
}

/**
* 获取SSL版本
*/
char* SSLSocket_get_version_string(int version) {
    int i;
    //生命周期和源程序相等
    static char buf[20];
    char* result = NULL;
    static struct {
        int code;
        char* string;
    } version_string_table[] =
    {
        { SSL2_VERSION, "SSL 2.0" },
        { SSL3_VERSION, "SSL 3.0" },
        { TLS1_VERSION, "TLS 1.0" },
#ifdef TLS2_VERSION
        { TLS2_VERSION, "TLS 1.1" },
#endif
#ifdef TLS3_VERSION
        { TLS3_VERSION, "TLS 1.2" },
#endif
    };

    for (i = 0; i < ARRAY_SIZE(version_string_table); ++i) {
        if (version_string_table[i].code == version) {
            result = version_string_table[i].string;
            break;
        }
    }
    if (result == NULL) {
        sprintf(buf, "%i", version);
        result = buf;
    }
    return result;
}

void SSL_CTX_msg_callback(int write_p, int version, int content_type, const void* buf,
        size_t len, SSL* ssl, void* arg) {

    LOG("SSL_CTX_msg_callback %s %s %d bufLen %d", (write_p ? "sent" : "received"),
        SSLSocket_get_version_string(version),
        content_type, (int)len);
}

int pem_passwd_cb(char* buf, int size, int rwflag, void* userdata) {
    int rc = 0;
    if (!rwflag) {
        strncpy(buf, (char*)(userdata), size);
        buf[size-1] = '\0';
        rc = (int)strlen(buf);
    }
    return rc;
}

/**
* 创建ssl mutex
*/
int SSL_create_mutex(ssl_mutex_type* mutex) {
    int rc = 0;
    rc = pthread_mutex_init(mutex, NULL);
    return rc;
}

/**
* 对mutex上锁
*/
int SSL_lock_mutex(ssl_mutex_type* mutex) {
    int rc = -1;
    if ((rc = pthread_mutex_lock(mutex)) == 0) {
        rc = 0;
    }
    return rc;
}

/**
* 释放mutex
*/
void SSL_destroy_mutex(ssl_mutex_type* mutex) {
    int rc = 0;
    rc = pthread_mutex_destroy(mutex);
}

#if (OPENSSL_VERSION_NUMBER >= 0x10000000)
extern void SSLThread_id(CRYPTO_THREADID *id) {
    CRYPTO_THREADID_set_numeric(id, (unsigned long)pthread_self());
}
#else
extern unsigned long SSLThread_id(void) {
    return (unsigned long)pthread_self();
}
#endif

extern void SSLLocks_callback(int mode, int n, const char* file, int line) {
    if (sslLocks) {
        if (mode & CRYPTO_LOCK) {
            SSL_lock_mutex(&sslLocks[n]);
        } else {
            SSL_unlock_mutex(&sslLocks[n]);
        }
    }
}

/**
* 初始化SSL Library
*/
int SSLSocket_initialize(void) {
    int rc = 0;
    int i;
    int lockMemSize;

    //初始化SSL libray
    if ((rc = SSL_library_init()) != 1) {
        rc = -1;
    }

    ERR_load_crypto_strings();
    SSL_load_error_strings();

    /* OpenSSL 0.9.8o和1.0.0a以及以后版本在SSL_library_init()中新增了SHA2算法
    如果想要在旧版本中使用SHA2算法，应当调用OpenSSL_add_all_algorithms()*/
    OpenSSL_add_all_algorithms();

    lockMemSize = CRYPTO_num_locks() * sizeof(ssl_mutex_type);
    sslLocks = malloc(lockMemSize);
    if (!sslLocks) {
        rc = -1;
        goto exit;
    } else {
        memset(sslLocks, 0x00, lockMemSize);
    }

    for (i = 0; i < CRYPTO_num_locks(); i++) {
        SSL_create_mutex(&sslLocks[i]);
    }

#if (OPENSSL_VERSION_NUMBER >= 0x010000000)
    CRYPTO_THREADID_set_callback(SSLThread_id);
#else
    CRYPTO_set_id_callback(SSLThread_id);
#endif

    CRYPTO_set_locking_callback(SSLLocks_callback);
    SSL_create_mutex(&sslCoreMutex);

exit:
    return rc;
}

void SSLSocket_terminate(void) {
    EVP_cleanup();
    ERR_free_strings();
    CRYPTO_set_locking_callback(NULL);
    if (sslLocks) {
        int i = 0;
        for (i = 0; i < CRYPTO_num_locks(); i++) {
            SSL_destroy_mutex(&sslLocks[i]);
        }
        free(sslLocks);
    }
}

int SSLSocket_createContext(NetworkHandles* net, MQTTClient_SSLOptions* opts) {
    int rc = 1;
    const char* ciphers = NULL;

    if (net->ctx == NULL) {
        //创建一个新的SSL_CTX用来建立TLS/SSL连接
        if ((net->ctx = SSL_CTX_new(SSLv23_client_method())) == NULL) {
            SSLSocket_error("SSL_CTX_new", NULL, net->socket, rc);
            goto exit;
        }
    }

    //keystore中包含证书链文件以及私钥信息
    if (opts->keyStore) {
        int rc1 = 0;
        //加载完整证书链和私钥到SSL_CTX和SSL对象中，这个证书是需要发送给对方进行验证的证书，其中包含证书的公钥
        if ((rc = SSL_CTX_use_certificate_chain_file(net->ctx, opts->keyStore)) != 1) {
            SSLSocket_error("SSL_CTX_use_certificate_chain_file", NULL, net->socket, rc);
            goto free_ctx;
        }

        if (opts->privateKey == NULL) {
            opts->privateKey = opts->keyStore;
        }

        if (opts->privateKeyPassword != NULL) {
            SSL_CTX_set_default_passwd_cb(net->ctx, pem_passwd_cb);
            SSL_CTX_set_default_passwd_cb_userdata(net->ctx, (void*)opts->privateKeyPassword);
        }
        //加载自己的私钥, SSL_FILETYPE_PEM指定索要加载的公约证书的文件编码类型为Base64
        rc1 = SSL_CTX_use_PrivateKey_file(net->ctx, opts->privateKey, SSL_FILETYPE_PEM);
        if (opts->privateKey == opts->keyStore) {
            opts->privateKey = NULL;
        }
        if (rc1 != 1) {
            SSLSocket_error("SSL_CTX_use_PrivateKey_file", NULL, net->socket, rc);
            goto free_ctx;
        }
    }

    //trustStore中包含证书
    if (opts->trustStore) {
        //加载CA证书
        if ((rc = SSL_CTX_load_verify_locations(net->ctx, opts->trustStore, NULL)) != 1 ) {
            SSLSocket_error("SSL_CTX_load_verify_locations", NULL, net->socket, rc);
            goto free_ctx;
        }
    } else if ((rc = SSL_CTX_set_default_verify_paths(net->ctx)) != 1) {
        SSLSocket_error("SSL_CTX_set_default_verify_paths", NULL, net->socket, rc);
        goto free_ctx;
    }

    //设置支持的加密算法列表
    if (opts->enabledCipherSuites == NULL) {
        ciphers = "DEFAULT";
    } else {
        ciphers = opts->enabledCipherSuites;
    }
    if ((rc = SSL_CTX_set_cipher_list(net->ctx, ciphers)) != 1) {
        SSLSocket_error("SSL_CTX_set_cipher_list", NULL, net->socket, rc);
        goto free_ctx;
    }

    SSL_CTX_set_mode(net->ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    goto exit;

free_ctx:
    SSL_CTX_free(net->ctx);
    net->ctx = NULL;

exit:
    return rc;
}

/*
* 绑定socket到SSL中
*/
int SSLSocket_setSocketForSSL(NetworkHandles* net, MQTTClient_SSLOptions* opts, char* hostname) {
    int rc = 1;
    if (net->ctx != NULL || (rc = SSLSocket_createContext(net, opts)) == 1) {
        int i;
        //可以获取SSL对象的状态信息
        SSL_CTX_set_info_callback(net->ctx, SSL_CTX_info_callback);
        //可以获取SSL/TLS协议消息，如握手消息
        SSL_CTX_set_msg_callback(net->ctx, SSL_CTX_msg_callback);
        if (opts->enableServerCertAuth) {
            //指明服务端也要进行验证,即客户端服务端进行双向验证
            SSL_CTX_set_verify(net->ctx, SSL_VERIFY_PEER, NULL);
        }
        net->ssl = SSL_new(net->ctx);

        for (i=0; ; i++) {
            const char* cipher = SSL_get_cipher_list(net->ssl, i);
            if (cipher == NULL) {
                break;
            }
            LOG("SSL cipher available: %d:%s", i, cipher);
        }
        if ((rc = SSL_set_fd(net->ssl, net->socket)) != 1) {
            SSLSocket_error("SSL_set_fd", net->ssl, net->socket, rc);
        }

        if ((rc = SSL_set_tlsext_host_name(net->ssl, hostname)) != 1) {
            SSLSocket_error("SSL_set_tlsext_host_name", NULL, net->socket, rc);
        }
    }

    return rc;
}

/**
* 连接SSL
*/
int SSLSocket_connect(SSL* ssl, int sock) {
    int rc = 0;
    rc = SSL_connect(ssl);
    if (rc != 1) {
        int error;
        error = SSLSocket_error("SSL_connect", ssl, sock, rc);
        if (error == SSL_FATAL) {
            rc = error;
        }
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            rc = TCPSOCKET_INTERRUPTED;
        }
    }
    return rc;
}

/**
* 从socket中读取一个字节
*/
int SSLSocket_getChar(SSL* ssl, int socket, char* c) {
    int rc = SOCKET_ERROR;
    //如果从缓存中读取了字符，直接返回
    if ((rc = SocketBuffer_getQueuedChar(socket, c)) != SOCKET_BUFFER_INTERRUPTED) {
        goto exit;
    }
    /*SSL_read返回值大于0,代表成功读取，返回值是读取的字节数,<=0则表明读取错误*/
    if ((rc = SSL_read(ssl, c, (size_t)1)) < 0) {
        int err = SSLSocket_error("SSL_read - getch", ssl, socket, rc);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            rc = TCPSOCKET_INTERRUPTED;
            SocketBuffer_interrupted(socket, 0);
        }
    } else if (rc == 0 ) {
        rc = SOCKET_ERROR;
    } else if (rc == 1) {
        SocketBuffer_queueChar(socket, *c);
        rc = TCPSOCKET_COMPLETE;
    }
exit:
    return rc;
}

/**
* 非阻塞的从socket中读取指定数目的字节，如果之前的读操作未完成，重新获取数据
* @param socket 目标socket
* @param bytes 希望读取的byte数目
* @param actual_len 实际读取的byte数目
* @return 读取的内容
*/
char* SSLSocket_getData(SSL* ssl, int socket, size_t bytes, size_t* actual_len) {
    int rc;
    char* buf;
    if (bytes == 0) {
        buf = SocketBuffer_complete(socket);
        goto exit;
    }
    //如果之前的读操作未完成，获取之前的数据, 并将新读取的数据拼接在后面
    buf = SocketBuffer_getQueuedData(socket, bytes, actual_len);
    if ((rc = SSL_read(ssl, buf + (*actual_len), (int)(bytes - (*actual_len)))) < 0) {
        rc = SSLSocket_error("SSL_read - getdata", ssl, socket, rc);
        if (rc != SSL_ERROR_WANT_READ && rc != SSL_ERROR_WANT_WRITE) {
            buf = NULL;
            goto exit;
        }
    } else if (rc == 0) {
        buf = NULL;
        goto exit;
    } else {
        *actual_len += rc;
    }

    if (*actual_len == bytes) {
        //如果实际读取的字节数跟预期要读的字节数一致
        SocketBuffer_complete(socket);
        /* 即使已经读取了整个packet, 仍然会有数据存留在SSL缓存中，所以这里需要检查一下SSL 缓存中
        是否还有数据，如果有，将这个socket加入pending SSL reads list */
        if (SSL_pending(ssl) > 0) {
            SSLSocket_addPendingRead(socket);
        }
    } else {
        //并没有读取完整的数据包
        SocketBuffer_interrupted(socket, *actual_len);
    }
exit:
    return buf;
}


void SSLSocket_destroyContext(NetworkHandles* net) {
    if (net->ctx) {
        SSL_CTX_free(net->ctx);
    }
    net->ctx = NULL;
}

/**
* 关闭TLS/SSL连接，并释放SSL_CTX, SSL的内存
*/
int SSLSocket_close(NetworkHandles* net) {
    int rc = 1;
    if (net->ssl) {
        //关闭TLS/SSL连接
        rc = SSL_shutdown(net->ssl);
        SSL_free(net->ssl);
        net->ssl = NULL;
    }
    SSLSocket_destroyContext(net);
    return rc;
}

/**
* iobuf定义在SocketBuffer.h，实际是struct iovec
* struct iovec {
*   void* iov_base; 指向缓冲区，这个缓冲区是存放readv所接收数据或writv将要发送数据
*   size_t iov_len; 接收的最大长度以及实际写入的长度
* };
* readv和writev函数用于在一次函数调用中读，写多个非连续缓冲区
* OpenSSL并没有提供类似SSL_writev()函数
*/
int SSLSocket_putDatas(SSL* ssl, int socket, char* buf0, size_t buf0len, int count, char** buffers, size_t* buflens, int* frees) {
    int rc = 0;
    int i;
    char *ptr;
    iobuf iovec;
    int sslerror;

    iovec.iov_len = (ULONG)buf0len;
    for (i=0; i < count; i++) {
        iovec.iov_len += (ULONG)buflens[i];
    }
    //分配iov_len大的缓冲区
    ptr = iovec.iov_base = (char *)malloc(iovec.iov_len);
    //先将buf0的内容复制到缓冲区
    memcpy(ptr, buf0, buf0len);
    ptr += buf0len;
    //依次将buffers中的内容逐个复制到缓冲区中
    for (i = 0; i < count; i++) {
        memcpy(ptr, buffers[i], buflens[i]);
        ptr += buflens[i];
    }
    SSL_lock_mutex(&sslCoreMutex);
    if ((rc = SSL_write(ssl, iovec.iov_base, iovec.iov_len)) == iovec.iov_len) {
        rc = TCPSOCKET_COMPLETE;
    } else {
        sslerror = SSLSocket_error("SSL_write", ssl, socket, rc);
        if (sslerror == SSL_ERROR_WANT_WRITE) {
            int* sockmem = (int*)malloc(sizeof(int));
            int free = 1;

            SocketBuffer_pendingWrite(socket, ssl, 1, &iovec, &free, iovec.iov_len, 0);
            *sockmem = socket;
            //s.write_pending是一个List, sockmem是content
            ListAppend(s.write_pending, sockmem, sizeof(int));
            //将socket这个文件描述符加入s.pendingWriteSet中
            FD_SET(socket, &(s.pendingWriteSet));
            rc = TCPSOCKET_INTERRUPTED;
        } else {
            rc = SOCKET_ERROR;
        }
    }
    SSL_unlock_mutex(&sslCoreMutex);

    if (rc != TCPSOCKET_INTERRUPTED) {
        free(iovec.iov_base);
    } else {
        int i;
        free(buf0);
        for (i = 0; i < count; ++i) {
            if (free[i]) {
                free(buffers[i]);
            }
        }
    }

    return rc;
}


static List pending_reads = {NULL, NULL, NULL, 0, 0};

void SSLSocket_addPendingRead(int sock) {
    if (ListFindItem(&pending_reads, &sock, intCompare) == NULL) {
        //如果在pending_reads中不存在sock，将sock添加至pending_reads
        int* psock = (int*)malloc(sizeof(sock));
        *psock = sock;
        ListAppend(&pending_reads, psock, sizeof(sock));
    } else {
        LOG("SSLSocket_addPendingRead: socket %d already in the list", sock);
    }
}

/**
* 获取pending read list中的第一个socket
*/
int SSLSocket_getPendingRead(void) {
    int sock = -1;
    if (pending_reads.count > 0) {
        sock = *(int*)(pending_reads.first->content);
        ListRemoveHead(&pending_reads);
    }
    return sock;
}

/**
* pendingWrite定义在SocketBuffer.h
*/
int SSLSocket_continueWrite(pendingWrite* pw) {
    int rc = 0;
    if ((rc = SSL_write(pw->ssl, pw->iovecs[0].iov_base, pw->iovecs[0].iov_len)) == pw->iovecs[0].iov_len) {
        free(pw->iovecs[0].iov_base);
        rc = 1;
    } else {
        int sslError = SSLSocket_error("SSL_write", pw->ssl, pw->socket, rc);
        if (sslError == SSL_ERROR_WANT_WRITE) {
            rc = 0;
        }
    }
    return rc;
}



#endif //OPENSSL
