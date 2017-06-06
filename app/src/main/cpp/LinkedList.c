/**
* 定义链表数据结构
*/

#include "LinkedList.h"

#include <string.h>


static int ListUnlink(List* aList, void* content, int(*callback)(void*, void*), int freeContent);

/**
* 设置List为空值(初始化)
*/
void ListZero(List* newList) {
    memset(newList, '\0', sizeof(List));
}

/**
* 创建一个新的List,并初始化
*/
List* ListInitialize(void) {
    List* newList = malloc(sizeof(List));
    ListZero(newList);
    return newList;
}

/**
* 添加一个ListElement到List中
* @param aList 目标List
* @param content ListElement的content
* @param element 将要被添加的ListElement
* @param size element的size
*/
void ListAppendNoMalloc(List* aList, void* content, ListElement* element, size_t size) {
    element->content = content;
    element->next = NULL;
    element->prev = aList->last;
    if (aList->first == NULL) {
        aList->first = element;
    } else {
        //将原来的last节点的next指向新的element
        aList->last->next = element;
    }
    //将last设为新添加的element
    aList->last = element;
    ++(aList->count);
    aList->size += size;
}

/**
* 添加一个新的item到List中
* @param aList 目标List
* @param content ListElement的content
* @param size ListElement的size
*/
void ListAppend(List* aList, void* content, size_t size) {
    ListElement* element = malloc(sizeof(ListElement));
    ListAppendNoMalloc(aList,  content, element, size);
}

/**
* 在指定的位置插入一个item
* @param aList 目标List
* @param content
*/
void ListInsert(List* aList, void* content, size_t size, ListElement* index) {
    ListElement* element = malloc(sizeof(ListElement));

    if (index == NULL) {
        //如果没有指定位置，则直接添加到末尾
        ListAppendNoMalloc(aList, content, element, size);
    } else {
        element->content = content;
        //将element的next指向原来位置的item
        element->next = index;
        //将element的prev指向原来位置item的prev
        element->prev = index->prev;

        index->prev = element;
        if (element->prev != NULL) {
            element->prev->next = element;
        } else {
            aList->first = element;
        }

        ++(aList->count);
        aList->size += size;
    }
}


/**
* 通过对比content指针查找ListElement
*/
ListElement* ListFind(List* aList, void* content) {
    return ListFindItem(aList, content, NULL);
}

/**
* 查找ListElement, callback回调用来定义element之间如何比较
* @param aList 目标List
* @param content 要查找的content指针
* @param callback 定义如何比较element的回调函数, 如果为null,则比较的原则是两个指针content是否一致
* @return 找到返回对应的ListElement，否则返回NULL
*/
ListElement* ListFindItem(List* aList, void* content, int(*callback)(void*, void*)) {
    ListElement* rc = NULL;

    if (aList->current != NULL &&
            ((callback == NULL && aList->current->content == content) ||
            (callback != NULL && callback(aList->current->content, content)))) {
        /* 如果current不为NULL, 并且满足以下条件中的一个时，要查找的就是current
        *  1. callback为NULL, current的content指针恰好就是要查找的content指针
        *  2. callback不为NULL, current的content指针所指的内容和要查找的content指针所指的内容经过callback比较后相等
        */
        rc = aList->current;
    } else {
        ListElement* current = NULL;
        //遍历List
        while (ListNextElement(aList, &current) != NULL) {
            if (callback == NULL) {
                //如果当前节点current指针和content指针相等
                if (current->content == content) {
                    rc = current;
                    break;
                }
            } else {
                if (callback(current->content, content)) {
                    rc = content;
                    break;
                }
            }
        }
        if (rc != NULL) {
            aList->current = rc;
        }
    }
    return rc;
}

/**
* 根据content删除并选择性的释放element
* @param aList 目标List
* @param content ListElement的content指针
* @param callback 回调函数，用于比较content
* @param freeContent bool值, 指定是否释放content内存
* @return 1=item removed, 0=item not removed
*/
static int ListUnlink(List* aList, void* content, int(*callback)(void*, void*), int freeContent) {
    ListElement* next = NULL;
    ListElement* saved = aList->current;
    int savedDeleted = 0;

    //如果没有找到对应的element,直接返回0
    if (!ListFindItem(aList, content, callback)) {
        return 0;
    }
    //经过ListFindItem, aList->current已经被置为马上要删除的这个节点
    if (aList->current->prev == NULL) {
        //如果当前要删除的element恰好就是第一个节点, 将first指向要删除节点的next
        aList->first = aList->current->next;
    } else {
        //将要删除节点的prev->next指向要删除节点的next
        aList->current->prev->next = aList->current->next;
    }

    if (aList->current->next == NULL) {
        //如果要删除节点的next为NULL,即要删除节点是last节点
        //设置last节点为要删除节点的prev
        aList->last = aList->current->prev;
    } else {
        //将要删除的节点的next->prev设为要删除节点的prev
        aList->current->next->prev = aList->current->prev;
    }

    next = aList->current->next;
    if (freeContent) {
        free(aList->current->content);
    }

    //如果要删除的节点恰好就是之前的current节点
    if (saved == aList->current) {
        savedDeleted = 1;
    }
    free(aList->current);

    if (savedDeleted) {
        aList->current = next;
    } else {
        //将current指针重新指向原来节点
        aList->current = saved;
    }
    --(aList->count);
    return 1;
}

/**
* 从List中删除一个节点，但是并不是释放content的内存
*/
int ListDetach(List* aList, void* content) {
    return ListUnlink(aList, content, NULL, 0);
}

/**
* 从List中删除指定的节点并释放对应的conent指针
*/
int ListRemove(List* aList, void* content) {
    return ListUnlink(aList, content, NULL, 1);
}

/**
* 删除first节点, 但不释放first的content
* @param aList 目标List
* @return first节点的content
*/
void* ListDetachHead(List* aList) {
    void *content = NULL;
    if (aList->count > 0) {
        ListElement* first = aList->first;
        if (aList->current == first) {
            aList->current = first->next;
        }

        if (aList->last == first) {
            aList->last = NULL;
        }

        content = first->content;
        aList->first = aList->first->next;
        if (aList->first) {
            aList->first->prev = NULL;
        }
        free(first);
        --(aList->count);
    }
    return content;
}

/**
* 删除first节点，同时释放content
* @param aList 目标List
* @return 1=item removed, 0=item not removed
*/
int ListRemoveHead(List* aList) {
    free(ListDetachHead(aList));
    return 0;
}

/**
* 删除最后一个元素但不释放content内存
* @param aList 目标List
* @return List中的最后一个元素
*/
void* ListPopTail(List* aList) {
    void* content = NULL;

    if (aList->count > 0) {
        ListElement* last = aList->last;
        if (aList->current == last) {
            //如果当前节点恰好是last节点, 将last指向当前节点的prev
            aList->current = last->prev;
        }

        if (aList->first == last) {
            //如果List只有一个节点，first置为NULL
            aList->first = NULL;
        }
        content = last->content;
        //将last指针指向前一个节点
        aList->last = aList->last->prev;
        if (aList->last) {
            //将新last节点的next置为null
            aList->last->next = NULL;
        }

        free(last);
        --(aList->count);
    }
    return content;
}

/**
* 根据content删除指定的item但并不释放content内存
* @param aList 目标List
* @param content 要删除节点的content
* @param callback 比较content的回调函数
* @return 1=item removed, 0=item not removed
*/
int ListDetachItem(List* aList, void* content, int(*callback)(void*, void*)) {
    return ListUnlink(aList, content, callback, 0);
}

/**
* 根据content删除指定的item同时释放content内存
* @param aList 目标List
* @param content 要删除节点的content
* @param callback 比较content的回调函数
* @return 1=item removed, 0=item not removed
*/
int ListRemoveItem(List* aList, void* content, int(*callback)(void*, void*)) {
    return ListUnlink(aList, content, callback, 1);
}

/**
* 删除List中全部节点
*/
void ListEmpty(List* aList) {
    while (aList->first != NULL) {
        ListElement* first = aList->first;
        if (first->content != NULL) {
            free(first->content);
        }
        aList->first = first->next;
        free(first);
    }
    aList->count = 0;
    aList->size = 0;
    aList->current = aList->first = aList->last = NULL;
}

/**
* 删除全部节点，同时释放List自身内存
*/
void ListFree(List* aList) {
    ListEmpty(aList);
    free(aList);
}

/**
* 删除全部节点，但并不释放每个节点的content
*/
void ListFreeNoContent(List* aList) {
    while (aList->first != NULL) {
        ListElement* first = aList->first;
        aList->first = first->next;
        free(first);
    }
    free(aList);
}

/**
* 向后遍历List
*/
ListElement* ListNextElement(List* aList, ListElement** pos) {
    return *pos = (*pos == NULL) ? aList->first : (*pos)->next;
}

ListElement* ListPrevElement(List* aList, ListElement** pos) {
    return *pos = (*pos == NULL) ? aList->last : (*pos)->prev;
}

/**
* List的int值比较回调函数
*/
int intCompare(void* a, void* b) {
    return *((int*)a) == *((int*)b);
}

/**
* List的string值比较回调
*/
int stringCompare(void* a, void* b) {
    return strcmp((char*)a, (char*)b) == 0;
}
