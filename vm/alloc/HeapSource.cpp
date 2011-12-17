/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cutils/mspace.h>
#include <stdint.h>
#include <sys/mman.h>
#include <errno.h>

#define SIZE_MAX UINT_MAX  // TODO: get SIZE_MAX from stdint.h

#include "Dalvik.h"
#include "alloc/Heap.h"
#include "alloc/HeapInternal.h"
#include "alloc/HeapSource.h"
#include "alloc/HeapBitmap.h"
#include "alloc/HeapBitmapInlines.h"

// TODO: find a real header file for these.
extern "C" int dlmalloc_trim(size_t);
extern "C" void dlmalloc_walk_free_pages(void(*)(void*, void*, void*), void*);

static void snapIdealFootprint();
static void setIdealFootprint(size_t max);
static size_t getMaximumSize(const HeapSource *hs);
static void trimHeaps();

#define HEAP_UTILIZATION_MAX        1024
#define DEFAULT_HEAP_UTILIZATION    512     // Range 1..HEAP_UTILIZATION_MAX
#define HEAP_IDEAL_FREE             (2 * 1024 * 1024)
#define HEAP_MIN_FREE               (HEAP_IDEAL_FREE / 4)

/* Number of seconds to wait after a GC before performing a heap trim
 * operation to reclaim unused pages.
 */
#define HEAP_TRIM_IDLE_TIME_SECONDS 5

/* Start a concurrent collection when free memory falls under this
 * many bytes.
 */
#define CONCURRENT_START (128 << 10)

/* The next GC will not be concurrent when free memory after a GC is
 * under this many bytes.
 */
#define CONCURRENT_MIN_FREE (CONCURRENT_START + (128 << 10))

#define HS_BOILERPLATE() \
    do { \
        assert(gDvm.gcHeap != NULL); \
        assert(gDvm.gcHeap->heapSource != NULL); \
        assert(gHs == gDvm.gcHeap->heapSource); \
    } while (0)

struct Heap {
    /* The mspace to allocate from.
     */
    mspace msp;

    /* The largest size that this heap is allowed to grow to.
     */
    size_t maximumSize;

    /* Number of bytes allocated from this mspace for objects,
     * including any overhead.  This value is NOT exact, and
     * should only be used as an input for certain heuristics.
     */
    size_t bytesAllocated;

    /* Number of bytes allocated from this mspace at which a
     * concurrent garbage collection will be started.
     */
    size_t concurrentStartBytes;

    /* Number of objects currently allocated from this mspace.
     */
    size_t objectsAllocated;

    /*
     * The lowest address of this heap, inclusive.
     */
    char *base;

    /*
     * The highest address of this heap, exclusive.
     */
    char *limit;
};

struct HeapSource {
    /* Target ideal heap utilization ratio; range 1..HEAP_UTILIZATION_MAX
     */
    size_t targetUtilization;

    /* The starting heap size.
     */
    size_t startSize;

    /* The largest that the heap source as a whole is allowed to grow.
     */
    size_t maximumSize;

    /*
     * The largest size we permit the heap to grow.  This value allows
     * the user to limit the heap growth below the maximum size.  This
     * is a work around until we can dynamically set the maximum size.
     * This value can range between the starting size and the maximum
     * size but should never be set below the current footprint of the
     * heap.
     */
    size_t growthLimit;

    /* The desired max size of the heap source as a whole.
     */
    size_t idealSize;

    /* The maximum number of bytes allowed to be allocated from the
     * active heap before a GC is forced.  This is used to "shrink" the
     * heap in lieu of actual compaction.
     */
    size_t softLimit;

    /* The heaps; heaps[0] is always the active heap,
     * which new objects should be allocated from.
     */
    Heap heaps[HEAP_SOURCE_MAX_HEAP_COUNT];

    /* The current number of heaps.
     */
    size_t numHeaps;

    /* True if zygote mode was active when the HeapSource was created.
     */
    bool sawZygote;

    /*
     * The base address of the virtual memory reservation.
     */
    char *heapBase;

    /*
     * The length in bytes of the virtual memory reservation.
     */
    size_t heapLength;

    /*
     * The live object bitmap.
     */
    HeapBitmap liveBits;

    /*
     * The mark bitmap.
     */
    HeapBitmap markBits;

    /*
     * State for the GC daemon.
     */
    bool hasGcThread;
    pthread_t gcThread;
    bool gcThreadShutdown;
    pthread_mutex_t gcThreadMutex;
    pthread_cond_t gcThreadCond;
    bool gcThreadTrimNeeded;
};

#define hs2heap(hs_) (&((hs_)->heaps[0]))

/*
 * Returns true iff a soft limit is in effect for the active heap.
 */
static bool isSoftLimited(const HeapSource *hs)
{
    /* softLimit will be either SIZE_MAX or the limit for the
     * active mspace.  idealSize can be greater than softLimit
     * if there is more than one heap.  If there is only one
     * heap, a non-SIZE_MAX softLimit should always be the same
     * as idealSize.
     */
    return hs->softLimit <= hs->idealSize;
}

/*
 * Returns approximately the maximum number of bytes allowed to be
 * allocated from the active heap before a GC is forced.
 */
static size_t getAllocLimit(const HeapSource *hs)
{
    if (isSoftLimited(hs)) {
        return hs->softLimit;
    } else {
        return mspace_max_allowed_footprint(hs2heap(hs)->msp);
    }
}

/*
 * Returns the current footprint of all heaps.  If includeActive
 * is false, don't count the heap at index 0.
 */
static size_t oldHeapOverhead(const HeapSource *hs, bool includeActive)
{
    size_t footprint = 0;
    size_t i;

    if (includeActive) {
        i = 0;
    } else {
        i = 1;
    }
    for (/* i = i */; i < hs->numHeaps; i++) {
//TODO: include size of bitmaps?  If so, don't use bitsLen, listen to .max
        footprint += mspace_footprint(hs->heaps[i].msp);
    }
    return footprint;
}

/*
 * Returns the heap that <ptr> could have come from, or NULL
 * if it could not have come from any heap.
 */
static Heap *ptr2heap(const HeapSource *hs, const void *ptr)
{
    const size_t numHeaps = hs->numHeaps;

    if (ptr != NULL) {
        for (size_t i = 0; i < numHeaps; i++) {
            const Heap *const heap = &hs->heaps[i];

            if ((const char *)ptr >= heap->base && (const char *)ptr < heap->limit) {
                return (Heap *)heap;
            }
        }
    }
    return NULL;
}

/*
 * Functions to update heapSource->bytesAllocated when an object
 * is allocated or freed.  mspace_usable_size() will give
 * us a much more accurate picture of heap utilization than
 * the requested byte sizes would.
 *
 * These aren't exact, and should not be treated as such.
 */
static void countAllocation(Heap *heap, const void *ptr)
{
    assert(heap->bytesAllocated < mspace_footprint(heap->msp));

    heap->bytesAllocated += mspace_usable_size(heap->msp, ptr) +
            HEAP_SOURCE_CHUNK_OVERHEAD;
    heap->objectsAllocated++;
    HeapSource* hs = gDvm.gcHeap->heapSource;
    dvmHeapBitmapSetObjectBit(&hs->liveBits, ptr);

    assert(heap->bytesAllocated < mspace_footprint(heap->msp));
}

static void countFree(Heap *heap, const void *ptr, size_t *numBytes)
{
    size_t delta = mspace_usable_size(heap->msp, ptr) + HEAP_SOURCE_CHUNK_OVERHEAD;
    assert(delta > 0);
    if (delta < heap->bytesAllocated) {
        heap->bytesAllocated -= delta;
    } else {
        heap->bytesAllocated = 0;
    }
    HeapSource* hs = gDvm.gcHeap->heapSource;
    dvmHeapBitmapClearObjectBit(&hs->liveBits, ptr);
    if (heap->objectsAllocated > 0) {
        heap->objectsAllocated--;
    }
    *numBytes += delta;
}

static HeapSource *gHs = NULL;

static mspace createMspace(void *base, size_t startSize, size_t maximumSize)
{
    /* Create an unlocked dlmalloc mspace to use as
     * a heap source.
     *
     * We start off reserving startSize / 2 bytes but
     * letting the heap grow to startSize.  This saves
     * memory in the case where a process uses even less
     * than the starting size.
     */
    LOGV_HEAP("Creating VM heap of size %zu", startSize);
    errno = 0;

    mspace msp = create_contiguous_mspace_with_base(startSize/2,
            maximumSize, /*locked=*/false, base);
    if (msp != NULL) {
        /* Don't let the heap grow past the starting size without
         * our intervention.
         */
        mspace_set_max_allowed_footprint(msp, startSize);
    } else {
        /* There's no guarantee that errno has meaning when the call
         * fails, but it often does.
         */
        LOGE_HEAP("Can't create VM heap of size (%zu,%zu): %s",
            startSize/2, maximumSize, strerror(errno));
    }

    return msp;
}

/*
 * Add the initial heap.  Returns false if the initial heap was
 * already added to the heap source.
 */
static bool addInitialHeap(HeapSource *hs, mspace msp, size_t maximumSize)
{
    assert(hs != NULL);
    assert(msp != NULL);
    if (hs->numHeaps != 0) {
        return false;
    }
    hs->heaps[0].msp = msp;
    hs->heaps[0].maximumSize = maximumSize;
    hs->heaps[0].concurrentStartBytes = SIZE_MAX;
    hs->heaps[0].base = hs->heapBase;
    hs->heaps[0].limit = hs->heapBase + hs->heaps[0].maximumSize;
    hs->numHeaps = 1;
    return true;
}

/*
 * Adds an additional heap to the heap source.  Returns false if there
 * are too many heaps or insufficient free space to add another heap.
 */
static bool addNewHeap(HeapSource *hs)
{
    Heap heap;

    assert(hs != NULL);
    if (hs->numHeaps >= HEAP_SOURCE_MAX_HEAP_COUNT) {
        LOGE("Attempt to create too many heaps (%zd >= %zd)",
                hs->numHeaps, HEAP_SOURCE_MAX_HEAP_COUNT);
        dvmAbort();
        return false;
    }

    memset(&heap, 0, sizeof(heap));

    /*
     * Heap storage comes from a common virtual memory reservation.
     * The new heap will start on the page after the old heap.
     */
    void *sbrk0 = contiguous_mspace_sbrk0(hs->heaps[0].msp);
    char *base = (char *)ALIGN_UP_TO_PAGE_SIZE(sbrk0);
    size_t overhead = base - hs->heaps[0].base;
    assert(((size_t)hs->heaps[0].base & (SYSTEM_PAGE_SIZE - 1)) == 0);

    if (overhead + HEAP_MIN_FREE >= hs->maximumSize) {
        LOGE_HEAP("No room to create any more heaps "
                  "(%zd overhead, %zd max)",
                  overhead, hs->maximumSize);
        return false;
    }

    heap.maximumSize = hs->growthLimit - overhead;
    heap.concurrentStartBytes = HEAP_MIN_FREE - CONCURRENT_START;
    heap.base = base;
    heap.limit = heap.base + heap.maximumSize;
    heap.msp = createMspace(base, HEAP_MIN_FREE, hs->maximumSize - overhead);
    if (heap.msp == NULL) {
        return false;
    }

    /* Don't let the soon-to-be-old heap grow any further.
     */
    hs->heaps[0].maximumSize = overhead;
    hs->heaps[0].limit = base;
    mspace msp = hs->heaps[0].msp;
    mspace_set_max_allowed_footprint(msp, mspace_footprint(msp));

    /* Put the new heap in the list, at heaps[0].
     * Shift existing heaps down.
     */
    memmove(&hs->heaps[1], &hs->heaps[0], hs->numHeaps * sizeof(hs->heaps[0]));
    hs->heaps[0] = heap;
    hs->numHeaps++;

    return true;
}

/*
 * The garbage collection daemon.  Initiates a concurrent collection
 * when signaled.  Also periodically trims the heaps when a few seconds
 * have elapsed since the last concurrent GC.
 */
static void *gcDaemonThread(void* arg)
{
    dvmChangeStatus(NULL, THREAD_VMWAIT);
    dvmLockMutex(&gHs->gcThreadMutex);
    while (gHs->gcThreadShutdown != true) {
        bool trim = false;
        if (gHs->gcThreadTrimNeeded) {
            int result = dvmRelativeCondWait(&gHs->gcThreadCond, &gHs->gcThreadMutex,
                    HEAP_TRIM_IDLE_TIME_SECONDS, 0);
            if (result == ETIMEDOUT) {
                /* Timed out waiting for a GC request, schedule a heap trim. */
                trim = true;
            }
        } else {
            dvmWaitCond(&gHs->gcThreadCond, &gHs->gcThreadMutex);
        }

        dvmLockHeap();
        /*
         * Another thread may have started a concurrent garbage
         * collection before we were scheduled.  Check for this
         * condition before proceeding.
         */
        if (!gDvm.gcHeap->gcRunning) {
            dvmChangeStatus(NULL, THREAD_RUNNING);
            if (trim) {
                trimHeaps();
                gHs->gcThreadTrimNeeded = false;
            } else {
                dvmCollectGarbageInternal(GC_CONCURRENT);
                gHs->gcThreadTrimNeeded = true;
            }
            dvmChangeStatus(NULL, THREAD_VMWAIT);
        }
        dvmUnlockHeap();
    }
    dvmChangeStatus(NULL, THREAD_RUNNING);
    return NULL;
}

static bool gcDaemonStartup()
{
    dvmInitMutex(&gHs->gcThreadMutex);
    pthread_cond_init(&gHs->gcThreadCond, NULL);
    gHs->gcThreadShutdown = false;
    gHs->hasGcThread = dvmCreateInternalThread(&gHs->gcThread, "GC",
                                               gcDaemonThread, NULL);
    return gHs->hasGcThread;
}

static void gcDaemonShutdown()
{
    if (gHs->hasGcThread) {
        dvmLockMutex(&gHs->gcThreadMutex);
        gHs->gcThreadShutdown = true;
        dvmSignalCond(&gHs->gcThreadCond);
        dvmUnlockMutex(&gHs->gcThreadMutex);
        pthread_join(gHs->gcThread, NULL);
    }
}

/*
 * Create a stack big enough for the worst possible case, where the
 * heap is perfectly full of the smallest object.
 * TODO: be better about memory usage; use a smaller stack with
 *       overflow detection and recovery.
 */
static bool allocMarkStack(GcMarkStack *stack, size_t maximumSize)
{
    const char *name = "dalvik-mark-stack";
    void *addr;

    assert(stack != NULL);
    stack->length = maximumSize * sizeof(Object*) /
        (sizeof(Object) + HEAP_SOURCE_CHUNK_OVERHEAD);
    addr = dvmAllocRegion(stack->length, PROT_READ | PROT_WRITE, name);
    if (addr == NULL) {
        return false;
    }
    stack->base = (const Object **)addr;
    stack->limit = (const Object **)((char *)addr + stack->length);
    stack->top = NULL;
    madvise(stack->base, stack->length, MADV_DONTNEED);
    return true;
}

static void freeMarkStack(GcMarkStack *stack)
{
    assert(stack != NULL);
    munmap(stack->base, stack->length);
    memset(stack, 0, sizeof(*stack));
}

/*
 * Initializes the heap source; must be called before any other
 * dvmHeapSource*() functions.  Returns a GcHeap structure
 * allocated from the heap source.
 */
GcHeap* dvmHeapSourceStartup(size_t startSize, size_t maximumSize,
                             size_t growthLimit)
{
    GcHeap *gcHeap;
    HeapSource *hs;
    mspace msp;
    size_t length;
    void *base;

    assert(gHs == NULL);

    if (!(startSize <= growthLimit && growthLimit <= maximumSize)) {
        LOGE("Bad heap size parameters (start=%zd, max=%zd, limit=%zd)",
             startSize, maximumSize, growthLimit);
        return NULL;
    }

    /*
     * Allocate a contiguous region of virtual memory to subdivided
     * among the heaps managed by the garbage collector.
     */
    length = ALIGN_UP_TO_PAGE_SIZE(maximumSize);
    base = dvmAllocRegion(length, PROT_NONE, "dalvik-heap");
    if (base == NULL) {
        return NULL;
    }

    /* Create an unlocked dlmalloc mspace to use as
     * a heap source.
     */
    msp = createMspace(base, startSize, maximumSize);
    if (msp == NULL) {
        goto fail;
    }

    gcHeap = (GcHeap *)malloc(sizeof(*gcHeap));
    if (gcHeap == NULL) {
        LOGE_HEAP("Can't allocate heap descriptor");
        goto fail;
    }
    memset(gcHeap, 0, sizeof(*gcHeap));

    hs = (HeapSource *)malloc(sizeof(*hs));
    if (hs == NULL) {
        LOGE_HEAP("Can't allocate heap source");
        free(gcHeap);
        goto fail;
    }
    memset(hs, 0, sizeof(*hs));

    hs->targetUtilization = DEFAULT_HEAP_UTILIZATION;
    hs->startSize = startSize;
    hs->maximumSize = maximumSize;
    hs->growthLimit = growthLimit;
    hs->idealSize = startSize;
    hs->softLimit = SIZE_MAX;    // no soft limit at first
    hs->numHeaps = 0;
    hs->sawZygote = gDvm.zygote;
    hs->hasGcThread = false;
    hs->heapBase = (char *)base;
    hs->heapLength = length;
    if (!addInitialHeap(hs, msp, growthLimit)) {
        LOGE_HEAP("Can't add initial heap");
        goto fail;
    }
    if (!dvmHeapBitmapInit(&hs->liveBits, base, length, "dalvik-bitmap-1")) {
        LOGE_HEAP("Can't create liveBits");
        goto fail;
    }
    if (!dvmHeapBitmapInit(&hs->markBits, base, length, "dalvik-bitmap-2")) {
        LOGE_HEAP("Can't create markBits");
        dvmHeapBitmapDelete(&hs->liveBits);
        goto fail;
    }
    if (!allocMarkStack(&gcHeap->markContext.stack, hs->maximumSize)) {
        LOGE("Can't create markStack");
        dvmHeapBitmapDelete(&hs->markBits);
        dvmHeapBitmapDelete(&hs->liveBits);
        goto fail;
    }
    gcHeap->markContext.bitmap = &hs->markBits;
    gcHeap->heapSource = hs;

    gHs = hs;
    return gcHeap;

fail:
    munmap(base, length);
    return NULL;
}

bool dvmHeapSourceStartupAfterZygote()
{
    return gDvm.concurrentMarkSweep ? gcDaemonStartup() : true;
}

/*
 * This is called while in zygote mode, right before we fork() for the
 * first time.  We create a heap for all future zygote process allocations,
 * in an attempt to avoid touching pages in the zygote heap.  (This would
 * probably be unnecessary if we had a compacting GC -- the source of our
 * troubles is small allocations filling in the gaps from larger ones.)
 */
bool dvmHeapSourceStartupBeforeFork()
{
    HeapSource *hs = gHs; // use a local to avoid the implicit "volatile"

    HS_BOILERPLATE();

    assert(gDvm.zygote);

    if (!gDvm.newZygoteHeapAllocated) {
        /* Create a new heap for post-fork zygote allocations.  We only
         * try once, even if it fails.
         */
        LOGV("Splitting out new zygote heap");
        gDvm.newZygoteHeapAllocated = true;
        return addNewHeap(hs);
    }
    return true;
}

void dvmHeapSourceThreadShutdown()
{
    if (gDvm.gcHeap != NULL && gDvm.concurrentMarkSweep) {
        gcDaemonShutdown();
    }
}

/*
 * Tears down the entire GcHeap structure and all of the substructures
 * attached to it.  This call has the side effect of setting the given
 * gcHeap pointer and gHs to NULL.
 */
void dvmHeapSourceShutdown(GcHeap **gcHeap)
{
    assert(gcHeap != NULL);
    if (*gcHeap != NULL && (*gcHeap)->heapSource != NULL) {
        HeapSource *hs = (*gcHeap)->heapSource;
        dvmHeapBitmapDelete(&hs->liveBits);
        dvmHeapBitmapDelete(&hs->markBits);
        freeMarkStack(&(*gcHeap)->markContext.stack);
        munmap(hs->heapBase, hs->heapLength);
        free(hs);
        gHs = NULL;
        free(*gcHeap);
        *gcHeap = NULL;
    }
}

/*
 * Gets the begining of the allocation for the HeapSource.
 */
void *dvmHeapSourceGetBase()
{
    return gHs->heapBase;
}

/*
 * Returns the requested value. If the per-heap stats are requested, fill
 * them as well.
 *
 * Caller must hold the heap lock.
 */
size_t dvmHeapSourceGetValue(HeapSourceValueSpec spec, size_t perHeapStats[],
                             size_t arrayLen)
{
    HeapSource *hs = gHs;
    size_t value = 0;
    size_t total = 0;

    HS_BOILERPLATE();

    assert(arrayLen >= hs->numHeaps || perHeapStats == NULL);
    for (size_t i = 0; i < hs->numHeaps; i++) {
        Heap *const heap = &hs->heaps[i];

        switch (spec) {
        case HS_FOOTPRINT:
            value = mspace_footprint(heap->msp);
            break;
        case HS_ALLOWED_FOOTPRINT:
            value = mspace_max_allowed_footprint(heap->msp);
            break;
        case HS_BYTES_ALLOCATED:
            value = heap->bytesAllocated;
            break;
        case HS_OBJECTS_ALLOCATED:
            value = heap->objectsAllocated;
            break;
        default:
            // quiet gcc
            break;
        }
        if (perHeapStats) {
            perHeapStats[i] = value;
        }
        total += value;
    }
    return total;
}

void dvmHeapSourceGetRegions(uintptr_t *base, uintptr_t *max, size_t numHeaps)
{
    HeapSource *hs = gHs;

    HS_BOILERPLATE();

    assert(numHeaps <= hs->numHeaps);
    for (size_t i = 0; i < numHeaps; ++i) {
        base[i] = (uintptr_t)hs->heaps[i].base;
        max[i] = MIN((uintptr_t)hs->heaps[i].limit - 1, hs->markBits.max);
    }
}

/*
 * Get the bitmap representing all live objects.
 */
HeapBitmap *dvmHeapSourceGetLiveBits()
{
    HS_BOILERPLATE();

    return &gHs->liveBits;
}

/*
 * Get the bitmap representing all marked objects.
 */
HeapBitmap *dvmHeapSourceGetMarkBits()
{
    HS_BOILERPLATE();

    return &gHs->markBits;
}

void dvmHeapSourceSwapBitmaps()
{
    HeapBitmap tmp = gHs->liveBits;
    gHs->liveBits = gHs->markBits;
    gHs->markBits = tmp;
}

void dvmHeapSourceZeroMarkBitmap()
{
    HS_BOILERPLATE();

    dvmHeapBitmapZero(&gHs->markBits);
}

void dvmMarkImmuneObjects(const char *immuneLimit)
{
    /*
     * Copy the contents of the live bit vector for immune object
     * range into the mark bit vector.
     */
    /* The only values generated by dvmHeapSourceGetImmuneLimit() */
    assert(immuneLimit == gHs->heaps[0].base ||
           immuneLimit == NULL);
    assert(gHs->liveBits.base == gHs->markBits.base);
    assert(gHs->liveBits.bitsLen == gHs->markBits.bitsLen);
    /* heap[0] is never immune */
    assert(gHs->heaps[0].base >= immuneLimit);
    assert(gHs->heaps[0].limit > immuneLimit);

    for (size_t i = 1; i < gHs->numHeaps; ++i) {
        if (gHs->heaps[i].base < immuneLimit) {
            assert(gHs->heaps[i].limit <= immuneLimit);
            /* Compute the number of words to copy in the bitmap. */
            size_t index = HB_OFFSET_TO_INDEX(
                (uintptr_t)gHs->heaps[i].base - gHs->liveBits.base);
            /* Compute the starting offset in the live and mark bits. */
            char *src = (char *)(gHs->liveBits.bits + index);
            char *dst = (char *)(gHs->markBits.bits + index);
            /* Compute the number of bytes of the live bitmap to copy. */
            size_t length = HB_OFFSET_TO_BYTE_INDEX(
                gHs->heaps[i].limit - gHs->heaps[i].base);
            /* Do the copy. */
            memcpy(dst, src, length);
            /* Make sure max points to the address of the highest set bit. */
            if (gHs->markBits.max < (uintptr_t)gHs->heaps[i].limit) {
                gHs->markBits.max = (uintptr_t)gHs->heaps[i].limit;
            }
        }
    }
}

/*
 * Allocates <n> bytes of zeroed data.
 */
void* dvmHeapSourceAlloc(size_t n)
{
    HS_BOILERPLATE();

    HeapSource *hs = gHs;
    Heap* heap = hs2heap(hs);
    if (heap->bytesAllocated + n > hs->softLimit) {
        /*
         * This allocation would push us over the soft limit; act as
         * if the heap is full.
         */
        LOGV_HEAP("softLimit of %zd.%03zdMB hit for %zd-byte allocation",
                  FRACTIONAL_MB(hs->softLimit), n);
        return NULL;
    }
    void* ptr = mspace_calloc(heap->msp, 1, n);
    if (ptr == NULL) {
        return NULL;
    }
    countAllocation(heap, ptr);
    /*
     * Check to see if a concurrent GC should be initiated.
     */
    if (gDvm.gcHeap->gcRunning || !hs->hasGcThread) {
        /*
         * The garbage collector thread is already running or has yet
         * to be started.  Do nothing.
         */
        return ptr;
    }
    if (heap->bytesAllocated > heap->concurrentStartBytes) {
        /*
         * We have exceeded the allocation threshold.  Wake up the
         * garbage collector.
         */
        dvmSignalCond(&gHs->gcThreadCond);
    }
    return ptr;
}

/* Remove any hard limits, try to allocate, and shrink back down.
 * Last resort when trying to allocate an object.
 */
static void* heapAllocAndGrow(HeapSource *hs, Heap *heap, size_t n)
{
    /* Grow as much as possible, but don't let the real footprint
     * go over the absolute max.
     */
    size_t max = heap->maximumSize;

    mspace_set_max_allowed_footprint(heap->msp, max);
    void* ptr = dvmHeapSourceAlloc(n);

    /* Shrink back down as small as possible.  Our caller may
     * readjust max_allowed to a more appropriate value.
     */
    mspace_set_max_allowed_footprint(heap->msp,
                                     mspace_footprint(heap->msp));
    return ptr;
}

/*
 * Allocates <n> bytes of zeroed data, growing as much as possible
 * if necessary.
 */
void* dvmHeapSourceAllocAndGrow(size_t n)
{
    HS_BOILERPLATE();

    HeapSource *hs = gHs;
    Heap* heap = hs2heap(hs);
    void* ptr = dvmHeapSourceAlloc(n);
    if (ptr != NULL) {
        return ptr;
    }

    size_t oldIdealSize = hs->idealSize;
    if (isSoftLimited(hs)) {
        /* We're soft-limited.  Try removing the soft limit to
         * see if we can allocate without actually growing.
         */
        hs->softLimit = SIZE_MAX;
        ptr = dvmHeapSourceAlloc(n);
        if (ptr != NULL) {
            /* Removing the soft limit worked;  fix things up to
             * reflect the new effective ideal size.
             */
            snapIdealFootprint();
            return ptr;
        }
        // softLimit intentionally left at SIZE_MAX.
    }

    /* We're not soft-limited.  Grow the heap to satisfy the request.
     * If this call fails, no footprints will have changed.
     */
    ptr = heapAllocAndGrow(hs, heap, n);
    if (ptr != NULL) {
        /* The allocation succeeded.  Fix up the ideal size to
         * reflect any footprint modifications that had to happen.
         */
        snapIdealFootprint();
    } else {
        /* We just couldn't do it.  Restore the original ideal size,
         * fixing up softLimit if necessary.
         */
        setIdealFootprint(oldIdealSize);
    }
    return ptr;
}

/*
 * Frees the first numPtrs objects in the ptrs list and returns the
 * amount of reclaimed storage. The list must contain addresses all in
 * the same mspace, and must be in increasing order. This implies that
 * there are no duplicates, and no entries are NULL.
 */
size_t dvmHeapSourceFreeList(size_t numPtrs, void **ptrs)
{
    HS_BOILERPLATE();

    if (numPtrs == 0) {
        return 0;
    }

    assert(ptrs != NULL);
    assert(*ptrs != NULL);
    Heap* heap = ptr2heap(gHs, *ptrs);
    size_t numBytes = 0;
    if (heap != NULL) {
        mspace msp = heap->msp;
        // Calling mspace_free on shared heaps disrupts sharing too
        // much. For heap[0] -- the 'active heap' -- we call
        // mspace_free, but on the other heaps we only do some
        // accounting.
        if (heap == gHs->heaps) {
            // mspace_merge_objects takes two allocated objects, and
            // if the second immediately follows the first, will merge
            // them, returning a larger object occupying the same
            // memory. This is a local operation, and doesn't require
            // dlmalloc to manipulate any freelists. It's pretty
            // inexpensive compared to free().

            // ptrs is an array of objects all in memory order, and if
            // client code has been allocating lots of short-lived
            // objects, this is likely to contain runs of objects all
            // now garbage, and thus highly amenable to this optimization.

            // Unroll the 0th iteration around the loop below,
            // countFree ptrs[0] and initializing merged.
            assert(ptrs[0] != NULL);
            assert(ptr2heap(gHs, ptrs[0]) == heap);
            countFree(heap, ptrs[0], &numBytes);
            void *merged = ptrs[0];
            for (size_t i = 1; i < numPtrs; i++) {
                assert(merged != NULL);
                assert(ptrs[i] != NULL);
                assert((intptr_t)merged < (intptr_t)ptrs[i]);
                assert(ptr2heap(gHs, ptrs[i]) == heap);
                countFree(heap, ptrs[i], &numBytes);
                // Try to merge. If it works, merged now includes the
                // memory of ptrs[i]. If it doesn't, free merged, and
                // see if ptrs[i] starts a new run of adjacent
                // objects to merge.
                if (mspace_merge_objects(msp, merged, ptrs[i]) == NULL) {
                    mspace_free(msp, merged);
                    merged = ptrs[i];
                }
            }
            assert(merged != NULL);
            mspace_free(msp, merged);
        } else {
            // This is not an 'active heap'. Only do the accounting.
            for (size_t i = 0; i < numPtrs; i++) {
                assert(ptrs[i] != NULL);
                assert(ptr2heap(gHs, ptrs[i]) == heap);
                countFree(heap, ptrs[i], &numBytes);
            }
        }
    }
    return numBytes;
}

/*
 * Returns true iff <ptr> is in the heap source.
 */
bool dvmHeapSourceContainsAddress(const void *ptr)
{
    HS_BOILERPLATE();

    return (dvmHeapBitmapCoversAddress(&gHs->liveBits, ptr));
}

/*
 * Returns true iff <ptr> was allocated from the heap source.
 */
bool dvmHeapSourceContains(const void *ptr)
{
    HS_BOILERPLATE();

    if (dvmHeapSourceContainsAddress(ptr)) {
        return dvmHeapBitmapIsObjectBitSet(&gHs->liveBits, ptr) != 0;
    }
    return false;
}

bool dvmIsZygoteObject(const Object* obj)
{
    HeapSource *hs = gHs;

    HS_BOILERPLATE();

    if (dvmHeapSourceContains(obj) && hs->sawZygote) {
        Heap *heap = ptr2heap(hs, obj);
        if (heap != NULL) {
            /* If the object is not in the active heap, we assume that
             * it was allocated as part of zygote.
             */
            return heap != hs->heaps;
        }
    }
    /* The pointer is outside of any known heap, or we are not
     * running in zygote mode.
     */
    return false;
}

/*
 * Returns the number of usable bytes in an allocated chunk; the size
 * may be larger than the size passed to dvmHeapSourceAlloc().
 */
size_t dvmHeapSourceChunkSize(const void *ptr)
{
    HS_BOILERPLATE();

    Heap* heap = ptr2heap(gHs, ptr);
    if (heap != NULL) {
        return mspace_usable_size(heap->msp, ptr);
    }
    return 0;
}

/*
 * Returns the number of bytes that the heap source has allocated
 * from the system using sbrk/mmap, etc.
 *
 * Caller must hold the heap lock.
 */
size_t dvmHeapSourceFootprint()
{
    HS_BOILERPLATE();

//TODO: include size of bitmaps?
    return oldHeapOverhead(gHs, true);
}

static size_t getMaximumSize(const HeapSource *hs)
{
    return hs->growthLimit;
}

/*
 * Returns the current maximum size of the heap source respecting any
 * growth limits.
 */
size_t dvmHeapSourceGetMaximumSize()
{
    HS_BOILERPLATE();
    return getMaximumSize(gHs);
}

/*
 * Removes any growth limits.  Allows the user to allocate up to the
 * maximum heap size.
 */
void dvmClearGrowthLimit()
{
    HS_BOILERPLATE();
    dvmLockHeap();
    dvmWaitForConcurrentGcToComplete();
    gDvm.gcHeap->cardTableLength = gDvm.gcHeap->cardTableMaxLength;
    gHs->growthLimit = gHs->maximumSize;
    size_t overhead = oldHeapOverhead(gHs, false);
    gHs->heaps[0].maximumSize = gHs->maximumSize - overhead;
    gHs->heaps[0].limit = gHs->heaps[0].base + gHs->heaps[0].maximumSize;
    dvmUnlockHeap();
}

/*
 * Return the real bytes used by old heaps plus the soft usage of the
 * current heap.  When a soft limit is in effect, this is effectively
 * what it's compared against (though, in practice, it only looks at
 * the current heap).
 */
static size_t getSoftFootprint(bool includeActive)
{
    HS_BOILERPLATE();

    HeapSource *hs = gHs;
    size_t ret = oldHeapOverhead(hs, false);
    if (includeActive) {
        ret += hs->heaps[0].bytesAllocated;
    }

    return ret;
}

/*
 * Gets the maximum number of bytes that the heap source is allowed
 * to allocate from the system.
 */
size_t dvmHeapSourceGetIdealFootprint()
{
    HeapSource *hs = gHs;

    HS_BOILERPLATE();

    return hs->idealSize;
}

/*
 * Sets the soft limit, handling any necessary changes to the allowed
 * footprint of the active heap.
 */
static void setSoftLimit(HeapSource *hs, size_t softLimit)
{
    /* Compare against the actual footprint, rather than the
     * max_allowed, because the heap may not have grown all the
     * way to the allowed size yet.
     */
    mspace msp = hs->heaps[0].msp;
    size_t currentHeapSize = mspace_footprint(msp);
    if (softLimit < currentHeapSize) {
        /* Don't let the heap grow any more, and impose a soft limit.
         */
        mspace_set_max_allowed_footprint(msp, currentHeapSize);
        hs->softLimit = softLimit;
    } else {
        /* Let the heap grow to the requested max, and remove any
         * soft limit, if set.
         */
        mspace_set_max_allowed_footprint(msp, softLimit);
        hs->softLimit = SIZE_MAX;
    }
}

/*
 * Sets the maximum number of bytes that the heap source is allowed
 * to allocate from the system.  Clamps to the appropriate maximum
 * value.
 */
static void setIdealFootprint(size_t max)
{
    HS_BOILERPLATE();

    HeapSource *hs = gHs;
    size_t maximumSize = getMaximumSize(hs);
    if (max > maximumSize) {
        LOGI_HEAP("Clamp target GC heap from %zd.%03zdMB to %u.%03uMB",
                FRACTIONAL_MB(max),
                FRACTIONAL_MB(maximumSize));
        max = maximumSize;
    }

    /* Convert max into a size that applies to the active heap.
     * Old heaps will count against the ideal size.
     */
    size_t overhead = getSoftFootprint(false);
    size_t activeMax;
    if (overhead < max) {
        activeMax = max - overhead;
    } else {
        activeMax = 0;
    }

    setSoftLimit(hs, activeMax);
    hs->idealSize = max;
}

/*
 * Make the ideal footprint equal to the current footprint.
 */
static void snapIdealFootprint()
{
    HS_BOILERPLATE();

    setIdealFootprint(getSoftFootprint(true));
}

/*
 * Gets the current ideal heap utilization, represented as a number
 * between zero and one.
 */
float dvmGetTargetHeapUtilization()
{
    HeapSource *hs = gHs;

    HS_BOILERPLATE();

    return (float)hs->targetUtilization / (float)HEAP_UTILIZATION_MAX;
}

/*
 * Sets the new ideal heap utilization, represented as a number
 * between zero and one.
 */
void dvmSetTargetHeapUtilization(float newTarget)
{
    HeapSource *hs = gHs;

    HS_BOILERPLATE();

    /* Clamp it to a reasonable range.
     */
    // TODO: This may need some tuning.
    if (newTarget < 0.2) {
        newTarget = 0.2;
    } else if (newTarget > 0.8) {
        newTarget = 0.8;
    }

    hs->targetUtilization =
            (size_t)(newTarget * (float)HEAP_UTILIZATION_MAX);
    LOGV("Set heap target utilization to %zd/%d (%f)",
            hs->targetUtilization, HEAP_UTILIZATION_MAX, newTarget);
}

/*
 * Given the size of a live set, returns the ideal heap size given
 * the current target utilization and MIN/MAX values.
 *
 * targetUtilization is in the range 1..HEAP_UTILIZATION_MAX.
 */
static size_t getUtilizationTarget(size_t liveSize, size_t targetUtilization)
{
    /* Use the current target utilization ratio to determine the
     * ideal heap size based on the size of the live set.
     */
    size_t targetSize = (liveSize / targetUtilization) * HEAP_UTILIZATION_MAX;

    /* Cap the amount of free space, though, so we don't end up
     * with, e.g., 8MB of free space when the live set size hits 8MB.
     */
    if (targetSize > liveSize + HEAP_IDEAL_FREE) {
        targetSize = liveSize + HEAP_IDEAL_FREE;
    } else if (targetSize < liveSize + HEAP_MIN_FREE) {
        targetSize = liveSize + HEAP_MIN_FREE;
    }
    return targetSize;
}

/*
 * Given the current contents of the active heap, increase the allowed
 * heap footprint to match the target utilization ratio.  This
 * should only be called immediately after a full mark/sweep.
 */
void dvmHeapSourceGrowForUtilization()
{
    HS_BOILERPLATE();

    HeapSource *hs = gHs;
    Heap* heap = hs2heap(hs);

    /* Use the current target utilization ratio to determine the
     * ideal heap size based on the size of the live set.
     * Note that only the active heap plays any part in this.
     *
     * Avoid letting the old heaps influence the target free size,
     * because they may be full of objects that aren't actually
     * in the working set.  Just look at the allocated size of
     * the current heap.
     */
    size_t currentHeapUsed = heap->bytesAllocated;
    size_t targetHeapSize =
            getUtilizationTarget(currentHeapUsed, hs->targetUtilization);

    /* The ideal size includes the old heaps; add overhead so that
     * it can be immediately subtracted again in setIdealFootprint().
     * If the target heap size would exceed the max, setIdealFootprint()
     * will clamp it to a legal value.
     */
    size_t overhead = getSoftFootprint(false);
    setIdealFootprint(targetHeapSize + overhead);

    size_t freeBytes = getAllocLimit(hs);
    if (freeBytes < CONCURRENT_MIN_FREE) {
        /* Not enough free memory to allow a concurrent GC. */
        heap->concurrentStartBytes = SIZE_MAX;
    } else {
        heap->concurrentStartBytes = freeBytes - CONCURRENT_START;
    }
}

/*
 * Return free pages to the system.
 * TODO: move this somewhere else, especially the native heap part.
 */
static void releasePagesInRange(void *start, void *end, void *nbytes)
{
    /* Linux requires that the madvise() start address is page-aligned.
    * We also align the end address.
    */
    start = (void *)ALIGN_UP_TO_PAGE_SIZE(start);
    end = (void *)((size_t)end & ~(SYSTEM_PAGE_SIZE - 1));
    if (start < end) {
        size_t length = (char *)end - (char *)start;
        madvise(start, length, MADV_DONTNEED);
        *(size_t *)nbytes += length;
    }
}

/*
 * Return unused memory to the system if possible.
 */
static void trimHeaps()
{
    HS_BOILERPLATE();

    HeapSource *hs = gHs;
    size_t heapBytes = 0;
    for (size_t i = 0; i < hs->numHeaps; i++) {
        Heap *heap = &hs->heaps[i];

        /* Return the wilderness chunk to the system.
         */
        mspace_trim(heap->msp, 0);

        /* Return any whole free pages to the system.
         */
        mspace_walk_free_pages(heap->msp, releasePagesInRange, &heapBytes);
    }

    /* Same for the native heap.
     */
    dlmalloc_trim(0);
    size_t nativeBytes = 0;
    dlmalloc_walk_free_pages(releasePagesInRange, &nativeBytes);

    LOGD_HEAP("madvised %zd (GC) + %zd (native) = %zd total bytes",
            heapBytes, nativeBytes, heapBytes + nativeBytes);
}

/*
 * Walks over the heap source and passes every allocated and
 * free chunk to the callback.
 */
void dvmHeapSourceWalk(void(*callback)(const void *chunkptr, size_t chunklen,
                                       const void *userptr, size_t userlen,
                                       void *arg),
                       void *arg)
{
    HS_BOILERPLATE();

    /* Walk the heaps from oldest to newest.
     */
//TODO: do this in address order
    HeapSource *hs = gHs;
    for (size_t i = hs->numHeaps; i > 0; --i) {
        mspace_walk_heap(hs->heaps[i-1].msp, callback, arg);
    }
}

/*
 * Gets the number of heaps available in the heap source.
 *
 * Caller must hold the heap lock, because gHs caches a field
 * in gDvm.gcHeap.
 */
size_t dvmHeapSourceGetNumHeaps()
{
    HS_BOILERPLATE();

    return gHs->numHeaps;
}

void *dvmHeapSourceGetImmuneLimit(bool isPartial)
{
    if (isPartial) {
        return hs2heap(gHs)->base;
    } else {
        return NULL;
    }
}
