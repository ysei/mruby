#ifndef MRUBY_THREAD_H
#define MRUBY_THREAD_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ENABLE_THREAD

typedef void    *mrb_thread;
typedef void    *mrb_gem_thread_t;
typedef uint32_t mrb_gem_thread_id_t;
typedef void    *mrb_gem_rwlock_t;

/**
 * Type of thread table
 */
typedef struct mrb_thread_table_t {
  size_t      capacity;
  size_t      count;
  mrb_thread *threads;
} mrb_thread_table_t;

/**
 * RWLock structure type
 */
typedef struct mrb_rwlock_t {
  mrb_gem_rwlock_t    rwlock;
} mrb_rwlock_t;

#define MRB_RWLOCK_INVALID ((mrb_rwlock_t){ .rwlock = NULL })

/**
 * Table of Lock APIs
 */
typedef struct mrb_thread_lock_api {
  int  (*rwlock_init)(struct mrb_state *mrb, mrb_rwlock_t *lock);
  int  (*rwlock_destroy)(struct mrb_state *mrb, mrb_rwlock_t *lock);
  int  (*rwlock_rdlock)(struct mrb_state *mrb, mrb_rwlock_t *lock, uint32_t timeout_ms);
  int  (*rwlock_wrlock)(struct mrb_state *mrb, mrb_rwlock_t *lock, uint32_t timeout_ms);
  int  (*rwlock_unlock)(struct mrb_state *mrb, mrb_rwlock_t *lock);
  void (*rwlock_deadlock_handler)(struct mrb_state *mrb, mrb_rwlock_t *lock);
} mrb_thread_lock_api;

/**
 * Table of Thread APIs
 */
typedef struct mrb_thread_api {
  mrb_gem_thread_t    (*thread_get_self)(struct mrb_state *mrb);
  int                 (*thread_equals)(struct mrb_state *mrb, mrb_gem_thread_t t1, mrb_gem_thread_t t2);
  mrb_value           (*thread_join)(struct mrb_state *mrb, mrb_gem_thread_t t);
  void                (*thread_free)(struct mrb_state *mrb, mrb_gem_thread_t t);
} mrb_thread_api;

#endif

#ifdef ENABLE_THREAD
extern int mrb_rwlock_init(struct mrb_state *mrb, mrb_rwlock_t *lock);
extern int mrb_rwlock_destroy(struct mrb_state *mrb, mrb_rwlock_t *lock);
extern int mrb_rwlock_wrlock(struct mrb_state *mrb, mrb_rwlock_t *lock);
extern int mrb_rwlock_rdlock(struct mrb_state *mrb, mrb_rwlock_t *lock);
extern int mrb_rwlock_unlock(struct mrb_state *mrb, mrb_rwlock_t *lock);
#define RWLOCK_INIT(mrb, lock)               mrb_rwlock_init(mrb, &lock)
#define RWLOCK_DESTROY(mrb, lock)            mrb_rwlock_destroy(mrb, &lock)
#define RWLOCK_RDLOCK(mrb, lock)             (var_lock_status_ = mrb_rwlock_rdlock(mrb, &lock))
#define RWLOCK_WRLOCK(mrb, lock)             (var_lock_status_ = mrb_rwlock_wrlock(mrb, &lock))
#define RWLOCK_UNLOCK(mrb, lock)             mrb_rwlock_unlock(mrb, &lock)
#define RWLOCK_RDLOCK_AND_DEFINE(mrb, lock)  int var_lock_status_ = RWLOCK_RDLOCK(mrb, lock)
#define RWLOCK_WRLOCK_AND_DEFINE(mrb, lock)  int var_lock_status_ = RWLOCK_WRLOCK(mrb, lock)
#define RWLOCK_UNLOCK_IF_LOCKED(mrb, lock)   ((var_lock_status_ == RWLOCK_STATUS_OK) ? RWLOCK_UNLOCK(mrb, lock) : var_lock_status_)
#else
#define RWLOCK_INIT(mrb, lock)               TRUE
#define RWLOCK_DESTROY(mrb, lock)            RWLOCK_STATUS_OK
#define RWLOCK_RDLOCK(mrb, lock)             RWLOCK_STATUS_OK
#define RWLOCK_WRLOCK(mrb, lock)             RWLOCK_STATUS_OK
#define RWLOCK_UNLOCK(mrb, lock)             RWLOCK_STATUS_OK
#define RWLOCK_RDLOCK_AND_DEFINE(mrb, lock)
#define RWLOCK_WRLOCK_AND_DEFINE(mrb, lock)
#define RWLOCK_UNLOCK_IF_LOCKED(mrb, lock)
#endif

#ifdef ENABLE_THREAD
extern void mrb_default_deadlock_handler(struct mrb_state *mrb, mrb_rwlock_t *lock);
#endif

#define RWLOCK_DEADLOCK_DETECTION_TIMEOUT 3000
#define RWLOCK_STATUS_OK                 0
#define RWLOCK_STATUS_NOT_SUPPORTED      1
#define RWLOCK_STATUS_TIMEOUT            2
#define RWLOCK_STATUS_INVALID_ARGUMENTS  3
#define RWLOCK_STATUS_ALREADY_LOCKED     4
#define RWLOCK_STATUS_UNKNOWN           99

#ifdef ENABLE_THREAD
extern mrb_gem_thread_t mrb_thread_get_self_invoke(struct mrb_state *mrb);
extern int              mrb_thread_equals_invoke(struct mrb_state *mrb, mrb_gem_thread_t t1, mrb_gem_thread_t t2);
extern mrb_value        mrb_thread_join_invoke(struct mrb_state *mrb, mrb_gem_thread_t t);
extern void             mrb_thread_free_invoke(struct mrb_state *mrb, mrb_gem_thread_t t);
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
extern mrb_bool mrb_vm_lock_init(struct mrb_state *mrb);
extern void     mrb_vm_lock_destroy(struct mrb_state *mrb);
#define MRB_VM_LOCK_INIT(mrb)                mrb_vm_lock_init((mrb))
#define MRB_VM_LOCK_DESTROY(mrb)             mrb_vm_lock_destroy((mrb))
#define MRB_VM_HEAP_RDLOCK(mrb)              RWLOCK_RDLOCK((mrb), (mrb)->lock_heap)
#define MRB_VM_HEAP_RDLOCK_AND_DEFINE(mrb)   RWLOCK_RDLOCK_AND_DEFINE((mrb), (mrb)->lock_heap)
#define MRB_VM_HEAP_WRLOCK(mrb)              RWLOCK_WRLOCK((mrb), (mrb)->lock_heap)
#define MRB_VM_HEAP_WRLOCK_AND_DEFINE(mrb)   RWLOCK_WRLOCK_AND_DEFINE((mrb), (mrb)->lock_heap)
#define MRB_VM_HEAP_UNLOCK(mrb)              RWLOCK_UNLOCK((mrb), (mrb)->lock_heap)
#define MRB_VM_HEAP_UNLOCK_IF_LOCKED(mrb)    RWLOCK_UNLOCK_IF_LOCKED((mrb), (mrb)->lock_heap)
#define MRB_VM_SYMTBL_RDLOCK(mrb)            RWLOCK_RDLOCK((mrb), (mrb)->lock_symtbl)
#define MRB_VM_SYMTBL_RDLOCK_AND_DEFINE(mrb) RWLOCK_RDLOCK_AND_DEFINE((mrb), (mrb)->lock_symtbl)
#define MRB_VM_SYMTBL_WRLOCK(mrb)            RWLOCK_WRLOCK((mrb), (mrb)->lock_symtbl)
#define MRB_VM_SYMTBL_WRLOCK_AND_DEFINE(mrb) RWLOCK_WRLOCK_AND_DEFINE((mrb), (mrb)->lock_symtbl)
#define MRB_VM_SYMTBL_UNLOCK(mrb)            RWLOCK_UNLOCK((mrb), (mrb)->lock_symtbl)
#define MRB_VM_SYMTBL_UNLOCK_IF_LOCKED(mrb)  RWLOCK_UNLOCK_IF_LOCKED((mrb), (mrb)->lock_symtbl)
#else
#define MRB_VM_LOCK_INIT(mrb)                TRUE
#define MRB_VM_LOCK_DESTROY(mrb)
#define MRB_VM_HEAP_RDLOCK(mrb)
#define MRB_VM_HEAP_RDLOCK_AND_DEFINE(mrb)
#define MRB_VM_HEAP_WRLOCK(mrb)
#define MRB_VM_HEAP_WRLOCK_AND_DEFINE(mrb)
#define MRB_VM_HEAP_UNLOCK(mrb)
#define MRB_VM_HEAP_UNLOCK_IF_LOCKED(mrb)
#define MRB_VM_SYMTBL_RDLOCK(mrb)
#define MRB_VM_SYMTBL_RDLOCK_AND_DEFINE(mrb)
#define MRB_VM_SYMTBL_WRLOCK(mrb)
#define MRB_VM_SYMTBL_WRLOCK_AND_DEFINE(mrb)
#define MRB_VM_SYMTBL_UNLOCK(mrb)
#define MRB_VM_SYMTBL_UNLOCK_IF_LOCKED(mrb)
#endif

#ifdef ENABLE_THREAD
extern struct mrb_state *mrb_vm_get_thread_state(mrb_thread thread);
extern mrb_bool   mrb_vm_attach_thread(struct mrb_state *mrb, mrb_thread *thread);
extern void       mrb_vm_detach_thread(struct mrb_state *mrb, mrb_thread thread);
extern void       mrb_vm_thread_api_set(struct mrb_state *mrb, mrb_thread_api const *api);
extern void       mrb_vm_lock_api_set(struct mrb_state *mrb, mrb_thread_lock_api const *api);
#endif

#ifdef __cplusplus
}
#endif

#endif
