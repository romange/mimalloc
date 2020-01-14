/*----------------------------------------------------------------------------
Copyright (c) 2018, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#include "mimalloc.h"
#include "mimalloc-internal.h"
#include "mimalloc-atomic.h"

#include <string.h>  // memset, memcpy



/* -----------------------------------------------------------
  Helpers
----------------------------------------------------------- */

// return `true` if ok, `false` to break
typedef bool (heap_page_visitor_fun)(mi_heap_t* heap, mi_page_queue_t* pq, mi_page_t* page, void* arg1, void* arg2);

// Visit all pages in a heap; returns `false` if break was called.
static bool mi_heap_visit_pages(mi_heap_t* heap, heap_page_visitor_fun* fn, void* arg1, void* arg2)
{
  if (heap==NULL || heap->page_count==0) return 0;

  // visit all pages
  #if MI_DEBUG>1
  size_t total = heap->page_count;
  #endif
  size_t count = 0;
  for (size_t i = 0; i <= MI_BIN_FULL; i++) {
    mi_page_queue_t* pq = &heap->pages[i];
    mi_page_t* page = pq->first;
    while(page != NULL) {
      mi_page_t* next = page->next; // save next in case the page gets removed from the queue
      mi_assert_internal(mi_page_heap(page) == heap);
      count++;
      if (!fn(heap, pq, page, arg1, arg2)) return false;
      page = next; // and continue
    }
  }
  mi_assert_internal(count == total);
  return true;
}


#if MI_DEBUG>=2
static bool mi_heap_page_is_valid(mi_heap_t* heap, mi_page_queue_t* pq, mi_page_t* page, void* arg1, void* arg2) {
  UNUSED(arg1);
  UNUSED(arg2);
  UNUSED(pq);
  mi_assert_internal(mi_page_heap(page) == heap);
  mi_segment_t* segment = _mi_page_segment(page);
  mi_assert_internal(segment->thread_id == heap->thread_id);
  mi_assert_expensive(_mi_page_is_valid(page));
  return true;
}

static bool mi_heap_is_valid(mi_heap_t* heap) {
  mi_assert_internal(heap!=NULL);
  mi_heap_visit_pages(heap, &mi_heap_page_is_valid, NULL, NULL);
  return true;
}
#endif




/* -----------------------------------------------------------
  "Collect" pages by migrating `local_free` and `thread_free`
  lists and freeing empty pages. This is done when a thread
  stops (and in that case abandons pages if there are still
  blocks alive)
----------------------------------------------------------- */

typedef enum mi_collect_e {
  NORMAL,
  FORCE,
  ABANDON
} mi_collect_t;

#if MI_DEBUG>=2
static bool mi_heap_page_check_collect(mi_heap_t* heap, mi_page_queue_t* pq, mi_page_t* page, void* arg_collect, void* arg2) {
  UNUSED(arg_collect);
  UNUSED(arg2);
  UNUSED(heap);
  mi_assert_internal(mi_heap_page_is_valid(heap, pq, page, NULL, NULL));
  mi_assert_internal(!mi_page_all_free(page));
  return true; // don't break
}
#endif

static void mi_heap_collect_ex(mi_heap_t* heap, mi_collect_t collect)
{
  if (!mi_heap_is_initialized(heap)) return;
  _mi_deferred_free(heap, collect > NORMAL);

  // absorb outstanding abandoned heaps (but not when abandoning)
  _mi_heap_try_reclaim_abandoned(heap, collect == FORCE /* all outstanding? */);
  
  // free thread delayed blocks. 
  _mi_heap_delayed_free(heap);

  // free retired pages
  _mi_heap_collect_retired(heap, true);

  #if MI_DEBUG>=2
  // check heap state
  mi_heap_visit_pages(heap, &mi_heap_page_check_collect, &collect, NULL);
  #endif
  
  // collect segment caches
  if (collect >= FORCE) {
    _mi_segment_thread_collect(&heap->tld->segments);
  }

  // collect regions
  if (collect >= FORCE && _mi_is_main_thread()) {
    _mi_mem_collect(&heap->tld->os);
  }
}

void mi_heap_collect(mi_heap_t* heap, bool force) mi_attr_noexcept {
  mi_heap_collect_ex(heap, (force ? FORCE : NORMAL));
}

void mi_collect(bool force) mi_attr_noexcept {
  mi_heap_collect(mi_get_default_heap(), force);
}

/* -----------------------------------------------------------
  Heap abandon
  When a thread terminates its backing heap is put in 
  a global abandoned list (if it still has live objects).
  Heaps in this list are reclaimed into the heaps of
  other threads on demand.
----------------------------------------------------------- */

static volatile _Atomic(mi_heap_t*) abandoned; // = NULL

// prepend a list of abondoned heaps atomically to the global abandoned list; O(n)
static void mi_heap_prepend_abandoned(mi_heap_t* first) {
  if (first == NULL) return;

  // first try if the abandoned list happens to be NULL
  if (mi_atomic_cas_ptr_weak(mi_atomic_cast(void*, &abandoned), first, NULL)) return;

  // if not, find the end 
  mi_heap_t* last = first;
  while (last->abandoned_next != NULL) {
    last = last->abandoned_next;
  }

  // and atomically prepend
  mi_heap_t* next;
  do {
    next = (mi_heap_t*)mi_atomic_read_ptr_relaxed(mi_atomic_cast(void*, &abandoned));
    last->abandoned_next = next;
  } while (!mi_atomic_cas_ptr_weak(mi_atomic_cast(void*, &abandoned), first, next));
}

// Release resources for a heap that is about to be abandoned (due to thread termination)
void _mi_heap_collect_abandon(mi_heap_t* heap) 
{
  mi_assert_internal(mi_heap_is_backing(heap));
  mi_heap_collect_ex(heap, ABANDON);
  _mi_stats_done(&heap->tld->stats);

  if (heap->page_count==0) {
    // free immediately
    _mi_heap_backing_free(heap);
    return;
  }
  else {
    // still live objects: push on the abandoned list
    heap->abandoned_next = NULL;
    mi_heap_prepend_abandoned(heap);  // a one-element list
  }
}

static void mi_heap_absorb(mi_heap_t* to, mi_heap_t* from);

// Try to reclaim an abandoned heap by absorbing it
void _mi_heap_try_reclaim_abandoned(mi_heap_t* heap, bool all) {
  if (heap->no_reclaim) return;

  // To avoid the A-B-A problem, grab the entire list atomically
  mi_heap_t* reclaim = (mi_heap_t*)mi_atomic_read_ptr_relaxed(mi_atomic_cast(void*, &abandoned));  // pre-read to avoid expensive atomic operations
  if (reclaim == NULL) return;
  reclaim = (mi_heap_t*)mi_atomic_exchange_ptr(mi_atomic_cast(void*, &abandoned), NULL);
  if (reclaim == NULL) return;

  // we got a non-empty list
  if (!all) {
    // pop one, and append the rest back to the abandoned list again
    // this is O(n) but simplifies the code a lot (as we don't have an A-B-A problem)
    // and probably ok since the length will tend to be small.
    mi_heap_t* next = reclaim->abandoned_next;  // next can be NULL
    reclaim->abandoned_next = NULL;
    mi_heap_prepend_abandoned(next);
  }

  // and absorb all the heaps we got
  do {
    mi_heap_t* next = reclaim->abandoned_next; // save next one
    mi_heap_absorb(heap, reclaim);
    _mi_segments_absorb(heap->thread_id, &heap->tld->segments, &reclaim->tld->segments);
    _mi_heap_backing_free(reclaim);
    reclaim = next;
  } while (reclaim != NULL);
}

/* -----------------------------------------------------------
  Heap new
----------------------------------------------------------- */

mi_heap_t* mi_heap_get_default(void) {
  mi_thread_init(); 
  return mi_get_default_heap();
}

mi_heap_t* mi_heap_get_backing(void) {
  mi_heap_t* heap = mi_heap_get_default();
  mi_assert_internal(heap!=NULL);
  mi_heap_t* bheap = heap->tld->heap_backing;
  mi_assert_internal(bheap!=NULL);
  mi_assert_internal(bheap->thread_id == _mi_thread_id());
  return bheap;
}

mi_heap_t* mi_heap_new(void) {
  mi_heap_t* bheap = mi_heap_get_backing();
  mi_heap_t* heap = mi_heap_malloc_tp(bheap, mi_heap_t);
  if (heap==NULL) return NULL;
  memcpy(heap, &_mi_heap_empty, sizeof(mi_heap_t));
  heap->tld = bheap->tld;
  heap->thread_id = _mi_thread_id();

  _mi_random_split(&bheap->random, &heap->random);
  heap->cookie = _mi_heap_random_next(heap) | 1;  
  heap->key[0] = _mi_heap_random_next(heap);
  heap->key[1] = _mi_heap_random_next(heap);
  heap->no_reclaim = true;  // don't reclaim abandoned pages or otherwise destroy is unsafe

  return heap;
}

uintptr_t _mi_heap_random_next(mi_heap_t* heap) {
  return _mi_random_next(&heap->random);
}

// zero out the page queues
static void mi_heap_reset_pages(mi_heap_t* heap) {
  mi_assert_internal(mi_heap_is_initialized(heap));
  // TODO: copy full empty heap instead?
  memset(&heap->pages_free_direct, 0, sizeof(heap->pages_free_direct));
#ifdef MI_MEDIUM_DIRECT
  memset(&heap->pages_free_medium, 0, sizeof(heap->pages_free_medium));
#endif
  memcpy(&heap->pages, &_mi_heap_empty.pages, sizeof(heap->pages));
  heap->thread_delayed_free = NULL;
  heap->page_count = 0;
}

// called from `mi_heap_destroy` and `mi_heap_delete` to free the internal heap resources.
static void mi_heap_free(mi_heap_t* heap) {
  mi_assert_internal(mi_heap_is_initialized(heap));
  if (mi_heap_is_backing(heap)) return; // dont free the backing heap
  
  // reset default
  if (mi_heap_is_default(heap)) {
    _mi_heap_set_default_direct(heap->tld->heap_backing);
  }
  // and free the used memory
  mi_free(heap);
}


/* -----------------------------------------------------------
  Heap destroy
----------------------------------------------------------- */

static bool _mi_heap_page_destroy(mi_heap_t* heap, mi_page_queue_t* pq, mi_page_t* page, void* arg1, void* arg2) {
  UNUSED(arg1);
  UNUSED(arg2);
  UNUSED(heap);
  UNUSED(pq);

  // TODO: ensure no more thread_delayed_free will be added?
  // _mi_page_use_delayed_free(page, MI_NEVER_DELAYED_FREE);  

  // stats
  const size_t bsize = mi_page_block_size(page);
  if (bsize > MI_LARGE_OBJ_SIZE_MAX) {
    if (bsize > MI_HUGE_OBJ_SIZE_MAX) {
      _mi_stat_decrease(&heap->tld->stats.giant,bsize);
    }
    else {
      _mi_stat_decrease(&heap->tld->stats.huge,bsize);
    }
  }
  #if (MI_STAT>1)
  _mi_page_free_collect(page, false);  // update used count
  const size_t inuse = page->used;
  if (bsize <= MI_LARGE_OBJ_SIZE_MAX)  {
    mi_heap_stat_decrease(heap,normal[_mi_bin(bsize)], inuse);
  }
  mi_heap_stat_decrease(heap,malloc, bsize * inuse);  // todo: off for aligned blocks...
  #endif

  // pretend it is all free now
  mi_assert_internal(mi_page_thread_free(page) == NULL);
  page->used = 0;

  // and free the page
  _mi_segment_page_free(page,false /* no force? */, &heap->tld->segments);

  return true; // keep going
}

void _mi_heap_destroy_pages(mi_heap_t* heap) {
  mi_heap_visit_pages(heap, &_mi_heap_page_destroy, NULL, NULL);
  mi_heap_reset_pages(heap);
}

void mi_heap_destroy(mi_heap_t* heap) {
  mi_assert(mi_heap_is_initialized(heap));
  mi_assert(heap->no_reclaim);
  mi_assert_expensive(mi_heap_is_valid(heap));
  if (!mi_heap_is_initialized(heap)) return;
  if (!heap->no_reclaim) {
    // don't free in case it may contain reclaimed pages
    mi_heap_delete(heap);
  }
  else {
    // free all pages
    _mi_heap_destroy_pages(heap);
    mi_heap_free(heap);
  }
}



/* -----------------------------------------------------------
  Safe Heap delete
----------------------------------------------------------- */

// Transfer the pages from one heap to the other
static void mi_heap_absorb(mi_heap_t* heap, mi_heap_t* from) {
  mi_assert_internal(heap!=NULL);
  if (from==NULL || from->page_count == 0) return;
  //mi_assert_internal(from->thread_id==0 || heap->thread_id == from->thread_id);
  //mi_assert_internal(from->thread_id==0 || heap->tld == from->tld);

  // transfer all pages by appending the queues; this will set
  // a new heap pointer in the page; this is ok but it means other 
  // threads may add to either heap's `thread_delayed_free` list during this time.
  for (size_t i = 0; i <= MI_BIN_FULL; i++) {
    mi_page_queue_t* pq = &heap->pages[i];
    mi_page_queue_t* append = &from->pages[i];
    size_t pcount = _mi_page_queue_append(heap, pq, append);
    heap->page_count += pcount;
    from->page_count -= pcount;
  }

  // Now append the `thread_delayed_free` list atomically
  mi_block_t* first;
  do {
    first = (mi_block_t*)mi_atomic_read_ptr_relaxed(mi_atomic_cast(void*, &from->thread_delayed_free));
  } while (!mi_atomic_cas_ptr_strong(mi_atomic_cast(void*, &from->thread_delayed_free), NULL, first));
  if (first != NULL) {
    // find the end and re-encode the list
    mi_block_t* last = first;
    mi_block_t* next;
    while((next = mi_block_nextx(from, last, from->key[0], from->key[1])) != NULL) {
      mi_block_set_nextx(heap, last, next, heap->key[0], heap->key[1]); // re-encode
      last = next;
    }
    // and append to the heap thread_delayed_free list
    mi_block_t* block;
    do {
      block = (mi_block_t*)mi_atomic_read_ptr_relaxed(mi_atomic_cast(void*,&heap->thread_delayed_free));
      mi_block_set_nextx(heap, last, block, heap->key[0], heap->key[1]); // append
    } while(!mi_atomic_cas_ptr_strong(mi_atomic_cast(void*, &heap->thread_delayed_free), first, block));
  }

  mi_assert_internal(from->thread_delayed_free == NULL);
  mi_assert_internal(from->page_count == 0);
  
  // and reset the `from` heap
  mi_heap_reset_pages(from);
}

// Safe delete a heap without freeing any still allocated blocks in that heap.
void mi_heap_delete(mi_heap_t* heap)
{
  mi_assert(mi_heap_is_initialized(heap));
  mi_assert_expensive(mi_heap_is_valid(heap));
  if (!mi_heap_is_initialized(heap)) return;

  if (!mi_heap_is_backing(heap)) {
    // transfer still used pages to the backing heap
    mi_heap_absorb(heap->tld->heap_backing, heap);
  }
  else {
    // the backing heap abandons its pages
    _mi_heap_collect_abandon(heap);
  }
  mi_assert_internal(heap->page_count==0);
  mi_heap_free(heap);
}

mi_heap_t* mi_heap_set_default(mi_heap_t* heap) {
  mi_assert(mi_heap_is_initialized(heap));
  if (!mi_heap_is_initialized(heap)) return NULL;
  mi_assert_expensive(mi_heap_is_valid(heap));
  mi_heap_t* old = mi_get_default_heap(); 
  _mi_heap_set_default_direct(heap);
  return old;
}




/* -----------------------------------------------------------
  Analysis
----------------------------------------------------------- */

// static since it is not thread safe to access heaps from other threads.
static mi_heap_t* mi_heap_of_block(const void* p) {
  if (p == NULL) return NULL;
  mi_segment_t* segment = _mi_ptr_segment(p);
  bool valid = (_mi_ptr_cookie(segment) == segment->cookie);
  mi_assert_internal(valid);
  if (mi_unlikely(!valid)) return NULL;
  return mi_page_heap(_mi_segment_page_of(segment,p));
}

bool mi_heap_contains_block(mi_heap_t* heap, const void* p) {
  mi_assert(heap != NULL);
  if (!mi_heap_is_initialized(heap)) return false;
  return (heap == mi_heap_of_block(p));
}


static bool mi_heap_page_check_owned(mi_heap_t* heap, mi_page_queue_t* pq, mi_page_t* page, void* p, void* vfound) {
  UNUSED(heap);
  UNUSED(pq);
  bool* found = (bool*)vfound;
  mi_segment_t* segment = _mi_page_segment(page);
  void* start = _mi_page_start(segment, page, NULL);
  void* end   = (uint8_t*)start + (page->capacity * mi_page_block_size(page));
  *found = (p >= start && p < end);
  return (!*found); // continue if not found
}

bool mi_heap_check_owned(mi_heap_t* heap, const void* p) {
  mi_assert(heap != NULL);
  if (!mi_heap_is_initialized(heap)) return false;
  if (((uintptr_t)p & (MI_INTPTR_SIZE - 1)) != 0) return false;  // only aligned pointers
  bool found = false;
  mi_heap_visit_pages(heap, &mi_heap_page_check_owned, (void*)p, &found);
  return found;
}

bool mi_check_owned(const void* p) {
  return mi_heap_check_owned(mi_get_default_heap(), p);
}

/* -----------------------------------------------------------
  Visit all heap blocks and areas
  Todo: enable visiting abandoned pages, and
        enable visiting all blocks of all heaps across threads
----------------------------------------------------------- */

// Separate struct to keep `mi_page_t` out of the public interface
typedef struct mi_heap_area_ex_s {
  mi_heap_area_t area;
  mi_page_t*     page;
} mi_heap_area_ex_t;

static bool mi_heap_area_visit_blocks(const mi_heap_area_ex_t* xarea, mi_block_visit_fun* visitor, void* arg) {
  mi_assert(xarea != NULL);
  if (xarea==NULL) return true;
  const mi_heap_area_t* area = &xarea->area;
  mi_page_t* page = xarea->page;
  mi_assert(page != NULL);
  if (page == NULL) return true;

  _mi_page_free_collect(page,true);
  mi_assert_internal(page->local_free == NULL);
  if (page->used == 0) return true;

  const size_t bsize = mi_page_block_size(page);
  size_t   psize;
  uint8_t* pstart = _mi_page_start(_mi_page_segment(page), page, &psize);

  if (page->capacity == 1) {
    // optimize page with one block
    mi_assert_internal(page->used == 1 && page->free == NULL);
    return visitor(mi_page_heap(page), area, pstart, bsize, arg);
  }

  // create a bitmap of free blocks.
  #define MI_MAX_BLOCKS   (MI_SMALL_PAGE_SIZE / sizeof(void*))
  uintptr_t free_map[MI_MAX_BLOCKS / sizeof(uintptr_t)];
  memset(free_map, 0, sizeof(free_map));

  size_t free_count = 0;
  for (mi_block_t* block = page->free; block != NULL; block = mi_block_next(page,block)) {
    free_count++;
    mi_assert_internal((uint8_t*)block >= pstart && (uint8_t*)block < (pstart + psize));
    size_t offset = (uint8_t*)block - pstart;
    mi_assert_internal(offset % bsize == 0);
    size_t blockidx = offset / bsize;  // Todo: avoid division?
    mi_assert_internal( blockidx < MI_MAX_BLOCKS);
    size_t bitidx = (blockidx / sizeof(uintptr_t));
    size_t bit = blockidx - (bitidx * sizeof(uintptr_t));
    free_map[bitidx] |= ((uintptr_t)1 << bit);
  }
  mi_assert_internal(page->capacity == (free_count + page->used));

  // walk through all blocks skipping the free ones
  size_t used_count = 0;
  for (size_t i = 0; i < page->capacity; i++) {
    size_t bitidx = (i / sizeof(uintptr_t));
    size_t bit = i - (bitidx * sizeof(uintptr_t));
    uintptr_t m = free_map[bitidx];
    if (bit == 0 && m == UINTPTR_MAX) {
      i += (sizeof(uintptr_t) - 1); // skip a run of free blocks
    }
    else if ((m & ((uintptr_t)1 << bit)) == 0) {
      used_count++;
      uint8_t* block = pstart + (i * bsize);
      if (!visitor(mi_page_heap(page), area, block, bsize, arg)) return false;
    }
  }
  mi_assert_internal(page->used == used_count);
  return true;
}

typedef bool (mi_heap_area_visit_fun)(const mi_heap_t* heap, const mi_heap_area_ex_t* area, void* arg);


static bool mi_heap_visit_areas_page(mi_heap_t* heap, mi_page_queue_t* pq, mi_page_t* page, void* vfun, void* arg) {
  UNUSED(heap);
  UNUSED(pq);
  _mi_page_free_collect(page, false); // update used
  mi_heap_area_visit_fun* fun = (mi_heap_area_visit_fun*)vfun;
  mi_heap_area_ex_t xarea;
  const size_t bsize = mi_page_block_size(page);
  xarea.page = page;
  xarea.area.reserved = page->reserved * bsize;
  xarea.area.committed = page->capacity * bsize;
  xarea.area.blocks = _mi_page_start(_mi_page_segment(page), page, NULL);
  xarea.area.used  = page->used;
  xarea.area.block_size = bsize;
  return fun(heap, &xarea, arg);
}

// Visit all heap pages as areas
static bool mi_heap_visit_areas(const mi_heap_t* heap, mi_heap_area_visit_fun* visitor, void* arg) {
  if (visitor == NULL) return false;
  return mi_heap_visit_pages((mi_heap_t*)heap, &mi_heap_visit_areas_page, (void*)(visitor), arg); // note: function pointer to void* :-{
}

// Just to pass arguments
typedef struct mi_visit_blocks_args_s {
  bool  visit_blocks;
  mi_block_visit_fun* visitor;
  void* arg;
} mi_visit_blocks_args_t;

static bool mi_heap_area_visitor(const mi_heap_t* heap, const mi_heap_area_ex_t* xarea, void* arg) {
  mi_visit_blocks_args_t* args = (mi_visit_blocks_args_t*)arg;
  if (!args->visitor(heap, &xarea->area, NULL, xarea->area.block_size, args->arg)) return false;
  if (args->visit_blocks) {
    return mi_heap_area_visit_blocks(xarea, args->visitor, args->arg);
  }
  else {
    return true;
  }
}

// Visit all blocks in a heap
bool mi_heap_visit_blocks(const mi_heap_t* heap, bool visit_blocks, mi_block_visit_fun* visitor, void* arg) {
  mi_visit_blocks_args_t args = { visit_blocks, visitor, arg };
  return mi_heap_visit_areas(heap, &mi_heap_area_visitor, &args);
}

