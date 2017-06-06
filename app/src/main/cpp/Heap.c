/**
* 抽象堆，定义管理堆内存的方法,对所使用的数据结构式Tree
*/
#include "Tree.h"
#include "StackTrace.h"
#include "Log.h"

#include "Heap.h"

#undef malloc
#undef realloc
#undef free

static pthread_mutex_t heap_mutex_store = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t* heap_mutex = &heap_mutex_store;

static heap_info state = {0, 0};
static int eyeCatcher = 0x88888888;

/**
 * Each item on the heap is recorded with this structure.
 */
typedef struct
{
	char* 	file;		/**< the name of the source file where the storage was allocated */
	int 	line;		/**< the line no in the source file where it was allocated */
	void* 	ptr;		/**< pointer to the allocated storage */
	size_t 	size;    /**< size of the allocated storage */
} storageElement;

static Tree heap;
static const char* errmsg = "Memory allocation error";

static size_t 	Heap_roundup(size_t size);

static int 		ptrCompare(void* a, void* b, int value);

static void 	Heap_check(char* string, void* ptr);

static void 	checkEyeCatchers(char* file, int line, void* p, size_t size);

static int 		Internal_heap_unlink(char* file, int line, void* p);

static void 	HeapScan();

/**
 * Round allocation size up to a multiple of the size of an int.  Apart from possibly reducing fragmentation,
 * on the old v3 gcc compilers I was hitting some weird behaviour, which might have been errors in
 * sizeof() used on structures and related to packing.  In any case, this fixes that too.
 * @param size the size actually needed
 * @return the rounded up size
 */
static size_t Heap_roundup(size_t size){
  	static int multiSize = 4*sizeof(int);

  	if (size % multiSize != 0)
		size += multiSize - (size % multiSize);
  	return size;
}

/**
 * List callback function for comparing storage elements
 * @param a pointer to the current content in the tree (storageElement*)
 * @param b pointer to the memory to free
 * @return boolean indicating whether a and b are equal
 */
static int ptrCompare(void* a, void* b, int value){
  	a = ((storageElement*)a)->ptr;
  	if (value)
		b = ((storageElement*)b)->ptr;

  	return (a > b) ? -1 : (a == b) ? 0 : 1;
}

static void Heap_check(char* string, void* ptr) {
    return;
}

void* myMalloc(char* file, int line, size_t size) {
    storageElement* s = NULL;
    size_t space = sizeof(storageElement);
    size_t fileNameLen = strlen(file) + 1;

    Thread_lock_mutex(heap_mutex);
    size = Heap_roundup(size);
    if ((s = malloc(sizeof(storageElement))) == NULL) {
        return NULL;
    }

    s->size = size;

    if ((s->file = malloc(fileNameLen)) == NULL) {
        free(s);
        return NULL;
    }

    space += fileNameLen;
    strcpy(s->file, file);
    s->line = line;
    if ((s->ptr = malloc(size + 2*sizeof(int))) == NULL) {
        free(s->file);
        free(s);
        return NULL;
    }
    space += size + 2*sizeof(int);
    *(int*)(s->ptr) = eyeCatcher; /* start eyecatcher */
	*(int*)(((char*)(s->ptr)) + (sizeof(int) + size)) = eyeCatcher; /* end eyecatcher */
    TreeAdd(&heap, s, space);
    state.current_size += size;
    if (state.current_size > state.max_size) {
        state.max_size = state.current_size;
    }
    Thread_unlock_mutex(heap_mutex);
    return ((int*)(s->ptr)) + 1;/* skip start eyecatcher */
}


static void checkEyeCatchers(char* file, int line, void* p, size_t size){
  	int *sp = (int*)p;
  	char *cp = (char*)p;
  	static const char *msg = "Invalid %s eyecatcher %d in heap item at file %s line %d";

  	if ((*--sp) != eyeCatcher) {
		LOG(msg, file, line);
	}

  	cp += size;
  	if ((*(int*)cp) != eyeCatcher) {
		LOG(msg, file ,line);
	}

}

/**
 * Remove an item from the recorded heap without actually freeing it.
 * Use sparingly!
 * @param file use the __FILE__ macro to indicate which file this item was allocated in
 * @param line use the __LINE__ macro to indicate which line this item was allocated at
 * @param p pointer to the item to be removed
 */
static int Internal_heap_unlink(char* file, int line, void* p){
  	Node* e = NULL;
  	int rc = 0;

  	e = TreeFind(&heap, ((int*)p)-1);
  	if (e == NULL) {
		// Log(LOG_ERROR, 13, "Failed to remove heap item at file %s line %d", file, line);
  	} else {
		storageElement* s = (storageElement*)(e->content);
		checkEyeCatchers(file, line, p, s->size);
		//free(s->ptr);
		free(s->file);
		state.current_size -= s->size;
		TreeRemoveNodeIndex(&heap, e, 0);
		free(s);
		rc = 1;
  	}
  	return rc;
}

/**
 * Frees a block of memory.  A direct replacement for free, but checks that a item is in
 * the allocates list first.
 * @param file use the __FILE__ macro to indicate which file this item was allocated in
 * @param line use the __LINE__ macro to indicate which line this item was allocated at
 * @param p pointer to the item to be freed
 */
void myFree(char* file, int line, void* p){
  	Thread_lock_mutex(heap_mutex);
  	if (Internal_heap_unlink(file, line, p))
		free(((int*)p)-1);
  	Thread_unlock_mutex(heap_mutex);
}

/**
 * Remove an item from the recorded heap without actually freeing it.
 * Use sparingly!
 * @param file use the __FILE__ macro to indicate which file this item was allocated in
 * @param line use the __LINE__ macro to indicate which line this item was allocated at
 * @param p pointer to the item to be removed
 */
void Heap_unlink(char* file, int line, void* p){
  	Thread_lock_mutex(heap_mutex);
  	Internal_heap_unlink(file, line, p);
  	Thread_unlock_mutex(heap_mutex);
}


/**
 * Reallocates a block of memory.  A direct replacement for realloc, but keeps track of items
 * allocated in a list, so that free can check that a item is being freed correctly and that
 * we can check that all memory is freed at shutdown.
 * We have to remove the item from the tree, as the memory is in order and so it needs to
 * be reinserted in the correct place.
 * @param file use the __FILE__ macro to indicate which file this item was reallocated in
 * @param line use the __LINE__ macro to indicate which line this item was reallocated at
 * @param p pointer to the item to be reallocated
 * @param size the new size of the item
 * @return pointer to the allocated item, or NULL if there was an error
 */
void *myRealloc(char* file, int line, void* p, size_t size){
  	void* rc = NULL;
  	storageElement* s = NULL;

  	Thread_lock_mutex(heap_mutex);
  	s = TreeRemoveKey(&heap, ((int*)p)-1);
  	if (s == NULL) {
  	    LOG( "Failed to reallocate heap item at file %s line %d", file, line);
  	} else {
		size_t space = sizeof(storageElement);
		size_t fileNameLen = strlen(file)+1;

		checkEyeCatchers(file, line, p, s->size);
		size = Heap_roundup(size);
		state.current_size += size - s->size;
		if (state.current_size > state.max_size)
			state.max_size = state.current_size;

		if ((s->ptr = realloc(s->ptr, size + 2*sizeof(int))) == NULL){
			return NULL;
		}
		space += size + 2*sizeof(int) - s->size;
		*(int*)(s->ptr) = eyeCatcher; /* start eyeCatcher */
		*(int*)(((char*)(s->ptr)) + (sizeof(int) + size)) = eyeCatcher; /* end eyeCatcher */
		s->size = size;
		space -= strlen(s->file);
		s->file = realloc(s->file, fileNameLen);
		space += fileNameLen;
		strcpy(s->file, file);
		s->line = line;
		rc = s->ptr;
		TreeAdd(&heap, s, space);
  	}
  	Thread_unlock_mutex(heap_mutex);
  	return (rc == NULL) ? NULL : ((int*)(rc)) + 1;	/* skip start eyeCatcher */
}


/**
 * Utility to find an item in the heap.  Lets you know if the heap already contains
 * the memory location in question.
 * @param p pointer to a memory location
 * @return pointer to the storage element if found, or NULL
 */
void* Heap_findItem(void* p){
  	Node* e = NULL;

  	Thread_lock_mutex(heap_mutex);
  	e = TreeFind(&heap, ((int*)p)-1);
  	Thread_unlock_mutex(heap_mutex);
  	return (e == NULL) ? NULL : e->content;
}

/**
 * Scans the heap and reports any items currently allocated.
 * To be used at shutdown if any heap items have not been freed.
 */
static void HeapScan(){
  	Node* current = NULL;

  	Thread_lock_mutex(heap_mutex);
  	LOG("Heap scan start, total %zu bytes", state.current_size);
  	while ((current = TreeNextElement(&heap, current)) != NULL) {
		storageElement* s = (storageElement*)(current->content);
		LOG( "Heap element size %zu, line %d, file %s, ptr %p", s->size, s->line, s->file, s->ptr);
  	}
	LOG("Heap scan end");
  	Thread_unlock_mutex(heap_mutex);
}


/**
 * Heap initialization.
 */
int Heap_initialize(void){
  	TreeInitializeNoMalloc(&heap, ptrCompare);
  	heap.heap_tracking = 0; /* no recursive heap tracking! */
  	return 0;
}


/**
 * Heap termination.
 */
void Heap_terminate(void){
  	// Log(TRACE_MIN, -1, "Maximum heap use was %d bytes", state.max_size);
  	if (state.current_size > 20) { /* One log list is freed after this function is called */
		// Log(LOG_ERROR, -1, "Some memory not freed at shutdown, possible memory leak");
		HeapScan();
  	}
}

/**
 * Access to heap state
 * @return pointer to the heap state structure
 */
heap_info* Heap_get_info(void){
	 return &state;
}

/**
 * Dump a string from the heap so that it can be displayed conveniently
 * @param file file handle to dump the heap contents to
 * @param str the string to dump, could be NULL
 */
int HeapDumpString(FILE* file, char* str){
	int rc = 0;
	size_t len = str ? strlen(str) + 1 : 0; /* include the trailing null */

	if (fwrite(&(str), sizeof(char*), 1, file) != 1)
		rc = -1;
	else if (fwrite(&(len), sizeof(int), 1 ,file) != 1)
		rc = -1;
	else if (len > 0 && fwrite(str, len, 1, file) != 1)
		rc = -1;
	return rc;
}


/**
 * Dump the state of the heap
 * @param file file handle to dump the heap contents to
 */
int HeapDump(FILE* file){
	int rc = 0;
	Node* current = NULL;

	while (rc == 0 && (current = TreeNextElement(&heap, current))) {
  		storageElement* s = (storageElement*)(current->content);

  		if (fwrite(&(s->ptr), sizeof(s->ptr), 1, file) != 1)
			rc = -1;
  		else if (fwrite(&(current->size), sizeof(current->size), 1, file) != 1)
			rc = -1;
  		else if (fwrite(s->ptr, current->size, 1, file) != 1)
			rc = -1;
	}
	return rc;
}
