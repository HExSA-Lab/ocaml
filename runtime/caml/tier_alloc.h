#ifndef CAML_TIER_ALLOC_H
#define CAML_TIER_ALLOC_H

#ifdef CAML_INTERNALS

#include "misc.h"

/* Far-memory buffers: mmap + mbind to the node in OCAML_FAR_NODE, which is read
   once on the first call. Zero bytes still returns a releasable buffer.
   On failure, return NULL, set errno, and set *error to a static string naming
   the failed step. These functions neither allocate OCaml values nor raise. */
CAMLextern void *caml_tier_alloc(size_t size, const char **error);

/* data is the pointer returned by caml_tier_alloc and size its original
   logical byte size. */
CAMLextern void caml_tier_free(void *data, size_t size);

#endif /* CAML_INTERNALS */
#endif /* CAML_TIER_ALLOC_H */
