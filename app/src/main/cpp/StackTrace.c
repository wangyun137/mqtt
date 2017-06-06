#include "StackTrace.h"
#include "LinkedList.h"
#include "Log.h"


#define MAX_STACK_DEPTH 50
#define MAX_FUNCTION_NAME_LENGTH 30
#define MAX_THREADS 255

typedef struct {
    pthread_t threadId;
    char name[MAX_FUNCTION_NAME_LENGTH];
    int line;
} stackEntry;

typedef struct {
    pthread_t id;
    int maxDepth;
    int current_depth;
    stackEntry callStack[MAX_STACK_DEPTH];
} threadEntry;

static int thread_count = 0;
static threadEntry threads[MAX_THREADS];
static threadEntry *cur_thread = NULL;

static pthread_mutex_t stack_mutex_store = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t* stack_mutex = &stack_mutex_store;

int setStack(int create);

int setStack(int create) {
    int i = -1;
    pthread_t curId = Thread_getId();

    cur_thread = NULL;
    for (i = 0; i < MAX_THREADS && i < thread_count; ++i) {
        if (threads[i].id == curId) {
            cur_thread = &threads[i];
            break;
        }
    }

    if (cur_thread == NULL && create && thread_count < MAX_THREADS) {
        cur_thread = &threads[thread_count];
        cur_thread->id = curId;
        cur_thread->maxDepth = 0;
        cur_thread->current_depth = 0;
        ++thread_count;
    }

    return cur_thread != NULL;
}


void StackEntry_entry(const char* name, int line) {
    Thread_lock_mutex(stack_mutex);
    if (!setStack(1)) {
        goto exit;
    }

    strncpy(cur_thread->callStack[cur_thread->current_depth].name, name, sizeof(cur_thread->callStack[0].name)-1);
    cur_thread->callStack[(cur_thread->current_depth)++].line = line;
    if (cur_thread->current_depth > cur_thread->maxDepth) {
        cur_thread->maxDepth = cur_thread->current_depth;
    }

exit:
    Thread_unlock_mutex(stack_mutex);
}


void StackTrace_exit(const char* name, int line, void* rc){
  	Thread_lock_mutex(stack_mutex);
  	if (!setStack(0))
  		  goto exit;

    if (--(cur_thread->current_depth) < 0)
  		  LOG("Minimum stack depth exceeded for thread %lu", cur_thread->id);

    if (strncmp(cur_thread->callStack[cur_thread->current_depth].name, name, sizeof(cur_thread->callStack[0].name)-1) != 0)
  		  LOG("Stack mismatch, Entry: %s Exit: %s\n", cur_thread->callStack[cur_thread->current_depth].name, name);

    if (rc == NULL) {
        LOG("StackTrace_exit rc == NULL current thread id is %lu name is %d at %s-%d",
            cur_thread->id, cur_thread->current_depth, name, line);
    } else {
        LOG("StackTrace_exit rc != NULL current thread id is %lu name is %d at %s-%d",
            cur_thread->id, cur_thread->current_depth, name, line);
    }

exit:
	 Thread_unlock_mutex(stack_mutex);
}

void StackTrace_printStack(FILE* dest) {
    FILE* file = stdout;
    int t = 0;

    if (dest)
        file = dest;
    for (t = 0; t < thread_count; ++t) {
        threadEntry *cur_thread = &threads[t];

        if (cur_thread->id > 0) {
            int i = cur_thread->current_depth - 1;

            fprintf(file, "=========== Start of stack trace for thread %lu ==========\n", (unsigned long)cur_thread->id);
            if (i >= 0) {
                fprintf(file, "%s (%d)\n", cur_thread->callStack[i].name, cur_thread->callStack[i].line);
                while (--i >= 0)
                    fprintf(file, "   at %s (%d)\n", cur_thread->callStack[i].name, cur_thread->callStack[i].line);
            }
            fprintf(file, "=========== End of stack trace for thread %lu ==========\n\n", (unsigned long)cur_thread->id);
        }
    }
    if (file != stdout && file != stderr && file != NULL)
        fclose(file);
}


char* StackTrace_get(pthread_t threadId) {
    int bufSize = 256;
    char* buf = NULL;
    int thread = 0;

    if ((buf = malloc(bufSize)) == NULL) {
        goto exit;
    }
    buf[0] = '\0';

    for (thread = 0 ; thread < thread_count; ++thread) {
        threadEntry *cur_thread = &threads[thread];
        if (cur_thread->id == threadId) {
            int i = cur_thread->current_depth - 1;
            int curpos = 0;

            if (i >= 0) {
                curpos += snprintf(&buf[curpos], bufSize - curpos - 1, "%s (%d)\n",
                          cur_thread->callStack[i].name, cur_thread->callStack[i].line);
            }
            if (buf[--curpos] == '\n') {
                buf[curpos] = '\n';
            }
            break;
        }
    }
exit:
    return buf;
}
