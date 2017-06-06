/**
* 定义红黑树数据结构
* 红黑树即二叉查找树，二叉查找树具有以下性质:
* 若任意节点的左子树不为空， 则左子树上所有节点的值均小于根节点的值
* 若任意节点的左子树不为空， 则右子树上所有节点的值均大于根节点的值
* 任意节点的左，右字数也分分别为二叉查找树
* 没有键值相等的节点
* 二叉查找树的高度为logn,所以时间复杂度也为logn
* 红黑树的5条性质：
* 1. 每个节点要么是红色要么是黑色
* 2. 根节点是黑色
* 3. 每个叶节点(叶节点是指书微端NIL指针或NULL节点)为黑色
* 4. 如果一个节点是红的，那么它的两个子节点必须都是黑的
* 5. 对于任一节点而言, 其到树尾端NIL指针的每一条路径都包含相同的数目的黑色节点
*/

#define NO_HEAP_TRACKING 1

#include "Tree.h"

#include <string.h>

#include "Heap.h"

int isRed(Node* aNode);
int isBlack(Node* aNode);
int TreeWalk(Node* curNode, int depth);
int TreeMaxDepth(Tree *aTree);
void TreeRotate(Tree* aTree, Node* curNode, int direction, int index);
Node* TreeBAASub(Tree* aTree, Node* curNode, int which, int index);
void TreeBalanceAfterAdd(Tree* aTree, Node* curNode, int index);
void* TreeAddByIndex(Tree* aTree, void* content, size_t size, int index);
Node* TreeFindIndex1(Tree* aTree, void* key, int index, int value);
Node* TreeFindContentIndex(Tree* aTree, void* key, int index);
Node* TreeMinimum(Node* curNode);
Node* TreeSuccessor(Node* curNode);
Node* TreeNextElementIndex(Tree* aTree, Node* curNode, int index);
Node* TreeBARSub(Tree* aTree, Node* curNode, int which, int index);
void TreeBalanceAfterRemove(Tree* aTree, Node* curNode, int index);
void* TreeRemoveIndex(Tree* aTree, void* content, int index);

/**
* 初始化Tree
*/
void TreeInitializeNoMalloc(Tree* aTree, int(*compare)(void*, void*, int)) {
    memset(aTree, '\0', sizeof(Tree));
    aTree->heap_tracking = 1;
    aTree->index[0].compare = compare;
    aTree->indexes = 1;
}

/**
* 创建并初始化一棵树
* @param compare
* @return 指向创建的树的指针
*/
Tree* TreeInitialize(int(*compare)(void*, void*, int)) {
    Tree* newt = myMalloc(__FILE__, __LINE__, sizeof(Tree));
    TreeInitializeNoMalloc(newt, compare);
    return newt;
}

/**
* 增加树的索引(默认aTree->index[]的长度是2)
*/
void TreeAddIndex(Tree* aTree, int(*compare)(void*, void*, int)) {
    aTree->index[aTree->indexes].compare = compare;
    ++(aTree->indexes);
}

/**
* 释放树的内存
*/
void TreeFree(Tree* aTree) {
    (aTree->heap_tracking) ? myFree(__FILE__, __LINE__, aTree) : free(aTree);
}


#define LEFT 0
#define RIGHT 1
#ifndef max
#define max(a, b) (a > b) ? a : b
#endif

/**
* 判断传入的节点是否是红色节点
*/
int isRed(Node* aNode) {
    return (aNode != NULL) && (aNode->red);
}

/**
* 判断传入的节点是否是黑色节点
*/
int isBlack(Node* aNode) {
    return (aNode == NULL) || (aNode->red == 0);
}

/**
* 从传入的节点开始遍历树，找出该节点到树末的最大路径
*/
int TreeWalk(Node* curNode, int depth) {
    if (curNode) {
        int left = TreeWalk(curNode->child[LEFT], depth+1);
        int right = TreeWalk(curNode->child[RIGHT], depth+1);
        depth = max(left, right);
        if (curNode->red) {}
    }
    return depth;
}

/**
* 从根节点开始遍历树，找出树的最大路径
*/
int TreeMaxDepth(Tree* aTree) {
    int rc = TreeWalk(aTree->index[0].root, 0);
    return rc;
}

/**
* 将给定的Node进行旋转，具体旋转由direction决定
* eg:
*       0                                      1
*      / \                                    / \
*     1   2   右旋                            3   0
*    / \      指定curNode为0根节点             \  / \
*   3   4     direction为1(右)     -->        5 4  2
*    \
*     5
* @param aTree 目标树
* @param curNode 要旋转的node
* @param direction 0或1
* @param index
*/
void TreeRotate(Tree* aTree, Node* curNode, int direction, int index){
    //other置为curNode的左子树如果指定的direction是右边，否则为右子树
  	Node* other = curNode->child[!direction];
  	curNode->child[!direction] = other->child[direction];
  	if (other->child[direction] != NULL) {
  		  other->child[direction]->parent = curNode;
    }

  	other->parent = curNode->parent;

  	if (curNode->parent == NULL) {
  		  aTree->index[index].root = other;
  	} else if (curNode == curNode->parent->child[direction]) {
  		  curNode->parent->child[direction] = other;
  	} else {
  		  curNode->parent->child[!direction] = other;
    }

  	other->child[direction] = curNode;
  	curNode->parent = other;
}

/**
* 进行完添加操作后，为保持树的平衡性，进行必要的操作
*/
Node* TreeBAASub(Tree* aTree, Node* curNode, int which, int index){
    //获取父节点同级的另一节点，也可能是父节点自身
    Node* uncle = curNode->parent->parent->child[which];

  	if (isRed(uncle)){
        //如果叔父节点为红色节点，则将父节点和叔父节点都置为黑色
    		curNode->parent->red = uncle->red = 0;
        //将curNode置为祖父节点,并将curNode变为红色节点
    		curNode = curNode->parent->parent;
    		curNode->red = 1;
  	} else {
    		if (curNode == curNode->parent->child[which]){
    	     curNode = curNode->parent;
    			 TreeRotate(aTree, curNode, !which, index);
    		}
    		curNode->parent->red = 0;
    		curNode->parent->parent->red = 1;
    		TreeRotate(aTree, curNode->parent->parent, which, index);
  	}
  	return curNode;
}

/**
* 保持树的平衡性
*/
void TreeBalanceAfterAdd(Tree* aTree, Node* curNode, int index){
    //当前节点是红色且当前节点的祖父节点不为空的情况下进行循环
  	while (curNode && isRed(curNode->parent) && curNode->parent->parent){
    		if (curNode->parent == curNode->parent->parent->child[LEFT]) {
            //如果当前节点的父节点恰好是祖父节点的左子树，右旋
    			 curNode = TreeBAASub(aTree, curNode, RIGHT, index);
    		 } else {
           //如果当前节点的父节点是祖父节点的右子树，左旋
    			 curNode = TreeBAASub(aTree, curNode, LEFT, index);
         }
    }
    //保证根节点为黑色
    aTree->index[index].root->red = 0;
}

/**
 * 向树中插入节点，插入节点前首先要确定插入的位置
 * @param aTree 目标树
 * @param content
 * @param size
 */
void* TreeAddByIndex(Tree* aTree, void* content, size_t size, int index){
  	Node* curParent = NULL;
  	Node* curNode = aTree->index[index].root;
  	Node* newel = NULL;
  	int left = 0;
  	int result = 1;
  	void* rc = NULL;

  	while (curNode){
        /*
        * 如果curNode->content > content, 遍历左子树, left = 1
        * 如果curNode->content = content, 中断循环， left = 0
        * 如果curNode->content < content, 遍历右子树, left = 0
        */
        result = aTree->index[index].compare(curNode->content, content, 1);
        //当curNode->content > content是left = 1;
    		left = (result > 0);
    		if (result == 0) {
    		    break;
    		} else{
    		    curParent = curNode;
    		    curNode = curNode->child[left];
    		}
  	}

  	if (result == 0){
        //如果找到content相等的节点，直接将newel指向curNode
		if (aTree->allow_duplicates)
			exit(-99);
		{
			newel = curNode;
			rc = newel->content;
			if (index == 0) {
				aTree->size += (size - curNode->size);
			}
		}
  	} else {
        //如果没找到相等content的node, 新创建一个Node结构体
		newel = (aTree->heap_tracking) ? myMalloc(__FILE__, __LINE__, sizeof(Node)) : malloc(sizeof(Node));
		memset(newel, '\0', sizeof(Node));
        /**
        * 将新节点放置在合适的位置，依据left的取值；
        * 如果是一棵空树，将新节点置为根节点
        */
		if (curParent)
			curParent->child[left] = newel;
		else
			aTree->index[index].root = newel;
    		newel->parent = curParent;
    		newel->red = 1;
    		if (index == 0){
      			++(aTree->count);
      			aTree->size += size;
    		}
  	}
  	newel->content = content;
  	newel->size = size;
    //对树进行平衡，确保根节点为黑色
  	TreeBalanceAfterAdd(aTree, newel, index);
  	return rc;
}

void* TreeAdd(Tree* aTree, void* content, size_t size){
  	void* rc = NULL;
  	int i;

  	for (i = 0; i < aTree->indexes; ++i)
		rc = TreeAddByIndex(aTree, content, size, i);

  	return rc;
}

/**
* 遍历Tree，根据给定的content查找相应的节点
*/
Node* TreeFindIndex1(Tree* aTree, void* key, int index, int value){
  	int result = 0;
  	Node* curNode = aTree->index[index].root;

  	while (curNode){
  		result = aTree->index[index].compare(curNode->content, key, value);
  		if (result == 0)
  		    break;
  		else
  			 curNode = curNode->child[result > 0];
  	}
  	return curNode;
}

/**
* 遍历Tree，根据给定的content查找相应的节点
*/
Node* TreeFindIndex(Tree* aTree, void* key, int index){
    return TreeFindIndex1(aTree, key, index, 0);
}

/**
* 遍历Tree，根据给定的content查找相应的节点
*/
Node* TreeFindContentIndex(Tree* aTree, void* key, int index){
	 return TreeFindIndex1(aTree, key, index, 1);
}

/**
* 遍历Tree，根据给定的content查找相应的节点
*/
Node* TreeFind(Tree* aTree, void* key){
	 return TreeFindIndex(aTree, key, 0);
}

/**
* 找出当前节点路径下最小的节点
*/
Node* TreeMinimum(Node* curNode){
  	if (curNode)
		while (curNode->child[LEFT])
			curNode = curNode->child[LEFT];
  	return curNode;
}

Node* TreeSuccessor(Node* curNode){
  	if (curNode->child[RIGHT]) {
      	//找出当前节点右子树下最小的节点
		    curNode = TreeMinimum(curNode->child[RIGHT]);
  	} else {
      //向上遍历父节点
  		Node* curParent = curNode->parent;
  		while (curParent && curNode == curParent->child[RIGHT]){
  			curNode = curParent;
  			curParent = curParent->parent;
  		}
  		curNode = curParent;
  	}
  	return curNode;
}


Node* TreeNextElementIndex(Tree* aTree, Node* curNode, int index){
  	if (curNode == NULL)
		  curNode = TreeMinimum(aTree->index[index].root);
  	else
		  curNode = TreeSuccessor(curNode);
  	return curNode;
}


Node* TreeNextElement(Tree* aTree, Node* curNode){
  	return TreeNextElementIndex(aTree, curNode, 0);
}

/**
* 在对树进行删除操作后，为保持树的平衡性，进行必要的操作
*/
Node* TreeBARSub(Tree* aTree, Node* curNode, int which, int index){
  	Node* sibling = curNode->parent->child[which];

  	if (isRed(sibling)){
        sibling->red = 0;
        curNode->parent->red = 1;
        TreeRotate(aTree, curNode->parent, !which, index);
        sibling = curNode->parent->child[which];
  	}
  	if (!sibling) {
  	    curNode = curNode->parent;
  	} else if (isBlack(sibling->child[!which]) && isBlack(sibling->child[which])){
  	    sibling->red = 1;
  		  curNode = curNode->parent;
  	} else {
  		if (isBlack(sibling->child[which])) {
  			sibling->child[!which]->red = 0;
  			sibling->red = 1;
  			TreeRotate(aTree, sibling, which, index);
  			sibling = curNode->parent->child[which];
  		}
  		sibling->red = curNode->parent->red;
  		curNode->parent->red = 0;
  		sibling->child[which]->red = 0;
  		TreeRotate(aTree, curNode->parent, !which, index);
  		curNode = aTree->index[index].root;
  	}
  	return curNode;
}


void TreeBalanceAfterRemove(Tree* aTree, Node* curNode, int index){
  	while (curNode != aTree->index[index].root && isBlack(curNode)){
    		/* curNode->content == NULL must equal curNode == NULL */
    		if (((curNode->content) ? curNode : NULL) == curNode->parent->child[LEFT])
      			curNode = TreeBARSub(aTree, curNode, RIGHT, index);
    		else
      			curNode = TreeBARSub(aTree, curNode, LEFT, index);
    }
  	curNode->red = 0;
}

/**
 * 删除一个节点
 * @param aTree 目标树
 * @param curNode 当前节点
 * @param index
 * @return 删除节点的content
 */
void* TreeRemoveNodeIndex(Tree* aTree, Node* curNode, int index){
  	Node* redundant = curNode;
  	Node* curchild = NULL;
  	size_t size = curNode->size;
  	void* content = curNode->content;

  	/* if the node to remove has 0 or 1 children, it can be removed without involving another node */
  	if (curNode->child[LEFT] && curNode->child[RIGHT]) /* 2 children */
    		redundant = TreeSuccessor(curNode); 	/* now redundant must have at most one child */

  	curchild = redundant->child[(redundant->child[LEFT] != NULL) ? LEFT : RIGHT];
  	if (curchild) /* we could have no children at all */
    		curchild->parent = redundant->parent;

  	if (redundant->parent == NULL) {
    		aTree->index[index].root = curchild;
  	} else {
    		if (redundant == redundant->parent->child[LEFT])
    		    redundant->parent->child[LEFT] = curchild;
    		else
    		    redundant->parent->child[RIGHT] = curchild;
  	}

  	if (redundant != curNode){
  		  curNode->content = redundant->content;
  		    curNode->size = redundant->size;
  	}

  	if (isBlack(redundant)) {
        if (curchild == NULL) {
      			if (redundant->parent){
        				Node temp;
        				memset(&temp, '\0', sizeof(Node));
        				temp.parent = (redundant) ? redundant->parent : NULL;
        				temp.red = 0;
        				TreeBalanceAfterRemove(aTree, &temp, index);
      			}
    		} else {
    		    TreeBalanceAfterRemove(aTree, curchild, index);
        }
  	}

  	(aTree->heap_tracking) ? myFree(__FILE__, __LINE__, redundant) : free(redundant);
  	if (index == 0){
    		aTree->size -= size;
    		--(aTree->count);
  	}
  	return content;
}

/**
 * Remove an item from a tree
 * @param aTree the list to which the item is to be added
 * @param curNode the list item content itself
 */
void* TreeRemoveIndex(Tree* aTree, void* content, int index){
  	Node* curNode = TreeFindContentIndex(aTree, content, index);

  	if (curNode == NULL)
  	   return NULL;

  	return TreeRemoveNodeIndex(aTree, curNode, index);
}


void* TreeRemove(Tree* aTree, void* content){
  	int i;
  	void* rc = NULL;

  	for (i = 0; i < aTree->indexes; ++i)
  		  rc = TreeRemoveIndex(aTree, content, i);

  	return rc;
}


void* TreeRemoveKeyIndex(Tree* aTree, void* key, int index){
  	Node* curNode = TreeFindIndex(aTree, key, index);
  	void* content = NULL;
  	int i;

  	if (curNode == NULL)
  		  return NULL;

  	content = TreeRemoveNodeIndex(aTree, curNode, index);
  	for (i = 0; i < aTree->indexes; ++i){
    		if (i != index)
    			 content = TreeRemoveIndex(aTree, content, i);
  	}
  	return content;
}


void* TreeRemoveKey(Tree* aTree, void* key){
	 return TreeRemoveKeyIndex(aTree, key, 0);
}


int TreeIntCompare(void* a, void* b, int content){
  	int i = *((int*)a);
  	int j = *((int*)b);

  	//printf("comparing %d %d\n", *((int*)a), *((int*)b));
  	return (i > j) ? -1 : (i == j) ? 0 : 1;
}

int TreePtrCompare(void* a, void* b, int content){
    return (a > b) ? -1 : (a == b) ? 0 : 1;
}


int TreeStringCompare(void* a, void* b, int content){
    return strcmp((char*)a, (char*)b);
}
