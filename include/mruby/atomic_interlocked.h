#ifndef MRUBY_ATOMIC_INTERLOCKED_H
#define MRUBY_ATOMIC_INTERLOCKED_H

#ifdef __cplusplus
extern "C" {
#endif

MRB_ATOMIC_API void
mrb_atomic_store(mrb_atomic_t *atom, mrb_atomic_t val)
{
#if defined(MRB_INT64)
  (void)InterlockedExchange64(atom, val);
#elif defined(MRB_INT16)
  (void)InterlockedExchange16(atom, val);
#else
  (void)InterlockedExchange(atom, val);
#endif
}

MRB_ATOMIC_API mrb_atomic_t
mrb_atomic_load(mrb_atomic_t *atom)
{
#if defined(MRB_INT64)
  return InterlockedCompareExchange64(atom, 0, 0);
#elif defined(MRB_INT16)
  return InterlockedCompareExchange16(atom, 0, 0);
#else
  return InterlockedCompareExchange(atom, 0, 0);
#endif
}

MRB_ATOMIC_API void
mrb_atomic_clear(mrb_atomic_t *atom)
{
#if defined(MRB_INT64)
  (void)InterlockedExchange64(atom, 0);
#elif defined(MRB_INT16)
  (void)InterlockedExchange16(atom, 0);
#else
  (void)InterlockedExchange(atom, 0);
#endif
}

MRB_ATOMIC_API mrb_atomic_t
mrb_atomic_inc(mrb_atomic_t *atom)
{
#if defined(MRB_INT64)
  return InterlockedIncrement64(atom);
#elif defined(MRB_INT16)
  return InterlockedIncrement16(atom);
#else
  return InterlockedIncrement(atom);
#endif
}

MRB_ATOMIC_API void
mrb_atomic_bool_store(mrb_atomic_bool_t *atom, mrb_bool val)
{
  (void)InterlockedExchange8((char*)atom, val);
}

MRB_ATOMIC_API mrb_bool
mrb_atomic_bool_load(mrb_atomic_bool_t *atom)
{
  return (mrb_bool)InterlockedOr8((char*)atom, 0);
}

#ifdef __cplusplus
}
#endif

#endif

