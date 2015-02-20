#ifdef MRB_USE_THREAD_API
#include "mruby/thread.h"
#ifdef MRB_USE_GVL_API
#include "mruby/gvl.h"
#endif
#include "mruby/atomic.h"
#include <pthread.h>
#if defined __linux__
#include <unistd.h>
#else
#error Unsupported platform.
#endif

struct mrb_thread_t {
  pthread_t thread;
};

struct mrb_threadattr_t {
  pthread_attr_t attr;
};

typedef struct mrb_thread_params_t {
  mrb_state         *mrb;
  mrb_thread_proc_t  proc;
  void              *arg;
} mrb_thread_params_t;

static void*
mrb_thread_proc_start(void *arg)
{
  mrb_thread_params_t *params = (mrb_thread_params_t*)arg;
  mrb_state        *mrb_  = params->mrb;
  mrb_thread_proc_t proc_ = params->proc;
  void             *arg_  = params->arg;

  mrb_free(mrb_, params);

  return proc_(mrb_, arg_);
}

MRB_API mrb_threadattr_t*
mrb_threadattr_create(mrb_state *mrb)
{
  mrb_threadattr_t *attr = (mrb_threadattr_t*)mrb_malloc(mrb, sizeof(mrb_threadattr_t));
  (void)pthread_attr_init(&attr->attr);
  return attr;
}

MRB_API void
mrb_threadattr_destroy(mrb_state *mrb, mrb_threadattr_t *attr)
{
  pthread_attr_destroy(&attr->attr);
}

MRB_API mrb_thread_t*
mrb_thread_create(mrb_state *mrb, mrb_threadattr_t *attr, mrb_thread_proc_t proc, void *arg)
{
  int err;
  mrb_thread_t *t = (mrb_thread_t*)mrb_malloc(mrb, sizeof(mrb_thread_t));
  mrb_thread_params_t *p = (mrb_thread_params_t*)mrb_malloc(mrb, sizeof(mrb_thread_params_t));
  p->mrb = mrb;
  p->proc = proc;
  p->arg = arg;
  if (attr) {
    err = pthread_create(&t->thread, &attr->attr, mrb_thread_proc_start, p);
  } else {
    err = pthread_create(&t->thread, 0, mrb_thread_proc_start, p);
  }
  if (err != 0) {
    mrb_free(mrb, p);
    mrb_free(mrb, t);
    mrb_raise(mrb, E_RUNTIME_ERROR, "cannot create a new thread.");
  }
  return t;
}

MRB_API void
mrb_thread_destroy(mrb_state *mrb, mrb_thread_t *thread)
{
  mrb_free(mrb, thread);
}

MRB_API int
mrb_thread_join(mrb_state *mrb, mrb_thread_t *thread, void *result)
{
#ifdef MRB_USE_GVL_API
  int err;
  const mrb_bool is_gvl_acquired = mrb_gvl_is_acquired(mrb);
  if (is_gvl_acquired) {
    mrb_gvl_release(mrb);
  }
  err = pthread_join(thread->thread, result);
  if (is_gvl_acquired) {
    mrb_gvl_acquire(mrb);
  }
  return err;
#else
  return pthread_join(thread->thread, result);
#endif
}

MRB_API mrb_state*
mrb_thread_attach_vm(mrb_state *mrb)
{
  mrb_int i;
  mrb_state *new_state = mrb_duplicate_core(mrb);
  for (i = 0; i < MRB_FIXED_THREAD_SIZE; ++i) {
    if (MRB_GET_VM(mrb)->threads[i] == NULL) {
      MRB_GET_VM(mrb)->threads[i] = MRB_GET_THREAD_CONTEXT(new_state);
      ++MRB_GET_VM(mrb)->thread_count;
      break;
    }
  }
  return new_state;
}

MRB_API void
mrb_thread_detach_vm(mrb_state *mrb)
{
  mrb_thread_context *context;
  mrb_int i;
  if (!mrb) {
    return;
  }
  context = MRB_GET_THREAD_CONTEXT(mrb);
  for (i = 0; i < MRB_FIXED_THREAD_SIZE; ++i) {
    if (MRB_GET_VM(mrb)->threads[i] == context) {
      MRB_GET_VM(mrb)->threads[i] = NULL;
      --MRB_GET_VM(mrb)->thread_count;
      mrb_close_duplicated(mrb);
      break;
    }
  }
}

MRB_API int
mrb_thread_sleep(mrb_state *mrb, uint32_t millis)
{
  return usleep(millis * 1000);
}

#ifdef MRB_USE_GVL_API

static void*
timer_thread(mrb_state *mrb, void *arg)
{
  size_t i;
  while (!mrb_atomic_bool_load(&MRB_GET_VM(mrb)->stop_timer_thread)) {
    mrb_thread_sleep(mrb, 1);
    if (MRB_GET_VM(mrb)->thread_count <= 1) {
      continue;
    }
    for (i = 0; i < MRB_FIXED_THREAD_SIZE; ++i) {
      mrb_thread_context * const context = MRB_GET_VM(mrb)->threads[i];
      if (!context) {
        continue;
      }
      if (!mrb_atomic_bool_load(&context->flag_gvl_acquired)) {
        continue;
      }
      mrb_atomic_bool_store(&context->flag_gvl_releasing_requested, TRUE);
    }
  }
  return 0;
}

MRB_API void
mrb_timer_thread_create(mrb_state *mrb)
{
  if (MRB_GET_VM(mrb)->timer_thread) {
    return;
  }
  MRB_GET_VM(mrb)->timer_thread = mrb_thread_create(mrb, NULL, timer_thread, NULL);
}

MRB_API void
mrb_timer_thread_destroy(mrb_state *mrb)
{
  if (!MRB_GET_VM(mrb)->timer_thread) {
    return;
  }
  mrb_atomic_bool_store(&MRB_GET_VM(mrb)->stop_timer_thread, TRUE);
  mrb_thread_join(mrb, MRB_GET_VM(mrb)->timer_thread, NULL);
  mrb_thread_destroy(mrb, MRB_GET_VM(mrb)->timer_thread);
  MRB_GET_VM(mrb)->timer_thread = NULL;
}

#endif
#endif
