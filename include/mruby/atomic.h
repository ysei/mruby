#ifndef MRUBY_ATOMIC_H
#define MRUBY_ATOMIC_H

#include "mruby/atomic_types.h"

#if defined(__SIZEOF_POINTER__) // __SIZEOF_POINTER__ might be defined by GCC
# if __SIZEOF_POINTER__ == 4
#   define MRB_PTR_SIZE 32
# elif __SIZEOF_POINTER__ == 8
#   define MRB_PTR_SIZE 64
# endif
#elif defined(__POINTER_WIDTH__) // __POINTER_WIDTH__ might be defined by Clang
# if __POINTER_WIDTH__ 32
#   define MRB_PTR_SIZE 32
# elif __POINTER_WIDTH 64
#   define MRB_PTR_SIZE 64
# endif
#elif defined(_WIN64)
#  define MRB_PTR_SIZE 64
#elif defined(_WIN32)
#  define MRB_PTR_SIZE 32
#endif

#if !defined(MRB_PTR_SIZE)
# error Cannot detect a size of ptr.
#endif

#if !defined(MRB_USE_ATOMIC_API_STUB)
#  if defined(__GNUC__)
#    define GCC_VERSION (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#    if GCC_VERSION >= 40700
#      define MRB_USE_BUILTIN_ATOMIC_APIs
#    elif GCC_VERSION >= 40100
#      define MRB_USE_BUILTIN_SYNC_APIs
#    endif
#  elif defined(__clang__)
#    define MRB_USE_BUILTINT_SYNC_APIs
#  elif defined(_WIN32)
#    define MRB_USE_INTERLOCKED_APIs
#  endif
#endif

/* All implementations should provide below APIs:
 *
 * for mrb_int
 * - void mrb_atomic_store(mrb_atomic_t *atom, mrb_atomic_t val)
 * - mrb_atomic_t mrb_atomic_load(mrb_atomic_t *atom)
 * - void mrb_atomic_clear(mrb_atomic_t *atom)
 * - mrb_atomic_t mrb_atomic_inc(mrb_atomic_t *atom)
 *
 * for mrb_bool
 * - void mrb_atomic_bool_store(mrb_atomic_bool_t *atom, mrb_bool val)
 * - mrb_bool mrb_atomic_bool_load(mrb_atomic_bool_t *atom)
 */

#if defined(__GNUC__)
#  define MRB_ATOMIC_API static inline __attribute__((always_inline))
#elif defined(__MSC_VER) || defined(__ICC)
#  define MRB_ATOMIC_API static __forceinline
#elif !defined(__NO_INLINE__)
#  define MRB_ATOMIC_API static inline
#else
#  error The functionality of inlining is required.
#endif

#if defined(MRB_USE_BUILTIN_ATOMIC_APIs)
#  include "mruby/atomic_builtin_atomic.h"
#elif defined(MRB_USE_BUILTIN_SYNC_APIs)
#  include "mruby/atomic_builtin_sync.h"
#elif defined(MRB_USE_INTERLOCKED_APIs)
#  include "mruby/atomic_interlocked.h"
#elif defined(MRB_USE_ATOMIC_API_STUB)
#  include "mruby/atomic_stub.h"
#else
#  error Atomic operations are not supported.
#endif

#endif

