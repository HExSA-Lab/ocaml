#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef __linux__
#error "The near/far allocator requires Linux."
#endif
#define CAML_INTERNALS

#include "caml/tier_alloc.h"
#include "caml/osdeps.h"
#include <errno.h>
#include <linux/mempolicy.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static int far_node = -1;
static unsigned long *far_mask;
static int far_errno;
static char far_error[128];
static pthread_once_t config_once = PTHREAD_ONCE_INIT;
static size_t page_bytes;
static unsigned long mask_bits;

static void config_error(int number, const char *message)
{
  far_errno = number;
  snprintf(far_error, sizeof(far_error), "OCAML_FAR_NODE: %s", message);
}

static void read_config(void)
{
  const char *text = caml_secure_getenv("OCAML_FAR_NODE");
  if (text == NULL) {
    config_error(EINVAL, "must be set for far allocation");
    return;
  }
  /* Accept decimal node IDs only, without signs, whitespace, or suffixes. */
  if (*text == '\0') goto invalid;
  for (const char *p = text; *p != '\0'; p++) {
    if (*p < '0' || *p > '9') goto invalid;
  }
  errno = 0;
  unsigned long node = strtoul(text, NULL, 10);
  if (errno == ERANGE || node > INT_MAX) goto invalid;
  far_node = (int)node;
  return;

invalid:
  config_error(EINVAL, "expected a non-negative decimal node ID");
}

static unsigned long *allowed_nodes(void)
{
  const unsigned long word_bits = sizeof(unsigned long) * CHAR_BIT;
  mask_bits = word_bits;
  for (;;) {
    size_t words = mask_bits / word_bits;
    if (words > SIZE_MAX / sizeof(unsigned long)) {
      errno = ENOMEM;
      return NULL;
    }
    unsigned long *mask = calloc(words, sizeof(unsigned long));
    if (mask == NULL) return NULL;
    if (syscall(SYS_get_mempolicy, (int *)NULL, mask, mask_bits,
                (void *)NULL, (unsigned long)MPOL_F_MEMS_ALLOWED) == 0)
      return mask;

    int saved_errno = errno;
    free(mask);
    /* Cover the host's supported node IDs, but stop if EINVAL persists at
       Linux's one-page node-mask limit (32768 bits with 4 KiB pages). */
    if (saved_errno != EINVAL || mask_bits >= page_bytes * CHAR_BIT) {
      errno = saved_errno;
      return NULL;
    }
    mask_bits *= 2;
  }
}

static void init_config(void)
{
  read_config();
  if (far_node < 0) return;

  errno = 0;
  long page = sysconf(_SC_PAGESIZE);
  if (page <= 0) {
    config_error(errno != 0 ? errno : EINVAL,
                 "cannot determine the system page size");
    return;
  }
  page_bytes = (size_t)page;

  unsigned long *allowed = allowed_nodes();
  if (allowed == NULL) {
    config_error(errno, "get_mempolicy(MPOL_F_MEMS_ALLOWED) failed");
    return;
  }

  const unsigned long word_bits = sizeof(unsigned long) * CHAR_BIT;
  if ((unsigned long)far_node >= mask_bits
      || !(allowed[far_node / word_bits] & (1UL << (far_node % word_bits)))) {
    config_error(EINVAL, "node is not an allowed memory node");
  } else {
    far_mask = calloc(mask_bits / word_bits, sizeof(unsigned long));
    if (far_mask == NULL)
      config_error(ENOMEM, "cannot allocate the node mask");
    else
      far_mask[far_node / word_bits] = 1UL << (far_node % word_bits);
  }
  free(allowed);
}

CAMLexport void *caml_tier_alloc(size_t size, const char **error)
{
  *error = NULL;
  int rc = pthread_once(&config_once, init_config);
  if (rc != 0) {
    errno = rc;
    *error = "pthread_once";
    return NULL;
  }
  if (far_mask == NULL) {
    errno = far_errno;
    *error = far_error;
    return NULL;
  }
  /* A non-NULL empty buffer also prevents views from requesting allocation. */
  if (size == 0) size = 1;
  *error = "size rounding";
  if (size > SIZE_MAX - (page_bytes - 1)) {
    errno = ENOMEM;
    return NULL;
  }
  size_t mapped_bytes = (size + page_bytes - 1) / page_bytes * page_bytes;

  *error = "mmap";
  void *data = mmap(NULL, mapped_bytes, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (data == MAP_FAILED) return NULL;

  /* Bind before touching pages. Linux's get_nodes consumes maxnode - 1 bits,
     so add one here to keep the highest mask bit (get_mempolicy differs). */
  *error = "OCAML_FAR_NODE: mbind";
  if (syscall(SYS_mbind, data, mapped_bytes, MPOL_BIND,
              (const unsigned long *)far_mask, mask_bits + 1, 0UL) != 0) {
    int saved_errno = errno;
    (void)munmap(data, mapped_bytes);
    errno = saved_errno;
    return NULL;
  }

  *error = NULL;
  return data;
}

CAMLexport void caml_tier_free(void *data, size_t size)
{
  /* Linux rounds the length up to pages; match the empty allocation case. */
  if (data != NULL) (void)munmap(data, size == 0 ? 1 : size);
}
