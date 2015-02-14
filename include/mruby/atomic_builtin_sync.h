#ifndef MRUBY_ATOMIC_BUILTIN_SYNC_H
#define MRUBY_ATOMIC_BUILTIN_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

MRB_ATOMIC_API void
mrb_atomic_store(mrb_atomic_t *atom, mrb_atomic_t val)
{
  *atom = val;
  __sync_synchronize();
}

MRB_ATOMIC_API mrb_atomic_t
mrb_atomic_load(mrb_atomic_t *atom)
{
  __sync_synchronize();
  return *atom;
}

MRB_ATOMIC_API void
mrb_atomic_clear(mrb_atomic_t *atom)
{
  (void)__sync_fetch_and_and(atom, 0);
}

MRB_ATOMIC_API mrb_atomic_t
mrb_atomic_inc(mrb_atomic_t *atom)
{
  return __sync_add_and_fetch(atom, 1);
}

MRB_ATOMIC_API void
mrb_atomic_bool_store(mrb_atomic_bool_t *atom, mrb_bool val)
{
  *atom = val;
  __sync_synchronize();
}

MRB_ATOMIC_API mrb_bool
mrb_atomic_bool_load(mrb_atomic_bool_t *atom)
{
  __sync_synchronize();
  return *atom;
}

#ifdef __cplusplus
}
#endif

#endif

