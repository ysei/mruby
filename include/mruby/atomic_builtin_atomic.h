#ifndef MRUBY_ATOMIC_BUILTIN_ATOMIC_H
#define MRUBY_ATOMIC_BUILTIN_ATOMIC_H

#include "mrbconf.h"
#include "mruby/value.h"

#ifdef __cplusplus
extern "C" {
#endif

MRB_ATOMIC_API void
mrb_atomic_store(mrb_atomic_t *atom, mrb_atomic_t val)
{
#if defined(MRB_INT64)
  __atomic_store_8(atom, val, __ATOMIC_SEQ_CST);
#elif defined(MRB_INT16)
  __atomic_store_2(atom, val, __ATOMIC_SEQ_CST);
#else
  __atomic_store_4(atom, val, __ATOMIC_SEQ_CST);
#endif
}

MRB_ATOMIC_API mrb_atomic_t
mrb_atomic_load(mrb_atomic_t *atom)
{
#if defined(MRB_INT64)
  return __atomic_load_8(atom, __ATOMIC_SEQ_CST);
#elif defined(MRB_INT16)
  return __atomic_load_2(atom, __ATOMIC_SEQ_CST);
#else
  return __atomic_load_4(atom, __ATOMIC_SEQ_CST);
#endif
}

MRB_ATOMIC_API void
mrb_atomic_clear(mrb_atomic_t *atom)
{
#if defined(MRB_INT64)
  __atomic_store_8(atom, 0, __ATOMIC_SEQ_CST);
#elif defined(MRB_INT16)
  __atomic_store_2(atom, 0, __ATOMIC_SEQ_CST);
#else
  __atomic_store_4(atom, 0, __ATOMIC_SEQ_CST);
#endif
}

MRB_ATOMIC_API mrb_atomic_t
mrb_atomic_inc(mrb_atomic_t *atom)
{
#if defined(MRB_INT64)
  return __atomic_add_fetch_8(atom, 1, __ATOMIC_SEQ_CST);
#elif defined(MRB_INT16)
  return __atomic_add_fetch_2(atom, 1, __ATOMIC_SEQ_CST);
#else
  return __atomic_add_fetch_4(atom, 1, __ATOMIC_SEQ_CST);
#endif
}

MRB_ATOMIC_API void
mrb_atomic_bool_store(mrb_atomic_bool_t *atom, mrb_bool val)
{
  __atomic_store_1(atom, val, __ATOMIC_SEQ_CST);
}

MRB_ATOMIC_API mrb_bool
mrb_atomic_bool_load(mrb_atomic_bool_t *atom)
{
  return __atomic_load_1(atom, __ATOMIC_SEQ_CST);
}

#ifdef __cplusplus
}
#endif

#endif
