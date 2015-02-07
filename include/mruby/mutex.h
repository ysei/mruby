#ifdef MRB_USE_MUTEX_API
#ifndef MRUBY_MUTEX_H
#define MRUBY_MUTEX_H

#include "mruby.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mrb_mutexattr_t;
struct mrb_mutex_t;

typedef struct mrb_mutexattr_t mrb_mutexattr_t;
typedef struct mrb_mutex_t mrb_mutex_t;

MRB_API mrb_mutexattr_t *mrb_mutexattr_create(mrb_state *mrb);
MRB_API int mrb_mutexattr_destroy(mrb_state *mrb, mrb_mutexattr_t *attr);
MRB_API mrb_mutex_t *mrb_mutex_create(mrb_state *mrb, mrb_mutexattr_t *attr);
MRB_API void mrb_mutex_destroy(mrb_state *mrb, mrb_mutex_t *mutex);
MRB_API int mrb_mutex_lock(mrb_state *mrb, mrb_mutex_t *mutex);
MRB_API int mrb_mutex_unlock(mrb_state *mrb, mrb_mutex_t *mutex);
MRB_API int mrb_mutex_trylock(mrb_state *mrb, mrb_mutex_t *mutex);

#ifdef __cplusplus
}
#endif

#endif
#endif

