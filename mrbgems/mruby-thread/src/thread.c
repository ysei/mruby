#ifdef MRB_USE_THREAD_API

#include "mruby.h"
#include "mruby/class.h"
#include "mruby/data.h"
#include "mruby/proc.h"
#include "mruby/value.h"
#include "mruby/thread.h"

static const char THREAD_CLASSNAME[] = "Thread";

typedef struct mrb_thread_context_t {
  mrb_state    *vm;
  mrb_value     proc;
  mrb_int       argc;
  mrb_value    *argv;
  mrb_value     result;
} mrb_thread_context_t;

typedef struct mrb_thread_data_t {
  mrb_thread_t         *thread;
  mrb_thread_context_t *context;
} mrb_thread_data_t;

static void
mrb_free_thread_data(mrb_state *mrb, void *ptr)
{
  mrb_thread_data_t *data = (mrb_thread_data_t*)ptr;
  if (data) {
    if (data->thread) {
      mrb_thread_join(mrb, data->thread, 0);
      mrb_thread_destroy(mrb, data->thread);
    }
    if (data->context) {
      mrb_free(mrb, data->context);
    }
    mrb_free(mrb, data);
  }
}

static const struct mrb_data_type mrb_thread_data_type = {
  THREAD_CLASSNAME, mrb_free_thread_data
};

static void*
mrb_thread_obj_func(mrb_state *mrb, void *arg)
{
  mrb_thread_context_t *context = (mrb_thread_context_t*)arg;
  mrb_state *vm = context->vm;
  context->result = mrb_yield_with_class(vm, context->proc, context->argc, context->argv, mrb_nil_value(), MRB_GET_VM(mrb)->object_class);
  return 0;
}

static mrb_value
mrb_thread_obj_init(mrb_state *mrb, mrb_value self)
{
  mrb_value proc;
  mrb_int argc;
  mrb_value *argv;
  mrb_thread_context_t *context;
  mrb_thread_data_t *data;
  mrb_int i;
  struct RProc *new_proc;

  mrb_get_args(mrb, "&*", &proc, &argv, &argc);
  if (mrb_nil_p(proc)) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "invalid parameter.");
  }

  data = (mrb_thread_data_t*)DATA_PTR(self);
  if (data) {
    mrb_free_thread_data(mrb, data);
  }
  mrb_data_init(self, 0, &mrb_thread_data_type);

  new_proc = mrb_proc_new(mrb, mrb_proc_ptr(proc)->body.irep);
  data = (mrb_thread_data_t*)mrb_malloc(mrb, sizeof(mrb_thread_data_t));
  context = (mrb_thread_context_t*)mrb_malloc(mrb, sizeof(mrb_thread_context_t));
  context->vm   = mrb_thread_attach_vm(mrb);
  context->proc = mrb_obj_value(new_proc);
  context->argc = argc;
  if (argc > 0) {
    context->argv = (mrb_value*)mrb_malloc(mrb, sizeof(mrb_value) * argc);
  } else {
    context->argv = 0;
  }
  context->result = mrb_nil_value();

  for (i = 0; i < argc; ++i) {
    context->argv[i] = argv[i];
    mrb_gc_protect(context->vm, argv[i]);
  }

  mrb_gc_protect(context->vm, context->proc);

  data->thread = mrb_thread_create(mrb, 0, mrb_thread_obj_func, context);
  data->context = context;

  mrb_data_init(self, data, &mrb_thread_data_type);

  return self;
}

static mrb_value
mrb_thread_obj_join(mrb_state *mrb, mrb_value self)
{
  mrb_value val;
  mrb_thread_data_t *data = (mrb_thread_data_t*)DATA_PTR(self);
  if (!data) {
    return mrb_nil_value();
  }
  mrb_thread_join(mrb, data->thread, 0);
  mrb_thread_destroy(mrb, data->thread);
  mrb_thread_detach_vm(data->context->vm);
  data->thread = 0;
  val = data->context->result;
  mrb_free(mrb, data->context->argv);
  mrb_free(mrb, data->context);
  data->context = 0;
  return val;
}

void
mrb_mruby_thread_gem_init(mrb_state *mrb)
{
  struct RClass *c = mrb_define_class(mrb, THREAD_CLASSNAME, MRB_GET_VM(mrb)->object_class);

  MRB_SET_INSTANCE_TT(c, MRB_TT_DATA);

  mrb_define_method(mrb, c, "initialize", mrb_thread_obj_init, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, c, "join",       mrb_thread_obj_join, MRB_ARGS_NONE());
}

void
mrb_mruby_thread_gem_final(mrb_state *mrb)
{
}

#endif
