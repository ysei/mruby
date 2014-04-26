#ifndef MRUBY_THREAD_H
#define MRUBY_THREAD_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ENABLE_THREAD

#ifdef USE_POSIX_THREAD
#include <pthread.h>
#endif

#ifdef USE_WIN32_THREAD
#include <windows.h>
#endif


/*
http://lists.freebsd.org/pipermail/freebsd-chromium/2011-April/000127.html

[SVN-Commit] r544 - in branches/experimental/www/firefox-devel: . files
Pan Tsu inyaoo at gmail.com 
Fri Apr 29 01:11:33 UTC 2011 

svn-freebsd-gecko at chruetertee.ch writes:

> Author: flo
> Date: Thu Apr 28 23:19:55 2011
> New Revision: 544
>
> Log:
> - add a new firefox-devel port which contains a snapshot of the Mozilla Firefox
>   Aurora Channel. Mozilla does not provide source tar balls for this channel
>   we will create snapshots on a regular basis.

> - enable ipc, based on a patch by Pan Tsu <inyaoo at gmail.com>
[...]
>  #elif defined(OS_LINUX)
> -  return syscall(__NR_gettid);
> +  // TODO(BSD): find a better thread ID
> +  return reinterpret_cast<int64>(pthread_self());
>  #endif

Why not use undocumented thr_self(2) syscall like emulators/wine does?

  $ ./test
  pthread_self() = 159413248
  thr_self() = 101107
  pthread_getthreadid_np() = 101107
  procstat -t = 101107

  // wine-1.3.18/dlls/ntdll/server.c
  static int get_unix_tid(void)
  {
      int ret = -1;
  #ifdef linux
      ret = syscall( SYS_gettid );
  #elif defined(__sun)
      ret = pthread_self();
  #elif defined(__APPLE__)
      ret = mach_thread_self();
      mach_port_deallocate(mach_task_self(), ret);
  #elif defined(__FreeBSD__)
      long lwpid;
      thr_self( &lwpid );
      ret = lwpid;
  #endif
      return ret;
  }

%%
Index: www/firefox-devel/files/patch-ipc-chromium-src-base-platform_thread_posix.cc
===================================================================
--- www/firefox-devel/files/patch-ipc-chromium-src-base-platform_thread_posix.cc	(revision 544)
+++ www/firefox-devel/files/patch-ipc-chromium-src-base-platform_thread_posix.cc	(working copy)
@@ -1,12 +1,25 @@
 --- ipc/chromium/src/base/platform_thread_posix.cc.orig	2011-04-27 09:34:28.000000000 +0200
 +++ ipc/chromium/src/base/platform_thread_posix.cc	2011-04-27 19:47:36.344446266 +0200
-@@ -34,7 +33,8 @@
+@@ -10,6 +10,7 @@
+ #include <mach/mach.h>
+ #elif defined(OS_LINUX)
+ #include <sys/syscall.h>
++#include <pthread_np.h>
+ #include <unistd.h>
+ #endif
+ 
+@@ -34,7 +33,13 @@ PlatformThreadId PlatformThread::Current
  #if defined(OS_MACOSX)
    return mach_thread_self();
  #elif defined(OS_LINUX)
 -  return syscall(__NR_gettid);
-+  // TODO(BSD): find a better thread ID
-+  return reinterpret_cast<int64>(pthread_self());
++  long lwpid;
++#if __FreeBSD_version < 900031
++  thr_self(&lwpid);
++#else
++  lwpid = pthread_getthreadid_np();
++#endif
++  return lwpid;
  #endif
  }
  
%%
*/

#include <sys/thr.h>	/* FreeBSD */


typedef union mrb_gem_thread_t {
  void * handle;
#ifdef USE_POSIX_THREAD
  pthread_t thread;
#endif
#ifdef USE_WIN32_THREAD
  HANDLE thread;
#endif
} mrb_gem_thread_t;

typedef union mrb_gem_lock_t {
  void *lock;
#ifdef USE_POSIX_THREAD
  pthread_mutex_t mutex;
#endif
#ifdef USE_WIN32_THREAD
  CRITICAL_SECTION cs;
#endif
} mrb_gem_lock_t;

#define MRB_GEM_THREAD_INITIALIZER ((mrb_gem_thread_t){ NULL })
#define MRB_GEM_LOCK_INITIALIZER   ((mrb_gem_lock_t)NULL)

#define MRB_GEM_THREAD_INVALID     ((mrb_gem_thread_t){ NULL })

/**
 * Type of thread
 */
typedef struct mrb_thread_t {
  struct mrb_state   *mrb;
  mrb_gem_thread_t    thread;
  mrb_value           retval;
} mrb_thread_t;

#define MRB_THREAD_INITIALIZER ((mrb_thread_t){ NULL, NULL, mrb_nil_value() })

/**
 * Type of thread table
 */
typedef struct mrb_thread_table_t {
  size_t        capacity;
  size_t        count;
  mrb_thread_t *threads;
} mrb_thread_table_t;

/**
 * RWLock structure type
 */
typedef struct mrb_lock_t {
  mrb_gem_lock_t    lock;
} mrb_lock_t;

#define MRB_LOCK_INVALID ((mrb_lock_t){ .lock = MRB_GEM_LOCK_INITIALIZER })

/**
 * Table of Lock APIs
 */
typedef struct mrb_thread_lock_api {
  int  (*lock_init)(struct mrb_state *mrb, mrb_lock_t *lock);
  int  (*lock_destroy)(struct mrb_state *mrb, mrb_lock_t *lock);
  int  (*lock_lock)(struct mrb_state *mrb, mrb_lock_t *lock, uint32_t timeout_ms);
  int  (*lock_unlock)(struct mrb_state *mrb, mrb_lock_t *lock);
  void (*lock_deadlock_handler)(struct mrb_state *mrb, mrb_lock_t *lock);
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
extern int mrb_lock_init(struct mrb_state *mrb, mrb_lock_t *lock);
extern int mrb_lock_destroy(struct mrb_state *mrb, mrb_lock_t *lock);
extern int mrb_lock_lock(struct mrb_state *mrb, mrb_lock_t *lock);
extern int mrb_lock_unlock(struct mrb_state *mrb, mrb_lock_t *lock);
#include <unistd.h>
#include <sys/syscall.h>


/*
static inline pid_t gettid() { return syscall(SYS_gettid); }
static inline pid_t gettid() { return syscall((size_t)thr_self); }
static inline pid_t gettid() { return (size_t)thr_self; }
static inline pid_t gettid() { uint64_t thptr; uint64_t volatile thptr128; thr_self(&thptr); return thptr; }
static inline pid_t gettid() { long thptr; long volatile thptr64; uint64_t volatile thptr128; (long)thr_self(&thptr); return thptr; }
static inline pid_t gettid() { long volatile thptr; long volatile thptr64; uint64_t volatile thptr128; thr_self(&thptr); return thptr; }
*/
static inline pid_t gettid() { long volatile thptr; long volatile thptr64; uint64_t volatile thptr128; (unsigned)thr_self(&thptr); return thptr; }


#define MRB_LOCK_INIT(mrb, lock)               mrb_lock_init(mrb, lock)
#define MRB_LOCK_DESTROY(mrb, lock)            mrb_lock_destroy(mrb, lock)
#define MRB_LOCK_LOCK(mrb, lock)               (var_lock_status_ = mrb_lock_lock(mrb, lock))
#define MRB_LOCK_UNLOCK(mrb, lock)             mrb_lock_unlock(mrb, lock)
#define MRB_LOCK_LOCK_AND_DEFINE(mrb, lock)    int var_lock_status_ = MRB_LOCK_LOCK(mrb, lock)
#define MRB_LOCK_UNLOCK_IF_LOCKED(mrb, lock)   ((var_lock_status_ == MRB_LOCK_STATUS_OK) ? MRB_LOCK_UNLOCK(mrb, lock) : var_lock_status_)
#else
#define MRB_LOCK_INIT(mrb, lock)               TRUE
#define MRB_LOCK_DESTROY(mrb, lock)            MRB_LOCK_STATUS_OK
#define MRB_LOCK_LOCK(mrb, lock)               MRB_LOCK_STATUS_OK
#define MRB_LOCK_UNLOCK(mrb, lock)             MRB_LOCK_STATUS_OK
#define MRB_LOCK_LOCK_AND_DEFINE(mrb, lock)
#define MRB_LOCK_UNLOCK_IF_LOCKED(mrb, lock)
#endif

#ifdef ENABLE_THREAD
extern void mrb_default_deadlock_handler(struct mrb_state *mrb, mrb_lock_t *lock);
#endif

#define MRB_LOCK_DEADLOCK_DETECTION_TIMEOUT 3000
#define MRB_LOCK_STATUS_OK                 0
#define MRB_LOCK_STATUS_NOT_SUPPORTED      1
#define MRB_LOCK_STATUS_TIMEOUT            2
#define MRB_LOCK_STATUS_INVALID_ARGUMENTS  3
#define MRB_LOCK_STATUS_ALREADY_LOCKED     4
#define MRB_LOCK_STATUS_UNKNOWN           99

#ifdef ENABLE_THREAD
extern mrb_gem_thread_t mrb_thread_get_self_invoke(struct mrb_state *mrb);
extern int              mrb_thread_equals_invoke(struct mrb_state *mrb, mrb_gem_thread_t t1, mrb_gem_thread_t t2);
extern mrb_value        mrb_thread_join_invoke(struct mrb_state *mrb, mrb_gem_thread_t t);
extern void             mrb_thread_free_invoke(struct mrb_state *mrb, mrb_gem_thread_t t);
#define MRB_THREAD_GET_SELF(mrb)       mrb_thread_get_self_invoke(mrb)
#define MRB_THREAD_EQUALS(mrb, t1, t2) mrb_thread_equals_invoke(mrb, t1, t2)
#define MRB_THREAD_JOIN(mrb, t)        mrb_thread_join_invoke(mrb, t)
#define MRB_THREAD_FREE(mrb, t)        mrb_thread_free_invoke(mrb, t)
#else
#define MRB_THREAD_GET_SELF(mrb)       ((void)0)
#define MRB_THREAD_EQUALS(mrb, t1, t2) ((void)0)
#define MRB_THREAD_JOIN(mrb, t)        ((void)0)
#define MRB_THREAD_FREE(mrb, t)        ((void)0)
#endif

#ifdef ENABLE_THREAD
#define MRB_VM_THREAD_TABLE            mrb_thread_table_t *thread_table
#define MRB_VM_THREAD_APIs             mrb_thread_api thread_api
#define MRB_VM_THREAD_LOCK_APIs        mrb_thread_lock_api thread_lock_api
#define MRB_DEFINE_LOCK_FIELD(name)    mrb_lock_t name
#define MRB_DEFINE_LOCKPTR_FIELD(name) mrb_lock_t *name
#else
#define MRB_VM_THREAD_TABLE
#define MRB_VM_THREAD_APIs
#define MRB_VM_THREAD_LOCK_APIs
#define MRB_DEFINE_LOCK_FIELD(name)
#endif

#define MRB_VM_THREAD_DEFAULT_CAPACITY 32

#define MRB_VM_LOCK_THREAD        MRB_DEFINE_LOCKPTR_FIELD(lock_thread)
#define MRB_VM_LOCK_HEAP          MRB_DEFINE_LOCKPTR_FIELD(lock_heap)
#define MRB_VM_LOCK_SYMTBL        MRB_DEFINE_LOCKPTR_FIELD(lock_symtbl)
#define MRB_VM_LOCK_GC            MRB_DEFINE_LOCKPTR_FIELD(lock_gc)

#ifdef ENABLE_THREAD
extern mrb_bool mrb_vm_lock_init(struct mrb_state *mrb);
extern void     mrb_vm_lock_destroy(struct mrb_state *mrb);
#define MRB_VM_LOCK_INIT(mrb)                mrb_vm_lock_init((mrb))
#define MRB_VM_LOCK_DESTROY(mrb)             mrb_vm_lock_destroy((mrb))
#define MRB_VM_HEAP_LOCK(mrb)                MRB_LOCK_LOCK((mrb), (mrb)->lock_heap)
#define MRB_VM_HEAP_LOCK_AND_DEFINE(mrb)     MRB_LOCK_LOCK_AND_DEFINE((mrb), (mrb)->lock_heap)
#define MRB_VM_HEAP_UNLOCK(mrb)              MRB_LOCK_UNLOCK((mrb), (mrb)->lock_heap)
#define MRB_VM_HEAP_UNLOCK_IF_LOCKED(mrb)    MRB_LOCK_UNLOCK_IF_LOCKED((mrb), (mrb)->lock_heap)
#define MRB_VM_SYMTBL_LOCK(mrb)              MRB_LOCK_LOCK((mrb), (mrb)->lock_symtbl)
#define MRB_VM_SYMTBL_LOCK_AND_DEFINE(mrb)   MRB_LOCK_LOCK_AND_DEFINE((mrb), (mrb)->lock_symtbl)
#define MRB_VM_SYMTBL_UNLOCK(mrb)            MRB_LOCK_UNLOCK((mrb), (mrb)->lock_symtbl)
#define MRB_VM_SYMTBL_UNLOCK_IF_LOCKED(mrb)  MRB_LOCK_UNLOCK_IF_LOCKED((mrb), (mrb)->lock_symtbl)
#define MRB_VM_GC_LOCK(mrb)                  MRB_LOCK_LOCK((mrb), (mrb)->lock_gc)
#define MRB_VM_GC_LOCK_AND_DEFINE(mrb)       MRB_LOCK_LOCK_AND_DEFINE((mrb), (mrb)->lock_gc)
#define MRB_VM_GC_UNLOCK(mrb)                MRB_LOCK_UNLOCK((mrb), (mrb)->lock_gc)
#define MRB_VM_GC_UNLOCK_IF_LOCKED(mrb)      MRB_LOCK_UNLOCK_IF_LOCKED((mrb), (mrb)->lock_gc)
#else
#define MRB_VM_LOCK_INIT(mrb)                TRUE
#define MRB_VM_LOCK_DESTROY(mrb)
#define MRB_VM_HEAP_LOCK(mrb)
#define MRB_VM_HEAP_LOCK_AND_DEFINE(mrb)
#define MRB_VM_HEAP_UNLOCK(mrb)
#define MRB_VM_HEAP_UNLOCK_IF_LOCKED(mrb)
#define MRB_VM_SYMTBL_LOCK(mrb)
#define MRB_VM_SYMTBL_LOCK_AND_DEFINE(mrb)
#define MRB_VM_SYMTBL_UNLOCK(mrb)
#define MRB_VM_SYMTBL_UNLOCK_IF_LOCKED(mrb)
#define MRB_VM_GC_LOCK(mrb)
#define MRB_VM_GC_LOCK_AND_DEFINE(mrb)
#define MRB_VM_GC_UNLOCK(mrb)
#define MRB_VM_GC_UNLOCK_IF_LOCKED(mrb)
#endif

#ifdef ENABLE_THREAD
extern struct mrb_state *mrb_vm_get_thread_state(mrb_thread_t *thread);
extern mrb_bool   mrb_vm_attach_thread(struct mrb_state *mrb, mrb_thread_t *thread);
extern mrb_value  mrb_vm_detach_thread(struct mrb_state *mrb, mrb_thread_t *thread);
extern void       mrb_vm_thread_api_set(struct mrb_state *mrb, mrb_thread_api const *api);
extern void       mrb_vm_lock_api_set(struct mrb_state *mrb, mrb_thread_lock_api const *api);
#endif

#ifdef __cplusplus
}
#endif

#endif
