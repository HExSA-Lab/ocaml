/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*   Copyright 2026 Plabon Dutta.                                          */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#ifndef CAML_TIER_ALLOC_H
#define CAML_TIER_ALLOC_H

#ifdef CAML_INTERNALS

#include "misc.h"

/* Linux allocation policy, read once on the first call (safe across domains):
   - far == 0, OCAML_NEAR_NODE unset: ordinary malloc, *mapped = 0.
   - far == 0, OCAML_NEAR_NODE set: mmap + mbind to that node, *mapped = 1.
   - far != 0: mmap + mbind to OCAML_FAR_NODE, *mapped = 1. This variable must
     be set for far allocations; missing/invalid far settings do not break
     default near allocations. Node IDs are non-negative decimal integers.
   The same near and far node is allowed. Configuration is fixed for the process
   lifetime; launch with numactl to select the default malloc/heap policy.

   Zero bytes still returns a non-NULL, releasable buffer on success.
   On failure, return NULL, set errno, and set *error to a static string naming
   the failed operation or environment variable, and set *mapped = 0.
   On success, set *error to NULL. mapped and error must both be non-NULL.
   These functions neither allocate OCaml values nor raise OCaml exceptions. */
CAMLextern void *caml_tier_alloc(size_t size, int far, int *mapped,
                                const char **error);

/* Release only buffers returned with *mapped = 1; use ordinary free otherwise.
   data must be NULL or the original mapping address, never a slice's interior
   pointer. size must be the original allocation's logical byte size, including
   zero for an empty buffer. The backend stores no per-buffer header. */
CAMLextern void caml_tier_free(void *data, size_t size);

#endif /* CAML_INTERNALS */
#endif /* CAML_TIER_ALLOC_H */
