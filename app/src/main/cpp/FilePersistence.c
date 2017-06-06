#ifndef NO_PERSISTENCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

int keysUnix(char *, char ***, int *);
int clearUnix(char *);
int containsKeyUnix(char*, char *);


#include "MQTTClientPersistence.h"
#include "FilePersistence.h"


int pstOpen(void** handle, const char* clientID, const char* serverURI, void* context) {
    int rc = 0;
    char* dataDir = context;
    char* clientDir;
    char* pToken = NULL;
    char* savePtr = NULL;
    char* pCrtDirName = NULL; //将要创建的目录
    char* pTokDirName = NULL; //通过strtok以后的目录
    char* perServerURI = NULL;
    char* ptrAux;

    perServerURI = malloc(strlen(serverURI) + 1);
    strcpy(perServerURI, serverURI);
    while ((ptrAux = strstr(perServerURI, ":")) != NULL) {
        *ptrAux = '-';
    }

    clientDir = malloc(strlen(dataDir) + strlen(clientID) + strlen(perServerURI) + 3);
    sprintf(clientDir, "%s/%s-%s", dataDir, clientID, perServerURI);

    pCrtDirName = (char*)malloc( strlen(clientDir) + 1);
    pTokDirName = (char*)malloc( strlen(clientDir) + 1);
    strcpy(pTokDirName, clientDir);

    pToken = strtok_r( pTokDirName, "\\/", &savePtr);

    strcpy(pCrtDirName, pToken);
    rc = pstMkdir(pCrtDirName);
    pToken = strtok_r(NULL, "\\/", &savePtr);
    while ((pToken != NULL) && (rc == 0)) {
        sprintf(pCrtDirName, "%s/%s", pCrtDirName, pToken);
        rc = pstMkdir(pCrtDirName);
        pToken = strtok_r(NULL, "\\/", &savePtr);
    }

    *handle = clientDir;

    free(perServerURI);
    free(pTokDirName);
    free(pCrtDirName);

    return rc;
}



int pstMkdir(char* pathName) {
    int rc = 0;
    if (access(pathName, F_OK) == 0) {
        return rc;
    }

    if (mkdir(pathName, S_IRWXU | S_IRGRP) != 0) {
        if (errno != EEXIST) {
            rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        }
    }

    return rc;
}

int pstPut(void* handle, char* key, int bufCount, char* buffers[], int bufLens[]) {
    int rc = 0;
    char* clientDir = handle;
    char* file;
    FILE *fp;
    size_t bytesWritten = 0;
    size_t bytesTotal = 0;
    int i;

    if (clientDir == NULL) {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        goto exit;
    }

    file = malloc(strlen(clientDir) + strlen(key) + strlen(MESSAGE_FILENAME_EXTENSION) + 2);
    sprintf(file, "%s/%s%s", clientDir, key, MESSAGE_FILENAME_EXTENSION);

    /** “wb” 只写打开或新建一个二进制文件；只允许写数据（若文件存在则文件长度清为零，即该文件内容会消失)*/
    fp = fopen(file, "wb");
    if (fp != NULL) {
        for (i = 0; i < bufCount; i++) {
            bytesTotal += bufLens[i];
            bytesWritten += fwrite(buffers[i], sizeof(char), bufLens[i], fp);
        }
        fclose(fp);
        fp = NULL;
    } else {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
    }

    if (bytesWritten != bytesTotal) {
        pstRemove(handle, key);
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
    }

    free(file);

exit:
    return rc;
}


int pstGet(void* handle, char* key, char** buffer, int* bufLen) {
    int rc = 0;
    FILE *fp;
    char* clientDir = handle;
    char* file;
    char* buf;
    unsigned long fileLen = 0;
    unsigned long bytesRead = 0;

    if (clientDir == NULL) {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        goto exit;
    }

    file = malloc(strlen(clientDir) + strlen(key) + strlen(MESSAGE_FILENAME_EXTENSION) + 2);
    sprintf(file, "%s/%s%s", clientDir, key, MESSAGE_FILENAME_EXTENSION);

    //“rb” 只读打开一个二进制文件，只允许读数据
    fp = fopen(file, "rb");
    if (fp != NULL) {
        fseek(fp, 0, SEEK_END);
        fileLen = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        buf = (char*)malloc(fileLen);
        bytesRead = (int)fread(buf, sizeof(char), fileLen, fp);
        *buffer = buf;
        *bufLen = bytesRead;

        if (bytesRead != fileLen) {
            rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        }
        fclose(fp);
        fp = NULL;
    } else {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
    }

    free(file);

exit:
    return rc;
}

int pstRemove(void* handle, char* key) {
    int rc = 0;
    char* clientDir = handle;
    char* file;

    if (clientDir == NULL) {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        goto exit;
    }

    file = malloc(strlen(clientDir) + strlen(key) + strlen(MESSAGE_FILENAME_EXTENSION) + 2);
    sprintf(file, "%s/%s%s", clientDir, key, MESSAGE_FILENAME_EXTENSION);

    if (unlink(file) != 0) {
        if (errno != ENOENT) {
            rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        }
    }
    free(file);

exit:
    return rc;
}


int pstClose(void* handle) {
    int rc = 0;
    char* clientDir = handle;

    if (clientDir == NULL) {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        goto exit;
    }

    if (rmdir(clientDir) != 0) {
        if (errno != ENOENT && errno != ENOTEMPTY)
            rc = MQTT_CLIENT_PERSISTENCE_ERROR;
    }

    free(clientDir);

exit:
    return rc;
}



int pstContainsKey(void *handle, char* key) {
    int rc = 0;
    char* clientDir = handle;

    if (clientDir == NULL) {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        goto error;
    }

    rc = containsKeyUnix(clientDir, key);

error:
    return rc;
}

int containsKeyUnix(char* dirname, char* key) {
    int notFound = MQTT_CLIENT_PERSISTENCE_ERROR;
    char* fileKey, *ptrAux;
    DIR *dp;
    struct dirent *dirEntry;
    struct stat statInfo;

    if ((dp = opendir(dirname)) != NULL) {
        while ((dirEntry = readdir(dp)) != NULL && notFound) {
            char* filename = malloc(strlen(dirname) + strlen(dirEntry->d_name) + 2);
            sprintf(filename, "%s/%s", dirname, dirEntry->d_name);
            lstat(filename, &statInfo);
            free(filename);
            if (S_ISREG(statInfo.st_mode)) {
                fileKey = malloc(strlen(dirEntry->d_name) + 1);
                strcpy(fileKey, dirEntry->d_name);
                ptrAux = strstr(fileKey, MESSAGE_FILENAME_EXTENSION);
                if (ptrAux != NULL) {
                    *ptrAux = '\0';
                }
                if (strcmp(fileKey, key) == 0) {
                    notFound = 0;
                }
                free(fileKey);
            }
        }
        closedir(dp);
    }
    return notFound;
}


int pstClear(void *handle) {
    int rc = 0;
    char* clientDir = handle;

    if (clientDir == NULL) {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        goto exit;
    }

    rc = clearUnix(clientDir);

exit:
    return rc;
}

int clearUnix(char* dirname) {
    int rc = 0;
    DIR *dp = 0;
    struct dirent * dirEntry;
    struct stat statInfo;

    if ((dp = opendir(dirname)) != NULL) {
        while ((dirEntry = readdir(dp)) != NULL && rc == 0) {
            lstat(dirEntry->d_name, &statInfo);
            if (S_ISREG(statInfo.st_mode)) {
                if (remove(dirEntry->d_name) != 0) {
                    rc = MQTT_CLIENT_PERSISTENCE_ERROR;
                }
            }
        }
        closedir(dp);
    } else {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
    }
    return rc;
}

int pstKeys(void* handle, char*** keys, int *nKeys) {
    int rc = 0;
    char* clientDir = handle;

    if (clientDir == NULL) {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        goto exit;
    }

    rc = keysUnix(clientDir, keys, nKeys);

exit:
    return rc;
}

int keysUnix(char *dirname, char*** keys, int *nKeys) {
    int rc = 0;
    char** fKeys = NULL;
    int nfKeys = 0;
    char* ptrAux;
    int i = 0;
    DIR *dp;
    struct dirent *dirEntry;
    struct stat statInfo;

    if ((dp = opendir(dirname)) != NULL) {
        while ((dirEntry = readdir(dp)) != NULL) {
            char* temp = malloc(strlen(dirname) + strlen(dirEntry->d_name) + 2);
            sprintf(temp, "%s/%s", dirname, dirEntry->d_name);
            if (lstat(temp, &statInfo) == 0 && S_ISREG(statInfo.st_mode)) {
                nfKeys++;
            }
            free(temp);
        }
        closedir(dp);
    } else {
        rc = MQTT_CLIENT_PERSISTENCE_ERROR;
        goto exit;
    }

    if (nfKeys != 0) {
        fKeys = (char**)malloc(nfKeys * sizeof(char *));

        if ((dp = opendir(dirname)) != NULL) {
            i = 0;
            while ((dirEntry = readdir(dp)) != NULL) {
                char* temp = malloc(strlen(dirname) + strlen(dirEntry->d_name) + 2);
                sprintf(temp, "%s/%s", dirname, dirEntry->d_name);
                if (lstat(temp, &statInfo) == 0 && S_ISREG(statInfo.st_mode)) {
                    fKeys[i] = malloc(strlen(dirEntry->d_name) + 1);
                    strcpy(fKeys[i], dirEntry->d_name);
                    ptrAux = strstr(fKeys[i], MESSAGE_FILENAME_EXTENSION);
                    if (ptrAux != NULL) {
                        *ptrAux = '\0';
                    }
                    i++;
                }
                free(temp);
            }
            closedir(dp);
        } else {
            rc = MQTT_CLIENT_PERSISTENCE_ERROR;
            goto exit;
        }
    }

    *nKeys = nfKeys;
    *keys = fKeys;

exit:

    return rc;
}


#endif

