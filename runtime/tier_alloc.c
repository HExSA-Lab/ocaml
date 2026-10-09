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

struct tier_config {
  const char *name;
  int node;
  unsigned long *mask;
  int error_number;
  char error[128];
};

static struct tier_config near_config =
  { .name = "OCAML_NEAR_NODE", .node = -1 };
static struct tier_config far_config =
  { .name = "OCAML_FAR_NODE", .node = -1 };
static pthread_once_t config_once = PTHREAD_ONCE_INIT;
static size_t page_bytes;
static unsigned long mask_bits;

static void config_error(struct tier_config *config, int number,
                         const char *message)
{
  config->error_number = number;
  snprintf(config->error, sizeof(config->error), "%s: %s",
           config->name, message);
}

static void read_config(struct tier_config *config, int required)
{
  const char *text = caml_secure_getenv(config->name);
  if (text == NULL) {
    if (required) config_error(config, EINVAL, "must be set for far allocation");
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
  config->node = (int)node;
  return;

invalid:
  config_error(config, EINVAL, "expected a non-negative decimal node ID");
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
  read_config(&near_config, 0);
  read_config(&far_config, 1);
  /* Default near allocation must work without NUMA configuration or queries. */
  if (near_config.node < 0 && far_config.node < 0) return;

  errno = 0;
  long page = sysconf(_SC_PAGESIZE);
  int init_errno = page > 0 ? 0 : (errno != 0 ? errno : EINVAL);
  const char *init_error = "cannot determine the system page size";
  unsigned long *allowed = NULL;
  if (init_errno == 0) {
    page_bytes = (size_t)page;
    allowed = allowed_nodes();
    if (allowed == NULL) {
      init_errno = errno;
      init_error = "get_mempolicy(MPOL_F_MEMS_ALLOWED) failed";
    }
  }

  struct tier_config *configs[] = { &near_config, &far_config };
  const unsigned long word_bits = sizeof(unsigned long) * CHAR_BIT;
  for (size_t i = 0; i < 2; i++) {
    struct tier_config *config = configs[i];
    if (config->node < 0) continue;
    if (init_errno != 0) {
      config_error(config, init_errno, init_error);
    } else if ((unsigned long)config->node >= mask_bits
               || !(allowed[config->node / word_bits]
                    & (1UL << (config->node % word_bits)))) {
      config_error(config, EINVAL, "node is not an allowed memory node");
    } else {
      config->mask = calloc(mask_bits / word_bits, sizeof(unsigned long));
      if (config->mask == NULL) {
        config_error(config, ENOMEM, "cannot allocate the node mask");
      } else {
        config->mask[config->node / word_bits] =
          1UL << (config->node % word_bits);
      }
    }
  }
  free(allowed);
  /* Successful masks live for the process lifetime. No OCaml allocations or
     exceptions occur inside this pthread_once initializer. */
}

CAMLexport void *caml_tier_alloc(size_t size, int far, int *mapped,
                                const char **error)
{
  *mapped = 0;
  *error = NULL;
  int rc = pthread_once(&config_once, init_config);
  if (rc != 0) {
    errno = rc;
    *error = "pthread_once";
    return NULL;
  }
  const struct tier_config *config = far ? &far_config : &near_config;
  if (config->error_number != 0) {
    errno = config->error_number;
    *error = config->error;
    return NULL;
  }
  /* A non-NULL empty buffer also prevents views from requesting allocation. */
  if (size == 0) size = 1;
  if (config->node < 0) {
    void *data = malloc(size);
    if (data == NULL) *error = "malloc";
    return data;
  }
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
  *error = far ? "OCAML_FAR_NODE: mbind" : "OCAML_NEAR_NODE: mbind";
  if (syscall(SYS_mbind, data, mapped_bytes, MPOL_BIND,
              (const unsigned long *)config->mask,
              mask_bits + 1, 0UL) != 0) {
    int saved_errno = errno;
    (void)munmap(data, mapped_bytes);
    errno = saved_errno;
    return NULL;
  }

  *mapped = 1;
  *error = NULL;
  return data;
}

CAMLexport void caml_tier_free(void *data, size_t size)
{
  /* Linux rounds the length up to pages; match the empty allocation case. */
  if (data != NULL) (void)munmap(data, size == 0 ? 1 : size);
}
