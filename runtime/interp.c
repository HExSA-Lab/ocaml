/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*             Xavier Leroy, projet Cristal, INRIA Rocquencourt           */
/*                                                                        */
/*   Copyright 1996 Institut National de Recherche en Informatique et     */
/*     en Automatique.                                                    */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#define CAML_INTERNALS

// Using the Fowler-Noll-Vo hash function
#define FNV_OFFSET 1465981039346656037UL
#define FNV_PRIME 1099511628211UL
#define CAPACITY 16

#include <assert.h> 
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>

/* The bytecode interpreter */
#include <stdio.h>
#include <string.h>
#include "caml/alloc.h"
#include "caml/backtrace.h"
#include "caml/callback.h"
#include "caml/codefrag.h"
#include "caml/debugger.h"
#include "caml/fail.h"
#include "caml/fix_code.h"
#include "caml/instrtrace.h"
#include "caml/instruct.h"
#include "caml/interp.h"
#include "caml/major_gc.h"
#include "caml/memory.h"
#include "caml/misc.h"
#include "caml/mlvalues.h"
#include "caml/prims.h"
#include "caml/signals.h"
#include "caml/fiber.h"
#include "caml/domain.h"
#include "caml/globroots.h"
#include "caml/startup.h"
#include "caml/startup_aux.h"

/* Registers for the abstract machine:
        pc         the code pointer
        sp         the stack pointer (grows downward)
        accu       the accumulator
        env        heap-allocated environment
        Caml_state->trap_sp_off offset to the current trap frame
        extra_args number of extra arguments provided by the caller

sp is a local copy of the global variable Caml_state->extern_sp. */

/* Instruction decoding */
#define RUNTIME_INFO(fmt, args...) printf("MD RUNTIME INFO: " fmt " at %s\n", ##args, __func__)

#define RUNTIME_ERR(fmt, args...) fprintf(stderr, "MD RUNTIME ERROR: " fmt " at %s\n", ##args, __func__)
#define RUNTIME_WARN(fmt, args...) printf("MD RUNTIME WARNING: " fmt " at %s\n", ##args, __func__)

#define CHECK_PC(pc) if ((pc) == NULL) {RUNTIME_WARN("PC FOR THIS STACK IS NULL (pc=%p)", (pc));}

#define LABELME(instr) \
asm volatile (".global __stub_" #instr "\n" \
        "__stub" #instr ":\n");

#ifdef THREADED_CODE
#  define Instruct(name) lbl_##name
#  if defined(ARCH_SIXTYFOUR) && !defined(ARCH_CODE32)
#    define Jumptbl_base ((char *) &&lbl_ACC0)
#  else
#    define Jumptbl_base ((char *) 0)
#    define jumptbl_base ((char *) 0)
#  endif
#  ifdef DEBUG
#    define Next goto next_instr
#  else
#    define Next goto *(void *)(jumptbl_base + *pc++)
#  endif
#else
#  define Instruct(name) case name
#  define Next break
#endif

/* GC interface */

#undef Alloc_small_origin
// Do call asynchronous callbacks from allocation functions
#define Alloc_small_origin CAML_FROM_CAML
#define Setup_for_gc \
  { sp -= 3; sp[0] = accu; sp[1] = env; sp[2] = (value)pc; \
    domain_state->current_stack->sp = sp; }
#define Restore_after_gc \
  { sp = domain_state->current_stack->sp; accu = sp[0]; env = sp[1]; sp += 3; }
#define Enter_gc \
  { Setup_for_gc; \
    caml_process_pending_actions();         \
    Restore_after_gc; }

/* We store [pc+1] in the stack so that, in case of an exception, the
   first backtrace slot points to the event following the C call
   instruction. */
#define Setup_for_c_call \
  { sp -= 2; sp[0] = env; sp[1] = (value)(pc + 1); \
    domain_state->current_stack->sp = sp; }
#define Restore_after_c_call \
  { sp = domain_state->current_stack->sp; env = *sp; sp += 2; }

/* For VM threads purposes, an event frame must look like accu + a
   C_CALL frame + a RETURN 1 frame.
   TODO: now that VM threads are gone, we could get rid of that. But
   we need to make sure that this is not used elsewhere. */
#define Setup_for_event \
  { sp -= 6; \
    sp[0] = accu; /* accu */ \
    sp[1] = Val_unit; /* C_CALL frame: dummy environment */ \
    sp[2] = Val_unit; /* RETURN frame: dummy local 0 */ \
    sp[3] = (value) pc; /* RETURN frame: saved return address */  \
    sp[4] = env; /* RETURN frame: saved environment */ \
    sp[5] = Val_long(extra_args); /* RETURN frame: saved extra args */ \
    domain_state->current_stack->sp = sp; }
#define Restore_after_event \
  { sp = domain_state->current_stack->sp; accu = sp[0]; \
    pc = (code_t) sp[3]; env = sp[4]; extra_args = Long_val(sp[5]); \
    sp += 6; }

/* Debugger interface */

#define Setup_for_debugger \
   { sp -= 4; \
     sp[0] = accu; sp[1] = (value)(pc - 1); \
     sp[2] = env; sp[3] = Val_long(extra_args); \
     domain_state->current_stack->sp = sp; }
#define Restore_after_debugger \
   { CAMLassert(sp == domain_state->current_stack->sp); \
     CAMLassert(sp[0] == accu); \
     CAMLassert(sp[2] == env); \
     sp += 4; }

#ifdef THREADED_CODE
#define Restart_curr_instr \
  goto *((void*)(jumptbl_base + caml_debugger_saved_instruction(pc - 1)))
#else
#define Restart_curr_instr \
  curr_instr = caml_debugger_saved_instruction(pc - 1); \
  goto dispatch_instr
#endif

#define Check_trap_barrier \
  if (domain_state->trap_sp_off >= domain_state->trap_barrier_off) \
    caml_debugger(TRAP_BARRIER, Val_unit)

/* Register optimization.
   Some compilers underestimate the use of the local variables representing
   the abstract machine registers, and don't put them in hardware registers,
   which slows down the interpreter considerably.
   For GCC, I have hand-assigned hardware registers for several architectures.
*/

#if defined(__GNUC__) && !defined(DEBUG) && !defined(__INTEL_COMPILER) \
    && !defined(__llvm__)
#ifdef __mips__
#define PC_REG asm("$16")
#define SP_REG asm("$17")
#define ACCU_REG asm("$18")
#endif
#ifdef __sparc__
#define PC_REG asm("%l0")
#define SP_REG asm("%l1")
#define ACCU_REG asm("%l2")
#endif
#ifdef __alpha__
#ifdef __CRAY__
#define PC_REG asm("r9")
#define SP_REG asm("r10")
#define ACCU_REG asm("r11")
#define JUMPTBL_BASE_REG asm("r12")
#else
#define PC_REG asm("$9")
#define SP_REG asm("$10")
#define ACCU_REG asm("$11")
#define JUMPTBL_BASE_REG asm("$12")
#endif
#endif
#ifdef __i386__
#define PC_REG asm("%esi")
#define SP_REG asm("%edi")
#define ACCU_REG
#endif
#if defined(__ppc__) || defined(__ppc64__)
#define PC_REG asm("26")
#define SP_REG asm("27")
#define ACCU_REG asm("28")
#endif
#ifdef __hppa__
#define PC_REG asm("%r18")
#define SP_REG asm("%r17")
#define ACCU_REG asm("%r16")
#endif
#ifdef __mc68000__
#define PC_REG asm("a5")
#define SP_REG asm("a4")
#define ACCU_REG asm("d7")
#endif
/* PR#4953: these specific registers not available in Thumb mode */
#if defined (__arm__) && !defined(__thumb__)
#define PC_REG asm("r6")
#define SP_REG asm("r8")
#define ACCU_REG asm("r7")
#endif
#ifdef __ia64__
#define PC_REG asm("36")
#define SP_REG asm("37")
#define ACCU_REG asm("38")
#define JUMPTBL_BASE_REG asm("39")
#endif
#ifdef __x86_64__
#define PC_REG asm("%r15")
#define SP_REG asm("%r14")
#define ACCU_REG asm("%r13")
#endif
#ifdef __aarch64__
#define PC_REG asm("%x19")
#define SP_REG asm("%x20")
#define ACCU_REG asm("%x21")
#define JUMPTBL_BASE_REG asm("%x22")
#endif
#endif

#ifdef DEBUG
static __thread intnat caml_bcodcount;
#endif

static value raise_unhandled;


/*
 * Function stack
 */
#ifdef DEBUG
typedef struct function_stack { 
    code_t * stack_data; // the stack is backed by an array of code_t
    unsigned long nr_items; // number of items the stack currently holds
    unsigned long max_items; 
    unsigned long cur_size_bytes; 
    unsigned long top;    // index  into the array of the top of stack
} function_stack_t;

// returns a function_stack_t on success
// returns NULL on error
static function_stack_t * create_func_stack(unsigned long max_items) { 

	code_t * stack_data = NULL;
	function_stack_t * global_stack = NULL;

	stack_data = malloc(max_items * sizeof(code_t));

	if (!stack_data) { 
		fprintf(stderr, "Could not allocate function stack array\n");
		return NULL;
	}
	memset(stack_data, 0, max_items * sizeof(code_t));
	
	global_stack = malloc(sizeof(function_stack_t)); 

	if (!global_stack) { 
		fprintf(stderr, "Could not allocate global function stack\n");
		exit(1);
	} 
	memset(global_stack, 0, sizeof(function_stack_t));
	
	global_stack->stack_data = stack_data; 

	// Max size is the total number of code_t's that the stack can hold.
	global_stack->max_items = max_items; 
	global_stack->nr_items = 0;
	global_stack->cur_size_bytes = 0;
	global_stack->top = 0; 

	return global_stack;
}
/*
static function_stack_t * create_func_stack(unsigned long max_items) { 
	
}*/



static void destroy_func_stack (function_stack_t * stack) {
	free(stack->stack_data);
	free(stack);
} 

static void dump_func_stack_meta (function_stack_t * stack) {
	printf("Stack occupancy: %lu\n", stack->nr_items);
	printf("Stack cur size bytes: %lu\n", stack->cur_size_bytes);
	printf("Stack max items: %lu\n", stack->max_items);
	printf("Stack top idx: %lu\n", stack->top);
}


static void dump_func_stack (function_stack_t * stack) {
	dump_func_stack_meta(stack);

	for (int i = 0; i < stack->nr_items; i++) {
		if (i == stack->top-1) {
			printf("[%d] = %p <-- last item\n", i, stack->stack_data[i]);
		} else { 
			printf("[%d] = %p\n", i, stack->stack_data[i]);
		}
	}
}

static inline unsigned long get_nr_items (function_stack_t * stack) {
	return stack->nr_items;
}
static inline unsigned long get_max_items (function_stack_t * stack) {
	return stack->max_items;
}
static inline unsigned long get_top_idx (function_stack_t * stack) {
	return stack->top;
}
static inline unsigned long get_cur_size_bytes (function_stack_t * stack) {
	return stack->cur_size_bytes;
}


static int isEmpty(function_stack_t * stack) { 
	return !stack->nr_items;
}

static inline int isFull(function_stack_t * stack) { 
	return stack->top == stack->max_items;
}   


// returns a code_t (PC value) on success
// returns 0 on error
static inline code_t peek(function_stack_t * stack) { 
	if (!isEmpty(stack)) {
        if (!stack->stack_data[stack->top-1])
            RUNTIME_WARN("Top stack entry is NULL");
		return stack->stack_data[stack->top-1];
	}
	RUNTIME_ERR("Attempt to peek empty stack");
	return 0;
}


// returns 0 on success
// -1 on error
static int func_stack_push(function_stack_t * stack, code_t pc) {
	if (!isFull(stack)) { 
		stack->stack_data[stack->top++] = pc;
		stack->nr_items++;
		stack->cur_size_bytes += sizeof(code_t);
        //dump_func_stack_meta(stack); 
		return 0;
	}

	RUNTIME_ERR("Attempt to push onto full stack");
	return -1;
}


// returns a code_t (PC value) on success
// returns 0 on error
static code_t func_stack_pop(function_stack_t * stack) {
	if (!isEmpty(stack)) { 
		stack->nr_items--;
		stack->cur_size_bytes -= sizeof(code_t);
        //dump_func_stack_meta(stack);
		return stack->stack_data[--stack->top];
	} 

	fprintf(stderr, "Attempt to pop empty stack\n");
	return 0;
}

#endif /* !DEBUG */

/*
 * Hash table implementation using the Fowler-Noll-Vo function (FNV-1a version) 
 */
#ifdef DEBUG

// Hash table entry
typedef struct {
    const char* key; 
//    unsigned long *value[]; // value is the number of op codes. 
    void *value;	
} ht_entry; 


typedef struct ht { 
    ht_entry* entries;  // hash slots 
    size_t capacity;    // size of _entries array 
    size_t length;      // number of items in hash table
} ht; 

ht* ht_create(void) { 
    
    ht* table = malloc(sizeof(ht));

    if(table == NULL) { 
        return NULL;
    }

    table->length = 0;
    table->capacity = CAPACITY;

    // allocating space for entry buckets 
    table->entries = calloc(table->capacity, sizeof(ht_entry));

    if(table->entries == NULL) {
        free(table);
        return NULL;
    }

    return table; 
}

void ht_destroy(ht* table) { 
    for(size_t i = 0; i < table->capacity; i++) {
#if 0
        if(table->entries[i].key != NULL) {
            free((void*)table->entries[i].key);
        }
#endif
    }

    free(table->entries);
    free(table);
}

/*
static uint64_t hash_key_buffer(const char *key) { 
    uint64_t hash = FNV_OFFSET;

    for(const char *p = key; *p; p++) {
        hash ^= (uint64_t)(unsigned char)(*p);
        hash *= FNV_PRIME;
    }
    return hash;
}
*/
static inline uint64_t hash_key(const char * key) {
	unsigned long hash = (unsigned long)key;
	unsigned long n = hash;
	n <<= 18;
	hash -= n;
	n <<= 33;
	hash -= n;
	n <<= 3;
	hash += n;
	n <<= 3;
	hash -= n;
	n <<= 4;
	hash += n;
	n <<= 2;
	hash += n;
	return hash;
}

void* ht_get(ht* table, const char* key) { 
    uint64_t hash = hash_key(key); 
    size_t index =(size_t)(hash & (uint64_t)(table->capacity - 1)); 
    
    while (table->entries[index].key != NULL) {
#if 0
        if(strcmp(key, table->entries[index].key) == 0) {
#endif 
	if ((void*)key == (void*)table->entries[index].key) {
            // Found the key, return the value
            return table->entries[index].value;
        }

        // Key wasn't there so move on to the next. 
        index++; 

        if (index >= table->capacity) {
            index = 0;
        }
    }

    return NULL;
}

static const char* ht_set_entry(ht_entry* entries, size_t capacity, const char* key, void* value, size_t* plength) {
    uint64_t hash;
    size_t index;

    if (!key) { 
        RUNTIME_ERR("Attempt to reference NULL key");
        exit(EXIT_FAILURE);
    }
    hash = hash_key(key);
    index = (size_t)(hash & (uint64_t)(capacity - 1));

    while(entries[index].key != NULL) { 
#if 0
        if (strcmp(key, entries[index].key) == 0) {
#endif
	if ((void*)key == (void*)entries[index].key) {
		entries[index].value= value; 
		return entries[index].key; 
	}

        index++;

        if (index >= capacity) { 
            index = 0; 
        }
    }

    if (plength != NULL) {
#if 0
        key = strdup(key); 
        if(key == NULL) {
            return NULL;
        }
#endif
        (*plength)++;       
    }
    entries[index].key = (char*)key; 
    entries[index].value = value; 
    return key; 
}

// Expand hash table to twice it's size; 
// Return true on success and false otherwise
static bool ht_expand(ht* table) {
    ht_entry * new_entries;
    size_t new_capacity = table->capacity * 2; 

    if (new_capacity < table->capacity) { 
	RUNTIME_ERR("Capacity error");
        return false; 
    }

    new_entries = calloc(new_capacity, sizeof(ht_entry));

    if (new_entries == NULL) { 
	RUNTIME_ERR("Calloc failed in ht_expand");
        return false; 
    }

    for(size_t i = 0; i < table->capacity; i++) {
        ht_entry entry = table->entries[i];

        if(entry.key != NULL) { 
            ht_set_entry(new_entries, new_capacity, entry.key, entry.value, NULL);
        }
    }

    free(table->entries);
    table->entries = new_entries; 
    table->capacity = new_capacity; 

    return true; 
}

const char* ht_set(ht* table, const char* key, void* value) { 
    assert(value != NULL); 

    if (key == NULL) {
        RUNTIME_ERR("Attempt to set NULL key");
        exit(EXIT_FAILURE);
    }

    if (value == NULL) { 
	RUNTIME_ERR("Attempt to set NULL value");
        return NULL;
    }

    // If length will exceed half of current capacity, expand it
    if (table->length >= table->capacity / 2) { 
        if (!ht_expand(table)) {
		RUNTIME_ERR("Could not expand hashtable");
		return NULL;
        }
    }

    return ht_set_entry(table->entries, table->capacity, key, value, &table->length);
}

size_t ht_length(ht* table) { 
    return table->length; 
}


static void ht_curr_inc_opcount(ht* table, function_stack_t * func_stack, opcode_t opcode) { 
	
	unsigned long * temp_curr_op_counts = NULL;
	code_t curr_func = NULL;

	curr_func = peek(func_stack);
	
	assert(curr_func != NULL);

	temp_curr_op_counts = ht_get(table, (const char *)curr_func);

	assert(temp_curr_op_counts != NULL);
	
	if (opcode < FIRST_UNIMPLEMENTED_OP) {
 		temp_curr_op_counts[opcode]++;
	} else { 
		RUNTIME_ERR("Trying to increment invalid opcode %u", opcode);
	}

		
}

static void array_alloc_op_counts(ht * table, function_stack_t * func_stack) { 
	
	unsigned long * curr_opcount_array; 
	code_t curr_func = peek(func_stack); 	
	
	if(ht_get(table, (const char *)curr_func)) { 
		return;  
	}

	curr_opcount_array = malloc(FIRST_UNIMPLEMENTED_OP * sizeof(unsigned long)); 
	
	
	assert(curr_func != NULL); 

	if(!curr_opcount_array) { 
		RUNTIME_ERR("Could not allocated array"); 
		return;
	}

	memset(curr_opcount_array, 0, FIRST_UNIMPLEMENTED_OP * sizeof(unsigned long)); 
		
	ht_set(table, (const char *)curr_func, curr_opcount_array); 

}
// Hash talbe iterator: create with ht_iterator, iterate with ht_next 
//
typedef struct { 
   const char* key;
   void* value; 
   ht* _table;
   size_t _index;  
} hti; 

hti ht_iterator(ht* table) { 
    hti it; 
    it._table = table; 
    it._index = 0;
    return it;
}

bool ht_next(hti* it) { 
    
    ht* table = it->_table; 
    while(it->_index < table->capacity) { 
        size_t i = it->_index;
        it->_index++; 

        if(table->entries[i].key != NULL) { 
            ht_entry entry = table->entries[i];
            it->key = entry.key;
            it->value = entry.value; 
            return true; 
        }
    }
        return false; 
}

#endif



// add, multiple, duplicate, etc. etc. 
// Also could I just return the value such as return stack.size[stack.top--]; 


/* The interpreter itself */

value caml_interprete(code_t prog, asize_t prog_size)
{

#ifdef DEBUG 
  unsigned long * op_counts;  
  unsigned long total_op_count;
  //unsigned long * curr_op_counts;
  //unsigned long counter; 
  //function_stack_t * global_func_stack;
  function_stack_t * local_func_stack; 
  ht * func_hash_table;
  //hti it;
  unsigned long * op_arr; 
#endif 

#ifdef PC_REG
  register code_t pc PC_REG;
  register value * sp SP_REG;
  register value accu ACCU_REG;
#else
  register code_t pc;
  register value * sp;
  register value accu;
#endif
#if defined(THREADED_CODE) && defined(ARCH_SIXTYFOUR) && !defined(ARCH_CODE32)
#ifdef JUMPTBL_BASE_REG
  register char * jumptbl_base JUMPTBL_BASE_REG;
#else
  register char * jumptbl_base;
#endif
#endif
  value env;
  intnat extra_args;
  struct caml_exception_context * initial_external_raise;
  int initial_stack_words;
  intnat initial_trap_sp_off;
  volatile value raise_exn_bucket = Val_unit;
  struct longjmp_buffer raise_buf;
  value resume_fn, resume_arg;
  caml_domain_state* domain_state = Caml_state;
  struct caml_exception_context exception_ctx =
    { &raise_buf, domain_state->local_roots, &raise_exn_bucket};
#ifndef THREADED_CODE
  opcode_t curr_instr;
#endif

#ifdef THREADED_CODE
  static void * jumptable[] = {
#    include "caml/jumptbl.h"
  };
#endif

  if (prog == NULL) {           /* Interpreter is initializing */
    static opcode_t raise_unhandled_code[] = { ACC, 0, RAISE };
    value raise_unhandled_closure;

    caml_register_code_fragment(
      (char *) raise_unhandled_code,
      (char *) raise_unhandled_code + sizeof(raise_unhandled_code),
      DIGEST_IGNORE, NULL);
#ifdef THREADED_CODE
    caml_instr_table = (char **) jumptable;
    caml_instr_base = Jumptbl_base;
    caml_thread_code(raise_unhandled_code,
                     sizeof(raise_unhandled_code));
#endif
    raise_unhandled_closure = caml_alloc_small (2, Closure_tag);
    Code_val(raise_unhandled_closure) = (code_t)raise_unhandled_code;
    Closinfo_val(raise_unhandled_closure) = Make_closinfo(0, 2);
    raise_unhandled = raise_unhandled_closure;
    caml_register_generational_global_root(&raise_unhandled);
    caml_global_data = Val_unit;
    caml_register_generational_global_root(&caml_global_data);
    caml_init_callbacks();
    return Val_unit;
  }

#if defined(THREADED_CODE) && defined(ARCH_SIXTYFOUR) && !defined(ARCH_CODE32)
  jumptbl_base = Jumptbl_base;
#endif
  initial_trap_sp_off = domain_state->trap_sp_off;
  initial_stack_words =
    Stack_high(domain_state->current_stack) - domain_state->current_stack->sp;
  initial_external_raise = domain_state->external_raise;

  if (sigsetjmp(raise_buf.buf, 0)) {
    /* no non-volatile local variables read here */
    sp = domain_state->current_stack->sp;
    accu = raise_exn_bucket;

    Check_trap_barrier;
    if (domain_state->backtrace_active) {
         /* pc has already been pushed on the stack when calling the C
         function that raised the exception. No need to push it again
         here. */
      caml_stash_backtrace(accu, sp, 0);
    }
    goto raise_notrace;
  }
  domain_state->external_raise = &exception_ctx;

  domain_state->trap_sp_off = 1;

  sp = domain_state->current_stack->sp;
  pc = prog;
  extra_args = 0;
  env = Atom(0);
  accu = Val_int(0);

/* Checkse if not defined for DEBUG is running */ 
// Commenting out to remove annoyances
/*   
#ifndef DEBUG
  fprintf(stderr, "Not defined for DEBUG is running\n"); 
#endif 
*/

#ifdef DEBUG  
  op_counts = NULL;
 // curr_op_counts = 0; 
  total_op_count = 0; 

  op_counts = malloc(FIRST_UNIMPLEMENTED_OP * sizeof(unsigned long));

  if(!op_counts) { 
	fprintf(stderr, "Could not allocate op count array\n"); 
	exit(1); 
  }

  memset(op_counts, 0, FIRST_UNIMPLEMENTED_OP * sizeof(unsigned long));


 /* curr_op_counts = malloc(FIRST_UNIMPLEMENTED_OP * sizeof(unsigned long)); 

  if(!curr_op_counts) { 

    fprintf(stderr, "Could not allocate function op count array\n"); 
    exit(1); 
  }

  memset(curr_op_counts, 0, FIRST_UNIMPLEMENTED_OP * sizeof(unsigned long)); 
*/
 // function_stack_t * func_stack;
 /* global_func_stack = create_func_stack(1024);
  if (!global_func_stack) {
	  fprintf(stderr, "Could not allocate global func stack\n");
	  exit(1);
  }*/

  local_func_stack = create_func_stack(1024); 
  if(!local_func_stack) { 
	  fprintf(stderr, "Could not allocate local func stack\n"); 
	  exit(1); 
	}	

  func_hash_table = ht_create(); 

#endif  

#ifdef THREADED_CODE
#ifdef DEBUG
 next_instr:
  if (*pc < FIRST_UNIMPLEMENTED_OP) {
      op_counts[*pc]++;  
      total_op_count++;
      //curr_op_counts++; 
	
       if(!isEmpty(local_func_stack)) { 
	 ht_curr_inc_opcount(func_hash_table, local_func_stack, *pc); 
	} 
//     func_stack_push(global_func_stack, pc);
//     ht_curr_inc_opcount(func_hash_table, global_func_stack, *pc); 	
  } else {
      fprintf(stderr, "Trying to inc opcode %u\n", *pc);
  }
  if (caml_icount-- == 0) caml_stop_here ();
  CAMLassert(Stack_base(domain_state->current_stack) <= sp);
  CAMLassert(sp <= Stack_high(domain_state->current_stack));
#endif
  goto *(void *)(jumptbl_base + *pc++); /* Jump to the first instruction */
#else
  while(1) {
#ifdef DEBUG
    caml_bcodcount++;
    if (caml_icount-- == 0) caml_stop_here ();
    if (caml_params->trace_level>1)
      printf("\n##%" ARCH_INTNAT_PRINTF_FORMAT "d\n", caml_bcodcount);
    if (caml_params->trace_level>0) caml_disasm_instr(pc);
    if (caml_params->trace_level>1) {
      printf("env=");
      caml_trace_value_file(env,prog,prog_size,stdout);
      putchar('\n');
      caml_trace_accu_sp_file(accu,sp,prog,prog_size,stdout);
      fflush(stdout);
    };
    CAMLassert(Stack_base(domain_state->current_stack) <= sp);
    CAMLassert(sp <= Stack_high(domain_state->current_stack));

    if (*pc < FIRST_UNIMPLEMENTED_OP) {
       op_counts[*pc]++;  
        total_op_count++;
  //      curr_op_counts++;
	//func_stack_push(global_func_stack, pc); 
	//ht_curr_inc_opcount(func_hash_table, global_func_stack, *pc); 	
	if(!isEmpty(local_func_stack)) { 
		ht_curr_inc_opcount(func_hash_table, local_func_stack, *pc); 
	}
    } else {
        fprintf(stderr, "ERROR: Trying to inc opcode %u but it's invalid\n", *pc);
    }


#endif
    curr_instr = *pc++;

  dispatch_instr:
    switch(curr_instr) {
#endif

/* Basic stack operations */

        Instruct(ACC0):
            LABELME(ACC0);   
        accu = sp[0]; Next;
        Instruct(ACC1):
            LABELME(ACC1);  
        accu = sp[1]; Next;
        Instruct(ACC2):
            LABELME(ACC2);  
        accu = sp[2]; Next;
        Instruct(ACC3):
            LABELME(ACC3);  
        accu = sp[3]; Next;
        Instruct(ACC4):
            LABELME(ACC4); 
        accu = sp[4]; Next;
        Instruct(ACC5):
            LABELME(ACC5);
        accu = sp[5]; Next;
        Instruct(ACC6):
            LABELME(ACC6);
        accu = sp[6]; Next;
        Instruct(ACC7):
            LABELME(ACC7);
        accu = sp[7]; Next;

        Instruct(PUSH): LABELME(PUSH); Instruct(PUSHACC0): LABELME(PUSHACC0);
        *--sp = accu; Next;
        Instruct(PUSHACC1):
            LABELME(PUSHACC1);
        *--sp = accu; accu = sp[1]; Next;
        Instruct(PUSHACC2):
            LABELME(PUSHACC2);
        *--sp = accu; accu = sp[2]; Next;
        Instruct(PUSHACC3):
            LABELME(PUSHACC3);
        *--sp = accu; accu = sp[3]; Next;
        Instruct(PUSHACC4):
            LABELME(PUSHACC4)
            *--sp = accu; accu = sp[4]; Next;
        Instruct(PUSHACC5):
            LABELME(PUSHACC5);
        *--sp = accu; accu = sp[5]; Next;
        Instruct(PUSHACC6):
            LABELME(PUSHACC6);
        *--sp = accu; accu = sp[6]; Next;
        Instruct(PUSHACC7):
            LABELME(PUSHACC7);
        *--sp = accu; accu = sp[7]; Next;

        Instruct(PUSHACC): 
            LABELME(PUSHACC);
        *--sp = accu;
        /* Fallthrough */
        Instruct(ACC):
            LABELME(ACC);
        accu = sp[*pc++];
        Next;

    Instruct(POP):
        LABELME(POP);
      sp += *pc++;
      Next;
    Instruct(ASSIGN):
        LABELME(ASSIGN);
      sp[*pc++] = accu;
      accu = Val_unit;
      Next;

/* Access in heap-allocated environment */

    Instruct(ENVACC1):
        LABELME(ENVACC1);
      accu = Field(env, 1); Next;
    Instruct(ENVACC2):
        LABELME(ENVACC2);
      accu = Field(env, 2); Next;
    Instruct(ENVACC3):
        LABELME(ENVACC3);
      accu = Field(env, 3); Next;
    Instruct(ENVACC4):
        LABELME(ENVACC4);
      accu = Field(env, 4); Next;

    Instruct(PUSHENVACC1):
        LABELME(PUSHENVACC1);
      *--sp = accu; accu = Field(env, 1); Next;
    Instruct(PUSHENVACC2):
        LABELME(PUSHENVACC2);
      *--sp = accu; accu = Field(env, 2); Next;
    Instruct(PUSHENVACC3):
        LABELME(PUSHENVACC3);
      *--sp = accu; accu = Field(env, 3); Next;
    Instruct(PUSHENVACC4):
        LABELME(PUSHENVACC4);
      *--sp = accu; accu = Field(env, 4); Next;

    Instruct(PUSHENVACC):
        LABELME(PUSHENVACC);
      *--sp = accu;
      /* Fallthrough */
    Instruct(ENVACC):
        LABELME(ENVACC);
      accu = Field(env, *pc++);
      Next;

      /* Function application */

      Instruct(PUSH_RETADDR): {
          LABELME(PUSH_RETADDR);
	      sp -= 3;
	      sp[0] = (value) (pc + *pc);
	      sp[1] = env;
	      sp[2] = Val_long(extra_args);
	      pc++;
	      Next;
      }
	
	Instruct(APPLY): {
        LABELME(APPLY);
		extra_args = *pc - 1;
	      pc = Code_val(accu);
	      env = accu;
	      CHECK_PC(pc);
#ifdef DEBUG
	      func_stack_push(local_func_stack, pc);
	      array_alloc_op_counts(func_hash_table, local_func_stack); 		
	      //dump_func_stack(local_func_stack); 			
	      //ht_curr_inc_opcount(func_hash_table, local_func_stack, *pc); 	
	      		
#endif
	      goto check_stacks;
      }
      Instruct(APPLY1): {
          // LABELME(APPLY1);
	      value arg1 = sp[0];
	      sp -= 3;
	      sp[0] = arg1;
	      sp[1] = (value)pc;
	      sp[2] = env;
	      sp[3] = Val_long(extra_args);
	      pc = Code_val(accu);
	      CHECK_PC(pc);
#ifdef DEBUG
	      func_stack_push(local_func_stack, pc);
	      array_alloc_op_counts(func_hash_table, local_func_stack); 	
      	      //dump_func_stack(local_func_stack); 
	      //ht_curr_inc_opcount(func_hash_table, local_func_stack, *pc); 
	     
#endif
	      env = accu;
	      extra_args = 0;
	      goto check_stacks;
      }
      Instruct(APPLY2): {
          //LABELME(APPLY2);
	      value arg1 = sp[0];
	      value arg2 = sp[1];
	      sp -= 3;
	      sp[0] = arg1;
	      sp[1] = arg2;
	      sp[2] = (value)pc;
	      sp[3] = env;
	      sp[4] = Val_long(extra_args);
	      pc = Code_val(accu);
	      CHECK_PC(pc);
#ifdef DEBUG
	   func_stack_push(local_func_stack, pc);
	   array_alloc_op_counts(func_hash_table, local_func_stack); 
	  // dump_func_stack(local_func_stack); 
	  // ht_curr_inc_opcount(func_hash_table, local_func_stack, *pc); 
#endif
	      env = accu;
	      extra_args = 1;
	      goto check_stacks;
      }
      Instruct(APPLY3): {
          //LABELME(APPLY3);
	      value arg1 = sp[0];
	      value arg2 = sp[1];
	      value arg3 = sp[2];
	      sp -= 3;
	      sp[0] = arg1;
	      sp[1] = arg2;
	      sp[2] = arg3;
	      sp[3] = (value)pc;
	      sp[4] = env;
	      sp[5] = Val_long(extra_args);
	      pc = Code_val(accu);
	      CHECK_PC(pc);
#ifdef DEBUG
             func_stack_push(local_func_stack, pc); 
	     array_alloc_op_counts(func_hash_table, local_func_stack);
	    // dump_func_stack(local_func_stack); 
	    // ht_curr_inc_opcount(func_hash_table, local_func_stack, *pc); 
		
#endif
	      env = accu;
	      extra_args = 2;
	      goto check_stacks;
      }

      Instruct(APPTERM): {
         // LABELME(APPTERM);
	      int nargs = *pc++;
	      int slotsize = *pc;
	      value * newsp;
	      int i;
	      /* Slide the nargs bottom words of the current frame to the top
		 of the frame, and discard the remainder of the frame */
	      newsp = sp + slotsize - nargs;
	      for (i = nargs - 1; i >= 0; i--) newsp[i] = sp[i];
	      sp = newsp;
	      pc = Code_val(accu);
	      env = accu;
	      extra_args += nargs - 1;
	      goto check_stacks;
      }
      Instruct(APPTERM1): {
          //LABELME(APPTERM1);
	      value arg1 = sp[0];
	      sp = sp + *pc - 1;
	      sp[0] = arg1;
	      pc = Code_val(accu);
	      env = accu;
	      goto check_stacks;
      }
      Instruct(APPTERM2): {
          //LABELME(APPTERM2);
	      value arg1 = sp[0];
	      value arg2 = sp[1];
	      sp = sp + *pc - 2;
	      sp[0] = arg1;
	      sp[1] = arg2;
	      pc = Code_val(accu);
	      env = accu;
	      extra_args += 1;
	      goto check_stacks;
      }
      Instruct(APPTERM3): {
          //LABELME(APPTERM3);
	      value arg1 = sp[0];
	      value arg2 = sp[1];
	      value arg3 = sp[2];
	      sp = sp + *pc - 3;
	      sp[0] = arg1;
	      sp[1] = arg2;
	      sp[2] = arg3;
	      pc = Code_val(accu);
	      env = accu;
	      extra_args += 2;
	      goto check_stacks;
      }

      Instruct(RETURN): {
          LABELME(RETURN);
	      sp += *pc++;
	      if (extra_args > 0) {
		      extra_args--;
		      pc = Code_val(accu);
		      env = accu;
#ifdef DEBUG 
	     // curr_op_counts = ht_curr_inc_opcount(func_hash_table, func_stack, *pc); 	
	    // Just prints out the key
	    // printf("Key %p:\n", (void*)peek(local_func_stack));
	     func_stack_pop(local_func_stack); 
#endif
	Next;
      } else {
#ifdef DEBUG
    //	printf("Key %p:\n", (void*)peek(local_func_stack));
    func_stack_pop(local_func_stack);
#endif
        goto do_return;
      }
    }

    do_return:

      if (sp == Stack_high(domain_state->current_stack)) {
        /* return to parent stack */
        struct stack_info* old_stack = domain_state->current_stack;
        struct stack_info* parent_stack = Stack_parent(old_stack);
        value hval = Stack_handle_value(old_stack);
        CAMLassert(parent_stack != NULL);

        domain_state->current_stack = parent_stack;
        sp = domain_state->current_stack->sp;
        caml_free_stack(old_stack);

        domain_state->trap_sp_off = Long_val(sp[0]);
        extra_args = Long_val(sp[1]);
        sp++;
        sp[0] = accu;
        
        accu = hval;
        pc = Code_val(accu);
        env = accu;
        goto check_stacks;
      } else {
        /* return to callee, no stack switching */
        pc = (code_t)(sp[0]);
        env = sp[1];
        extra_args = Long_val(sp[2]);
        sp += 3;
      }
      Next;

    Instruct(RESTART): {
       // LABELME(RESTART);
      int num_args = Wosize_val(env) - 3;
      int i;
      sp -= num_args;
      for (i = 0; i < num_args; i++) sp[i] = Field(env, i + 3);
      env = Field(env, 2);
      extra_args += num_args;
      Next;
    }

    Instruct(GRAB): {
       // LABELME(GRAB);
      int required = *pc++;
      if (extra_args >= required) {
        extra_args -= required;
        Next;
      } else {
        mlsize_t num_args, i;
        num_args = 1 + extra_args; /* arg1 + extra args */
        Alloc_small(accu, num_args + 3, Closure_tag, Enter_gc);
        Field(accu, 2) = env;
        for (i = 0; i < num_args; i++) Field(accu, i + 3) = sp[i];
        Code_val(accu) = pc - 3; /* Point to the preceding RESTART instr. */
        Closinfo_val(accu) = Make_closinfo(0, 2);
        sp += num_args;
        goto do_return;
      }
    }

    Instruct(CLOSURE): {
       // LABELME(CLOSURE);
      int nvars = *pc++;
      int i;
      if (nvars > 0) *--sp = accu;
      if (nvars <= Max_young_wosize - 2) {
        /* nvars + 2 <= Max_young_wosize, can allocate in minor heap */
        Alloc_small(accu, 2 + nvars, Closure_tag, Enter_gc);
        for (i = 0; i < nvars; i++) Field(accu, i + 2) = sp[i];
      } else {
        /* PR#6385: must allocate in major heap */
        /* caml_alloc_shr and caml_initialize never trigger a GC,
           so no need to Setup_for_gc */
        accu = caml_alloc_shr(2 + nvars, Closure_tag);
        for (i = 0; i < nvars; i++) caml_initialize(&Field(accu, i + 2), sp[i]);
      }
      /* The code pointer is not in the heap, so no need to go through
         caml_initialize. */
      Code_val(accu) = pc + *pc;
      Closinfo_val(accu) = Make_closinfo(0, 2);
      pc++;
      sp += nvars;
      Next;
    }

    Instruct(CLOSUREREC): {
      //  LABELME(CLOSUREREC);
      int nfuncs = *pc++;
      int nvars = *pc++;
      mlsize_t envofs = nfuncs * 3 - 1;
      mlsize_t blksize = envofs + nvars;
      int i;
      value * p;
      if (nvars > 0) *--sp = accu;
      if (blksize <= Max_young_wosize) {
        Alloc_small(accu, blksize, Closure_tag, Enter_gc);
        p = &Field(accu, envofs);
        for (i = 0; i < nvars; i++, p++) *p = sp[i];
      } else {
        /* PR#6385: must allocate in major heap */
        /* caml_alloc_shr and caml_initialize never trigger a GC,
           so no need to Setup_for_gc */
        accu = caml_alloc_shr(blksize, Closure_tag);
        p = &Field(accu, envofs);
        for (i = 0; i < nvars; i++, p++) caml_initialize(p, sp[i]);
      }
      sp += nvars;
      /* The code pointers and infix headers are not in the heap,
         so no need to go through caml_initialize. */
      *--sp = accu;
      p = &Field(accu, 0);
      *p++ = (value) (pc + pc[0]);
      *p++ = Make_closinfo(0, envofs);
      for (i = 1; i < nfuncs; i++) {
        *p++ = Make_header(i * 3, Infix_tag, 0); /* color irrelevant */
        *--sp = (value) p;
        *p++ = (value) (pc + pc[i]);
        envofs -= 3;
        *p++ = Make_closinfo(0, envofs);
      }
      pc += nfuncs;
      Next;
    }

    Instruct(PUSHOFFSETCLOSURE):
        LABELME(PUSHOFFSETCLOSURE);
      *--sp = accu; /* fallthrough */
    Instruct(OFFSETCLOSURE):
        LABELME(OFFSETCLOSURE);
      accu = env + *pc++ * sizeof(value); Next;

    Instruct(PUSHOFFSETCLOSUREM3):
        LABELME(PUSHOFFSETCLOSUREM3);
      *--sp = accu; /* fallthrough */
    Instruct(OFFSETCLOSUREM3):
        LABELME(OFFSETCLOSUREM3);
      accu = env - 3 * sizeof(value); Next;
    Instruct(PUSHOFFSETCLOSURE0):
        LABELME(PUSHOFFSETCLOSURE0);
      *--sp = accu; /* fallthrough */
    Instruct(OFFSETCLOSURE0):
        LABELME(OFFSETCLOSURE0);
      accu = env; Next;
    Instruct(PUSHOFFSETCLOSURE3):
        LABELME(PUSHOFFSETCLOSURE3);
      *--sp = accu; /* fallthrough */
    Instruct(OFFSETCLOSURE3):
        LABELME(OFFSETCLOSURE3);
      accu = env + 3 * sizeof(value); Next;


/* Access to global variables */

    Instruct(PUSHGETGLOBAL):
        LABELME(PUSHGETGLOBAL);
      *--sp = accu;
      /* Fallthrough */
    Instruct(GETGLOBAL):
        LABELME(GETGLOBAL);
      accu = Field(caml_global_data, *pc);
      pc++;
      Next;

    Instruct(PUSHGETGLOBALFIELD):
        LABELME(PUSHGETGLOBALFIELD);
      *--sp = accu;
      /* Fallthrough */
    Instruct(GETGLOBALFIELD): {
        LABELME(GETGLOBALFIELD);
      accu = Field(caml_global_data, *pc);
      pc++;
      accu = Field(accu, *pc);
      pc++;
      Next;
    }

    Instruct(SETGLOBAL):  {
        LABELME(SETGLOBAL);
      caml_modify(&Field(caml_global_data, *pc), accu);
      accu = Val_unit;
      pc++;
      Next;
    }

/* Allocation of blocks */

    Instruct(PUSHATOM0):
        LABELME(PUSHATOM0);
      *--sp = accu;
      /* Fallthrough */
    Instruct(ATOM0):
        LABELME(ATOM0);
      accu = Atom(0); Next;

    Instruct(PUSHATOM):
        LABELME(PUSHATOM);
      *--sp = accu;
      /* Fallthrough */
    Instruct(ATOM):
        LABELME(ATOM);
      accu = Atom(*pc++); Next;

    Instruct(MAKEBLOCK): {
      //LABELME(MAKEBLOCK); 
      mlsize_t wosize = *pc++;
      tag_t tag = *pc++;
      mlsize_t i;
      value block;
      if (wosize <= Max_young_wosize) {
        Alloc_small(block, wosize, tag, Enter_gc);
        Field(block, 0) = accu;
        for (i = 1; i < wosize; i++) Field(block, i) = *sp++;
      } else {
        block = caml_alloc_shr(wosize, tag);
        caml_initialize(&Field(block, 0), accu);
        for (i = 1; i < wosize; i++) caml_initialize(&Field(block, i), *sp++);
      }
      accu = block;
      Next;
    }
    Instruct(MAKEBLOCK1): {
      //LABELME(MAKEBLOCK1); 
      tag_t tag = *pc++;
      value block;
      Alloc_small(block, 1, tag, Enter_gc);
      Field(block, 0) = accu;
      accu = block;
      Next;
    }
    Instruct(MAKEBLOCK2): {
       //LABELME(MAKEBLOCK2);
      tag_t tag = *pc++;
      value block;
      Alloc_small(block, 2, tag, Enter_gc);
      Field(block, 0) = accu;
      Field(block, 1) = sp[0];
      sp += 1;
      accu = block;
      Next;
    }
    Instruct(MAKEBLOCK3): {
       // LABELME(MAKEBLOCK3);
      tag_t tag = *pc++;
      value block;
      Alloc_small(block, 3, tag, Enter_gc);
      Field(block, 0) = accu;
      Field(block, 1) = sp[0];
      Field(block, 2) = sp[1];
      sp += 2;
      accu = block;
      Next;
    }
    Instruct(MAKEFLOATBLOCK): {
      //  LABELME(MAKEFLOATBLOCK);
      mlsize_t size = *pc++;
      mlsize_t i;
      value block;
      if (size <= Max_young_wosize / Double_wosize) {
        Alloc_small(block, size * Double_wosize, Double_array_tag, Enter_gc);
      } else {
        block = caml_alloc_shr(size * Double_wosize, Double_array_tag);
      }
      Store_double_flat_field(block, 0, Double_val(accu));
      for (i = 1; i < size; i++){
        Store_double_flat_field(block, i, Double_val(*sp));
        ++ sp;
      }
      accu = block;
      Next;
    }

/* Access to components of blocks */

    Instruct(GETFIELD0):
        LABELME(GETFIELD0);
      accu = Field(accu, 0); Next;
    Instruct(GETFIELD1):
        LABELME(GETFIELD1);
      accu = Field(accu, 1); Next;
    Instruct(GETFIELD2):
        LABELME(GETFIELD2);
      accu = Field(accu, 2); Next;
    Instruct(GETFIELD3):
        LABELME(GETFIELD3);
      accu = Field(accu, 3); Next;
    Instruct(GETFIELD):
        LABELME(GETFIELD);
      accu = Field(accu, *pc); pc++; Next;
    Instruct(GETFLOATFIELD): {
      //  LABELME(GETFLOATFIELD);
      double d = Double_flat_field(accu, *pc++);
      Alloc_small(accu, Double_wosize, Double_tag, Enter_gc);
      Store_double_val(accu, d);
      Next;
    }

    Instruct(SETFIELD0):
        LABELME(SETFIELD0);
      caml_modify(&Field(accu, 0), *sp++);
      accu = Val_unit;
      Next;
    Instruct(SETFIELD1):
        LABELME(SETFIELD1);
      caml_modify(&Field(accu, 1), *sp++);
      accu = Val_unit;
      Next;
    Instruct(SETFIELD2):
        LABELME(SETFIELD2);
      caml_modify(&Field(accu, 2), *sp++);
      accu = Val_unit;
      Next;
    Instruct(SETFIELD3):
        LABELME(SETFIELD3);
      caml_modify(&Field(accu, 3), *sp++);
      accu = Val_unit;
      Next;
    Instruct(SETFIELD):
        LABELME(SETFIELD);
      caml_modify(&Field(accu, *pc), *sp++);
      accu = Val_unit;
      pc++;
      Next;
    Instruct(SETFLOATFIELD):
        LABELME(SETFLOATFIELD);
      Store_double_flat_field(accu, *pc, Double_val(*sp));
      accu = Val_unit;
      sp++;
      pc++;
      Next;

/* Array operations */

    Instruct(VECTLENGTH): {
      /* Todo: when FLAT_FLOAT_ARRAY is false, this instruction should
         be split into VECTLENGTH and FLOATVECTLENGTH because we know
         statically which one it is. */
      //  LABELME(VECTLENGTH);
      mlsize_t size = Wosize_val(accu);
      if (Tag_val(accu) == Double_array_tag) size = size / Double_wosize;
      accu = Val_long(size);
      Next;
    }
    Instruct(GETVECTITEM):
        LABELME(GETVECTITEM);
      accu = Field(accu, Long_val(sp[0]));
      sp += 1;
      Next;
    Instruct(SETVECTITEM):
        LABELME(SETVECTITEM);
      caml_modify(&Field(accu, Long_val(sp[0])), sp[1]);
      accu = Val_unit;
      sp += 2;
      Next;

/* Bytes/String operations */
    Instruct(GETSTRINGCHAR): LABELME(GETSTRINGCHAR);
    Instruct(GETBYTESCHAR): LABELME(GETBYTESCHAR);
      accu = Val_int(Byte_u(accu, Long_val(sp[0])));
      sp += 1;
      Next;
    Instruct(SETBYTESCHAR): LABELME(SETBYTESCHAR);
      Byte_u(accu, Long_val(sp[0])) = Int_val(sp[1]);
      sp += 2;
      accu = Val_unit;
      Next;

/* Branches and conditional branches */

    Instruct(BRANCH):
        LABELME(BRANCH);
      pc += *pc;
      Next;
    Instruct(BRANCHIF):
        LABELME(BRANCHIF);
      if (accu != Val_false) pc += *pc; else pc++;
      Next;
    Instruct(BRANCHIFNOT):
        LABELME(BRANCHIFNOT);
      if (accu == Val_false) pc += *pc; else pc++;
      Next;
    Instruct(SWITCH): {
       // LABELME(SWITCH);
      uint32_t sizes = *pc++;
      if (Is_block(accu)) {
        intnat index = Tag_val(accu);
        CAMLassert ((uintnat) index < (sizes >> 16));
        pc += pc[(sizes & 0xFFFF) + index];
      } else {
        intnat index = Long_val(accu);
        CAMLassert ((uintnat) index < (sizes & 0xFFFF)) ;
        pc += pc[index];
      }
      Next;
    }
    Instruct(BOOLNOT):
        LABELME(BOOLNOT);
      accu = Val_not(accu);
      Next;

/* Exceptions */

    Instruct(PUSHTRAP):
        LABELME(PUSHTRAP);
      sp -= 4;
      Trap_pc(sp) = pc + *pc;
      Trap_link(sp) = Val_long(domain_state->trap_sp_off);
      sp[2] = env;
      sp[3] = Val_long(extra_args);
      domain_state->trap_sp_off = sp - Stack_high(domain_state->current_stack);
      pc++;
      Next;

    Instruct(POPTRAP):
        LABELME(POPTRAP);
      if (Caml_check_gc_interrupt(domain_state) ||
          caml_check_for_pending_signals()) {
        /* We must check here so that if a signal is pending and its
           handler triggers an exception, the exception is trapped
           by the current try...with, not the enclosing one. */
        pc--; /* restart the POPTRAP after processing the signal */
        goto process_signal;
      }
      domain_state->trap_sp_off = Long_val(Trap_link(sp));
      sp += 4;
      Next;

    Instruct(RAISE_NOTRACE):
        LABELME(RAISE_NOTRACE);
      Check_trap_barrier;
      goto raise_notrace;

    Instruct(RERAISE):
        LABELME(RERAISE);
      Check_trap_barrier;
      if (domain_state->backtrace_active) {
        *--sp = (value)(pc - 1);
        caml_stash_backtrace(accu, sp, 1);
      }
      goto raise_notrace;

    Instruct(RAISE):
        LABELME(RAISE);
    raise_exception:
      Check_trap_barrier;
      if (domain_state->backtrace_active) {
        *--sp = (value)(pc - 1);
        caml_stash_backtrace(accu, sp, 0);
      }
    raise_notrace:
      if (domain_state->trap_sp_off > 0) {
        if (Stack_parent(domain_state->current_stack) == NULL) {
          domain_state->external_raise = initial_external_raise;
          domain_state->trap_sp_off = initial_trap_sp_off;
          domain_state->current_stack->sp =
            Stack_high(domain_state->current_stack) - initial_stack_words ;
          return Make_exception_result(accu);
        } else {
          struct stack_info* old_stack = domain_state->current_stack;
          struct stack_info* parent_stack = Stack_parent(old_stack);
          value hexn = Stack_handle_exception(old_stack);
          old_stack->sp = sp;
          domain_state->current_stack = parent_stack;
          sp = domain_state->current_stack->sp;
          caml_free_stack(old_stack);

          domain_state->trap_sp_off = Long_val(sp[0]);
          extra_args = Long_val(sp[1]);
          sp++;
          sp[0] = accu;

          accu = hexn;
          pc = Code_val(accu);
          env = accu;
          goto check_stacks;
        }
      } else {
        sp =
           Stack_high(domain_state->current_stack) + domain_state->trap_sp_off;
        pc = Trap_pc(sp);
        domain_state->trap_sp_off = Long_val(Trap_link(sp));
        env = sp[2];
        extra_args = Long_val(sp[3]);
        sp += 4;
      }
      Next;

/* Stack checks */

    check_stacks:
      if (sp < Stack_threshold_ptr(domain_state->current_stack)) {
        domain_state->current_stack->sp = sp;
        if (!caml_try_realloc_stack(Stack_threshold / sizeof(value))) {
          Setup_for_c_call; caml_raise_stack_overflow();
        }
        sp = domain_state->current_stack->sp;
      }
      /* Fall through CHECK_SIGNALS */

/* Signal handling */

    Instruct(CHECK_SIGNALS):    /* accu not preserved */
      LABELME(CHECK_SIGNALS);
        if (Caml_check_gc_interrupt(domain_state) ||
          caml_check_for_pending_signals())
        goto process_signal;
      Next;

    process_signal:
      Setup_for_event;
      caml_process_pending_actions();
      Restore_after_event;
      Next;

/* Calling C functions */

    Instruct(C_CALL1):
        LABELME(C_CALL1);
      Setup_for_c_call;
      accu = Primitive(*pc)(accu);
      Restore_after_c_call;
      pc++;
      Next;
    Instruct(C_CALL2):
        LABELME(C_CALL2);
      Setup_for_c_call;
      accu = Primitive(*pc)(accu, sp[2]);
      Restore_after_c_call;
      sp += 1;
      pc++;
      Next;
    Instruct(C_CALL3):
        LABELME(C_CALL3);
      Setup_for_c_call;
      accu = Primitive(*pc)(accu, sp[2], sp[3]);
      Restore_after_c_call;
      sp += 2;
      pc++;
      Next;
    Instruct(C_CALL4):
        LABELME(C_CALL4);
      Setup_for_c_call;
      accu = Primitive(*pc)(accu, sp[2], sp[3], sp[4]);
      Restore_after_c_call;
      sp += 3;
      pc++;
      Next;
    Instruct(C_CALL5):
        LABELME(C_CALL5);
      Setup_for_c_call;
      accu = Primitive(*pc)(accu, sp[2], sp[3], sp[4], sp[5]);
      Restore_after_c_call;
      sp += 4;
      pc++;
      Next;
    Instruct(C_CALLN): {
      //  LABELME(C_CALLN);
      int nargs = *pc++;
      *--sp = accu;
      Setup_for_c_call;
      accu = Primitive(*pc)(sp + 2, nargs);
      Restore_after_c_call;
      sp += nargs;
      pc++;
      Next;
    }

/* Integer constants */

    Instruct(CONST0): LABELME(CONST0);
      accu = Val_int(0); Next;
    Instruct(CONST1): LABELME(CONST1);
      accu = Val_int(1); Next;
    Instruct(CONST2): LABELME(CONST2);
      accu = Val_int(2); Next;
    Instruct(CONST3): LABELME(CONST3);
      accu = Val_int(3); Next;

    Instruct(PUSHCONST0): LABELME(PUSHCONST0);
      *--sp = accu; accu = Val_int(0); Next;
    Instruct(PUSHCONST1): LABELME(PUSHCONST1);
      *--sp = accu; accu = Val_int(1); Next;
    Instruct(PUSHCONST2): LABELME(PUSHCONST2);
      *--sp = accu; accu = Val_int(2); Next;
    Instruct(PUSHCONST3): LABELME(PUSHCONST3);
      *--sp = accu; accu = Val_int(3); Next;

    Instruct(PUSHCONSTINT):
        LABELME(PUSHCONSTINT);
      *--sp = accu;
      /* Fallthrough */
    Instruct(CONSTINT):
        LABELME(CONSTINT);
      accu = Val_int(*pc);
      pc++;
      Next;

/* Integer arithmetic */

    Instruct(NEGINT): LABELME(NEGINT);
      accu = (value)(2 - (intnat)accu); Next;
    Instruct(ADDINT): LABELME(ADDINT);
      accu = (value)((intnat) accu + (intnat) *sp++ - 1); Next;
    Instruct(SUBINT): LABELME(SUBINT);
      accu = (value)((intnat) accu - (intnat) *sp++ + 1); Next;
    Instruct(MULINT): LABELME(MULINT);
      accu = Val_long(Long_val(accu) * Long_val(*sp++)); Next;

    Instruct(DIVINT): { 
        //LABELME(DIVINT);
      intnat divisor = Long_val(*sp++);
      if (divisor == 0) { Setup_for_c_call; caml_raise_zero_divide(); }
      accu = Val_long(Long_val(accu) / divisor);
      Next;
    }
    Instruct(MODINT): {
      //  LABELME(MODINT);
      intnat divisor = Long_val(*sp++);
      if (divisor == 0) { Setup_for_c_call; caml_raise_zero_divide(); }
      accu = Val_long(Long_val(accu) % divisor);
      Next;
    }
    Instruct(ANDINT):
        LABELME(ANDINT);
      accu = (value)((intnat) accu & (intnat) *sp++); Next;
    Instruct(ORINT):
        LABELME(ORINT);
      accu = (value)((intnat) accu | (intnat) *sp++); Next;
    Instruct(XORINT):
        LABELME(XORINT);
      accu = (value)(((intnat) accu ^ (intnat) *sp++) | 1); Next;
    Instruct(LSLINT):
        LABELME(LSLINT);
      accu = (value)((((intnat) accu - 1) << Long_val(*sp++)) + 1); Next;
    Instruct(LSRINT):
        LABELME(LSRINT);
      accu = (value)((((uintnat) accu) >> Long_val(*sp++)) | 1); Next;
    Instruct(ASRINT):
        LABELME(ASRINT);
      accu = (value)((((intnat) accu) >> Long_val(*sp++)) | 1); Next;

#define Integer_comparison(typ,opname,tst) \
    Instruct(opname): \
      accu = Val_int((typ) accu tst (typ) *sp++); Next;

    Integer_comparison(intnat,EQ, ==)
    Integer_comparison(intnat,NEQ, !=)
    Integer_comparison(intnat,LTINT, <)
    Integer_comparison(intnat,LEINT, <=)
    Integer_comparison(intnat,GTINT, >)
    Integer_comparison(intnat,GEINT, >=)
    Integer_comparison(uintnat,ULTINT, <)
    Integer_comparison(uintnat,UGEINT, >=)

#define Integer_branch_comparison(typ,opname,tst,debug) \
    Instruct(opname): \
      if ( *pc++ tst (typ) Long_val(accu)) { \
        pc += *pc ; \
      } else { \
        pc++ ; \
      } ; Next;

    Integer_branch_comparison(intnat,BEQ, ==, "==")
    Integer_branch_comparison(intnat,BNEQ, !=, "!=")
    Integer_branch_comparison(intnat,BLTINT, <, "<")
    Integer_branch_comparison(intnat,BLEINT, <=, "<=")
    Integer_branch_comparison(intnat,BGTINT, >, ">")
    Integer_branch_comparison(intnat,BGEINT, >=, ">=")
    Integer_branch_comparison(uintnat,BULTINT, <, "<")
    Integer_branch_comparison(uintnat,BUGEINT, >=, ">=")

    Instruct(OFFSETINT):
        LABELME(OFFSETINT);
      accu += *pc << 1;
      pc++;
      Next;
    Instruct(OFFSETREF):
        LABELME(OFFSETREF);
      Field(accu, 0) += *pc << 1;
      accu = Val_unit;
      pc++;
      Next;
    Instruct(ISINT):
        LABELME(ISINT);
      accu = Val_long(accu & 1);
      Next;

/* Object-oriented operations */

#define Lookup(obj, lab) Field (Field (obj, 0), Int_val(lab))

    Instruct(GETMETHOD):
        LABELME(GETMETHOD);
      accu = Lookup(sp[0], accu);
      Next;

#define CAML_METHOD_CACHE
#ifdef CAML_METHOD_CACHE
    Instruct(GETPUBMET): {
       // LABELME(GETPUBMET);
      /* accu == object, pc[0] == tag, pc[1] == cache */
      value meths = Field (accu, 0);
      value ofs;
#ifdef CAML_TEST_CACHE
      static int calls = 0, hits = 0;
      if (calls >= 10000000) {
        fprintf(stderr, "cache hit = %d%%\n", hits / 100000);
        calls = 0; hits = 0;
      }
      calls++;
#endif
      *--sp = accu;
      accu = Val_int(*pc++);
      ofs = *pc & Field(meths,1);
      if (*(value*)(((char*)&Field(meths,3)) + ofs) == accu) {
#ifdef CAML_TEST_CACHE
        hits++;
#endif
        accu = *(value*)(((char*)&Field(meths,2)) + ofs);
      }
      else
      {
        int li = 3, hi = Field(meths,0), mi;
        while (li < hi) {
          mi = ((li+hi) >> 1) | 1;
          if (accu < Field(meths,mi)) hi = mi-2;
          else li = mi;
        }
        *pc = (li-3)*sizeof(value);
        accu = Field (meths, li-1);
      }
      pc++;
      Next;
    }
#else
    Instruct(GETPUBMET):
        LABELME(GETPUBMET);
      *--sp = accu;
      accu = Val_int(*pc);
      pc += 2;
      /* Fallthrough */
#endif
    Instruct(GETDYNMET): {
       // LABELME(GETDYNMET);
      /* accu == tag, sp[0] == object, *pc == cache */
      value meths = Field (sp[0], 0);
      int li = 3, hi = Field(meths,0), mi;
      while (li < hi) {
        mi = ((li+hi) >> 1) | 1;
        if (accu < Field(meths,mi)) hi = mi-2;
        else li = mi;
      }
      accu = Field (meths, li-1);
      Next;
    }

/* Debugging and machine control */

    Instruct(STOP):
        LABELME(STOP);
#ifdef DEBUG
	
      // Prints out the total opcounts with respective functions
      	    
      printf("Total op_count = %lu\n", total_op_count);

      /*	      
      // Counts Op counts for certain function at that time. 	
      for (int i = 0; i < FIRST_UNIMPLEMENTED_OP; i++) {
          printf("Op_counts[%d] = %lu\n", i, op_counts[i]);
      }*/ 
	
	    //counter = 0; 
	    // Prints out hash table
	    //void * size_of_func_array = sizeof(func_hash_table->entries[0].value)/sizeof(* func_hash_table->entries[0].value[0]); 	 
	    for(int i = 0; i < func_hash_table->capacity; i++) {
		    if(func_hash_table->entries[i].key != NULL) { 
			RUNTIME_INFO("Array for func %p", func_hash_table->entries[i].key);
			op_arr = (unsigned long*)func_hash_table->entries[i].value;

			for(int j = 0; j < FIRST_UNIMPLEMENTED_OP; j++) 
				RUNTIME_INFO("Opcode: %d Count %lu", j, op_arr[j]);
		    	     // counter += (long)func_hash_table->entries[i].value; 
		    }
		    else {
			    printf("index %d in hashtab: empty\n", i); 
		    }		
	    }

	printf("Unique Functions: %d\n", (int)ht_length(func_hash_table));
	//printf("Total Number of Counts from Hash table: %ld\n", counter); 
 //   dump_func_stack(global_func_stack); 
    dump_func_stack(local_func_stack);
//      dump_func_stack_meta(func_stack);
	
//      destroy_func_stack(global_func_stack);
      destroy_func_stack(local_func_stack);	
      ht_destroy(func_hash_table); 
//        get_nr_items(func_stack);
//        get_max_items(func_stack); 
//        get_top_idx(func_stack); 
//        get_cur_size_bytes(func_stack);
//        peek(func_stack);
      
#endif 
      
      


      domain_state->external_raise = initial_external_raise;
      domain_state->trap_sp_off = initial_trap_sp_off;
      domain_state->current_stack->sp = sp;
      return accu;

    Instruct(EVENT):
      if (--caml_event_count == 0) {
        Setup_for_debugger;
        caml_debugger(EVENT_COUNT, Val_unit);
        Restore_after_debugger;
      }
      Restart_curr_instr;

    Instruct(BREAK):
      Setup_for_debugger;
      caml_debugger(BREAKPOINT, Val_unit);
      Restore_after_debugger;
      Restart_curr_instr;

/* Context switching */

    Instruct(RESUME):
      resume_fn = sp[0];
      resume_arg = sp[1];
      sp -= 3;
      sp[0] = Val_long(domain_state->trap_sp_off);
      sp[1] = Val_long(0);
      sp[2] = (value)pc;
      sp[3] = env;
      sp[4] = Val_long(extra_args);
      goto do_resume;

do_resume: {
      struct stack_info* stk = Ptr_val(accu);
      if (stk == NULL) {
         accu = Field(caml_global_data, CONTINUATION_ALREADY_TAKEN_EXN);
         goto raise_exception;
      }
      while (Stack_parent(stk) != NULL) stk = Stack_parent(stk);
      Stack_parent(stk) = Caml_state->current_stack;

      domain_state->current_stack->sp = sp;
      domain_state->current_stack = Ptr_val(accu);
      sp = domain_state->current_stack->sp;

      domain_state->trap_sp_off = Long_val(sp[0]);
      sp[0] = resume_arg;
      accu = resume_fn;
      pc = Code_val(accu);
      env = accu;
      extra_args = 0;
      goto check_stacks;
    }

    Instruct(RESUMETERM):
      resume_fn = sp[0];
      resume_arg = sp[1];
      sp = sp + *pc - 2;
      sp[0] = Val_long(domain_state->trap_sp_off);
      sp[1] = Val_long(extra_args);
      goto do_resume;


    Instruct(PERFORM): {
      value cont;
      struct stack_info* old_stack = domain_state->current_stack;
      struct stack_info* parent_stack = Stack_parent(old_stack);

      if (parent_stack == NULL) {
        accu = Field(caml_global_data, UNHANDLED_EXN);
        goto raise_exception;
      }

      Alloc_small(cont, 1, Cont_tag, Enter_gc);

      sp -= 4;
      sp[0] = Val_long(domain_state->trap_sp_off);
      sp[1] = (value)pc;
      sp[2] = env;
      sp[3] = Val_long(extra_args);

      old_stack->sp = sp;
      domain_state->current_stack = parent_stack;
      sp = parent_stack->sp;
      Stack_parent(old_stack) = NULL;
      Field(cont, 0) = Val_ptr(old_stack);

      domain_state->trap_sp_off = Long_val(sp[0]);
      extra_args = Long_val(sp[1]);
      sp--;
      sp[0] = accu;
      sp[1] = cont;
      sp[2] = Val_ptr(old_stack);
      accu = Stack_handle_effect(old_stack);
      pc = Code_val(accu);
      env = accu;
      extra_args += 2;
      goto check_stacks;
    }

    Instruct(REPERFORMTERM): {
      value eff = accu;
      value cont = sp[0];
      struct stack_info* cont_tail = Ptr_val(sp[1]);
      struct stack_info* self = domain_state->current_stack;
      struct stack_info* parent = Stack_parent(domain_state->current_stack);

      sp = sp + *pc - 2;
      sp[0] = Val_long(domain_state->trap_sp_off);
      sp[1] = Val_long(extra_args);

      if (parent == NULL) {
        accu = caml_continuation_use(cont);
        resume_fn = raise_unhandled;
        resume_arg = Field(caml_global_data, UNHANDLED_EXN);
        goto do_resume;
      }

      self->sp = sp;
      domain_state->current_stack = parent;
      sp = parent->sp;

      CAMLassert(Stack_parent(cont_tail) == NULL);
      Stack_parent(self) = NULL;
      Stack_parent(cont_tail) = self;

      domain_state->trap_sp_off = Long_val(sp[0]);
      extra_args = Long_val(sp[1]);
      sp--;
      sp[0] = eff;
      sp[1] = cont;
      sp[2] = Val_ptr(self);
      accu = Stack_handle_effect(self);
      pc = Code_val(accu);
      env = accu;
      extra_args += 2;
      goto check_stacks;
    }

#ifndef THREADED_CODE
    default:
#if _MSC_VER >= 1200
      __assume(0);
#else
      caml_fatal_error("bad opcode (%"
                           ARCH_INTNAT_PRINTF_FORMAT "x)",
                           (intnat) *(pc-1));
#endif
    }
  }
#endif
}
