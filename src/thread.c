#include "mruby.h"
#include "mruby/thread.h"
#include "mruby/variable.h"
#include "mruby/gc.h"

#ifdef ENABLE_THREAD

#ifdef ENABLE_STDIO
#include <stdio.h>
#endif

void
mrb_default_deadlock_handler(mrb_state *mrb, mrb_lock_t *lock)
{
#ifdef ENABLE_STDIO
  fprintf(
    stderr,
    "BUG: Deadlock is detected (Lock: %p)\n", lock);
#endif
  __builtin_trap();
}

int
mrb_lock_init(mrb_state *mrb, mrb_lock_t *lock)
{
  if (mrb->thread_lock_api.lock_init) {
    return mrb->thread_lock_api.lock_init(mrb, lock);
  }
  return MRB_LOCK_STATUS_NOT_SUPPORTED;
}

int
mrb_lock_destroy(mrb_state *mrb, mrb_lock_t *lock)
{
  if (mrb->thread_lock_api.lock_destroy) {
    return mrb->thread_lock_api.lock_destroy(mrb, lock);
  }
  return MRB_LOCK_STATUS_NOT_SUPPORTED;
}

int
mrb_lock_lock(mrb_state *mrb, mrb_lock_t *lock)
{
  if (mrb->thread_lock_api.lock_lock) {
    if (!lock) {
      return MRB_LOCK_STATUS_INVALID_ARGUMENTS;
    }
    int const status = mrb->thread_lock_api.lock_lock(mrb, lock, MRB_LOCK_DEADLOCK_DETECTION_TIMEOUT);
    if (status == MRB_LOCK_STATUS_TIMEOUT) {
      if (mrb->thread_lock_api.lock_deadlock_handler) {
        mrb->thread_lock_api.lock_deadlock_handler(mrb, lock);
      } else {
        mrb_default_deadlock_handler(mrb, lock);
      }
    }
    return status;
  }
  return MRB_LOCK_STATUS_NOT_SUPPORTED;
}

int
mrb_lock_unlock(mrb_state *mrb, mrb_lock_t *lock)
{
  if (mrb->thread_lock_api.lock_unlock) {
    if (!lock) {
      return MRB_LOCK_STATUS_INVALID_ARGUMENTS;
    }
    return mrb->thread_lock_api.lock_unlock(mrb, lock);
  }
  return MRB_LOCK_STATUS_NOT_SUPPORTED;
}

mrb_gem_thread_t
mrb_thread_get_self_invoke(mrb_state *mrb)
{
  if (mrb->thread_api.thread_get_self) {
    return mrb->thread_api.thread_get_self(mrb);
  }
  return MRB_GEM_THREAD_INVALID;
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


static void
mrb_init_core_for_thread(mrb_state *mrb)
{
}

void
mrb_final_core_for_thread(mrb_state *mrb)
{
}

extern void mrb_init_core(mrb_state*);

static mrb_state*
mrb_vm_thread_open(mrb_state *mrb)
{
  static const struct mrb_context mrb_context_zero = { 0 };
  mrb_state *thread_mrb;

#ifdef MRB_NAN_BOXING
  mrb_assert(sizeof(void*) == 4);
#endif

  thread_mrb = (mrb_state *)mrb_malloc_without_gc_lock(mrb, sizeof(mrb_state));
  if (thread_mrb == NULL) return NULL;

  *thread_mrb = *mrb;

  thread_mrb->jmp = NULL;

#ifndef MRB_GC_FIXED_ARENA
  thread_mrb->arena = (struct RBasic**)mrb_malloc_without_gc_lock(mrb, sizeof(struct RBasic*)*MRB_GC_ARENA_SIZE);
  thread_mrb->arena_capa = MRB_GC_ARENA_SIZE;
#endif
  thread_mrb->arena_idx = 0;

  thread_mrb->c = (struct mrb_context*)mrb_malloc_without_gc_lock(mrb, sizeof(struct mrb_context));
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
mrb_vm_get_thread_state(mrb_thread_t *thread)
{
  return thread->mrb;
}

mrb_bool
mrb_vm_attach_thread(mrb_state *mrb, mrb_thread_t *thread)
{
  if ((mrb == NULL) || (thread == NULL)) {
    /* invalid parameter */
    return FALSE;
  }

  MRB_VM_GC_LOCK_AND_DEFINE(mrb);

  mrb_thread_t *entry = NULL;

  size_t i;
  size_t const capacity = mrb->thread_table->capacity;
  for (i = 1; i < capacity; ++i) {
    if (mrb->thread_table->threads[i].mrb != NULL) {
      mrb_thread_t *t = &mrb->thread_table->threads[i];
      if (MRB_THREAD_EQUALS(mrb, MRB_THREAD_GET_SELF(mrb), t->thread)) {
        *thread = mrb->thread_table->threads[i];
        MRB_VM_GC_UNLOCK_IF_LOCKED(mrb);
        return TRUE;
      }
      continue;
    }
    if (mrb->thread_table->threads[i].mrb != NULL) {
      continue;
    }
    entry = &mrb->thread_table->threads[i];
    mrb->thread_table->count += 1;
    break;
  }

  if (entry == NULL) {
    MRB_VM_GC_UNLOCK_IF_LOCKED(mrb);
    return FALSE;
  }

  entry->mrb = mrb_vm_thread_open(mrb);
  entry->thread = MRB_THREAD_GET_SELF(entry->mrb);

  if (entry->mrb == NULL) {
    MRB_VM_GC_UNLOCK_IF_LOCKED(mrb);
    return FALSE;
  }

  *thread = *entry;

  MRB_VM_GC_UNLOCK_IF_LOCKED(mrb);

  return TRUE;
}

mrb_value
mrb_vm_detach_thread(mrb_state *mrb, mrb_thread_t *thread)
{
  if (thread == NULL) {
    return mrb_nil_value();
  }

  if (!mrb->thread_table) {
    return mrb_nil_value();
  }

  MRB_VM_GC_LOCK_AND_DEFINE(mrb);

  if (!mrb->thread_table) {
    MRB_VM_GC_UNLOCK_IF_LOCKED(mrb);
    return mrb_nil_value();
  }

  if (!mrb->thread_table->threads) {
    MRB_VM_GC_UNLOCK_IF_LOCKED(mrb);
    return mrb_nil_value();
  }

  mrb_thread_t entry = MRB_THREAD_INITIALIZER;
  size_t i;
  size_t const capacity = mrb->thread_table->capacity;
  for (i = 0; i < capacity; ++i) {
    if (mrb->thread_table->threads[i].mrb != thread->mrb) {
      continue;
    }
    if (mrb->thread_table->threads[i].mrb == NULL) {
      continue;
    }
    entry = mrb->thread_table->threads[i];
    mrb_gc_protect(mrb, entry.retval);
    mrb->thread_table->threads[i].mrb = NULL;
    mrb->thread_table->threads[i].thread = MRB_GEM_THREAD_INITIALIZER;
    mrb->thread_table->threads[i].retval = mrb_nil_value();
    mrb->thread_table->count -= 1;
    break;
  }

  if (entry.mrb != NULL) {
    MRB_THREAD_FREE(entry.mrb, entry.thread);
    mrb_vm_thread_close(entry.mrb);
  }

  MRB_VM_GC_UNLOCK_IF_LOCKED(mrb);

  return entry.retval;
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
  mrb->lock_heap   = (mrb_lock_t*)mrb_malloc(mrb, sizeof(mrb_lock_t));
  mrb->lock_symtbl = (mrb_lock_t*)mrb_malloc(mrb, sizeof(mrb_lock_t));
  mrb->lock_thread = (mrb_lock_t*)mrb_malloc(mrb, sizeof(mrb_lock_t));
  mrb->lock_gc     = (mrb_lock_t*)mrb_malloc(mrb, sizeof(mrb_lock_t));

  if (MRB_LOCK_INIT(mrb, mrb->lock_heap) != MRB_LOCK_STATUS_OK) {
    return FALSE;
  }
  if (MRB_LOCK_INIT(mrb, mrb->lock_symtbl) != MRB_LOCK_STATUS_OK) {
    return FALSE;
  }
  if (MRB_LOCK_INIT(mrb, mrb->lock_thread) != MRB_LOCK_STATUS_OK) {
    return FALSE;
  }
  if (MRB_LOCK_INIT(mrb, mrb->lock_gc) != MRB_LOCK_STATUS_OK) {
    return FALSE;
  }
  return TRUE;
}

void
mrb_vm_lock_destroy(mrb_state *mrb)
{
  MRB_LOCK_DESTROY(mrb, mrb->lock_symtbl);
  MRB_LOCK_DESTROY(mrb, mrb->lock_heap);
  MRB_LOCK_DESTROY(mrb, mrb->lock_thread);
  MRB_LOCK_DESTROY(mrb, mrb->lock_gc);
  mrb_free(mrb, mrb->lock_symtbl);
  mrb_free(mrb, mrb->lock_heap);
  mrb_free(mrb, mrb->lock_thread);
  mrb_free(mrb, mrb->lock_gc);
}

#endif
