#include "mruby.h"
#include "mruby/thread.h"
#include "mruby/variable.h"
#include "mruby/gc.h"

#ifdef ENABLE_THREAD

int
mrb_rwlock_init(mrb_state *mrb, mrb_rwlock_t *lock)
{
  if (mrb->thread_lock_api.rwlock_init) {
    return mrb->thread_lock_api.rwlock_init(mrb, lock);
  }
  return -1;
}

int
mrb_rwlock_destroy(mrb_state *mrb, mrb_rwlock_t lock)
{
  if (mrb->thread_lock_api.rwlock_destroy) {
    return mrb->thread_lock_api.rwlock_destroy(mrb, lock);
  }
  return -1;
}

int
mrb_rwlock_wrlock(mrb_state *mrb, mrb_rwlock_t lock)
{
  if (mrb->thread_lock_api.rwlock_wrlock && lock) {
    return mrb->thread_lock_api.rwlock_wrlock(mrb, lock);
  }
  return -1;
}

int
mrb_rwlock_rdlock(mrb_state *mrb, mrb_rwlock_t lock)
{
  if (mrb->thread_lock_api.rwlock_rdlock && lock) {
    return mrb->thread_lock_api.rwlock_rdlock(mrb, lock);
  }
  return -1;
}

int
mrb_rwlock_unlock(mrb_state *mrb, mrb_rwlock_t lock)
{
  if (mrb->thread_lock_api.rwlock_unlock && lock) {
    return mrb->thread_lock_api.rwlock_unlock(mrb, lock);
  }
  return -1;
}

mrb_gem_thread_t
mrb_thread_get_self_invoke(mrb_state *mrb)
{
  if (mrb->thread_api.thread_get_self) {
    return mrb->thread_api.thread_get_self(mrb);
  }
  return NULL;
}

int
mrb_thread_equals_invoke(mrb_state *mrb, mrb_gem_thread_t t1, mrb_gem_thread_t t2)
{
  if (mrb->thread_api.thread_equals) {
    return mrb->thread_api.thread_equals(mrb, t1, t2);
  }
  return FALSE;
}

mrb_value
mrb_thread_join_invoke(mrb_state *mrb, mrb_gem_thread_t t)
{
  if (mrb->thread_api.thread_join) {
    return mrb->thread_api.thread_join(mrb, t);
  }
  return mrb_nil_value();
}

void
mrb_thread_free_invoke(mrb_state *mrb, mrb_gem_thread_t t)
{
  if (mrb->thread_api.thread_free) {
    mrb->thread_api.thread_free(mrb, t);
  }
}


typedef struct mrb_thread_impl {
  mrb_state *mrb;
  mrb_gem_thread_t thread;
} mrb_thread_impl;

extern void mrb_init_symtbl(mrb_state*);
extern void mrb_init_class(mrb_state*);
extern void mrb_init_object(mrb_state*);
extern void mrb_init_kernel(mrb_state*);
extern void mrb_init_comparable(mrb_state*);
extern void mrb_init_enumerable(mrb_state*);
extern void mrb_init_symbol(mrb_state*);
extern void mrb_init_exception(mrb_state*);
extern void mrb_init_proc(mrb_state*);
extern void mrb_init_string(mrb_state*);
extern void mrb_init_array(mrb_state*);
extern void mrb_init_hash(mrb_state*);
extern void mrb_init_heap(mrb_state*);
extern void mrb_init_numeric(mrb_state*);
extern void mrb_init_range(mrb_state*);
extern void mrb_init_gc(mrb_state*);
extern void mrb_init_math(mrb_state*);
extern void mrb_init_mrblib(mrb_state*);
extern void mrb_init_mrbgems(mrb_state*);
extern void mrb_final_core(mrb_state*);
extern void mrb_final_mrbgems(mrb_state*);
extern void mrb_free_heap(mrb_state *mrb);

#define DONE mrb_gc_arena_restore(mrb, 0);
static void
mrb_init_core_for_thread(mrb_state *mrb)
{
  mrb_init_class(mrb); DONE;
  mrb_init_object(mrb); DONE;
  mrb_init_kernel(mrb); DONE;
  mrb_init_comparable(mrb); DONE;
  mrb_init_enumerable(mrb); DONE;

  mrb_init_symbol(mrb); DONE;
  mrb_init_exception(mrb); DONE;
  mrb_init_proc(mrb); DONE;
  mrb_init_string(mrb); DONE;
  mrb_init_array(mrb); DONE;
  mrb_init_hash(mrb); DONE;
  mrb_init_numeric(mrb); DONE;
  mrb_init_range(mrb); DONE;
  mrb_init_gc(mrb); DONE;
  mrb_init_mrblib(mrb); DONE;
#ifndef DISABLE_GEMS
  mrb_init_mrbgems(mrb); DONE;
#endif
}

void
mrb_final_core_for_thread(mrb_state *mrb)
{
#ifndef DISABLE_GEMS
  mrb_final_mrbgems(mrb); DONE;
#endif
}

extern void mrb_init_core(mrb_state*);

#define DEFAULT_GC_INTERVAL_RATIO 200
#define DEFAULT_GC_STEP_RATIO 200
#define DEFAULT_MAJOR_GC_INC_RATIO 200
#define is_generational(mrb) ((mrb)->is_generational_gc_mode)
#define is_major_gc(mrb) (is_generational(mrb) && (mrb)->gc_full)
#define is_minor_gc(mrb) (is_generational(mrb) && !(mrb)->gc_full)

static mrb_state*
mrb_vm_thread_open(mrb_state *mrb)
{
  static const struct mrb_context mrb_context_zero = { 0 };
  mrb_state *thread_mrb;

#ifdef MRB_NAN_BOXING
  mrb_assert(sizeof(void*) == 4);
#endif

  thread_mrb = (mrb_state *)mrb_malloc(mrb, sizeof(mrb_state));
  if (thread_mrb == NULL) return NULL;

  *thread_mrb = *mrb;

  thread_mrb->jmp = NULL;

  thread_mrb->current_white_part = MRB_GC_WHITE_A;

#ifndef MRB_GC_FIXED_ARENA
  thread_mrb->arena = (struct RBasic**)mrb_malloc(mrb, sizeof(struct RBasic*)*MRB_GC_ARENA_SIZE);
  thread_mrb->arena_capa = MRB_GC_ARENA_SIZE;
#endif

  thread_mrb->c = (struct mrb_context*)mrb_malloc(mrb, sizeof(struct mrb_context));
  *thread_mrb->c = mrb_context_zero;
  thread_mrb->root_c = thread_mrb->c;

  thread_mrb->mems = NULL;

  mrb_init_core_for_thread(thread_mrb);

  return thread_mrb;
}

struct alloca_header {
  struct alloca_header *next;
  char buf[];
};

static void
mrb_alloca_free(mrb_state *mrb)
{
  struct alloca_header *p;
  struct alloca_header *tmp;

  if (mrb == NULL) return;
  p = mrb->mems;

  while (p) {
    tmp = p;
    p = p->next;
    mrb_free(mrb, tmp);
  }
}

void
mrb_vm_thread_close(mrb_state *mrb)
{
  mrb_final_core_for_thread(mrb);

  /* free */
  mrb->name2sym = NULL;
  mrb->globals = NULL;
  mrb_free_context(mrb, mrb->root_c);
  mrb->heaps = NULL;
  mrb_alloca_free(mrb);
  mrb_free(mrb, mrb);
}

mrb_state *
mrb_vm_get_thread_state(mrb_thread thread)
{
  return ((mrb_thread_impl*)thread)->mrb;
}

mrb_bool
mrb_vm_attach_thread(mrb_state *mrb, mrb_thread *thread)
{
  mrb_thread_impl *entry = NULL;

  if ((mrb == NULL) || (thread == NULL)) {
    /* invalid parameter */
    return FALSE;
  }

  RWLOCK_RDLOCK(mrb, mrb->lock_thread);

  if (!mrb->thread_table) {
    RWLOCK_UNLOCK(mrb, mrb->lock_thread);
    RWLOCK_WRLOCK(mrb, mrb->lock_thread);
    mrb->thread_table = (mrb_thread_table_t*)mrb_malloc(mrb, sizeof(mrb_thread_table_t));
    if (NULL == mrb->thread_table) {
      RWLOCK_UNLOCK(mrb, mrb->lock_thread);
      return FALSE;
    }
    mrb->thread_table->capacity = 0;
    mrb->thread_table->count    = 0;
    mrb->thread_table->threads  = NULL;
    RWLOCK_UNLOCK(mrb, mrb->lock_thread);
    RWLOCK_RDLOCK(mrb, mrb->lock_thread);
  }

  if (!mrb->thread_table->threads) {
    RWLOCK_UNLOCK(mrb, mrb->lock_thread);
    RWLOCK_WRLOCK(mrb, mrb->lock_thread);
    mrb_thread *threads = (mrb_thread*)mrb_malloc(mrb, sizeof(mrb_thread) * MRB_VM_THREAD_DEFAULT_CAPACITY);
    if (!threads) {
      RWLOCK_UNLOCK(mrb, mrb->lock_thread);
      return FALSE;
    }
    mrb->thread_table->capacity = MRB_VM_THREAD_DEFAULT_CAPACITY;
    mrb->thread_table->count = 0;
    mrb->thread_table->threads = threads;
    RWLOCK_UNLOCK(mrb, mrb->lock_thread);
    RWLOCK_RDLOCK(mrb, mrb->lock_thread);
  }

  size_t i;
  size_t const capacity = mrb->thread_table->capacity;
  for (i = 0; i < capacity; ++i) {
    if (mrb->thread_table->threads[i] != NULL) {
      mrb_thread_impl *impl = (mrb_thread_impl*)mrb->thread_table->threads[i];
      if (THREAD_EQUALS(mrb, THREAD_GET_SELF(mrb), impl->thread)) {
        *thread = mrb->thread_table->threads[i];
        RWLOCK_UNLOCK(mrb, mrb->lock_thread);
        return TRUE;
      }
      continue;
    }
    RWLOCK_UNLOCK(mrb, mrb->lock_thread);
    RWLOCK_WRLOCK(mrb, mrb->lock_thread);
    if (mrb->thread_table->threads[i] != NULL) {
      RWLOCK_UNLOCK(mrb, mrb->lock_thread);
      RWLOCK_RDLOCK(mrb, mrb->lock_thread);
      continue;
    }
    entry = (mrb_thread_impl*)mrb_malloc(mrb, sizeof(mrb_thread_impl));
    mrb->thread_table->threads[i] = (mrb_thread)entry;
    mrb->thread_table->count += 1;
    RWLOCK_UNLOCK(mrb, mrb->lock_thread);
    break;
  }

  if (entry == NULL) {
    RWLOCK_UNLOCK(mrb, mrb->lock_thread);
    return FALSE;
  }

  entry->mrb = mrb_vm_thread_open(mrb);
  entry->thread = THREAD_GET_SELF(entry->mrb);

  if (entry->mrb == NULL) {
    mrb_free(mrb, entry);
    return FALSE;
  }

  *thread = (mrb_thread)entry;

  return TRUE;
}

void
mrb_vm_detach_thread(mrb_state *mrb, mrb_thread thread)
{
  if (thread == NULL) {
    return;
  }

  mrb_thread_impl *entry = NULL;

  RWLOCK_RDLOCK(mrb, mrb->lock_thread);

  if (!mrb->thread_table) {
    RWLOCK_UNLOCK(mrb, mrb->lock_thread);
    return;
  }
  if (!mrb->thread_table->threads) {
    RWLOCK_UNLOCK(mrb, mrb->lock_thread);
    return;
  }

  size_t i;
  size_t const capacity = mrb->thread_table->capacity;
  for (i = 0; i < capacity; ++i) {
    if (mrb->thread_table->threads[i] != thread) {
      continue;
    }
    RWLOCK_UNLOCK(mrb, mrb->lock_thread);
    RWLOCK_WRLOCK(mrb, mrb->lock_thread);
    if (mrb->thread_table->threads[i] == NULL) {
      RWLOCK_UNLOCK(mrb, mrb->lock_thread);
      RWLOCK_RDLOCK(mrb, mrb->lock_thread);
      continue;
    }
    entry = (mrb_thread_impl*)mrb->thread_table->threads[i];
    mrb->thread_table->threads[i] = NULL;
    mrb->thread_table->count -= 1;
    RWLOCK_UNLOCK(mrb, mrb->lock_thread);
    break;
  }

  THREAD_FREE(entry->mrb, entry->thread);
  mrb_vm_thread_close(entry->mrb);
}

void
mrb_vm_thread_api_set(mrb_state *mrb, mrb_thread_api const *api)
{
  mrb->thread_api = *api;
}

void
mrb_vm_lock_api_set(mrb_state *mrb, mrb_thread_lock_api const *api)
{
  mrb->thread_lock_api = *api;
}

mrb_bool
mrb_vm_lock_init(mrb_state *mrb)
{
  RWLOCK_INIT(mrb, &mrb->lock_thread);
  RWLOCK_INIT(mrb, &mrb->lock_heap);
  RWLOCK_INIT(mrb, &mrb->lock_symtbl);
  return TRUE;
}

void
mrb_vm_lock_destroy(mrb_state *mrb)
{
  RWLOCK_DESTROY(mrb, mrb->lock_symtbl);
  RWLOCK_DESTROY(mrb, mrb->lock_heap);
  RWLOCK_DESTROY(mrb, mrb->lock_thread);
}

#endif
