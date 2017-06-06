#ifndef DIM_MQTT_LINKED_LIST_H_
#define DIM_MQTT_LINKED_LIST_H_

#include <stdlib.h>

/**
 * Structure to hold all data for one list element
 */
typedef struct ListElementStruct {
    struct ListElementStruct*   prev;/**< pointer to previous list element */
    struct ListElementStruct*   next;/**< pointer to next list element */
    void*                       content;/**< pointer to element content */
} ListElement;


typedef struct {
    ListElement *first;/**< first element in the list */
    ListElement *last;/**< last element in the list */
    ListElement *current;/**< current element in the list, for iteration */
    int         count;/**< no of items */
    size_t      size;/**< heap storage used */
} List;

void ListZero(List*);

List* ListInitialize(void);

void ListAppend(List* aList, void* content, size_t size);

void ListAppendNoMalloc(List* aList, void* content, ListElement* newel, size_t size);

void ListInsert(List* aList, void* content, size_t size, ListElement* index);

int ListRemove(List* aList, void* content);

int ListRemoveItem(List* aList, void* content, int(*callback)(void*, void*));

void* ListDetachHead(List* aList);

int ListRemoveHead(List* aList);

void* ListPopTail(List* aList);

int ListDetach(List* aList, void* content);

int ListDetachItem(List* aList, void* content, int(*callback)(void*, void*));

void ListFree(List* aList);

void ListEmpty(List* aList);

void ListFreeNoContent(List* aList);

ListElement* ListNextElement(List* aList, ListElement** pos);

ListElement* ListPrevElement(List* aList, ListElement** pos);

ListElement* ListFind(List* aList, void* content);

ListElement* ListFindItem(List* aList, void* content, int(*callback)(void*, void*));

int intCompare(void* a, void* b);

int stringCompare(void* a, void* b);

#endif //DIM_MQTT_LINKED_LIST_H_
