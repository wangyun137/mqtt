# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

# Sets the minimum version of CMake required to build the native library.

cmake_minimum_required(VERSION 3.4.1)

# 导入libcrypto.a
add_library(crypto STATIC IMPORTED)
set_target_properties(crypto
                      PROPERTIES IMPORTED_LOCATION
                      ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI}/libcrypto.a)

# 导入libssl.a
add_library(ssl STATIC IMPORTED)
set_target_properties(ssl
                      PROPERTIES IMPORTED_LOCATION
                      ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI}/libssl.a)

set(SRC_PATH src/main/cpp)


set(COMMON_SRC
    ${SRC_PATH}/Clients.c
    ${SRC_PATH}/FilePersistence.c
    ${SRC_PATH}/Hashmap.c
    ${SRC_PATH}/Heap.c
    ${SRC_PATH}/LinkedList.c
    ${SRC_PATH}/MemoryPersistence.c
    ${SRC_PATH}/MQTTPacket.c
    ${SRC_PATH}/MQTTPacketOut.c
    ${SRC_PATH}/MQTTPersistence.c
    ${SRC_PATH}/MQTTProtocolClient.c
    ${SRC_PATH}/MQTTProtocolOut.c
    ${SRC_PATH}/Socket.c
    ${SRC_PATH}/SocketBuffer.c
    ${SRC_PATH}/SSLSocket.c
    ${SRC_PATH}/StackTrace.c
    ${SRC_PATH}/Thread.c
    ${SRC_PATH}/Tree.c
    ${SRC_PATH}/utf-8.c)



add_library( common

             STATIC

             ${COMMON_SRC})

# 设置include目录
include_directories( ${CMAKE_SOURCE_DIR}/include/${ANDROID_ABI} )


find_library( # Sets the name of the path variable.
              log-lib

              # Specifies the name of the NDK library that
              # you want CMake to locate.
              log )


target_link_libraries( common

                       ${log-lib}
                       ssl
                       crypto)


add_library( mqtt-sync

              SHARED

              ${SRC_PATH}/MQTTClient.c)

target_link_libraries( mqtt-sync

                       common)


add_library( mqtt-async

             SHARED

             ${SRC_PATH}/MQTTAsync.c)

target_link_libraries( mqtt-async

                      common)
