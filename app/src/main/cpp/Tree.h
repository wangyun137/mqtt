#ifndef DIM_MQTT_TREE_H_
#define DIM_MQTT_TREE_H_

#include <stdlib.h>

/* Node结构体*/
typedef struct NodeStruct {
    struct NodeStruct *parent;      //指向父节点
    struct NodeStruct *child[2];    //指向两个子节点. 0 = left, 1 = right

    void* content;
    size_t size;
    unsigned int red : 1;           //表示该机诶单是否为红色节点, 表示red只占1bit
} Node;

typedef struct {
    struct {
        Node *root; //根节点
        int (*compare)(void*, void*, int);//回调函数
    } index[2];
    int indexes;
    int count;
    size_t size;
    unsigned int heap_tracking : 1; //只占1bit, 是否要进行heap track
    unsigned int allow_duplicates : 1; // 是否允许有duplicate entry
} Tree;

Tree* TreeInitialize(int(*compare)(void*, void*, int));

void TreeInitializeNoMalloc(Tree* aTree, int(*compare)(void*, void*, int));

void TreeAddIndex(Tree* aTree, int(*compare)(void*, void*, int));

void* TreeAdd(Tree* aTree, void* content, size_t size);

void* TreeRemove(Tree* aTree, void* content);

void* TreeRemoveKey(Tree* aTree, void* key);

void* TreeRemoveKeyIndex(Tree* aTree, void* key, int index);

void* TreeRemoveNodeIndex(Tree* aTree, Node* aNode, int index);

void TreeFree(Tree* aTree);

Node* TreeFind(Tree* aTree, void* key);

Node* TreeFindIndex(Tree* aTree, void* key, int index);

Node* TreeNextElement(Tree* aTree, Node* curNode);

int TreeIntCompare(void* a, void* b, int);
int TreePtrCompare(void* a, void* b, int);
int TreeStringCompare(void* a, void* b, int);

#endif
