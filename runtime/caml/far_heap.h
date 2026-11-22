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

#ifndef CAML_FAR_HEAP_H
#define CAML_FAR_HEAP_H

#ifdef CAML_INTERNALS

#include "misc.h"

/* Far memory.

   A single contiguous virtual region is reserved (lazily, on the first far
   allocation) and bound to the far NUMA node (OCAMLRUNPARAM=N, default 1).
   Pages are faulted in on that node on first touch. Far pools and far large
   allocations of the shared heap are carved out of this region, so whether
   an address is far is a simple range check.

   If libnuma is unavailable, or the requested node does not exist, the
   region is still reserved but left unbound, so far semantics (and tests)
   still work on single-node machines. */

extern uintnat caml_far_start;
extern uintnat caml_far_end;

/* Is [p] inside the far region? Before the region is reserved this is
   always false, which is correct since no far memory exists yet. */
Caml_inline int Is_far_addr(const void* p)
{
  return (uintnat)p >= caml_far_start && (uintnat)p < caml_far_end;
}

/* Allocate [bsize] bytes (rounded up to whole pages) of page-aligned far
   memory. Returns NULL on failure. Thread-safe. */
void* caml_far_alloc(uintnat bsize);

/* Return memory obtained from [caml_far_alloc]. [bsize] must be the size
   passed to [caml_far_alloc]. The physical pages are released, but the
   range stays bound to the far node for reuse. Thread-safe. */
void caml_far_free(void* mem, uintnat bsize);

/* The NUMA node far memory is bound to, or -1 if it is unbound. Only
   meaningful once far memory has been allocated. */
int caml_far_node(void);

#endif /* CAML_INTERNALS */

#endif /* CAML_FAR_HEAP_H */
