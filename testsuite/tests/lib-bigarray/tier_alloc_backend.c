/* Standalone tier allocator checks. Run from a configured project root on Linux
   (run ./configure first if needed). Supply nodes accessible to this process;
   the examples below use near node 0 and far node 1.

   The test supplies caml_secure_getenv so it can link without the whole runtime.

   cc -std=c11 -Wall -Wextra -Werror -pthread -Iruntime \
     runtime/tier_alloc.c testsuite/tests/lib-bigarray/tier_alloc_backend.c \
     -o /tmp/tier_alloc_backend
   env -u OCAML_NEAR_NODE -u OCAML_FAR_NODE /tmp/tier_alloc_backend default
   env -u OCAML_NEAR_NODE OCAML_FAR_NODE=1 \
     numactl --cpunodebind=0 --membind=0 /tmp/tier_alloc_backend bound -1 1
   OCAML_NEAR_NODE=0 OCAML_FAR_NODE=1 /tmp/tier_alloc_backend bound 0 1
   OCAML_NEAR_NODE=0 OCAML_FAR_NODE=0 /tmp/tier_alloc_backend bound 0 0
   OCAML_NEAR_NODE=0 OCAML_FAR_NODE=1 /tmp/tier_alloc_backend threads 0 1

   Each command starts a fresh process because configuration is read once.
   Reject checks take the failing tier (0=near, 1=far) and the other tier's
   expected node (-1 means malloc). Repeat for malformed/unavailable node IDs:

   env -u OCAML_NEAR_NODE OCAML_FAR_NODE=invalid \
     /tmp/tier_alloc_backend reject 1 -1
   OCAML_NEAR_NODE=invalid OCAML_FAR_NODE=1 \
     /tmp/tier_alloc_backend reject 0 1

   Failure injection and simulated nodes use GNU linker wrappers confined
   to this executable. No machine-wide settings are changed:

   cc -std=c11 -Wall -Wextra -Werror -pthread -DTIER_ALLOC_TEST_FAILURES \
     -Iruntime runtime/tier_alloc.c \
     testsuite/tests/lib-bigarray/tier_alloc_backend.c \
     -Wl,--wrap=mmap,--wrap=munmap,--wrap=syscall -o /tmp/tier_alloc_failures
   env -u OCAML_NEAR_NODE -u OCAML_FAR_NODE /tmp/tier_alloc_failures default
   env -u OCAML_NEAR_NODE OCAML_FAR_NODE=1 /tmp/tier_alloc_failures failures
   env -u OCAML_NEAR_NODE OCAML_FAR_NODE=1 /tmp/tier_alloc_failures query-failure
   env -u OCAML_NEAR_NODE OCAML_FAR_NODE=1 /tmp/tier_alloc_failures query-limit
   env -u OCAML_NEAR_NODE OCAML_FAR_NODE=63 /tmp/tier_alloc_failures wide-mask
   env -u OCAML_NEAR_NODE OCAML_FAR_NODE=65 /tmp/tier_alloc_failures wide-mask
   env -u OCAML_NEAR_NODE OCAML_FAR_NODE=127 /tmp/tier_alloc_failures wide-mask
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef __linux__
#error "Run the tier allocator checks on Linux (CloudLab)."
#endif
/* Keep test actions and checks active even when compiled with -DNDEBUG. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#define CAML_INTERNALS
#include "caml/tier_alloc.h"
#include "caml/osdeps.h"
#include <assert.h>
#include <errno.h>
#include <linux/mempolicy.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

char *caml_secure_getenv(const char *name)
{
  return secure_getenv(name);
}

#ifdef TIER_ALLOC_TEST_FAILURES

static int fail_mapping, bind_error = EPERM, query_error, wide_mask;
static unsigned long simulated_node, largest_query_bits;
static void *last_base;
static size_t last_length;
static int mappings, unmappings, bindings, queries;

void *__real_mmap(void *, size_t, int, int, int, off_t);
int __real_munmap(void *, size_t);
long __real_syscall(long, ...);

void *__wrap_mmap(void *addr, size_t length, int prot, int flags,
                   int fd, off_t offset)
{
  mappings++;
  if (fail_mapping) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  last_base = __real_mmap(addr, length, prot, flags, fd, offset);
  last_length = length;
  assert(last_base != MAP_FAILED);
  return last_base;
}

int __wrap_munmap(void *addr, size_t length)
{
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  assert(addr == last_base);
  assert((length + page - 1) / page * page == last_length);
  unmappings++;
  int result = __real_munmap(addr, length);
  errno = EIO; /* Binding failures must preserve their original errno. */
  return result;
}

long __wrap_syscall(long number, ...)
{
  va_list args;
  va_start(args, number);
  if (number == SYS_get_mempolicy) {
    int *mode = va_arg(args, int *);
    unsigned long *mask = va_arg(args, unsigned long *);
    unsigned long bits = va_arg(args, unsigned long);
    void *addr = va_arg(args, void *);
    unsigned long flags = va_arg(args, unsigned long);
    va_end(args);
    queries++;
    largest_query_bits = bits;
    assert(flags == MPOL_F_MEMS_ALLOWED);
    size_t word_bits = sizeof(unsigned long) * CHAR_BIT;
    unsigned long needed_bits = (simulated_node / word_bits + 1) * word_bits;
    if (query_error || (wide_mask && bits < needed_bits)) {
      errno = query_error ? query_error : EINVAL;
      return -1;
    }
    if (wide_mask) {
      mask[simulated_node / word_bits] = 1UL << (simulated_node % word_bits);
      return 0;
    }
    return __real_syscall(number, mode, mask, bits, addr, flags);
  }
  assert(number == SYS_mbind);
  void *addr = va_arg(args, void *);
  size_t length = va_arg(args, size_t);
  int mode = va_arg(args, int);
  const unsigned long *mask = va_arg(args, const unsigned long *);
  unsigned long bits = va_arg(args, unsigned long);
  unsigned long flags = va_arg(args, unsigned long);
  va_end(args);
  bindings++;
  assert(addr == last_base && length == last_length);
  assert(mode == MPOL_BIND && flags == 0);
  if (wide_mask) {
    size_t word_bits = sizeof(unsigned long) * CHAR_BIT;
    /* Linux get_nodes consumes maxnode - 1 bits, unlike get_mempolicy.
       Reject a selected bit outside that range, as the real kernel would. */
    if (bits == 0 || simulated_node >= bits - 1
        || !(mask[simulated_node / word_bits]
             & (1UL << (simulated_node % word_bits)))) {
      errno = EINVAL;
      return -1;
    }
    return 0;
  }
  errno = bind_error;
  return -1;
}

int main(int argc, char **argv)
{
  assert(argc == 2);
  const char *error;
  int mapped = -1;
  if (strcmp(argv[1], "default") == 0) {
    void *data = caml_tier_alloc(32, 0, &mapped, &error);
    assert(data != NULL && mapped == 0 && error == NULL);
    free(data);
    assert(queries == 0 && mappings == 0 && bindings == 0 && unmappings == 0);
    puts("tier allocator: default near uses malloc without NUMA calls");
    return 0;
  }
  if (strcmp(argv[1], "query-failure") == 0) query_error = EPERM;
  if (strcmp(argv[1], "query-limit") == 0) query_error = EINVAL;
  wide_mask = strcmp(argv[1], "wide-mask") == 0;
  if (wide_mask) simulated_node = strtoul(getenv("OCAML_FAR_NODE"), NULL, 10);
  if (query_error) {
    assert(caml_tier_alloc(1, 1, &mapped, &error) == NULL);
    assert(mapped == 0 && errno == query_error && strstr(error, "OCAML_FAR_NODE"));
    assert(mappings == 0);
    if (query_error == EINVAL) {
      unsigned long limit = (unsigned long)sysconf(_SC_PAGESIZE) * CHAR_BIT;
      int expected_queries = 0;
      for (unsigned long bits = sizeof(unsigned long) * CHAR_BIT;
           bits <= limit; bits *= 2)
        expected_queries++;
      assert(queries == expected_queries && largest_query_bits == limit);
    } else {
      assert(queries == 1);
    }
    void *data = caml_tier_alloc(1, 0, &mapped, &error);
    assert(data != NULL && mapped == 0 && error == NULL);
    free(data);
    puts("tier allocator: bounded failed NUMA queries preserve default near");
    return 0;
  }
  if (wide_mask) {
    void *data = caml_tier_alloc(1, 1, &mapped, &error);
    assert(data != NULL && mapped == 1 && error == NULL && queries >= 1);
    assert(last_length == (size_t)sysconf(_SC_PAGESIZE));
    caml_tier_free(data, 1);
    assert(unmappings == 1);
    printf("tier allocator: simulated node %lu and header-free mapping passed\n",
           simulated_node);
    return 0;
  }
  assert(strcmp(argv[1], "failures") == 0);
  assert(caml_tier_alloc(SIZE_MAX, 1, &mapped, &error) == NULL);
  assert(mapped == 0 && errno == ENOMEM && strcmp(error, "size rounding") == 0);
  assert(mappings == 0);
  fail_mapping = 1;
  assert(caml_tier_alloc(1, 1, &mapped, &error) == NULL);
  assert(mapped == 0 && errno == ENOMEM && strcmp(error, "mmap") == 0);
  assert(mappings == 1 && bindings == 0 && unmappings == 0);
  fail_mapping = 0;
  for (int i = 0; i < 2; i++) {
    bind_error = i == 0 ? EPERM : ENOMEM;
    assert(caml_tier_alloc(1, 1, &mapped, &error) == NULL);
    assert(mapped == 0 && errno == bind_error && strstr(error, "mbind"));
    assert(last_length == (size_t)sysconf(_SC_PAGESIZE));
    unsigned char residency;
    assert(mincore(last_base, 1, &residency) == -1 && errno == ENOMEM);
  }
  assert(mappings == 3 && bindings == 2 && unmappings == 2);
  puts("tier allocator: overflow and mapping/binding failure cleanup passed");
  return 0;
}

#else

static void check_buffer(size_t size, int far, int expected_node,
                          int check_unmapped)
{
  const char *error;
  int mapped = -1;
  unsigned char *data = caml_tier_alloc(size, far, &mapped, &error);
  if (data == NULL) {
    fprintf(stderr, "far=%d size=%zu: %s: %s\n", far, size,
            error, strerror(errno));
    abort();
  }
  assert(error == NULL && mapped == (expected_node >= 0));
  size_t bytes = size == 0 ? 1 : size;
  memset(data, 0x5a, bytes);
  assert(data[0] == 0x5a && data[bytes - 1] == 0x5a);
  if (mapped) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    assert((uintptr_t)data % page == 0);
    for (size_t offset = 0; offset < bytes; offset += page) {
      int node = -1;
      assert(syscall(SYS_get_mempolicy, &node, NULL, 0UL, data + offset,
                     (unsigned long)(MPOL_F_NODE | MPOL_F_ADDR)) == 0);
      assert(node == expected_node);
    }
    caml_tier_free(data, size);
    /* Concurrent allocations can reuse this address immediately after free. */
    if (check_unmapped) {
      unsigned char residency;
      for (size_t offset = 0; offset < bytes; offset += page)
        assert(mincore(data + offset, 1, &residency) == -1 && errno == ENOMEM);
    }
  } else {
    free(data);
  }
}

static void *allocate_thread(void *arg)
{
  const int *nodes = arg;
  for (int i = 0; i < 12; i++) {
    for (int far = 0; far < 2; far++)
      check_buffer(37 + i, far, nodes[far], 0);
  }
  return NULL;
}

int main(int argc, char **argv)
{
  assert(argc >= 2);
  const size_t sizes[] = {0, 1, 4095, 4096, 4097, 2 * 1024 * 1024 + 13};
  if (strcmp(argv[1], "default") == 0) {
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
      check_buffer(sizes[i], 0, -1, 1);
    /* Environment changes after the first allocation do not change policy. */
    assert(setenv("OCAML_NEAR_NODE", "invalid", 1) == 0);
    assert(setenv("OCAML_FAR_NODE", "0", 1) == 0);
    check_buffer(1, 0, -1, 1);
    const char *error;
    int mapped;
    assert(caml_tier_alloc(1, 1, &mapped, &error) == NULL);
    assert(mapped == 0 && errno == EINVAL && strstr(error, "OCAML_FAR_NODE"));
    puts("tier allocator: default near, missing far, and fixed configuration passed");
    return 0;
  }
  assert(argc == 4);
  if (strcmp(argv[1], "reject") == 0) {
    int far = atoi(argv[2]), mapped;
    const char *error;
    assert(caml_tier_alloc(1, far, &mapped, &error) == NULL);
    assert(mapped == 0 && errno == EINVAL);
    assert(strstr(error, far ? "OCAML_FAR_NODE" : "OCAML_NEAR_NODE"));
    check_buffer(1, !far, atoi(argv[3]), 1);
    printf("tier allocator: rejected %s without breaking the other tier\n",
           far ? "OCAML_FAR_NODE" : "OCAML_NEAR_NODE");
    return 0;
  }
  int nodes[] = { atoi(argv[2]), atoi(argv[3]) };
  if (strcmp(argv[1], "threads") == 0) {
    pthread_t threads[8];
    for (size_t i = 0; i < 8; i++)
      assert(pthread_create(&threads[i], NULL, allocate_thread, nodes) == 0);
    for (size_t i = 0; i < 8; i++)
      assert(pthread_join(threads[i], NULL) == 0);
    puts("tier allocator: concurrent first allocations and placement passed");
  } else {
    assert(strcmp(argv[1], "bound") == 0);
    for (int far = 0; far < 2; far++) {
      for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        check_buffer(sizes[i], far, nodes[far], 1);
    }
    assert(setenv("OCAML_NEAR_NODE", "invalid", 1) == 0);
    assert(setenv("OCAML_FAR_NODE", "invalid", 1) == 0);
    check_buffer(1, 0, nodes[0], 1);
    check_buffer(1, 1, nodes[1], 1);
    caml_tier_free(NULL, 0);
    puts("tier allocator: allocation, placement, release, and fixed configuration passed");
  }
  return 0;
}

#endif
