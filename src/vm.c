/*
** vm.c - virtual machine for mruby
**
** See Copyright Notice in mruby.h
*/

#include <stddef.h>
#include <stdarg.h>
#include <math.h>
#include "mruby.h"
#include "mruby/array.h"
#include "mruby/class.h"
#include "mruby/hash.h"
#include "mruby/irep.h"
#include "mruby/numeric.h"
#include "mruby/proc.h"
#include "mruby/range.h"
#include "mruby/string.h"
#include "mruby/variable.h"
#include "mruby/error.h"
#include "mruby/opcode.h"
#include "mruby/gvl.h"
#include "value_array.h"
#include "mrb_throw.h"

#ifndef ENABLE_STDIO
#if defined(__cplusplus)
extern "C" {
#endif
void abort(void);
#if defined(__cplusplus)
}  /* extern "C" { */
#endif
#endif

#define STACK_INIT_SIZE 128
#define CALLINFO_INIT_SIZE 32

/* Define amount of linear stack growth. */
#ifndef MRB_STACK_GROWTH
#define MRB_STACK_GROWTH 128
#endif

/* Maximum stack depth. Should be set lower on memory constrained systems.
The value below allows about 60000 recursive calls in the simplest case. */
#ifndef MRB_STACK_MAX
#define MRB_STACK_MAX (0x40000 - MRB_STACK_GROWTH)
#endif

#ifdef VM_DEBUG
# define DEBUG(x) (x)
#else
# define DEBUG(x)
#endif

#define ARENA_RESTORE(mrb,ai) MRB_GET_THREAD_CONTEXT(mrb)->arena_idx = (ai)

static inline void
stack_clear(mrb_value *from, size_t count)
{
#ifndef MRB_NAN_BOXING
  const mrb_value mrb_value_zero = { { 0 } };

  while (count-- > 0) {
    *from++ = mrb_value_zero;
  }
#else
  while (count-- > 0) {
    SET_NIL_VALUE(*from);
    from++;
  }
#endif
}

static inline void
stack_copy(mrb_value *dst, const mrb_value *src, size_t size)
{
  while (size-- > 0) {
    *dst++ = *src++;
  }
}

static void
stack_init(mrb_state *mrb)
{
  struct mrb_context *c = MRB_GET_CONTEXT(mrb);

  /* mrb_assert(MRB_GET_VM(mrb)->stack == NULL); */
  c->stbase = (mrb_value *)mrb_calloc(mrb, STACK_INIT_SIZE, sizeof(mrb_value));
  c->stend = c->stbase + STACK_INIT_SIZE;
  c->stack = c->stbase;

  /* mrb_assert(ci == NULL); */
  c->cibase = (mrb_callinfo *)mrb_calloc(mrb, CALLINFO_INIT_SIZE, sizeof(mrb_callinfo));
  c->ciend = c->cibase + CALLINFO_INIT_SIZE;
  c->ci = c->cibase;
  c->ci->target_class = MRB_GET_VM(mrb)->object_class;
  c->ci->stackent = c->stack;
}

static inline void
envadjust(mrb_state *mrb, mrb_value *oldbase, mrb_value *newbase)
{
  mrb_callinfo *ci = MRB_GET_CONTEXT(mrb)->cibase;

  if (newbase == oldbase) return;
  while (ci <= MRB_GET_CONTEXT(mrb)->ci) {
    struct REnv *e = ci->env;
    if (e && MRB_ENV_STACK_SHARED_P(e)) {
      ptrdiff_t off = e->stack - oldbase;

      e->stack = newbase + off;
    }
    ci->stackent = newbase + (ci->stackent - oldbase);
    ci++;
  }
}

static inline void
init_new_stack_space(mrb_state *mrb, int room, int keep)
{
  if (room > keep) {
    /* do not leave uninitialized malloc region */
    stack_clear(&(MRB_GET_CONTEXT(mrb)->stack[keep]), room - keep);
  }
}

/** def rec ; $deep =+ 1 ; if $deep > 1000 ; return 0 ; end ; rec ; end  */

static void
stack_extend_alloc(mrb_state *mrb, int room, int keep)
{
  mrb_value *oldbase = MRB_GET_CONTEXT(mrb)->stbase;
  int size = MRB_GET_CONTEXT(mrb)->stend - MRB_GET_CONTEXT(mrb)->stbase;
  int off = MRB_GET_CONTEXT(mrb)->stack - MRB_GET_CONTEXT(mrb)->stbase;

#ifdef MRB_STACK_EXTEND_DOUBLING
  if (room <= size)
    size *= 2;
  else
    size += room;
#else
  /* Use linear stack growth.
     It is slightly slower than doubling the stack space,
     but it saves memory on small devices. */
  if (room <= MRB_STACK_GROWTH)
    size += MRB_STACK_GROWTH;
  else
    size += room;
#endif

  MRB_GET_CONTEXT(mrb)->stbase = (mrb_value *)mrb_realloc(mrb, MRB_GET_CONTEXT(mrb)->stbase, sizeof(mrb_value) * size);
  MRB_GET_CONTEXT(mrb)->stack = MRB_GET_CONTEXT(mrb)->stbase + off;
  MRB_GET_CONTEXT(mrb)->stend = MRB_GET_CONTEXT(mrb)->stbase + size;
  envadjust(mrb, oldbase, MRB_GET_CONTEXT(mrb)->stbase);

  /* Raise an exception if the new stack size will be too large,
     to prevent infinite recursion. However, do this only after resizing the stack, so mrb_raise has stack space to work with. */
  if (size > MRB_STACK_MAX) {
    init_new_stack_space(mrb, room, keep);
    mrb_raise(mrb, E_SYSSTACK_ERROR, "stack level too deep. (limit=" MRB_STRINGIZE(MRB_STACK_MAX) ")");
  }
}

static inline void
stack_extend(mrb_state *mrb, int room, int keep)
{
  if (MRB_GET_CONTEXT(mrb)->stack + room >= MRB_GET_CONTEXT(mrb)->stend) {
    stack_extend_alloc(mrb, room, keep);
  }
  init_new_stack_space(mrb, room, keep);
}

static inline struct REnv*
uvenv(mrb_state *mrb, int up)
{
  struct REnv *e = MRB_GET_CONTEXT(mrb)->ci->proc->env;

  while (up--) {
    if (!e) return NULL;
    e = (struct REnv*)e->c;
  }
  return e;
}

static inline mrb_bool
is_strict(mrb_state *mrb, struct REnv *e)
{
  int cioff = e->cioff;

  if (MRB_ENV_STACK_SHARED_P(e) && MRB_GET_CONTEXT(mrb)->cibase[cioff].proc &&
      MRB_PROC_STRICT_P(MRB_GET_CONTEXT(mrb)->cibase[cioff].proc)) {
    return TRUE;
  }
  return FALSE;
}

static inline struct REnv*
top_env(mrb_state *mrb, struct RProc *proc)
{
  struct REnv *e = proc->env;

  if (is_strict(mrb, e)) return e;
  while (e->c) {
    e = (struct REnv*)e->c;
    if (is_strict(mrb, e)) return e;
  }
  return e;
}

#define CI_ACC_SKIP    -1
#define CI_ACC_DIRECT  -2

static mrb_callinfo*
cipush(mrb_state *mrb)
{
  struct mrb_context *c = MRB_GET_CONTEXT(mrb);
  mrb_callinfo *ci = c->ci;

  int eidx = ci->eidx;
  int ridx = ci->ridx;

  if (ci + 1 == c->ciend) {
    size_t size = ci - c->cibase;

    c->cibase = (mrb_callinfo *)mrb_realloc(mrb, c->cibase, sizeof(mrb_callinfo)*size*2);
    c->ci = c->cibase + size;
    c->ciend = c->cibase + size * 2;
  }
  ci = ++c->ci;
  ci->eidx = eidx;
  ci->ridx = ridx;
  ci->env = 0;
  ci->pc = 0;
  ci->err = 0;
  ci->proc = 0;

  return ci;
}

static void
cipop(mrb_state *mrb)
{
  struct mrb_context *c = MRB_GET_CONTEXT(mrb);

  if (c->ci->env) {
    struct REnv *e = c->ci->env;
    size_t len = (size_t)MRB_ENV_STACK_LEN(e);
    mrb_value *p = (mrb_value *)mrb_malloc(mrb, sizeof(mrb_value)*len);

    MRB_ENV_UNSHARE_STACK(e);
    if (len > 0) {
      stack_copy(p, e->stack, len);
    }
    e->stack = p;
    mrb_write_barrier(mrb, (struct RBasic *)e);
  }

  c->ci--;
}

static void
ecall(mrb_state *mrb, int i)
{
  struct RProc *p;
  mrb_callinfo *ci;
  mrb_value *self = MRB_GET_CONTEXT(mrb)->stack;
  struct RObject *exc;

  p = MRB_GET_CONTEXT(mrb)->ensure[i];
  if (!p) return;
  if (MRB_GET_CONTEXT(mrb)->ci->eidx > i)
    MRB_GET_CONTEXT(mrb)->ci->eidx = i;
  ci = cipush(mrb);
  ci->stackent = MRB_GET_CONTEXT(mrb)->stack;
  ci->mid = ci[-1].mid;
  ci->acc = CI_ACC_SKIP;
  ci->argc = 0;
  ci->proc = p;
  ci->nregs = p->body.irep->nregs;
  ci->target_class = p->target_class;
  MRB_GET_CONTEXT(mrb)->stack = MRB_GET_CONTEXT(mrb)->stack + ci[-1].nregs;
  exc = MRB_GET_VM(mrb)->exc; MRB_GET_VM(mrb)->exc = 0;
  mrb_run(mrb, p, *self);
  MRB_GET_CONTEXT(mrb)->ensure[i] = NULL;
  if (!MRB_GET_VM(mrb)->exc) MRB_GET_VM(mrb)->exc = exc;
}

#ifndef MRB_FUNCALL_ARGC_MAX
#define MRB_FUNCALL_ARGC_MAX 16
#endif

MRB_API mrb_value
mrb_funcall(mrb_state *mrb, mrb_value self, const char *name, mrb_int argc, ...)
{
  mrb_value argv[MRB_FUNCALL_ARGC_MAX];
  va_list ap;
  mrb_int i;
  mrb_sym mid;
  mrb_value val;
#ifdef MRB_USE_GVL_API
  const mrb_bool is_gvl_acquired = mrb_gvl_is_acquired(mrb);

  if (!is_gvl_acquired) {
    mrb_gvl_acquire(mrb);
  }
#endif

  mid = mrb_intern_cstr(mrb, name);

  if (argc > MRB_FUNCALL_ARGC_MAX) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Too long arguments. (limit=" MRB_STRINGIZE(MRB_FUNCALL_ARGC_MAX) ")");
  }

  va_start(ap, argc);
  for (i = 0; i < argc; i++) {
    argv[i] = va_arg(ap, mrb_value);
  }
  va_end(ap);

  val = mrb_funcall_argv(mrb, self, mid, argc, argv);

#ifdef MRB_USE_GVL_API
  if (!is_gvl_acquired) {
    mrb_gvl_release(mrb);
  }
#endif
  return val;
}

MRB_API mrb_value
mrb_funcall_with_block(mrb_state *mrb, mrb_value self, mrb_sym mid, mrb_int argc, const mrb_value *argv, mrb_value blk)
{
  mrb_value val;
#ifdef MRB_USE_GVL_API
  const mrb_bool is_gvl_acquired = mrb_gvl_is_acquired(mrb);

  if (!is_gvl_acquired) {
    mrb_gvl_acquire(mrb);
  }
#endif

  if (!MRB_GET_THREAD_CONTEXT(mrb)->jmp) {
    struct mrb_jmpbuf c_jmp;
    mrb_callinfo *old_ci = MRB_GET_CONTEXT(mrb)->ci;

    MRB_TRY(&c_jmp) {
      MRB_GET_THREAD_CONTEXT(mrb)->jmp = &c_jmp;
      /* recursive call */
      val = mrb_funcall_with_block(mrb, self, mid, argc, argv, blk);
      MRB_GET_THREAD_CONTEXT(mrb)->jmp = 0;
    }
    MRB_CATCH(&c_jmp) { /* error */
      while (old_ci != MRB_GET_CONTEXT(mrb)->ci) {
        MRB_GET_CONTEXT(mrb)->stack = MRB_GET_CONTEXT(mrb)->ci->stackent;
        cipop(mrb);
      }
      MRB_GET_THREAD_CONTEXT(mrb)->jmp = 0;
      val = mrb_obj_value(MRB_GET_VM(mrb)->exc);
    }
    MRB_END_EXC(&c_jmp);
  }
  else {
    struct RProc *p;
    struct RClass *c;
    mrb_sym undef = 0;
    mrb_callinfo *ci;
    int n;

    if (!MRB_GET_CONTEXT(mrb)->stack) {
      stack_init(mrb);
    }
    n = MRB_GET_CONTEXT(mrb)->ci->nregs;
    if (argc < 0) {
      mrb_raisef(mrb, E_ARGUMENT_ERROR, "negative argc for funcall (%S)", mrb_fixnum_value(argc));
    }
    c = mrb_class(mrb, self);
    p = mrb_method_search_vm(mrb, &c, mid);
    if (!p) {
      undef = mid;
      mid = mrb_intern_lit(mrb, "method_missing");
      p = mrb_method_search_vm(mrb, &c, mid);
      n++; argc++;
    }
    ci = cipush(mrb);
    ci->mid = mid;
    ci->proc = p;
    ci->stackent = MRB_GET_CONTEXT(mrb)->stack;
    ci->argc = argc;
    ci->target_class = c;
    MRB_GET_CONTEXT(mrb)->stack = MRB_GET_CONTEXT(mrb)->stack + n;
    if (MRB_PROC_CFUNC_P(p)) {
      ci->nregs = argc + 2;
      stack_extend(mrb, ci->nregs, 0);
    }
    else {
      ci->nregs = p->body.irep->nregs + n;
      stack_extend(mrb, ci->nregs, argc+2);
    }
    MRB_GET_CONTEXT(mrb)->stack[0] = self;
    if (undef) {
      MRB_GET_CONTEXT(mrb)->stack[1] = mrb_symbol_value(undef);
      if (argc > 1) {
        stack_copy(MRB_GET_CONTEXT(mrb)->stack+2, argv, argc-1);
      }
    }
    else if (argc > 0) {
      stack_copy(MRB_GET_CONTEXT(mrb)->stack+1, argv, argc);
    }
    MRB_GET_CONTEXT(mrb)->stack[argc+1] = blk;

    if (MRB_PROC_CFUNC_P(p)) {
      int ai = mrb_gc_arena_save(mrb);

      ci->acc = CI_ACC_DIRECT;
      val = p->body.func(mrb, self);
      MRB_GET_CONTEXT(mrb)->stack = MRB_GET_CONTEXT(mrb)->ci->stackent;
      cipop(mrb);
      mrb_gc_arena_restore(mrb, ai);
    }
    else {
      ci->acc = CI_ACC_SKIP;
      val = mrb_run(mrb, p, self);
    }
  }
  mrb_gc_protect(mrb, val);
#ifdef MRB_USE_GVL_API
  if (!is_gvl_acquired) {
    mrb_gvl_release(mrb);
  }
#endif
  return val;
}

MRB_API mrb_value
mrb_funcall_argv(mrb_state *mrb, mrb_value self, mrb_sym mid, mrb_int argc, const mrb_value *argv)
{
  return mrb_funcall_with_block(mrb, self, mid, argc, argv, mrb_nil_value());
}

/* 15.3.1.3.4  */
/* 15.3.1.3.44 */
/*
 *  call-seq:
 *     obj.send(symbol [, args...])        -> obj
 *     obj.__send__(symbol [, args...])      -> obj
 *
 *  Invokes the method identified by _symbol_, passing it any
 *  arguments specified. You can use <code>__send__</code> if the name
 *  +send+ clashes with an existing method in _obj_.
 *
 *     class Klass
 *       def hello(*args)
 *         "Hello " + args.join(' ')
 *       end
 *     end
 *     k = Klass.new
 *     k.send :hello, "gentle", "readers"   #=> "Hello gentle readers"
 */
MRB_API mrb_value
mrb_f_send(mrb_state *mrb, mrb_value self)
{
  mrb_sym name;
  mrb_value block, *argv, *regs;
  mrb_int argc, i, len;
  struct RProc *p;
  struct RClass *c;
  mrb_callinfo *ci;

  mrb_get_args(mrb, "n*&", &name, &argv, &argc, &block);

  c = mrb_class(mrb, self);
  p = mrb_method_search_vm(mrb, &c, name);

  if (!p) {                     /* call method_mising */
    return mrb_funcall_with_block(mrb, self, name, argc, argv, block);
  }

  ci = MRB_GET_CONTEXT(mrb)->ci;
  ci->mid = name;
  ci->target_class = c;
  ci->proc = p;
  regs = MRB_GET_CONTEXT(mrb)->stack+1;
  /* remove first symbol from arguments */
  if (ci->argc >= 0) {
    for (i=0,len=ci->argc; i<len; i++) {
      regs[i] = regs[i+1];
    }
    ci->argc--;
  }
  else {                     /* variable length arguments */
    mrb_ary_shift(mrb, regs[0]);
  }

  if (MRB_PROC_CFUNC_P(p)) {
    mrb_value ret;
    ret = p->body.func(mrb, self);
    return ret;
  }

  ci->nregs = p->body.irep->nregs;
  ci = cipush(mrb);
  ci->nregs = 0;
  ci->target_class = 0;
  ci->pc = p->body.irep->iseq;
  ci->stackent = MRB_GET_CONTEXT(mrb)->stack;
  ci->acc = 0;

  return self;
}

static mrb_value
eval_under(mrb_state *mrb, mrb_value self, mrb_value blk, struct RClass *c)
{
  struct RProc *p;
  mrb_callinfo *ci;

  if (mrb_nil_p(blk)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  }
  ci = MRB_GET_CONTEXT(mrb)->ci;
  if (ci->acc == CI_ACC_DIRECT) {
    return mrb_yield_with_class(mrb, blk, 0, 0, self, c);
  }
  ci->target_class = c;
  p = mrb_proc_ptr(blk);
  ci->proc = p;
  if (MRB_PROC_CFUNC_P(p)) {
    mrb_value ret;
    ret = p->body.func(mrb, self);
    return ret;
  }
  ci->nregs = p->body.irep->nregs;
  ci = cipush(mrb);
  ci->nregs = 0;
  ci->target_class = 0;
  ci->pc = p->body.irep->iseq;
  ci->stackent = MRB_GET_CONTEXT(mrb)->stack;
  ci->acc = 0;

  return self;
}

/* 15.2.2.4.35 */
/*
 *  call-seq:
 *     mod.class_eval {| | block }  -> obj
 *     mod.module_eval {| | block } -> obj
 *
 *  Evaluates block in the context of _mod_. This can
 *  be used to add methods to a class. <code>module_eval</code> returns
 *  the result of evaluating its argument.
 */
mrb_value
mrb_mod_module_eval(mrb_state *mrb, mrb_value mod)
{
  mrb_value a, b;

  if (mrb_get_args(mrb, "|S&", &a, &b) == 1) {
    mrb_raise(mrb, E_NOTIMP_ERROR, "module_eval/class_eval with string not implemented");
  }
  return eval_under(mrb, mod, b, mrb_class_ptr(mod));
}

/* 15.3.1.3.18 */
/*
 *  call-seq:
 *     obj.instance_eval {| | block }                       -> obj
 *
 *  Evaluates the given block,within  the context of the receiver (_obj_).
 *  In order to set the context, the variable +self+ is set to _obj_ while
 *  the code is executing, giving the code access to _obj_'s
 *  instance variables. In the version of <code>instance_eval</code>
 *  that takes a +String+, the optional second and third
 *  parameters supply a filename and starting line number that are used
 *  when reporting compilation errors.
 *
 *     class KlassWithSecret
 *       def initialize
 *         @secret = 99
 *       end
 *     end
 *     k = KlassWithSecret.new
 *     k.instance_eval { @secret }   #=> 99
 */
mrb_value
mrb_obj_instance_eval(mrb_state *mrb, mrb_value self)
{
  mrb_value a, b;
  mrb_value cv;
  struct RClass *c;

  if (mrb_get_args(mrb, "|S&", &a, &b) == 1) {
    mrb_raise(mrb, E_NOTIMP_ERROR, "instance_eval with string not implemented");
  }
  switch (mrb_type(self)) {
  case MRB_TT_SYMBOL:
  case MRB_TT_FIXNUM:
  case MRB_TT_FLOAT:
    c = 0;
    break;
  default:
    cv = mrb_singleton_class(mrb, self);
    c = mrb_class_ptr(cv);
    break;
  }
  return eval_under(mrb, self, b, c);
}

MRB_API mrb_value
mrb_yield_with_class(mrb_state *mrb, mrb_value b, mrb_int argc, const mrb_value *argv, mrb_value self, struct RClass *c)
{
  struct RProc *p;
  mrb_sym mid;
  mrb_callinfo *ci;
  int n;
  mrb_value val;
#ifdef MRB_USE_GVL_API
  const mrb_bool is_gvl_acquired = mrb_gvl_is_acquired(mrb);
#endif

  if (!MRB_GET_CONTEXT(mrb)->stack) {
    stack_init(mrb);
  }

  mid = MRB_GET_CONTEXT(mrb)->ci->mid;
  n = MRB_GET_CONTEXT(mrb)->ci->nregs;

  if (mrb_nil_p(b)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  }
  p = mrb_proc_ptr(b);
  ci = cipush(mrb);
  ci->mid = mid;
  ci->proc = p;
  ci->stackent = MRB_GET_CONTEXT(mrb)->stack;
  ci->argc = argc;
  ci->target_class = c;
  ci->acc = CI_ACC_SKIP;
  MRB_GET_CONTEXT(mrb)->stack = MRB_GET_CONTEXT(mrb)->stack + n;
  if (MRB_PROC_CFUNC_P(p)) {
    ci->nregs = argc + 2;
    stack_extend(mrb, ci->nregs, 0);
  }
  else {
    ci->nregs = p->body.irep->nregs;
    stack_extend(mrb, ci->nregs, argc+2);
  }

  MRB_GET_CONTEXT(mrb)->stack[0] = self;
  if (argc > 0) {
    stack_copy(MRB_GET_CONTEXT(mrb)->stack+1, argv, argc);
  }
  MRB_GET_CONTEXT(mrb)->stack[argc+1] = mrb_nil_value();

#ifdef MRB_USE_GVL_API
  if (!is_gvl_acquired) {
    mrb_gvl_acquire(mrb);
  }
#endif

  if (MRB_PROC_CFUNC_P(p)) {
    val = p->body.func(mrb, self);
    MRB_GET_CONTEXT(mrb)->stack = MRB_GET_CONTEXT(mrb)->ci->stackent;
    cipop(mrb);
  }
  else {
    val = mrb_run(mrb, p, self);
  }

#ifdef MRB_USE_GVL_API
  if (!is_gvl_acquired) {
    mrb_gvl_release(mrb);
  }
#endif
  return val;
}

MRB_API mrb_value
mrb_yield_argv(mrb_state *mrb, mrb_value b, mrb_int argc, const mrb_value *argv)
{
  struct RProc *p = mrb_proc_ptr(b);

  return mrb_yield_with_class(mrb, b, argc, argv, p->env->stack[0], p->target_class);
}

MRB_API mrb_value
mrb_yield(mrb_state *mrb, mrb_value b, mrb_value arg)
{
  struct RProc *p = mrb_proc_ptr(b);

  return mrb_yield_with_class(mrb, b, 1, &arg, p->env->stack[0], p->target_class);
}

typedef enum {
  LOCALJUMP_ERROR_RETURN = 0,
  LOCALJUMP_ERROR_BREAK = 1,
  LOCALJUMP_ERROR_YIELD = 2
} localjump_error_kind;

static void
localjump_error(mrb_state *mrb, localjump_error_kind kind)
{
  char kind_str[3][7] = { "return", "break", "yield" };
  char kind_str_len[] = { 6, 5, 5 };
  static const char lead[] = "unexpected ";
  mrb_value msg;
  mrb_value exc;

  msg = mrb_str_buf_new(mrb, sizeof(lead) + 7);
  mrb_str_cat(mrb, msg, lead, sizeof(lead) - 1);
  mrb_str_cat(mrb, msg, kind_str[kind], kind_str_len[kind]);
  exc = mrb_exc_new_str(mrb, E_LOCALJUMP_ERROR, msg);
  MRB_GET_VM(mrb)->exc = mrb_obj_ptr(exc);
}

static void
argnum_error(mrb_state *mrb, mrb_int num)
{
  mrb_value exc;
  mrb_value str;

  if (MRB_GET_CONTEXT(mrb)->ci->mid) {
    str = mrb_format(mrb, "'%S': wrong number of arguments (%S for %S)",
                  mrb_sym2str(mrb, MRB_GET_CONTEXT(mrb)->ci->mid),
                  mrb_fixnum_value(MRB_GET_CONTEXT(mrb)->ci->argc), mrb_fixnum_value(num));
  }
  else {
    str = mrb_format(mrb, "wrong number of arguments (%S for %S)",
                  mrb_fixnum_value(MRB_GET_CONTEXT(mrb)->ci->argc), mrb_fixnum_value(num));
  }
  exc = mrb_exc_new_str(mrb, E_ARGUMENT_ERROR, str);
  MRB_GET_VM(mrb)->exc = mrb_obj_ptr(exc);
}

#define ERR_PC_SET(mrb, pc) MRB_GET_CONTEXT(mrb)->ci->err = pc;
#define ERR_PC_CLR(mrb)     MRB_GET_CONTEXT(mrb)->ci->err = 0;
#ifdef ENABLE_DEBUG
#define CODE_FETCH_HOOK(mrb, irep, pc, regs) if (MRB_GET_VM(mrb)->code_fetch_hook) MRB_GET_VM(mrb)->code_fetch_hook((mrb), (irep), (pc), (regs));
#else
#define CODE_FETCH_HOOK(mrb, irep, pc, regs)
#endif

#if defined __GNUC__ || defined __clang__ || defined __INTEL_COMPILER
#define DIRECT_THREADED
#endif

#ifndef DIRECT_THREADED

#define INIT_DISPATCH for (;;) { i = *pc; CODE_FETCH_HOOK(mrb, irep, pc, regs); switch (GET_OPCODE(i)) {
#define CASE(op) case op:
#define NEXT pc++; break
#define JUMP break
#define END_DISPATCH }}

#else

#define INIT_DISPATCH JUMP; return mrb_nil_value();
#define CASE(op) L_ ## op:
#define NEXT i=*++pc; CODE_FETCH_HOOK(mrb, irep, pc, regs); goto *optable[GET_OPCODE(i)]
#define JUMP i=*pc; CODE_FETCH_HOOK(mrb, irep, pc, regs); goto *optable[GET_OPCODE(i)]

#define END_DISPATCH

#endif

mrb_value mrb_gv_val_get(mrb_state *mrb, mrb_sym sym);
void mrb_gv_val_set(mrb_state *mrb, mrb_sym sym, mrb_value val);

#define CALL_MAXARGS 127

MRB_API mrb_value
mrb_context_run(mrb_state *mrb, struct RProc *proc, mrb_value self, unsigned int stack_keep)
{
  /* mrb_assert(mrb_proc_cfunc_p(proc)) */
  mrb_irep *irep = proc->body.irep;
  mrb_code *pc = irep->iseq;
  mrb_value *pool = irep->pool;
  mrb_sym *syms = irep->syms;
  mrb_value *regs = NULL;
  mrb_code i;
  int ai = mrb_gc_arena_save(mrb);
  struct mrb_jmpbuf *prev_jmp = MRB_GET_THREAD_CONTEXT(mrb)->jmp;
  struct mrb_jmpbuf c_jmp;

#ifdef DIRECT_THREADED
  static void *optable[] = {
    &&L_OP_NOP, &&L_OP_MOVE,
    &&L_OP_LOADL, &&L_OP_LOADI, &&L_OP_LOADSYM, &&L_OP_LOADNIL,
    &&L_OP_LOADSELF, &&L_OP_LOADT, &&L_OP_LOADF,
    &&L_OP_GETGLOBAL, &&L_OP_SETGLOBAL, &&L_OP_GETSPECIAL, &&L_OP_SETSPECIAL,
    &&L_OP_GETIV, &&L_OP_SETIV, &&L_OP_GETCV, &&L_OP_SETCV,
    &&L_OP_GETCONST, &&L_OP_SETCONST, &&L_OP_GETMCNST, &&L_OP_SETMCNST,
    &&L_OP_GETUPVAR, &&L_OP_SETUPVAR,
    &&L_OP_JMP, &&L_OP_JMPIF, &&L_OP_JMPNOT,
    &&L_OP_ONERR, &&L_OP_RESCUE, &&L_OP_POPERR, &&L_OP_RAISE, &&L_OP_EPUSH, &&L_OP_EPOP,
    &&L_OP_SEND, &&L_OP_SENDB, &&L_OP_FSEND,
    &&L_OP_CALL, &&L_OP_SUPER, &&L_OP_ARGARY, &&L_OP_ENTER,
    &&L_OP_KARG, &&L_OP_KDICT, &&L_OP_RETURN, &&L_OP_TAILCALL, &&L_OP_BLKPUSH,
    &&L_OP_ADD, &&L_OP_ADDI, &&L_OP_SUB, &&L_OP_SUBI, &&L_OP_MUL, &&L_OP_DIV,
    &&L_OP_EQ, &&L_OP_LT, &&L_OP_LE, &&L_OP_GT, &&L_OP_GE,
    &&L_OP_ARRAY, &&L_OP_ARYCAT, &&L_OP_ARYPUSH, &&L_OP_AREF, &&L_OP_ASET, &&L_OP_APOST,
    &&L_OP_STRING, &&L_OP_STRCAT, &&L_OP_HASH,
    &&L_OP_LAMBDA, &&L_OP_RANGE, &&L_OP_OCLASS,
    &&L_OP_CLASS, &&L_OP_MODULE, &&L_OP_EXEC,
    &&L_OP_METHOD, &&L_OP_SCLASS, &&L_OP_TCLASS,
    &&L_OP_DEBUG, &&L_OP_STOP, &&L_OP_ERR,
  };
#endif

  mrb_bool exc_catched = FALSE;
#ifdef MRB_USE_GVL_API
  const mrb_bool is_gvl_acquired = mrb_gvl_is_acquired(mrb);
  if (!is_gvl_acquired) {
    mrb_gvl_acquire(mrb);
  }
#endif
RETRY_TRY_BLOCK:

  MRB_TRY(&c_jmp) {

  if (exc_catched) {
    exc_catched = FALSE;
    goto L_RAISE;
  }
  MRB_GET_THREAD_CONTEXT(mrb)->jmp = &c_jmp;
  if (!MRB_GET_CONTEXT(mrb)->stack) {
    stack_init(mrb);
  }
  stack_extend(mrb, irep->nregs, stack_keep);
  MRB_GET_CONTEXT(mrb)->ci->proc = proc;
  MRB_GET_CONTEXT(mrb)->ci->nregs = irep->nregs;
  regs = MRB_GET_CONTEXT(mrb)->stack;
  regs[0] = self;

  INIT_DISPATCH {
    CASE(OP_NOP) {
      /* do nothing */
      NEXT;
    }

    CASE(OP_MOVE) {
      /* A B    R(A) := R(B) */
      regs[GETARG_A(i)] = regs[GETARG_B(i)];
      NEXT;
    }

    CASE(OP_LOADL) {
      /* A Bx   R(A) := Pool(Bx) */
      regs[GETARG_A(i)] = pool[GETARG_Bx(i)];
      NEXT;
    }

    CASE(OP_LOADI) {
      /* A sBx  R(A) := sBx */
      SET_INT_VALUE(regs[GETARG_A(i)], GETARG_sBx(i));
      NEXT;
    }

    CASE(OP_LOADSYM) {
      /* A Bx   R(A) := Syms(Bx) */
      SET_SYM_VALUE(regs[GETARG_A(i)], syms[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_LOADSELF) {
      /* A      R(A) := self */
      regs[GETARG_A(i)] = regs[0];
      NEXT;
    }

    CASE(OP_LOADT) {
      /* A      R(A) := true */
      SET_TRUE_VALUE(regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_LOADF) {
      /* A      R(A) := false */
      SET_FALSE_VALUE(regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETGLOBAL) {
      /* A Bx   R(A) := getglobal(Syms(Bx)) */
      regs[GETARG_A(i)] = mrb_gv_get(mrb, syms[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_SETGLOBAL) {
      /* setglobal(Syms(Bx), R(A)) */
      mrb_gv_set(mrb, syms[GETARG_Bx(i)], regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETSPECIAL) {
      /* A Bx   R(A) := Special[Bx] */
      regs[GETARG_A(i)] = mrb_vm_special_get(mrb, GETARG_Bx(i));
      NEXT;
    }

    CASE(OP_SETSPECIAL) {
      /* A Bx   Special[Bx] := R(A) */
      mrb_vm_special_set(mrb, GETARG_Bx(i), regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETIV) {
      /* A Bx   R(A) := ivget(Bx) */
      regs[GETARG_A(i)] = mrb_vm_iv_get(mrb, syms[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_SETIV) {
      /* ivset(Syms(Bx),R(A)) */
      mrb_vm_iv_set(mrb, syms[GETARG_Bx(i)], regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETCV) {
      /* A Bx   R(A) := cvget(Syms(Bx)) */
      ERR_PC_SET(mrb, pc);
      regs[GETARG_A(i)] = mrb_vm_cv_get(mrb, syms[GETARG_Bx(i)]);
      ERR_PC_CLR(mrb);
      NEXT;
    }

    CASE(OP_SETCV) {
      /* cvset(Syms(Bx),R(A)) */
      mrb_vm_cv_set(mrb, syms[GETARG_Bx(i)], regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETCONST) {
      /* A Bx    R(A) := constget(Syms(Bx)) */
      mrb_value val;

      ERR_PC_SET(mrb, pc);
      val = mrb_vm_const_get(mrb, syms[GETARG_Bx(i)]);
      ERR_PC_CLR(mrb);
      regs = MRB_GET_CONTEXT(mrb)->stack;
      regs[GETARG_A(i)] = val;
      NEXT;
    }

    CASE(OP_SETCONST) {
      /* A Bx   constset(Syms(Bx),R(A)) */
      mrb_vm_const_set(mrb, syms[GETARG_Bx(i)], regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETMCNST) {
      /* A Bx   R(A) := R(A)::Syms(Bx) */
      mrb_value val;
      int a = GETARG_A(i);

      ERR_PC_SET(mrb, pc);
      val = mrb_const_get(mrb, regs[a], syms[GETARG_Bx(i)]);
      ERR_PC_CLR(mrb);
      regs = MRB_GET_CONTEXT(mrb)->stack;
      regs[a] = val;
      NEXT;
    }

    CASE(OP_SETMCNST) {
      /* A Bx    R(A+1)::Syms(Bx) := R(A) */
      int a = GETARG_A(i);

      mrb_const_set(mrb, regs[a+1], syms[GETARG_Bx(i)], regs[a]);
      NEXT;
    }

    CASE(OP_GETUPVAR) {
      /* A B C  R(A) := uvget(B,C) */
      mrb_value *regs_a = regs + GETARG_A(i);
      int up = GETARG_C(i);

      struct REnv *e = uvenv(mrb, up);

      if (!e) {
        *regs_a = mrb_nil_value();
      }
      else {
        int idx = GETARG_B(i);
        *regs_a = e->stack[idx];
      }
      NEXT;
    }

    CASE(OP_SETUPVAR) {
      /* A B C  uvset(B,C,R(A)) */
      int up = GETARG_C(i);

      struct REnv *e = uvenv(mrb, up);

      if (e) {
        mrb_value *regs_a = regs + GETARG_A(i);
        int idx = GETARG_B(i);
        e->stack[idx] = *regs_a;
        mrb_write_barrier(mrb, (struct RBasic*)e);
      }
      NEXT;
    }

    CASE(OP_JMP) {
      /* sBx    pc+=sBx */
      pc += GETARG_sBx(i);
      JUMP;
    }

    CASE(OP_JMPIF) {
      /* A sBx  if R(A) pc+=sBx */
      if (mrb_test(regs[GETARG_A(i)])) {
        pc += GETARG_sBx(i);
        JUMP;
      }
      NEXT;
    }

    CASE(OP_JMPNOT) {
      /* A sBx  if !R(A) pc+=sBx */
      if (!mrb_test(regs[GETARG_A(i)])) {
        pc += GETARG_sBx(i);
        JUMP;
      }
      NEXT;
    }

    CASE(OP_ONERR) {
      /* sBx    pc+=sBx on exception */
      if (MRB_GET_CONTEXT(mrb)->rsize <= MRB_GET_CONTEXT(mrb)->ci->ridx) {
        if (MRB_GET_CONTEXT(mrb)->rsize == 0) MRB_GET_CONTEXT(mrb)->rsize = 16;
        else MRB_GET_CONTEXT(mrb)->rsize *= 2;
        MRB_GET_CONTEXT(mrb)->rescue = (mrb_code **)mrb_realloc(mrb, MRB_GET_CONTEXT(mrb)->rescue, sizeof(mrb_code*) * MRB_GET_CONTEXT(mrb)->rsize);
      }
      MRB_GET_CONTEXT(mrb)->rescue[MRB_GET_CONTEXT(mrb)->ci->ridx++] = pc + GETARG_sBx(i);
      NEXT;
    }

    CASE(OP_RESCUE) {
      /* A      R(A) := exc; clear(exc) */
      SET_OBJ_VALUE(regs[GETARG_A(i)], MRB_GET_VM(mrb)->exc);
      MRB_GET_VM(mrb)->exc = 0;
      NEXT;
    }

    CASE(OP_POPERR) {
      /* A      A.times{rescue_pop()} */
      int a = GETARG_A(i);

      while (a--) {
        MRB_GET_CONTEXT(mrb)->ci->ridx--;
      }
      NEXT;
    }

    CASE(OP_RAISE) {
      /* A      raise(R(A)) */
      MRB_GET_VM(mrb)->exc = mrb_obj_ptr(regs[GETARG_A(i)]);
      goto L_RAISE;
    }

    CASE(OP_EPUSH) {
      /* Bx     ensure_push(SEQ[Bx]) */
      struct RProc *p;

      p = mrb_closure_new(mrb, irep->reps[GETARG_Bx(i)]);
      /* push ensure_stack */
      if (MRB_GET_CONTEXT(mrb)->esize <= MRB_GET_CONTEXT(mrb)->ci->eidx) {
        if (MRB_GET_CONTEXT(mrb)->esize == 0) MRB_GET_CONTEXT(mrb)->esize = 16;
        else MRB_GET_CONTEXT(mrb)->esize *= 2;
        MRB_GET_CONTEXT(mrb)->ensure = (struct RProc **)mrb_realloc(mrb, MRB_GET_CONTEXT(mrb)->ensure, sizeof(struct RProc*) * MRB_GET_CONTEXT(mrb)->esize);
      }
      MRB_GET_CONTEXT(mrb)->ensure[MRB_GET_CONTEXT(mrb)->ci->eidx++] = p;
      ARENA_RESTORE(mrb, ai);
      NEXT;
    }

    CASE(OP_EPOP) {
      /* A      A.times{ensure_pop().call} */
      int a = GETARG_A(i);
      mrb_callinfo *ci = MRB_GET_CONTEXT(mrb)->ci;
      int n, eidx = ci->eidx;

      for (n=0; n<a && eidx > ci[-1].eidx; n++) {
        ecall(mrb, --eidx);
        ARENA_RESTORE(mrb, ai);
      }
      NEXT;
    }

    CASE(OP_LOADNIL) {
      /* A     R(A) := nil */
      int a = GETARG_A(i);

      SET_NIL_VALUE(regs[a]);
      NEXT;
    }

    CASE(OP_SENDB) {
      /* A B C  R(A) := call(R(A),Syms(B),R(A+1),...,R(A+C),&R(A+C+1))*/
      /* fall through */
    };

  L_SEND:
    CASE(OP_SEND) {
      /* A B C  R(A) := call(R(A),Syms(B),R(A+1),...,R(A+C)) */
      int a = GETARG_A(i);
      int n = GETARG_C(i);
      struct RProc *m;
      struct RClass *c;
      mrb_callinfo *ci;
      mrb_value recv, result;
      mrb_sym mid = syms[GETARG_B(i)];

      recv = regs[a];
      if (GET_OPCODE(i) != OP_SENDB) {
        if (n == CALL_MAXARGS) {
          SET_NIL_VALUE(regs[a+2]);
        }
        else {
          SET_NIL_VALUE(regs[a+n+1]);
        }
      }
      c = mrb_class(mrb, recv);
      m = mrb_method_search_vm(mrb, &c, mid);
      if (!m) {
        mrb_value sym = mrb_symbol_value(mid);

        mid = mrb_intern_lit(mrb, "method_missing");
        m = mrb_method_search_vm(mrb, &c, mid);
        if (n == CALL_MAXARGS) {
          mrb_ary_unshift(mrb, regs[a+1], sym);
        }
        else {
          value_move(regs+a+2, regs+a+1, ++n);
          regs[a+1] = sym;
        }
      }

      /* push callinfo */
      ci = cipush(mrb);
      ci->mid = mid;
      ci->proc = m;
      ci->stackent = MRB_GET_CONTEXT(mrb)->stack;
      if (c->tt == MRB_TT_ICLASS) {
        ci->target_class = c->c;
      }
      else {
        ci->target_class = c;
      }

      ci->pc = pc + 1;
      ci->acc = a;

      /* prepare stack */
      MRB_GET_CONTEXT(mrb)->stack += a;

      if (MRB_PROC_CFUNC_P(m)) {
        if (n == CALL_MAXARGS) {
          ci->argc = -1;
          ci->nregs = 3;
        }
        else {
          ci->argc = n;
          ci->nregs = n + 2;
        }
        result = m->body.func(mrb, recv);
        MRB_GET_CONTEXT(mrb)->stack[0] = result;
        mrb_gc_arena_restore(mrb, ai);
        if (MRB_GET_VM(mrb)->exc) goto L_RAISE;
        /* pop stackpos */
        ci = MRB_GET_CONTEXT(mrb)->ci;
        if (!ci->target_class) { /* return from context modifying method (resume/yield) */
          if (!MRB_PROC_CFUNC_P(ci[-1].proc)) {
            proc = ci[-1].proc;
            irep = proc->body.irep;
            pool = irep->pool;
            syms = irep->syms;
          }
        }
        regs = MRB_GET_CONTEXT(mrb)->stack = ci->stackent;
        pc = ci->pc;
        cipop(mrb);
        JUMP;
      }
      else {
        /* setup environment for calling method */
        proc = MRB_GET_CONTEXT(mrb)->ci->proc = m;
        irep = m->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        ci->nregs = irep->nregs;
        if (n == CALL_MAXARGS) {
          ci->argc = -1;
          stack_extend(mrb, (irep->nregs < 3) ? 3 : irep->nregs, 3);
        }
        else {
          ci->argc = n;
          stack_extend(mrb, irep->nregs,  n+2);
        }
        regs = MRB_GET_CONTEXT(mrb)->stack;
        pc = irep->iseq;
        JUMP;
      }
    }

    CASE(OP_FSEND) {
      /* A B C  R(A) := fcall(R(A),Syms(B),R(A+1),... ,R(A+C-1)) */
      NEXT;
    }

    CASE(OP_CALL) {
      /* A      R(A) := self.call(frame.argc, frame.argv) */
      mrb_callinfo *ci;
      mrb_value recv = MRB_GET_CONTEXT(mrb)->stack[0];
      struct RProc *m = mrb_proc_ptr(recv);

      /* replace callinfo */
      ci = MRB_GET_CONTEXT(mrb)->ci;
      ci->target_class = m->target_class;
      ci->proc = m;
      if (m->env) {
        if (m->env->mid) {
          ci->mid = m->env->mid;
        }
        if (!m->env->stack) {
          m->env->stack = MRB_GET_CONTEXT(mrb)->stack;
        }
      }

      /* prepare stack */
      if (MRB_PROC_CFUNC_P(m)) {
        recv = m->body.func(mrb, recv);
        mrb_gc_arena_restore(mrb, ai);
        if (MRB_GET_VM(mrb)->exc) goto L_RAISE;
        /* pop stackpos */
        ci = MRB_GET_CONTEXT(mrb)->ci;
        regs = MRB_GET_CONTEXT(mrb)->stack = ci->stackent;
        regs[ci->acc] = recv;
        pc = ci->pc;
        cipop(mrb);
        irep = MRB_GET_CONTEXT(mrb)->ci->proc->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        JUMP;
      }
      else {
        /* setup environment for calling method */
        proc = m;
        irep = m->body.irep;
        if (!irep) {
          MRB_GET_CONTEXT(mrb)->stack[0] = mrb_nil_value();
          goto L_RETURN;
        }
        pool = irep->pool;
        syms = irep->syms;
        ci->nregs = irep->nregs;
        if (ci->argc < 0) {
          stack_extend(mrb, (irep->nregs < 3) ? 3 : irep->nregs, 3);
        }
        else {
          stack_extend(mrb, irep->nregs, ci->argc+2);
        }
        regs = MRB_GET_CONTEXT(mrb)->stack;
        regs[0] = m->env->stack[0];
        pc = irep->iseq;
        JUMP;
      }
    }

    CASE(OP_SUPER) {
      /* A C  R(A) := super(R(A+1),... ,R(A+C+1)) */
      mrb_value recv;
      mrb_callinfo *ci = MRB_GET_CONTEXT(mrb)->ci;
      struct RProc *m;
      struct RClass *c;
      mrb_sym mid = ci->mid;
      int a = GETARG_A(i);
      int n = GETARG_C(i);

      recv = regs[0];
      c = MRB_GET_CONTEXT(mrb)->ci->target_class->super;
      m = mrb_method_search_vm(mrb, &c, mid);
      if (!m) {
        mid = mrb_intern_lit(mrb, "method_missing");
        m = mrb_method_search_vm(mrb, &c, mid);
        if (n == CALL_MAXARGS) {
          mrb_ary_unshift(mrb, regs[a+1], mrb_symbol_value(ci->mid));
        }
        else {
          value_move(regs+a+2, regs+a+1, ++n);
          SET_SYM_VALUE(regs[a+1], ci->mid);
        }
      }

      /* push callinfo */
      ci = cipush(mrb);
      ci->mid = mid;
      ci->proc = m;
      ci->stackent = MRB_GET_CONTEXT(mrb)->stack;
      if (n == CALL_MAXARGS) {
        ci->argc = -1;
      }
      else {
        ci->argc = n;
      }
      ci->target_class = c;
      ci->pc = pc + 1;

      /* prepare stack */
      MRB_GET_CONTEXT(mrb)->stack += a;
      MRB_GET_CONTEXT(mrb)->stack[0] = recv;

      if (MRB_PROC_CFUNC_P(m)) {
        ci->nregs = 0;
        MRB_GET_CONTEXT(mrb)->stack[0] = m->body.func(mrb, recv);
        mrb_gc_arena_restore(mrb, ai);
        if (MRB_GET_VM(mrb)->exc) goto L_RAISE;
        /* pop stackpos */
        regs = MRB_GET_CONTEXT(mrb)->stack = MRB_GET_CONTEXT(mrb)->ci->stackent;
        cipop(mrb);
        NEXT;
      }
      else {
        /* fill callinfo */
        ci->acc = a;

        /* setup environment for calling method */
        ci->proc = m;
        irep = m->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        ci->nregs = irep->nregs;
        if (n == CALL_MAXARGS) {
          stack_extend(mrb, (irep->nregs < 3) ? 3 : irep->nregs, 3);
        }
        else {
          stack_extend(mrb, irep->nregs, ci->argc+2);
        }
        regs = MRB_GET_CONTEXT(mrb)->stack;
        pc = irep->iseq;
        JUMP;
      }
    }

    CASE(OP_ARGARY) {
      /* A Bx   R(A) := argument array (16=6:1:5:4) */
      int a = GETARG_A(i);
      int bx = GETARG_Bx(i);
      int m1 = (bx>>10)&0x3f;
      int r  = (bx>>9)&0x1;
      int m2 = (bx>>4)&0x1f;
      int lv = (bx>>0)&0xf;
      mrb_value *stack;

      if (lv == 0) stack = regs + 1;
      else {
        struct REnv *e = uvenv(mrb, lv-1);
        if (!e) {
          mrb_value exc;
          exc = mrb_exc_new_str_lit(mrb, E_NOMETHOD_ERROR, "super called outside of method");
          MRB_GET_VM(mrb)->exc = mrb_obj_ptr(exc);
          goto L_RAISE;
        }
        stack = e->stack + 1;
      }
      if (r == 0) {
        regs[a] = mrb_ary_new_from_values(mrb, m1+m2, stack);
      }
      else {
        mrb_value *pp = NULL;
        struct RArray *rest;
        int len = 0;

        if (mrb_array_p(stack[m1])) {
          struct RArray *ary = mrb_ary_ptr(stack[m1]);

          pp = ary->ptr;
          len = ary->len;
        }
        regs[a] = mrb_ary_new_capa(mrb, m1+len+m2);
        rest = mrb_ary_ptr(regs[a]);
        if (m1 > 0) {
          stack_copy(rest->ptr, stack, m1);
        }
        if (len > 0) {
          stack_copy(rest->ptr+m1, pp, len);
        }
        if (m2 > 0) {
          stack_copy(rest->ptr+m1+len, stack+m1+1, m2);
        }
        rest->len = m1+len+m2;
      }
      regs[a+1] = stack[m1+r+m2];
      ARENA_RESTORE(mrb, ai);
      NEXT;
    }

    CASE(OP_ENTER) {
      /* Ax             arg setup according to flags (23=5:5:1:5:5:1:1) */
      /* number of optional arguments times OP_JMP should follow */
      mrb_aspec ax = GETARG_Ax(i);
      int m1 = MRB_ASPEC_REQ(ax);
      int o  = MRB_ASPEC_OPT(ax);
      int r  = MRB_ASPEC_REST(ax);
      int m2 = MRB_ASPEC_POST(ax);
      /* unused
      int k  = MRB_ASPEC_KEY(ax);
      int kd = MRB_ASPEC_KDICT(ax);
      int b  = MRB_ASPEC_BLOCK(ax);
      */
      int argc = MRB_GET_CONTEXT(mrb)->ci->argc;
      mrb_value *argv = regs+1;
      mrb_value *argv0 = argv;
      int len = m1 + o + r + m2;
      mrb_value *blk = &argv[argc < 0 ? 1 : argc];

      if (!mrb_nil_p(*blk) && mrb_type(*blk) != MRB_TT_PROC) {
        *blk = mrb_convert_type(mrb, *blk, MRB_TT_PROC, "Proc", "to_proc");
      }
      if (argc < 0) {
        struct RArray *ary = mrb_ary_ptr(regs[1]);
        argv = ary->ptr;
        argc = ary->len;
        mrb_gc_protect(mrb, regs[1]);
      }
      if (MRB_GET_CONTEXT(mrb)->ci->proc && MRB_PROC_STRICT_P(MRB_GET_CONTEXT(mrb)->ci->proc)) {
        if (argc >= 0) {
          if (argc < m1 + m2 || (r == 0 && argc > len)) {
            argnum_error(mrb, m1+m2);
            goto L_RAISE;
          }
        }
      }
      else if (len > 1 && argc == 1 && mrb_array_p(argv[0])) {
        mrb_gc_protect(mrb, argv[0]);
        argc = mrb_ary_ptr(argv[0])->len;
        argv = mrb_ary_ptr(argv[0])->ptr;
      }
      MRB_GET_CONTEXT(mrb)->ci->argc = len;
      if (argc < len) {
        int mlen = m2;
        if (argc < m1+m2) {
          if (m1 < argc)
            mlen = argc - m1;
          else
            mlen = 0;
        }
        regs[len+1] = *blk; /* move block */
        SET_NIL_VALUE(regs[argc+1]);
        if (argv0 != argv) {
          value_move(&regs[1], argv, argc-mlen); /* m1 + o */
        }
        if (mlen) {
          value_move(&regs[len-m2+1], &argv[argc-mlen], mlen);
        }
        if (r) {
          regs[m1+o+1] = mrb_ary_new_capa(mrb, 0);
        }
        if (o == 0 || argc < m1+m2) pc++;
        else
          pc += argc - m1 - m2 + 1;
      }
      else {
        int rnum = 0;
        if (argv0 != argv) {
          regs[len+1] = *blk; /* move block */
          value_move(&regs[1], argv, m1+o);
        }
        if (r) {
          rnum = argc-m1-o-m2;
          regs[m1+o+1] = mrb_ary_new_from_values(mrb, rnum, argv+m1+o);
        }
        if (m2) {
          if (argc-m2 > m1) {
            value_move(&regs[m1+o+r+1], &argv[m1+o+rnum], m2);
          }
        }
        if (argv0 == argv) {
          regs[len+1] = *blk; /* move block */
        }
        pc += o + 1;
      }
      JUMP;
    }

    CASE(OP_KARG) {
      /* A B C          R(A) := kdict[Syms(B)]; if C kdict.rm(Syms(B)) */
      /* if C == 2; raise unless kdict.empty? */
      /* OP_JMP should follow to skip init code */
      NEXT;
    }

    CASE(OP_KDICT) {
      /* A C            R(A) := kdict */
      NEXT;
    }

    L_RETURN:
      i = MKOP_AB(OP_RETURN, GETARG_A(i), OP_R_NORMAL);
      /* fall through */
    CASE(OP_RETURN) {
      /* A B     return R(A) (B=normal,in-block return/break) */
      if (MRB_GET_VM(mrb)->exc) {
        mrb_callinfo *ci;
        int eidx;

      L_RAISE:
        ci = MRB_GET_CONTEXT(mrb)->ci;
        mrb_obj_iv_ifnone(mrb, MRB_GET_VM(mrb)->exc, mrb_intern_lit(mrb, "lastpc"), mrb_cptr_value(mrb, pc));
        mrb_obj_iv_ifnone(mrb, MRB_GET_VM(mrb)->exc, mrb_intern_lit(mrb, "ciidx"), mrb_fixnum_value(ci - MRB_GET_CONTEXT(mrb)->cibase));
        eidx = ci->eidx;
        if (ci == MRB_GET_CONTEXT(mrb)->cibase) {
          if (ci->ridx == 0) goto L_STOP;
          goto L_RESCUE;
        }
        while (eidx > ci[-1].eidx) {
          ecall(mrb, --eidx);
        }
        while (ci[0].ridx == ci[-1].ridx) {
          cipop(mrb);
          ci = MRB_GET_CONTEXT(mrb)->ci;
          MRB_GET_CONTEXT(mrb)->stack = ci[1].stackent;
          if (ci[1].acc == CI_ACC_SKIP && prev_jmp) {
            MRB_GET_THREAD_CONTEXT(mrb)->jmp = prev_jmp;
            MRB_THROW(prev_jmp);
          }
          if (ci > MRB_GET_CONTEXT(mrb)->cibase) {
            while (eidx > ci[-1].eidx) {
              ecall(mrb, --eidx);
            }
          }
          else if (ci == MRB_GET_CONTEXT(mrb)->cibase) {
            if (ci->ridx == 0) {
              if (MRB_GET_CONTEXT(mrb) == MRB_GET_ROOT_CONTEXT(mrb)) {
                regs = MRB_GET_CONTEXT(mrb)->stack = MRB_GET_CONTEXT(mrb)->stbase;
                goto L_STOP;
              }
              else {
                struct mrb_context *c = MRB_GET_CONTEXT(mrb);

                MRB_GET_CONTEXT(mrb) = c->prev;
                c->prev = NULL;
                goto L_RAISE;
              }
            }
            break;
          }
        }
      L_RESCUE:
        if (ci->ridx == 0) goto L_STOP;
        proc = ci->proc;
        irep = proc->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        regs = MRB_GET_CONTEXT(mrb)->stack = ci[1].stackent;
        pc = MRB_GET_CONTEXT(mrb)->rescue[--ci->ridx];
      }
      else {
        mrb_callinfo *ci = MRB_GET_CONTEXT(mrb)->ci;
        int acc, eidx = MRB_GET_CONTEXT(mrb)->ci->eidx;
        mrb_value v = regs[GETARG_A(i)];

        switch (GETARG_B(i)) {
        case OP_R_RETURN:
          /* Fall through to OP_R_NORMAL otherwise */
          if (proc->env && !MRB_PROC_STRICT_P(proc)) {
            struct REnv *e = top_env(mrb, proc);

            if (!MRB_ENV_STACK_SHARED_P(e)) {
              localjump_error(mrb, LOCALJUMP_ERROR_RETURN);
              goto L_RAISE;
            }
            ci = MRB_GET_CONTEXT(mrb)->cibase + e->cioff;
            if (ci == MRB_GET_CONTEXT(mrb)->cibase) {
              localjump_error(mrb, LOCALJUMP_ERROR_RETURN);
              goto L_RAISE;
            }
            MRB_GET_CONTEXT(mrb)->ci = ci;
            break;
          }
        case OP_R_NORMAL:
          if (ci == MRB_GET_CONTEXT(mrb)->cibase) {
            if (!MRB_GET_CONTEXT(mrb)->prev) { /* toplevel return */
              localjump_error(mrb, LOCALJUMP_ERROR_RETURN);
              goto L_RAISE;
            }
            if (MRB_GET_CONTEXT(mrb)->prev->ci == MRB_GET_CONTEXT(mrb)->prev->cibase) {
              mrb_value exc = mrb_exc_new_str_lit(mrb, E_FIBER_ERROR, "double resume");
              MRB_GET_VM(mrb)->exc = mrb_obj_ptr(exc);
              goto L_RAISE;
            }
            /* automatic yield at the end */
            MRB_GET_CONTEXT(mrb)->status = MRB_FIBER_TERMINATED;
            MRB_GET_CONTEXT(mrb) = MRB_GET_CONTEXT(mrb)->prev;
            MRB_GET_CONTEXT(mrb)->status = MRB_FIBER_RUNNING;
          }
          ci = MRB_GET_CONTEXT(mrb)->ci;
          break;
        case OP_R_BREAK:
          if (!proc->env || !MRB_ENV_STACK_SHARED_P(proc->env)) {
            localjump_error(mrb, LOCALJUMP_ERROR_BREAK);
            goto L_RAISE;
          }
          /* break from fiber block */
          if (MRB_GET_CONTEXT(mrb)->ci == MRB_GET_CONTEXT(mrb)->cibase && MRB_GET_CONTEXT(mrb)->ci->pc) {
            struct mrb_context *c = MRB_GET_CONTEXT(mrb);

            MRB_GET_CONTEXT(mrb) = c->prev;
            c->prev = NULL;
          }
          ci = MRB_GET_CONTEXT(mrb)->ci;
          MRB_GET_CONTEXT(mrb)->ci = MRB_GET_CONTEXT(mrb)->cibase + proc->env->cioff + 1;
          while (ci > MRB_GET_CONTEXT(mrb)->ci) {
            if (ci[-1].acc == CI_ACC_SKIP) {
              MRB_GET_CONTEXT(mrb)->ci = ci;
              break;
            }
            ci--;
          }
          break;
        default:
          /* cannot happen */
          break;
        }
        while (eidx > MRB_GET_CONTEXT(mrb)->ci[-1].eidx) {
          ecall(mrb, --eidx);
        }
        cipop(mrb);
        acc = ci->acc;
        pc = ci->pc;
        regs = MRB_GET_CONTEXT(mrb)->stack = ci->stackent;
        if (acc == CI_ACC_SKIP) {
          MRB_GET_THREAD_CONTEXT(mrb)->jmp = prev_jmp;
#ifdef MRB_USE_GVL_API
          if (!is_gvl_acquired) {
            mrb_gvl_release(mrb);
          }
#endif
          return v;
        }
        DEBUG(printf("from :%s\n", mrb_sym2name(mrb, ci->mid)));
        proc = MRB_GET_CONTEXT(mrb)->ci->proc;
        irep = proc->body.irep;
        pool = irep->pool;
        syms = irep->syms;

        regs[acc] = v;
      }
      JUMP;
    }

    CASE(OP_TAILCALL) {
      /* A B C  return call(R(A),Syms(B),R(A+1),... ,R(A+C+1)) */
      int a = GETARG_A(i);
      int n = GETARG_C(i);
      struct RProc *m;
      struct RClass *c;
      mrb_callinfo *ci;
      mrb_value recv;
      mrb_sym mid = syms[GETARG_B(i)];

      recv = regs[a];
      c = mrb_class(mrb, recv);
      m = mrb_method_search_vm(mrb, &c, mid);
      if (!m) {
        mrb_value sym = mrb_symbol_value(mid);

        mid = mrb_intern_lit(mrb, "method_missing");
        m = mrb_method_search_vm(mrb, &c, mid);
        if (n == CALL_MAXARGS) {
          mrb_ary_unshift(mrb, regs[a+1], sym);
        }
        else {
          value_move(regs+a+2, regs+a+1, ++n);
          regs[a+1] = sym;
        }
      }

      /* replace callinfo */
      ci = MRB_GET_CONTEXT(mrb)->ci;
      ci->mid = mid;
      ci->target_class = c;
      if (n == CALL_MAXARGS) {
        ci->argc = -1;
      }
      else {
        ci->argc = n;
      }

      /* move stack */
      value_move(MRB_GET_CONTEXT(mrb)->stack, &regs[a], ci->argc+1);

      if (MRB_PROC_CFUNC_P(m)) {
        MRB_GET_CONTEXT(mrb)->stack[0] = m->body.func(mrb, recv);
        mrb_gc_arena_restore(mrb, ai);
        goto L_RETURN;
      }
      else {
        /* setup environment for calling method */
        irep = m->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        if (ci->argc < 0) {
          stack_extend(mrb, (irep->nregs < 3) ? 3 : irep->nregs, 3);
        }
        else {
          stack_extend(mrb, irep->nregs, ci->argc+2);
        }
        regs = MRB_GET_CONTEXT(mrb)->stack;
        pc = irep->iseq;
      }
      JUMP;
    }

    CASE(OP_BLKPUSH) {
      /* A Bx   R(A) := block (16=6:1:5:4) */
      int a = GETARG_A(i);
      int bx = GETARG_Bx(i);
      int m1 = (bx>>10)&0x3f;
      int r  = (bx>>9)&0x1;
      int m2 = (bx>>4)&0x1f;
      int lv = (bx>>0)&0xf;
      mrb_value *stack;

      if (lv == 0) stack = regs + 1;
      else {
        struct REnv *e = uvenv(mrb, lv-1);
        if (!e) {
          localjump_error(mrb, LOCALJUMP_ERROR_YIELD);
          goto L_RAISE;
        }
        stack = e->stack + 1;
      }
      regs[a] = stack[m1+r+m2];
      NEXT;
    }

#define TYPES2(a,b) ((((uint16_t)(a))<<8)|(((uint16_t)(b))&0xff))
#define OP_MATH_BODY(op,v1,v2) do {\
  v1(regs[a]) = v1(regs[a]) op v2(regs[a+1]);\
} while(0)

    CASE(OP_ADD) {
      /* A B C  R(A) := R(A)+R(A+1) (Syms[B]=:+,C=1)*/
      int a = GETARG_A(i);

      /* need to check if op is overridden */
      switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):
        {
          mrb_int x, y, z;
          mrb_value *regs_a = regs + a;

          x = mrb_fixnum(regs_a[0]);
          y = mrb_fixnum(regs_a[1]);
          if (mrb_int_add_overflow(x, y, &z)) {
            SET_FLOAT_VALUE(mrb, regs_a[0], (mrb_float)x + (mrb_float)y);
            break;
          }
          SET_INT_VALUE(regs[a], z);
        }
        break;
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FLOAT):
        {
          mrb_int x = mrb_fixnum(regs[a]);
          mrb_float y = mrb_float(regs[a+1]);
          SET_FLOAT_VALUE(mrb, regs[a], (mrb_float)x + y);
        }
        break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FIXNUM):
#ifdef MRB_WORD_BOXING
        {
          mrb_float x = mrb_float(regs[a]);
          mrb_int y = mrb_fixnum(regs[a+1]);
          SET_FLOAT_VALUE(mrb, regs[a], x + y);
        }
#else
        OP_MATH_BODY(+,mrb_float,mrb_fixnum);
#endif
        break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FLOAT):
#ifdef MRB_WORD_BOXING
        {
          mrb_float x = mrb_float(regs[a]);
          mrb_float y = mrb_float(regs[a+1]);
          SET_FLOAT_VALUE(mrb, regs[a], x + y);
        }
#else
        OP_MATH_BODY(+,mrb_float,mrb_float);
#endif
        break;
      case TYPES2(MRB_TT_STRING,MRB_TT_STRING):
        regs[a] = mrb_str_plus(mrb, regs[a], regs[a+1]);
        break;
      default:
        goto L_SEND;
      }
      ARENA_RESTORE(mrb, ai);
      NEXT;
    }

    CASE(OP_SUB) {
      /* A B C  R(A) := R(A)-R(A+1) (Syms[B]=:-,C=1)*/
      int a = GETARG_A(i);

      /* need to check if op is overridden */
      switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):
        {
          mrb_int x, y, z;

          x = mrb_fixnum(regs[a]);
          y = mrb_fixnum(regs[a+1]);
          if (mrb_int_sub_overflow(x, y, &z)) {
            SET_FLOAT_VALUE(mrb, regs[a], (mrb_float)x - (mrb_float)y);
            break;
          }
          SET_INT_VALUE(regs[a], z);
        }
        break;
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FLOAT):
        {
          mrb_int x = mrb_fixnum(regs[a]);
          mrb_float y = mrb_float(regs[a+1]);
          SET_FLOAT_VALUE(mrb, regs[a], (mrb_float)x - y);
        }
        break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FIXNUM):
#ifdef MRB_WORD_BOXING
        {
          mrb_float x = mrb_float(regs[a]);
          mrb_int y = mrb_fixnum(regs[a+1]);
          SET_FLOAT_VALUE(mrb, regs[a], x - y);
        }
#else
        OP_MATH_BODY(-,mrb_float,mrb_fixnum);
#endif
        break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FLOAT):
#ifdef MRB_WORD_BOXING
        {
          mrb_float x = mrb_float(regs[a]);
          mrb_float y = mrb_float(regs[a+1]);
          SET_FLOAT_VALUE(mrb, regs[a], x - y);
        }
#else
        OP_MATH_BODY(-,mrb_float,mrb_float);
#endif
        break;
      default:
        goto L_SEND;
      }
      NEXT;
    }

    CASE(OP_MUL) {
      /* A B C  R(A) := R(A)*R(A+1) (Syms[B]=:*,C=1)*/
      int a = GETARG_A(i);

      /* need to check if op is overridden */
      switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):
        {
          mrb_value z;

          z = mrb_fixnum_mul(mrb, regs[a], regs[a+1]);

          switch (mrb_type(z)) {
          case MRB_TT_FIXNUM:
            {
              SET_INT_VALUE(regs[a], mrb_fixnum(z));
            }
            break;
          case MRB_TT_FLOAT:
            {
              SET_FLOAT_VALUE(mrb, regs[a], mrb_float(z));
            }
            break;
          default:
            /* cannot happen */
            break;
          }
        }
        break;
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FLOAT):
        {
          mrb_int x = mrb_fixnum(regs[a]);
          mrb_float y = mrb_float(regs[a+1]);
          SET_FLOAT_VALUE(mrb, regs[a], (mrb_float)x * y);
        }
        break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FIXNUM):
#ifdef MRB_WORD_BOXING
        {
          mrb_float x = mrb_float(regs[a]);
          mrb_int y = mrb_fixnum(regs[a+1]);
          SET_FLOAT_VALUE(mrb, regs[a], x * y);
        }
#else
        OP_MATH_BODY(*,mrb_float,mrb_fixnum);
#endif
        break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FLOAT):
#ifdef MRB_WORD_BOXING
        {
          mrb_float x = mrb_float(regs[a]);
          mrb_float y = mrb_float(regs[a+1]);
          SET_FLOAT_VALUE(mrb, regs[a], x * y);
        }
#else
        OP_MATH_BODY(*,mrb_float,mrb_float);
#endif
        break;
      default:
        goto L_SEND;
      }
      NEXT;
    }

    CASE(OP_DIV) {
      /* A B C  R(A) := R(A)/R(A+1) (Syms[B]=:/,C=1)*/
      int a = GETARG_A(i);

      /* need to check if op is overridden */
      switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):
        {
          mrb_int x = mrb_fixnum(regs[a]);
          mrb_int y = mrb_fixnum(regs[a+1]);
          SET_FLOAT_VALUE(mrb, regs[a], (mrb_float)x / (mrb_float)y);
        }
        break;
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FLOAT):
        {
          mrb_int x = mrb_fixnum(regs[a]);
          mrb_float y = mrb_float(regs[a+1]);
          SET_FLOAT_VALUE(mrb, regs[a], (mrb_float)x / y);
        }
        break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FIXNUM):
#ifdef MRB_WORD_BOXING
        {
          mrb_float x = mrb_float(regs[a]);
          mrb_int y = mrb_fixnum(regs[a+1]);
          SET_FLOAT_VALUE(mrb, regs[a], x / y);
        }
#else
        OP_MATH_BODY(/,mrb_float,mrb_fixnum);
#endif
        break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FLOAT):
#ifdef MRB_WORD_BOXING
        {
          mrb_float x = mrb_float(regs[a]);
          mrb_float y = mrb_float(regs[a+1]);
          SET_FLOAT_VALUE(mrb, regs[a], x / y);
        }
#else
        OP_MATH_BODY(/,mrb_float,mrb_float);
#endif
        break;
      default:
        goto L_SEND;
      }
#ifdef MRB_NAN_BOXING
      if (isnan(mrb_float(regs[a]))) {
        regs[a] = mrb_float_value(mrb, mrb_float(regs[a]));
      }
#endif
      NEXT;
    }

    CASE(OP_ADDI) {
      /* A B C  R(A) := R(A)+C (Syms[B]=:+)*/
      int a = GETARG_A(i);

      /* need to check if + is overridden */
      switch (mrb_type(regs[a])) {
      case MRB_TT_FIXNUM:
        {
          mrb_int x = mrb_fixnum(regs[a]);
          mrb_int y = GETARG_C(i);
          mrb_int z;

          if (mrb_int_add_overflow(x, y, &z)) {
            SET_FLOAT_VALUE(mrb, regs[a], (mrb_float)x + (mrb_float)y);
            break;
          }
          mrb_fixnum(regs[a]) = z;
        }
        break;
      case MRB_TT_FLOAT:
#ifdef MRB_WORD_BOXING
        {
          mrb_float x = mrb_float(regs[a]);
          SET_FLOAT_VALUE(mrb, regs[a], x + GETARG_C(i));
        }
#else
        mrb_float(regs[a]) += GETARG_C(i);
#endif
        break;
      default:
        SET_INT_VALUE(regs[a+1], GETARG_C(i));
        i = MKOP_ABC(OP_SEND, a, GETARG_B(i), 1);
        goto L_SEND;
      }
      NEXT;
    }

    CASE(OP_SUBI) {
      /* A B C  R(A) := R(A)-C (Syms[B]=:-)*/
      int a = GETARG_A(i);
      mrb_value *regs_a = regs + a;

      /* need to check if + is overridden */
      switch (mrb_type(regs_a[0])) {
      case MRB_TT_FIXNUM:
        {
          mrb_int x = mrb_fixnum(regs_a[0]);
          mrb_int y = GETARG_C(i);
          mrb_int z;

          if (mrb_int_sub_overflow(x, y, &z)) {
            SET_FLOAT_VALUE(mrb, regs_a[0], (mrb_float)x - (mrb_float)y);
          }
          else {
            mrb_fixnum(regs_a[0]) = z;
          }
        }
        break;
      case MRB_TT_FLOAT:
#ifdef MRB_WORD_BOXING
        {
          mrb_float x = mrb_float(regs[a]);
          SET_FLOAT_VALUE(mrb, regs[a], x - GETARG_C(i));
        }
#else
        mrb_float(regs_a[0]) -= GETARG_C(i);
#endif
        break;
      default:
        SET_INT_VALUE(regs_a[1], GETARG_C(i));
        i = MKOP_ABC(OP_SEND, a, GETARG_B(i), 1);
        goto L_SEND;
      }
      NEXT;
    }

#define OP_CMP_BODY(op,v1,v2) (v1(regs[a]) op v2(regs[a+1]))

#define OP_CMP(op) do {\
  int result;\
  /* need to check if - is overridden */\
  switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {\
  case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):\
    result = OP_CMP_BODY(op,mrb_fixnum,mrb_fixnum);\
    break;\
  case TYPES2(MRB_TT_FIXNUM,MRB_TT_FLOAT):\
    result = OP_CMP_BODY(op,mrb_fixnum,mrb_float);\
    break;\
  case TYPES2(MRB_TT_FLOAT,MRB_TT_FIXNUM):\
    result = OP_CMP_BODY(op,mrb_float,mrb_fixnum);\
    break;\
  case TYPES2(MRB_TT_FLOAT,MRB_TT_FLOAT):\
    result = OP_CMP_BODY(op,mrb_float,mrb_float);\
    break;\
  default:\
    goto L_SEND;\
  }\
  if (result) {\
    SET_TRUE_VALUE(regs[a]);\
  }\
  else {\
    SET_FALSE_VALUE(regs[a]);\
  }\
} while(0)

    CASE(OP_EQ) {
      /* A B C  R(A) := R(A)==R(A+1) (Syms[B]=:==,C=1)*/
      int a = GETARG_A(i);
      if (mrb_obj_eq(mrb, regs[a], regs[a+1])) {
        SET_TRUE_VALUE(regs[a]);
      }
      else {
        OP_CMP(==);
      }
      NEXT;
    }

    CASE(OP_LT) {
      /* A B C  R(A) := R(A)<R(A+1) (Syms[B]=:<,C=1)*/
      int a = GETARG_A(i);
      OP_CMP(<);
      NEXT;
    }

    CASE(OP_LE) {
      /* A B C  R(A) := R(A)<=R(A+1) (Syms[B]=:<=,C=1)*/
      int a = GETARG_A(i);
      OP_CMP(<=);
      NEXT;
    }

    CASE(OP_GT) {
      /* A B C  R(A) := R(A)>R(A+1) (Syms[B]=:>,C=1)*/
      int a = GETARG_A(i);
      OP_CMP(>);
      NEXT;
    }

    CASE(OP_GE) {
      /* A B C  R(A) := R(A)>=R(A+1) (Syms[B]=:>=,C=1)*/
      int a = GETARG_A(i);
      OP_CMP(>=);
      NEXT;
    }

    CASE(OP_ARRAY) {
      /* A B C          R(A) := ary_new(R(B),R(B+1)..R(B+C)) */
      regs[GETARG_A(i)] = mrb_ary_new_from_values(mrb, GETARG_C(i), &regs[GETARG_B(i)]);
      ARENA_RESTORE(mrb, ai);
      NEXT;
    }

    CASE(OP_ARYCAT) {
      /* A B            mrb_ary_concat(R(A),R(B)) */
      mrb_ary_concat(mrb, regs[GETARG_A(i)],
                     mrb_ary_splat(mrb, regs[GETARG_B(i)]));
      ARENA_RESTORE(mrb, ai);
      NEXT;
    }

    CASE(OP_ARYPUSH) {
      /* A B            R(A).push(R(B)) */
      mrb_ary_push(mrb, regs[GETARG_A(i)], regs[GETARG_B(i)]);
      NEXT;
    }

    CASE(OP_AREF) {
      /* A B C          R(A) := R(B)[C] */
      int a = GETARG_A(i);
      int c = GETARG_C(i);
      mrb_value v = regs[GETARG_B(i)];

      if (!mrb_array_p(v)) {
        if (c == 0) {
          regs[GETARG_A(i)] = v;
        }
        else {
          SET_NIL_VALUE(regs[a]);
        }
      }
      else {
        regs[GETARG_A(i)] = mrb_ary_ref(mrb, v, c);
      }
      NEXT;
    }

    CASE(OP_ASET) {
      /* A B C          R(B)[C] := R(A) */
      mrb_ary_set(mrb, regs[GETARG_B(i)], GETARG_C(i), regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_APOST) {
      /* A B C  *R(A),R(A+1)..R(A+C) := R(A) */
      int a = GETARG_A(i);
      mrb_value v = regs[a];
      int pre  = GETARG_B(i);
      int post = GETARG_C(i);

      if (!mrb_array_p(v)) {
        regs[a++] = mrb_ary_new_capa(mrb, 0);
        while (post--) {
          SET_NIL_VALUE(regs[a]);
          a++;
        }
      }
      else {
        struct RArray *ary = mrb_ary_ptr(v);
        int len = ary->len;
        int idx;

        if (len > pre + post) {
          regs[a++] = mrb_ary_new_from_values(mrb, len - pre - post, ary->ptr+pre);
          while (post--) {
            regs[a++] = ary->ptr[len-post-1];
          }
        }
        else {
          regs[a++] = mrb_ary_new_capa(mrb, 0);
          for (idx=0; idx+pre<len; idx++) {
            regs[a+idx] = ary->ptr[pre+idx];
          }
          while (idx < post) {
            SET_NIL_VALUE(regs[a+idx]);
            idx++;
          }
        }
      }
      ARENA_RESTORE(mrb, ai);
      NEXT;
    }

    CASE(OP_STRING) {
      /* A Bx           R(A) := str_new(Lit(Bx)) */
      regs[GETARG_A(i)] = mrb_str_dup(mrb, pool[GETARG_Bx(i)]);
      ARENA_RESTORE(mrb, ai);
      NEXT;
    }

    CASE(OP_STRCAT) {
      /* A B    R(A).concat(R(B)) */
      mrb_str_concat(mrb, regs[GETARG_A(i)], regs[GETARG_B(i)]);
      NEXT;
    }

    CASE(OP_HASH) {
      /* A B C   R(A) := hash_new(R(B),R(B+1)..R(B+C)) */
      int b = GETARG_B(i);
      int c = GETARG_C(i);
      int lim = b+c*2;
      mrb_value hash = mrb_hash_new_capa(mrb, c);

      while (b < lim) {
        mrb_hash_set(mrb, hash, regs[b], regs[b+1]);
        b+=2;
      }
      regs[GETARG_A(i)] = hash;
      ARENA_RESTORE(mrb, ai);
      NEXT;
    }

    CASE(OP_LAMBDA) {
      /* A b c  R(A) := lambda(SEQ[b],c) (b:c = 14:2) */
      struct RProc *p;
      int c = GETARG_c(i);

      if (c & OP_L_CAPTURE) {
        p = mrb_closure_new(mrb, irep->reps[GETARG_b(i)]);
      }
      else {
        p = mrb_proc_new(mrb, irep->reps[GETARG_b(i)]);
        if (c & OP_L_METHOD) {
          if (p->target_class->tt == MRB_TT_SCLASS) {
            mrb_value klass;
            klass = mrb_obj_iv_get(mrb,
                                   (struct RObject *)p->target_class,
                                   mrb_intern_lit(mrb, "__attached__"));
            p->target_class = mrb_class_ptr(klass);
          }
        }
      }
      if (c & OP_L_STRICT) p->flags |= MRB_PROC_STRICT;
      regs[GETARG_A(i)] = mrb_obj_value(p);
      ARENA_RESTORE(mrb, ai);
      NEXT;
    }

    CASE(OP_OCLASS) {
      /* A      R(A) := ::Object */
      regs[GETARG_A(i)] = mrb_obj_value(MRB_GET_VM(mrb)->object_class);
      NEXT;
    }

    CASE(OP_CLASS) {
      /* A B    R(A) := newclass(R(A),Syms(B),R(A+1)) */
      struct RClass *c = 0;
      int a = GETARG_A(i);
      mrb_value base, super;
      mrb_sym id = syms[GETARG_B(i)];

      base = regs[a];
      super = regs[a+1];
      if (mrb_nil_p(base)) {
        base = mrb_obj_value(MRB_GET_CONTEXT(mrb)->ci->target_class);
      }
      c = mrb_vm_define_class(mrb, base, super, id);
      regs[a] = mrb_obj_value(c);
      ARENA_RESTORE(mrb, ai);
      NEXT;
    }

    CASE(OP_MODULE) {
      /* A B            R(A) := newmodule(R(A),Syms(B)) */
      struct RClass *c = 0;
      int a = GETARG_A(i);
      mrb_value base;
      mrb_sym id = syms[GETARG_B(i)];

      base = regs[a];
      if (mrb_nil_p(base)) {
        base = mrb_obj_value(MRB_GET_CONTEXT(mrb)->ci->target_class);
      }
      c = mrb_vm_define_module(mrb, base, id);
      regs[a] = mrb_obj_value(c);
      ARENA_RESTORE(mrb, ai);
      NEXT;
    }

    CASE(OP_EXEC) {
      /* A Bx   R(A) := blockexec(R(A),SEQ[Bx]) */
      int a = GETARG_A(i);
      mrb_callinfo *ci;
      mrb_value recv = regs[a];
      struct RProc *p;

      /* prepare stack */
      ci = cipush(mrb);
      ci->pc = pc + 1;
      ci->acc = a;
      ci->mid = 0;
      ci->stackent = MRB_GET_CONTEXT(mrb)->stack;
      ci->argc = 0;
      ci->target_class = mrb_class_ptr(recv);

      /* prepare stack */
      MRB_GET_CONTEXT(mrb)->stack += a;

      p = mrb_proc_new(mrb, irep->reps[GETARG_Bx(i)]);
      p->target_class = ci->target_class;
      ci->proc = p;

      if (MRB_PROC_CFUNC_P(p)) {
        ci->nregs = 0;
        MRB_GET_CONTEXT(mrb)->stack[0] = p->body.func(mrb, recv);
        mrb_gc_arena_restore(mrb, ai);
        if (MRB_GET_VM(mrb)->exc) goto L_RAISE;
        /* pop stackpos */
        regs = MRB_GET_CONTEXT(mrb)->stack = MRB_GET_CONTEXT(mrb)->ci->stackent;
        cipop(mrb);
        NEXT;
      }
      else {
        irep = p->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        stack_extend(mrb, irep->nregs, 1);
        ci->nregs = irep->nregs;
        regs = MRB_GET_CONTEXT(mrb)->stack;
        pc = irep->iseq;
        JUMP;
      }
    }

    CASE(OP_METHOD) {
      /* A B            R(A).newmethod(Syms(B),R(A+1)) */
      int a = GETARG_A(i);
      struct RClass *c = mrb_class_ptr(regs[a]);

      mrb_define_method_vm(mrb, c, syms[GETARG_B(i)], regs[a+1]);
      ARENA_RESTORE(mrb, ai);
      NEXT;
    }

    CASE(OP_SCLASS) {
      /* A B    R(A) := R(B).singleton_class */
      regs[GETARG_A(i)] = mrb_singleton_class(mrb, regs[GETARG_B(i)]);
      ARENA_RESTORE(mrb, ai);
      NEXT;
    }

    CASE(OP_TCLASS) {
      /* A      R(A) := target_class */
      if (!MRB_GET_CONTEXT(mrb)->ci->target_class) {
        mrb_value exc = mrb_exc_new_str_lit(mrb, E_TYPE_ERROR, "no target class or module");
        MRB_GET_VM(mrb)->exc = mrb_obj_ptr(exc);
        goto L_RAISE;
      }
      regs[GETARG_A(i)] = mrb_obj_value(MRB_GET_CONTEXT(mrb)->ci->target_class);
      NEXT;
    }

    CASE(OP_RANGE) {
      /* A B C  R(A) := range_new(R(B),R(B+1),C) */
      int b = GETARG_B(i);
      regs[GETARG_A(i)] = mrb_range_new(mrb, regs[b], regs[b+1], GETARG_C(i));
      ARENA_RESTORE(mrb, ai);
      NEXT;
    }

    CASE(OP_DEBUG) {
      /* A B C    debug print R(A),R(B),R(C) */
#ifdef ENABLE_DEBUG
      MRB_GET_VM(mrb)->debug_op_hook(mrb, irep, pc, regs);
#else
#ifdef ENABLE_STDIO
      printf("OP_DEBUG %d %d %d\n", GETARG_A(i), GETARG_B(i), GETARG_C(i));
#else
      abort();
#endif
#endif
      NEXT;
    }

    CASE(OP_STOP) {
      /*        stop VM */
      mrb_value ret;
    L_STOP:
      {
        int eidx_stop = MRB_GET_CONTEXT(mrb)->ci == MRB_GET_CONTEXT(mrb)->cibase ? 0 : MRB_GET_CONTEXT(mrb)->ci[-1].eidx;
        int eidx = MRB_GET_CONTEXT(mrb)->ci->eidx;
        while (eidx > eidx_stop) {
          ecall(mrb, --eidx);
        }
      }
      ERR_PC_CLR(mrb);
      MRB_GET_THREAD_CONTEXT(mrb)->jmp = prev_jmp;
      if (MRB_GET_VM(mrb)->exc) {
        ret = mrb_obj_value(MRB_GET_VM(mrb)->exc);
#ifdef MRB_USE_GVL_API
        if (!is_gvl_acquired) {
          mrb_gvl_release(mrb);
        }
#endif
        return ret;
      }
      ret = regs[irep->nlocals];
#ifdef MRB_USE_GVL_API
      if (!is_gvl_acquired) {
        mrb_gvl_release(mrb);
      }
#endif
      return ret;
    }

    CASE(OP_ERR) {
      /* Bx     raise RuntimeError with message Lit(Bx) */
      mrb_value msg = mrb_str_dup(mrb, pool[GETARG_Bx(i)]);
      mrb_value exc;

      if (GETARG_A(i) == 0) {
        exc = mrb_exc_new_str(mrb, E_RUNTIME_ERROR, msg);
      }
      else {
        exc = mrb_exc_new_str(mrb, E_LOCALJUMP_ERROR, msg);
      }
      MRB_GET_VM(mrb)->exc = mrb_obj_ptr(exc);
      goto L_RAISE;
    }
  }
  END_DISPATCH;
  }
  MRB_CATCH(&c_jmp) {
    exc_catched = TRUE;
    goto RETRY_TRY_BLOCK;
  }
  MRB_END_EXC(&c_jmp);
}

MRB_API mrb_value
mrb_run(mrb_state *mrb, struct RProc *proc, mrb_value self)
{
  return mrb_context_run(mrb, proc, self, MRB_GET_CONTEXT(mrb)->ci->argc + 2); /* argc + 2 (receiver and block) */
}

MRB_API mrb_value
mrb_toplevel_run_keep(mrb_state *mrb, struct RProc *proc, unsigned int stack_keep)
{
  mrb_callinfo *ci;
  mrb_value v;

  if (!MRB_GET_CONTEXT(mrb)->cibase || MRB_GET_CONTEXT(mrb)->ci == MRB_GET_CONTEXT(mrb)->cibase) {
    return mrb_context_run(mrb, proc, mrb_top_self(mrb), stack_keep);
  }
  ci = cipush(mrb);
  ci->nregs = 1;   /* protect the receiver */
  ci->acc = CI_ACC_SKIP;
  ci->target_class = MRB_GET_VM(mrb)->object_class;
  v = mrb_context_run(mrb, proc, mrb_top_self(mrb), stack_keep);
  cipop(mrb);

  return v;
}

MRB_API mrb_value
mrb_toplevel_run(mrb_state *mrb, struct RProc *proc)
{
  return mrb_toplevel_run_keep(mrb, proc, 0);
}
