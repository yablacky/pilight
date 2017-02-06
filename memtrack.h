/**
 * @abstract Track, analyse and log memory allocation.
 * @author yablacky <schwarz.ware@gmx.de>
 * @copyright 2017
 */

#ifndef _MEMTRACK_H_
#define _MEMTRACK_H_

#ifndef MEMTRACK
#define MEMTRACK 1
#endif

/**
 * Usage:
 * call memtrack_init() as early as possible.
 * call memtrack_enable(yes) to enable/disable memtracking.
 * call memtrack_thread_allocs() to get number of allocations done by current thread (since tracking).
 * Memtrack remembers code positions that calls malloc() etc. and echos allocations
 * done from that code position which are not already free()'d.
 * Memtrack echos calls to free() using a pointer that has not been allocated before (at least not tracked).
 */

#include <malloc.h>

#if ! MEMTRACK

#define memtrack_init()		(void)0
#define	memtrack_enable(yes)	(void)0

extern size_t memtrack_thread_allocs();

#else

extern void *__libc_malloc(size_t size);
extern void *__libc_realloc(void *ptr, size_t size);
extern void  __libc_free(void *ptr);
extern void *__libc_memalign(size_t alignment, size_t size);
extern void *__libc_calloc(size_t nmemb, size_t size);

/**
 * Implementation. Interface see at the end.
 */

#include <ctype.h>
#include <pthread.h>

static pthread_mutex_t memtrack_lock;
static int             memtrack_use_lock;

void memtrack_init()
{
    static pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&memtrack_lock, &attr);
    memtrack_use_lock = 1;
}

#define memtrack_lock()		if(memtrack_use_lock) pthread_mutex_lock(&memtrack_lock)
#define memtrack_unlock()	if(memtrack_use_lock) pthread_mutex_unlock(&memtrack_lock)

typedef struct {
    void *ptr;
    size_t size;
    const void *caller;
    pthread_t threadid;
    size_t trackix;
    size_t trackok;
} memtrack_t;

#define NMEMTRACK	10000
static memtrack_t	memtracked[NMEMTRACK] = { { 0 } };
static size_t		memtrackix = 0;
static size_t		memtrackok = 0;
static int		memtrack_hooks_enabled = 0;

void memtrack_enable(int yes)
{
    memtrack_hooks_enabled = yes;
}

#define memtrack_return_address() __builtin_return_address(0)

#define memtrack_enter_intf()	(void)0
#define memtrack_leave_intf()	(void)0

#define memtrack_enter_impl()	memtrack_lock()
#define memtrack_leave_impl()	memtrack_unlock()

static void* memtracked_malloc(size_t size, const void *caller);
static void* memtracked_realloc(void *ptr, size_t size, const void *caller);
static void  memtracked_free(void *ptr, const void *caller);
static void* memtracked_memalign(size_t alignment, size_t size, const void *caller);
static void* memtracked_calloc(size_t nmemb, size_t size, const void *caller);

size_t memtrack_thread_allocs()
{
    size_t count = 0;
    pthread_t me = pthread_self();
    memtrack_t *tracked = memtracked, *end = memtracked + NMEMTRACK;
    for (; tracked < end; tracked++)
	if (tracked->ptr && pthread_equal(tracked->threadid, me))
	    count++;
    return count;
}

static memtrack_t* memtrack_find_and_replace_ptr(void *ptr, void *new_ptr)
{
    memtrack_t *tracked = memtracked, *end = memtracked + NMEMTRACK;
    for (; tracked < end; tracked++)
// We use memtrack_enter_intf/unlock now.
//	if (__sync_bool_compare_and_swap(&tracked->ptr, ptr, new_ptr))
//	    return tracked;
	if (tracked->ptr == ptr) {
	    tracked->ptr = new_ptr;
	    return tracked;
	}
    return NULL;
}

static memtrack_t* memtrack_find_caller(const void *caller, memtrack_t *start, pthread_t threadid)
{
    memtrack_t *tracked = start, *end = memtracked + NMEMTRACK;
    for (; tracked < end; tracked++)
	if (tracked->ptr && tracked->caller == caller &&
		(!threadid || pthread_equal(tracked->threadid, threadid)))
	    return tracked;
    return NULL;
}

static char *memtrack_gen_dump(char *buf, size_t len, memtrack_t *tracked)
{
    char *dst = buf, *tail = "\"";
    const char *ptr = (const char*) tracked->ptr;
    if (!ptr)
	return "(null)";
    if (len < 4) // " and " and term null.
	return "\"...";
    len -= 3;
    *dst++ = '"';
    if (tracked->size <= len)
	len = tracked->size;
    else if (len > 2) {
	len -= 2;
	tail = "...";
    }
    for(; len > 1 || (len && *ptr); ptr++, len--)
	*dst++ = isprint(*ptr) ? *ptr : '.';

    while ((*dst++ = *tail++) != 0)
	;
    return buf;
}

static size_t memtrack_log_caller(const void *caller, size_t size, const char *op)
{
    memtrack_t *tracked = memtrack_find_caller(caller, memtracked, (pthread_t)0);
    if (!tracked)
	return 0;

    char dump[64];
    size_t count = 0, limit = 64;
    fprintf(stderr, "memtrack[%6u|%6u]: caller %10p want %5u bytes for %s in thread 0x%lX has still\n",
		    memtrackix, memtrackok, caller, size, op, pthread_self());
    do if(++count < limit) fprintf(stderr,
		    "   from [%6u|%6u]:     at %10p      %5u bytes: %s\n",
		    tracked->trackix, tracked->trackok,
		    tracked->ptr, tracked->size, memtrack_gen_dump(dump, sizeof(dump), tracked));
    while ((tracked = memtrack_find_caller(caller, tracked + 1, (pthread_t)0)) != NULL);
    fprintf(stderr, "   shown: %u of %u\n", count < limit ? count : limit, count);

    return count;
}

static void* memtracked_malloc(size_t size, const void *caller)
{
    memtrack_enter_impl();
    memtrackix++;
    memtrack_log_caller(caller, size, "malloc");

    void *ptr = __libc_malloc(size);
    if (ptr) {
	memtrack_t *tracked = memtrack_find_and_replace_ptr(NULL, ptr);
	if (tracked) {
//	    tracked->ptr = ptr;
	    tracked->size = size;
	    tracked->caller = caller;
	    tracked->threadid = pthread_self();
	    tracked->trackix = memtrackix;
	    tracked->trackok = memtrackok;
	}
    }
    memtrack_leave_impl();
    return ptr;
}

static void* memtracked_realloc(void *ptr, size_t size, const void *caller)
{
    memtrack_enter_impl();
    memtrackix++;

    if (ptr) {
	memtrack_t* tracked = memtrack_find_and_replace_ptr(ptr, NULL);
	if (!tracked) {
	    fprintf(stderr, "memtrack[%6u|%6u]: caller %10p does realloc(%10p,%u) but was not allocated; thread 0x%lX\n",
		    memtrackix, memtrackok, caller,  ptr, size, pthread_self());
	} else {
	    memtrackok++;
	}
    }
    if(size)
	memtrack_log_caller(caller, size, "realloc");

    ptr = __libc_realloc(ptr, size);

    if (ptr) {
	memtrack_t* tracked = memtrack_find_and_replace_ptr(NULL, ptr);
	if (tracked) {
//	    tracked->ptr = ptr;
	    tracked->size = size;
	    tracked->caller = caller;
	    tracked->threadid = pthread_self();
	    tracked->trackix = memtrackix;
	    tracked->trackok = memtrackok;
	}
    }

    memtrack_leave_impl();
    return ptr;
}

static void memtracked_free(void *ptr, const void *caller)
{
    memtrack_enter_impl();

    if (ptr) {
	memtrack_t* tracked = memtrack_find_and_replace_ptr(ptr, NULL);
	if (!tracked) {
	    fprintf(stderr, "memtrack[%6u|%6u]: caller %10p does free(%10p) but was not allocated; thread 0x%lX\n",
		    memtrackix, memtrackok, caller,  ptr, pthread_self());
	} else {
	    memtrackok++;
	}
    }

    __libc_free(ptr);
    memtrack_leave_impl();
}

static void* memtracked_memalign(size_t alignment, size_t size, const void *caller)
{
    memtrack_enter_impl();
    memtrackix++;
    memtrack_log_caller(caller, size, "memalign");

    void *ptr = __libc_memalign(alignment, size);
    if (ptr) {
	memtrack_t *tracked = memtrack_find_and_replace_ptr(NULL, ptr);
	if (tracked) {
//	    tracked->ptr = ptr;
	    tracked->size = size;
	    tracked->caller = caller;
	    tracked->threadid = pthread_self();
	    tracked->trackix = memtrackix;
	    tracked->trackok = memtrackok;
	}
    }
    memtrack_leave_impl();
    return ptr;
}

static void* memtracked_calloc(size_t nmemb, size_t size, const void *caller)
{
    memtrack_enter_impl();
    memtrackix++;
    memtrack_log_caller(caller, size * nmemb, "calloc");

    void *ptr = __libc_calloc(nmemb, size);
    if (ptr) {
	memtrack_t *tracked = memtrack_find_and_replace_ptr(NULL, ptr);
	if (tracked) {
//	    tracked->ptr = ptr;
	    tracked->size = size * nmemb;
	    tracked->caller = caller;
	    tracked->threadid = pthread_self();
	    tracked->trackix = memtrackix;
	    tracked->trackok = memtrackok;
	}
    }
    memtrack_leave_impl();
    return ptr;
}

/**
 * Interface. These functions (hopefully if linked dynamically)
 * replace the (weak declared) functions of same name in glibc.
 */

void* malloc(size_t size)
{
    memtrack_enter_intf();
    if (!memtrack_hooks_enabled) {
	memtrack_leave_intf();
	return __libc_malloc(size);
    }
    void *caller = memtrack_return_address();
    void *ptr = memtracked_malloc(size, caller);
    memtrack_leave_intf();
    return ptr;
}

void* realloc(void *ptr, size_t size)
{
    memtrack_enter_intf();
    if (!memtrack_hooks_enabled) {
	memtrack_leave_intf();
	return __libc_realloc(ptr, size);
    }
    void *caller = memtrack_return_address();
    ptr = memtracked_realloc(ptr, size, caller);
    memtrack_leave_intf();
    return ptr;
}

void free(void *ptr)
{
    memtrack_enter_intf();
    if (!memtrack_hooks_enabled) {
	memtrack_leave_intf();
	return __libc_free(ptr);
    }
    void *caller = memtrack_return_address();
    memtracked_free(ptr, caller);
    memtrack_leave_intf();
}

void* memalign(size_t alignment, size_t size)
{
    memtrack_enter_intf();
    if (!memtrack_hooks_enabled) {
	memtrack_leave_intf();
	return __libc_memalign(alignment, size);
    }
    void *caller = memtrack_return_address();
    void *ptr =  memtracked_memalign(alignment, size, caller);
    memtrack_leave_intf();
    return ptr;
}

void* calloc(size_t nmemb, size_t size)
{
    memtrack_enter_intf();
    if (!memtrack_hooks_enabled) {
	memtrack_leave_intf();
	return __libc_calloc(nmemb, size);
    }
    void *caller = memtrack_return_address();
    void *ptr = memtracked_calloc(nmemb, size, caller);
    memtrack_leave_intf();
    return ptr;
}

#endif	/* MEMTRACK */

#endif	/* _MEMTRACK_H_*/
