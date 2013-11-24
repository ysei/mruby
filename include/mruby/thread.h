#ifndef MRUBY_THREAD_H
#define MRUBY_THREAD_H

#ifdef __cplusplus
extern "C" {
#endif

struct mrb_state;
typedef struct mrb_state mrb_state;

#ifdef ENABLE_THREAD

typedef void *mrb_thread;

typedef struct mrb_thread_table_t {
  size_t      capacity;
  size_t      count;
  mrb_thread *threads;
} mrb_thread_table_t;

typedef void *mrb_rwlock_t;

typedef struct mrb_thread_lock_api {
  int (*rwlock_init)(mrb_state *mrb, mrb_rwlock_t *lock);
  int (*rwlock_destroy)(mrb_state *mrb, mrb_rwlock_t lock);
  int (*rwlock_rdlock)(mrb_state *mrb, mrb_rwlock_t lock);
  int (*rwlock_wrlock)(mrb_state *mrb, mrb_rwlock_t lock);
  int (*rwlock_unlock)(mrb_state *mrb, mrb_rwlock_t lock);
} mrb_thread_lock_api;

typedef void *mrb_gem_thread_t;

typedef struct mrb_thread_api {
  mrb_gem_thread_t (*thread_get_self)(mrb_state *mrb);
  int              (*thread_equals)(mrb_state *mrb, mrb_gem_thread_t t1, mrb_gem_thread_t t2);
  mrb_value        (*thread_join)(mrb_state *mrb, mrb_gem_thread_t t);
  void             (*thread_free)(mrb_state *mrb, mrb_gem_thread_t t);
} mrb_thread_api;

#endif

#ifdef ENABLE_THREAD
extern int mrb_rwlock_init(mrb_state *mrb, mrb_rwlock_t *lock);
extern int mrb_rwlock_destroy(mrb_state *mrb, mrb_rwlock_t lock);
extern int mrb_rwlock_wrlock(mrb_state *mrb, mrb_rwlock_t lock);
extern int mrb_rwlock_rdlock(mrb_state *mrb, mrb_rwlock_t lock);
extern int mrb_rwlock_unlock(mrb_state *mrb, mrb_rwlock_t lock);
#define RWLOCK_INIT(mrb, lock)    mrb_rwlock_init(mrb, lock)
#define RWLOCK_DESTROY(mrb, lock) mrb_rwlock_destroy(mrb, lock)
#define RWLOCK_WRLOCK(mrb, lock)  mrb_rwlock_wrlock(mrb, lock)
#define RWLOCK_RDLOCK(mrb, lock)  mrb_rwlock_rdlock(mrb, lock)
#define RWLOCK_UNLOCK(mrb, lock)  mrb_rwlock_unlock(mrb, lock)
#else
#define RWLOCK_INIT(mrb, lock)    ((void)0)
#define RWLOCK_DESTROY(mrb, lock) ((void)0)
#define RWLOCK_WRLOCK(mrb, lock)  ((void)0)
#define RWLOCK_RDLOCK(mrb, lock)  ((void)0)
#define RWLOCK_UNLOCK(mrb, lock)  ((void)0)
#endif

#ifdef ENABLE_THREAD
extern mrb_gem_thread_t mrb_thread_get_self_invoke(mrb_state *mrb);
extern int              mrb_thread_equals_invoke(mrb_state *mrb, mrb_gem_thread_t t1, mrb_gem_thread_t t2);
extern mrb_value        mrb_thread_join_invoke(mrb_state *mrb, mrb_gem_thread_t t);
extern void             mrb_thread_free_invoke(mrb_state *mrb, mrb_gem_thread_t t);
#define THREAD_GET_SELF(mrb)       mrb_thread_get_self_invoke(mrb)
#define THREAD_EQUALS(mrb, t1, t2) mrb_thread_equals_invoke(mrb, t1, t2)
#define THREAD_JOIN(mrb, t)        mrb_thread_join_invoke(mrb, t)
#define THREAD_FREE(mrb, t)        mrb_thread_free_invoke(mrb, t)
#else
#define THREAD_GET_SELF(mrb)       ((void)0)
#define THREAD_EQUALS(mrb, t1, t2) ((void)0)
#define THREAD_JOIN(mrb, t)        ((void)0)
#define THREAD_FREE(mrb, t)        ((void)0)
#endif

#ifdef ENABLE_THREAD
#define MRB_VM_THREAD_TABLE         mrb_thread_table_t *thread_table
#define MRB_VM_THREAD_APIs          mrb_thread_api thread_api
#define MRB_VM_THREAD_LOCK_APIs     mrb_thread_lock_api thread_lock_api
#define MRB_DEFINE_LOCK_FIELD(name) mrb_rwlock_t name
#else
#define MRB_VM_THREAD_TABLE
#define MRB_VM_THREAD_APIs
#define MRB_VM_THREAD_LOCK_APIs
#define MRB_DEFINE_LOCK_FIELD(name)
#endif

#define MRB_VM_THREAD_DEFAULT_CAPACITY 32

#define MRB_VM_LOCK_THREAD        MRB_DEFINE_LOCK_FIELD(lock_thread)
#define MRB_VM_LOCK_HEAP          MRB_DEFINE_LOCK_FIELD(lock_heap)
#define MRB_VM_LOCK_SYMTBL        MRB_DEFINE_LOCK_FIELD(lock_symtbl)

#ifdef ENABLE_THREAD
extern mrb_bool mrb_vm_lock_init(mrb_state *mrb);
extern void     mrb_vm_lock_destroy(mrb_state *mrb);
#define MRB_VM_LOCK_INIT(mrb)     mrb_vm_lock_init((mrb))
#define MRB_VM_LOCK_DESTROY(mrb)  mrb_vm_lock_destroy((mrb))
#define MRB_VM_HEAP_RDLOCK(mrb)   RWLOCK_RDLOCK((mrb), (mrb)->lock_heap)
#define MRB_VM_HEAP_WRLOCK(mrb)   RWLOCK_WRLOCK((mrb), (mrb)->lock_heap)
#define MRB_VM_HEAP_UNLOCK(mrb)   RWLOCK_UNLOCK((mrb), (mrb)->lock_heap)
#define MRB_VM_SYMTBL_RDLOCK(mrb) RWLOCK_RDLOCK((mrb), (mrb)->lock_symtbl)
#define MRB_VM_SYMTBL_WRLOCK(mrb) RWLOCK_WRLOCK((mrb), (mrb)->lock_symtbl)
#define MRB_VM_SYMTBL_UNLOCK(mrb) RWLOCK_UNLOCK((mrb), (mrb)->lock_symtbl)
#else
#define MRB_VM_LOCK_INIT(mrb)     ((void)0)
#define MRB_VM_LOCK_DESTROY(mrb)  ((void)0)
#define MRB_VM_HEAP_RDLOCK(mrb)   ((void)0)
#define MRB_VM_HEAP_WRLOCK(mrb)   ((void)0)
#define MRB_VM_HEAP_UNLOCK(mrb)   ((void)0)
#define MRB_VM_SYMTBL_RDLOCK(mrb) ((void)0)
#define MRB_VM_SYMTBL_WRLOCK(mrb) ((void)0)
#define MRB_VM_SYMTBL_UNLOCK(mrb) ((void)0)
#endif

#ifdef ENABLE_THREAD
extern mrb_state *mrb_vm_get_thread_state(mrb_thread thread);
extern mrb_bool   mrb_vm_attach_thread(mrb_state *mrb, mrb_thread *thread);
extern void       mrb_vm_detach_thread(mrb_state *mrb, mrb_thread thread);
extern void       mrb_vm_thread_api_set(mrb_state *mrb, mrb_thread_api const *api);
extern void       mrb_vm_lock_api_set(mrb_state *mrb, mrb_thread_lock_api const *api);
#endif

#ifdef __cplusplus
}
#endif

#endif
