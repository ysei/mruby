#ifdef MRB_USE_MUTEX_API
#include "mruby/mutex.h"
#include <pthread.h>

struct mrb_mutex_t {
  pthread_mutex_t mutex;
};

struct mrb_mutexattr_t {
  pthread_mutexattr_t attr;
};

mrb_mutexattr_t *
mrb_mutexattr_create(mrb_state *mrb)
{
  mrb_mutexattr_t *attr = (mrb_mutexattr_t*)mrb_malloc(mrb, sizeof(mrb_mutexattr_t));
  (void)pthread_mutexattr_init(&attr->attr);
  return attr;
}

int
mrb_mutexattr_destroy(mrb_state *mrb, mrb_mutexattr_t *attr)
{
  if (attr) {
    pthread_mutexattr_destroy(&attr->attr);
    mrb_free(mrb, attr);
  }
  return 0;
}

mrb_mutex_t *
mrb_mutex_create(mrb_state *mrb, mrb_mutexattr_t *attr)
{
  mrb_mutex_t *mutex = (mrb_mutex_t*)mrb_malloc(mrb, sizeof(mrb_mutex_t));
  if (attr) {
    (void)pthread_mutex_init(&mutex->mutex, &attr->attr);
  } else {
    (void)pthread_mutex_init(&mutex->mutex, 0);
  }
  return mutex;
}

void
mrb_mutex_destroy(mrb_state *mrb, mrb_mutex_t *mutex)
{
  pthread_mutex_destroy(&mutex->mutex);
  mrb_free(mrb, mutex);
}

int
mrb_mutex_lock(mrb_state *mrb, mrb_mutex_t *mutex)
{
  return pthread_mutex_lock(&mutex->mutex);
}

int
mrb_mutex_unlock(mrb_state *mrb, mrb_mutex_t *mutex)
{
  return pthread_mutex_unlock(&mutex->mutex);
}

int
mrb_mutex_trylock(mrb_state *mrb, mrb_mutex_t *mutex)
{
  return pthread_mutex_trylock(&mutex->mutex);
}

#endif
