#include "global.h"

int num_threads = 1;
int threads_disabled = 0;

#ifdef _REENTRANT
#include "threads.h"
#include "array.h"
#include "object.h"
#include "macros.h"
#include "callback.h"

struct object *thread_id;
static struct callback *threads_evaluator_callback=0;

MUTEX_T interpreter_lock = PTHREAD_MUTEX_INITIALIZER;
struct program *mutex_key = 0;
struct program *thread_id_prog = 0;
pthread_attr_t pattr;

struct thread_starter
{
  struct object *id;
  struct array *args;
};

static void check_threads(struct callback *cb, void *arg)
{
  THREADS_ALLOW();

  /* Allow other threads to run */

  THREADS_DISALLOW();
}

void *new_thread_func(void * data)
{
  struct thread_starter arg = *(struct thread_starter *)data;
  JMP_BUF back;
  INT32 tmp;

  free((char *)data);

  if(tmp=mt_lock( & interpreter_lock))
    fatal("Failed to lock interpreter, errno %d\n",tmp);

  init_interpreter();

  thread_id=arg.id;

  if(SETJMP(back))
  {
    ONERROR tmp;
    SET_ONERROR(tmp,exit_on_error,"Error in handle_error in master object!");
    assign_svalue_no_free(sp++, & throw_value);
    APPLY_MASTER("handle_error", 1);
    pop_stack();
    UNSET_ONERROR(tmp);
  } else {
    INT32 args=arg.args->size;
    
    push_array_items(arg.args);
    f_call_function(args);
    arg.args=0;
  }

  UNSETJMP(back);

  destruct(thread_id);

  free_object(thread_id);
  thread_id=0;

  cleanup_interpret();
  num_threads--;
  if(!num_threads)
  {
    remove_callback(threads_evaluator_callback);
    threads_evaluator_callback=0;
  }
  mt_unlock(& interpreter_lock);
  th_exit(0);
}


void f_thread_create(INT32 args)
{
  pthread_t dummy;
  struct thread_starter *arg;
  int tmp;
  arg=ALLOC_STRUCT(thread_starter);
  arg->args=aggregate_array(args);
  arg->id=clone(thread_id_prog,0);
  tmp=th_create(&dummy,new_thread_func,arg);
  if(!tmp)
  {
    num_threads++;

    if(num_threads == 1 && !threads_evaluator_callback)
    {
      threads_evaluator_callback=add_to_callback(&evaluator_callbacks,
						 check_threads, 0,0);
    }

    push_object(arg->id);
    arg->id->refs++;
  } else {
    free_object(arg->id);
    free_array(arg->args);
    free((char *)arg);
    push_int(0);
  }
}

void f_this_thread(INT32 args)
{
  pop_n_elems(args);
  push_object(thread_id);
  thread_id->refs++;
}

void th_init()
{
  mt_lock( & interpreter_lock);
  pthread_attr_init(&pattr);
  pthread_attr_setstacksize(&pattr, 2 * 1024 * 1204);
  pthread_attr_setdetachstate(&pattr, PTHREAD_CREATE_DETACHED);

  add_efun("thread_create",f_thread_create,"function(mixed ...:int)",OPT_SIDE_EFFECT);
  add_efun("this_thread",f_this_thread,"function(:object)",OPT_EXTERNAL_DEPEND);
}


#define THIS_MUTEX ((struct mutex_storage *)(fp->current_storage))


/* Note:
 * No reference is kept to the key object, it is destructed if the
 * mutex is destructed. The key pointer is set to zero by the
 * key object when the key is destructed.
 */

struct mutex_storage
{
  COND_T condition;
  struct object *key;
};

struct key_storage
{
  struct mutex_storage *mut;
  int initialized;
};

static MUTEX_T mutex_kluge = PTHREAD_MUTEX_INITIALIZER;

#define OB2KEY(X) ((struct key_storage *)((X)->storage))

void f_mutex_lock(INT32 args)
{
  struct mutex_storage  *m;
  struct object *o;

  pop_n_elems(args);
  m=THIS_MUTEX;
  o=clone(mutex_key,0);
  mt_lock(& mutex_kluge);
  THREADS_ALLOW();
  while(m->key) co_wait(& m->condition, & mutex_kluge);
  OB2KEY(o)->mut=m;
  m->key=o;
  mt_unlock(&mutex_kluge);
  THREADS_DISALLOW();
  push_object(o);
}

void f_mutex_trylock(INT32 args)
{
  struct mutex_storage  *m;
  struct object *o;
  int i=0;
  pop_n_elems(args);

  o=clone(mutex_key,0);
  m=THIS_MUTEX;
  mt_lock(& mutex_kluge);
  THREADS_ALLOW();
  if(!m->key)
  {
    OB2KEY(o)->mut=THIS_MUTEX;
    m->key=o;
    i=1;
  }
  mt_unlock(&mutex_kluge);
  THREADS_DISALLOW();
  
  if(i)
  {
    push_object(o);
  } else {
    destruct(o);
    free_object(o);
    push_int(0);
  }
}

void init_mutex_obj(struct object *o)
{
  co_init(& THIS_MUTEX->condition);
  THIS_MUTEX->key=0;
}

void exit_mutex_obj(struct object *o)
{
  if(THIS_MUTEX->key) destruct(THIS_MUTEX->key);
  co_destroy(& THIS_MUTEX->condition);
}

#define THIS_KEY ((struct key_storage *)(fp->current_storage))
void init_mutex_key_obj(struct object *o)
{
  THIS_KEY->mut=0;
  THIS_KEY->initialized=1;
}

void exit_mutex_key_obj(struct object *o)
{
  mt_lock(& mutex_kluge);
  if(THIS_KEY->mut)
  {
#ifdef DEBUG
    if(THIS_KEY->mut->key != o)
      fatal("Mutex unlock from wrong key %p != %p!\n",THIS_KEY->mut->key,o);
#endif
    THIS_KEY->mut->key=0;
    co_signal(& THIS_KEY->mut->condition);
    THIS_KEY->mut=0;
    THIS_KEY->initialized=0;
  }
  mt_unlock(& mutex_kluge);
}

#define THIS_COND ((COND_T *)(fp->current_storage))
void f_cond_wait(INT32 args)
{
  COND_T *c;
  struct object *key;

  if(args > 1) pop_n_elems(args - 1);

  c=THIS_COND;

  if(args > 0)
  {
    struct mutex_storage *mut;

    if(sp[-1].type != T_OBJECT)
      error("Bad argument 1 to condition->wait()\n");
    
    key=sp[-1].u.object;
    
    if(key->prog != mutex_key)
      error("Bad argument 1 to condition->wait()\n");
    
    mt_lock(&mutex_kluge);
    mut=OB2KEY(key)->mut;
    THREADS_ALLOW();

    /* Unlock mutex */
    mut->key=0;
    OB2KEY(key)->mut=0;
    co_signal(& mut->condition);

    /* Wait and allow mutex operations */
    co_wait(c,&mutex_kluge);

    if(OB2KEY(key)->initialized)
    {
      /* Lock mutex */
      while(mut->key) co_wait(& mut->condition, & mutex_kluge);
      mut->key=key;
      OB2KEY(key)->mut=mut;
    }
    mt_unlock(&mutex_kluge);
    THREADS_DISALLOW();
    pop_stack();
  } else {
    THREADS_ALLOW();
    co_wait(c, 0);
    THREADS_DISALLOW();
  }
}

void f_cond_signal(INT32 args) { pop_n_elems(args); co_signal(THIS_COND); }
void f_cond_broadcast(INT32 args) { pop_n_elems(args); co_broadcast(THIS_COND); }
void init_cond_obj(struct object *o) { co_init(THIS_COND); }
void exit_cond_obj(struct object *o) { co_destroy(THIS_COND); }

void th_init_programs()
{
  start_new_program();
  add_storage(sizeof(struct mutex_storage));
  add_function("lock",f_mutex_lock,"function(:object)",0);
  add_function("trylock",f_mutex_trylock,"function(:object)",0);
  set_init_callback(init_mutex_obj);
  set_exit_callback(exit_mutex_obj);
  end_c_program("/precompiled/mutex");

  start_new_program();
  add_storage(sizeof(struct key_storage));
  set_init_callback(init_mutex_key_obj);
  set_exit_callback(exit_mutex_key_obj);
  mutex_key=end_c_program("/precompiled/mutex_key");
  mutex_key->refs++;

  start_new_program();
  add_storage(sizeof(COND_T));
  add_function("wait",f_cond_wait,"function(void|object:void)",0);
  add_function("signal",f_cond_signal,"function(:void)",0);
  add_function("broadcast",f_cond_broadcast,"function(:void)",0);
  set_init_callback(init_cond_obj);
  set_exit_callback(exit_cond_obj);
  end_c_program("/precompiled/condition");

  start_new_program();
  thread_id_prog=end_c_program("/precompiled/thread");
  thread_id_prog->refs++;

  thread_id=clone(thread_id_prog,0);
}

void th_cleanup()
{
  if(mutex_key)
  {
    free_program(mutex_key);
    mutex_key=0;
  }

  if(thread_id_prog)
  {
    free_program(thread_id_prog);
    thread_id_prog=0;
  }

  if(thread_id)
  {
    destruct(thread_id);
    free_object(thread_id);
    thread_id=0;
  }
}

#endif
