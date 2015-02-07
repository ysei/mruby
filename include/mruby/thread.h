#ifdef MRB_USE_THREAD_API
#ifndef MRUBY_THREAD_H
#define MRUBY_THREAD_H

#include "mruby.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mrb_thread_t;
struct mrb_threadattr_t;

typedef struct mrb_thread_t      mrb_thread_t;
typedef struct mrb_threadattr_t mrb_threadattr_t;

typedef void *(*mrb_thread_proc_t)(mrb_state *mrb, void *arg);

MRB_API mrb_threadattr_t *mrb_thread_attr_create(mrb_state *mrb);
MRB_API void mrb_thread_attr_destroy(mrb_state *mrb, mrb_threadattr_t *attr);
MRB_API mrb_thread_t *mrb_thread_create(mrb_state *mrb, mrb_threadattr_t *attr, mrb_thread_proc_t proc, void *arg);
MRB_API void mrb_thread_destroy(mrb_state *mrb, mrb_thread_t *thread);
MRB_API int mrb_thread_join(mrb_state *mrb, mrb_thread_t *thread, void *result);
MRB_API mrb_state *mrb_thread_attach_vm(mrb_state *mrb);
MRB_API void mrb_thread_detach_vm(mrb_state *mrb);
MRB_API int  mrb_thread_sleep(mrb_state *mrb, uint32_t millis);

#ifdef __cplusplus
}
#endif

#endif
#endif
