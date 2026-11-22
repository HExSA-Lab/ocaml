/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*            Nathaniel Chappelle, HExSA Lab                              */
/*                                                                        */
/*   Copyright 2026 Nathaniel Chappelle, HExSA Lab                        */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#define CAML_INTERNALS

/* Far memory region: reservation, NUMA binding and a page-granular
   first-fit allocator. See caml/far_heap.h. */

#include <sys/mman.h>
#include <numa.h>
#include "caml/far_heap.h"
#include "caml/memory.h"
#include "caml/misc.h"
#include "caml/platform.h"
#include "caml/startup_aux.h"

uintnat caml_far_start = 0;
uintnat caml_far_end = 0;

static int far_node = -1;

/* A free range of the far region. The list is kept sorted by address and
   adjacent ranges are coalesced. Nodes live in malloc'd memory so that
   freed far pages can be released. */
struct far_range {
  uintnat start;
  uintnat bsize;
  struct far_range* next;
};

static caml_plat_mutex far_lock = CAML_PLAT_MUTEX_INITIALIZER;
static struct far_range* far_free_list = NULL;
static int far_initialized = 0;

/* Must hold [far_lock]. Returns 0 on failure. */
static int far_init_with_lock(void)
{
  uintnat bsize = caml_mem_round_up_pages(caml_params->far_region_bsz);
  void* mem;

  if (far_initialized) return caml_far_start != 0;
  far_initialized = 1;

  mem = mmap(NULL, bsize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (mem == MAP_FAILED) {
    caml_gc_log("Far region: failed to reserve %" CAML_PRIuNAT " bytes",
                bsize);
    return 0;
  }

  if (numa_available() >= 0
      && (intnat)caml_params->far_numa_node <= numa_max_node()) {
    far_node = (int)caml_params->far_numa_node;
    numa_tonode_memory(mem, bsize, far_node);
  } else {
    far_node = -1;
    caml_gc_log("Far region: NUMA node %" CAML_PRIuNAT " unavailable, "
                "far memory is unbound", caml_params->far_numa_node);
  }

  far_free_list = caml_stat_alloc_noexc(sizeof(struct far_range));
  if (far_free_list == NULL) {
    munmap(mem, bsize);
    return 0;
  }
  far_free_list->start = (uintnat)mem;
  far_free_list->bsize = bsize;
  far_free_list->next = NULL;

  caml_far_start = (uintnat)mem;
  caml_far_end = (uintnat)mem + bsize;
  caml_gc_log("Far region: %" CAML_PRIuNAT " bytes at %p, node %d",
              bsize, mem, far_node);
  return 1;
}

void* caml_far_alloc(uintnat bsize)
{
  struct far_range **prev, *r;
  void* res = NULL;

  bsize = caml_mem_round_up_pages(bsize);

  caml_plat_lock_blocking(&far_lock);
  if (!far_init_with_lock()) goto out;

  for (prev = &far_free_list; (r = *prev) != NULL; prev = &r->next) {
    if (r->bsize >= bsize) {
      res = (void*)r->start;
      r->start += bsize;
      r->bsize -= bsize;
      if (r->bsize == 0) {
        *prev = r->next;
        caml_stat_free(r);
      }
      break;
    }
  }

 out:
  caml_plat_unlock(&far_lock);
  return res;
}

void caml_far_free(void* mem, uintnat bsize)
{
  uintnat start = (uintnat)mem;
  struct far_range **prev, *r, *prev_r = NULL;

  bsize = caml_mem_round_up_pages(bsize);
  CAMLassert(Is_far_addr(mem) && start + bsize <= caml_far_end);

  /* Release the physical pages; the NUMA binding of the range is kept. */
  madvise(mem, bsize, MADV_DONTNEED);

  caml_plat_lock_blocking(&far_lock);

  /* Find the insertion point, keeping the list sorted by address. */
  for (prev = &far_free_list; (r = *prev) != NULL && r->start < start;
       prev = &r->next)
    prev_r = r;

  if (prev_r != NULL && prev_r->start + prev_r->bsize == start) {
    /* Coalesce with the previous range. */
    prev_r->bsize += bsize;
    if (r != NULL && start + bsize == r->start) {
      prev_r->bsize += r->bsize;
      prev_r->next = r->next;
      caml_stat_free(r);
    }
  } else if (r != NULL && start + bsize == r->start) {
    /* Coalesce with the next range. */
    r->start = start;
    r->bsize += bsize;
  } else {
    struct far_range* n = caml_stat_alloc_noexc(sizeof(struct far_range));
    /* If we cannot record the range, it is leaked (but already released
       to the OS by the madvise above). */
    if (n != NULL) {
      n->start = start;
      n->bsize = bsize;
      n->next = r;
      *prev = n;
    }
  }

  caml_plat_unlock(&far_lock);
}

int caml_far_node(void)
{
  return far_node;
}
