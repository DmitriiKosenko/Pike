/*
 * $Id: gc.h,v 1.39 2000/04/27 02:13:28 hubbe Exp $
 */
#ifndef GC_H
#define GC_H

#include "global.h"
#include "callback.h"
#include "queue.h"
#include "threads.h"

extern struct pike_queue gc_mark_queue;
extern INT32 num_objects;
extern INT32 num_allocs;
extern INT32 alloc_threshold;
extern int Pike_in_gc;

extern struct callback *gc_evaluator_callback;
extern struct callback_list evaluator_callbacks;
#ifdef PIKE_DEBUG
extern void *gc_svalue_location;
#endif

#define ADD_GC_CALLBACK() gc_evaluator_callback=add_to_callback(&evaluator_callbacks,(callback_func)do_gc,0,0)

#define LOW_GC_ALLOC(OBJ) do {						\
 extern int d_flag;							\
 num_objects++;								\
 num_allocs++;								\
 DO_IF_DEBUG(								\
   if(d_flag) CHECK_INTERPRETER_LOCK();					\
   if(Pike_in_gc > GC_PASS_PREPARE && Pike_in_gc <= GC_PASS_MARK)	\
     fatal("Allocating new objects within gc is not allowed!\n");	\
 )									\
 if (Pike_in_gc) remove_marker(OBJ);					\
} while (0)

#ifdef ALWAYS_GC
#define GC_ALLOC(OBJ) do{						\
  LOW_GC_ALLOC(OBJ);							\
  if(!gc_evaluator_callback) ADD_GC_CALLBACK();				\
} while(0)
#else
#define GC_ALLOC(OBJ)  do{						\
  LOW_GC_ALLOC(OBJ);							\
  if(num_allocs == alloc_threshold && !gc_evaluator_callback)		\
    ADD_GC_CALLBACK();							\
} while(0)
#endif

struct marker
{
  struct marker *next;
  INT32 refs;			/* Internal references. */
#ifdef PIKE_DEBUG
  INT32 xrefs;			/* Known external references. */
  INT32 saved_refs;		/* Object refcount during check and mark pass. */
#endif
  INT32 flags;
  void *data;
};

#include "block_alloc_h.h"
PTR_HASH_ALLOC(marker,MARKER_CHUNK_SIZE)

/* Prototypes begin here */
struct callback *debug_add_gc_callback(callback_func call,
				 void *arg,
				 callback_func free_func);
void dump_gc_info(void);
TYPE_T attempt_to_identify(void *something);
void describe_location(void *memblock, int type, void *location,int indent, int depth, int flags);
void debug_gc_xmark_svalues(struct svalue *s, int num, char *fromwhere);
TYPE_FIELD debug_gc_check_svalues(struct svalue *s, int num, TYPE_T t, void *data);
void debug_gc_check_short_svalue(union anything *u, TYPE_T type, TYPE_T t, void *data);
int debug_gc_check(void *x, TYPE_T t, void *data);
void describe_something(void *a, int t, int indent, int depth, int flags);
void describe(void *x);
void debug_describe_svalue(struct svalue *s);
void debug_gc_touch(void *a);
INT32 real_gc_check(void *a);
void locate_references(void *a);
int debug_gc_is_referenced(void *a);
int gc_external_mark3(void *a, void *in, char *where);
int gc_mark(void *a);
int debug_gc_do_free(void *a);
void do_gc(void);
void f__gc_status(INT32 args);
/* Prototypes end here */

#define gc_check_svalues(S,N) real_gc_check_svalues(debug_malloc_pass(S),N)
#define gc_check_short_svalue(S,N) real_gc_check_short_svalue(debug_malloc_pass(S),N)
#define gc_xmark_svalues(S,N) real_gc_xmark_svalues(debug_malloc_pass(S),N)
#define gc_check(VP) real_gc_check(debug_malloc_pass(VP))

#define LOW_GC_FREE(X) do {				\
  DO_IF_DEBUG(						\
    extern int d_flag;					\
    if(d_flag) CHECK_INTERPRETER_LOCK();		\
    if(num_objects < 1)					\
      fatal("Panic!! less than zero objects!\n");	\
  )							\
  num_objects-- ;					\
  if(Pike_in_gc) remove_marker(X);			\
}while(0)

#define GC_FREE(X) do {						\
  DO_IF_DEBUG(							\
    if(Pike_in_gc == GC_PASS_MARK)				\
      fatal("Freeing objects within gc is not allowed!\n");	\
  )								\
  LOW_GC_FREE((X));						\
}while(0)

#ifndef PIKE_DEBUG
#define debug_gc_check_svalues(S,N,T,V) gc_check_svalues((S),N)
#define debug_gc_check_short_svalue(S,N,T,V) gc_check_short_svalue((S),N)
#define debug_gc_xmark_svalues(S,N,X) gc_xmark_svalues((S),N)
#define debug_gc_check(VP,T,V) gc_check((VP))
#endif


#define gc_external_mark2(X,Y,Z) gc_external_mark3( debug_malloc_pass(X),(Y),(Z))
#define gc_external_mark(X) gc_external_mark2( (X),"externally", 0)

#define add_gc_callback(X,Y,Z) \
  dmalloc_touch(struct callback *,debug_add_gc_callback((X),(Y),(Z)))

#define GC_REFERENCED 1
#define GC_XREFERENCED 2
#define GC_CHECKED 4
#define GC_OBJ_DESTROY_CHECK 8
#define GC_DO_FREE_OBJ 16
#define GC_TOUCHED 32

#define GC_PASS_PREPARE		 50
#define GC_PASS_PRETOUCH	 90
#define GC_PASS_CHECK		100
#define GC_PASS_MARK		200
#define GC_PASS_DESTROY		300
#define GC_PASS_FREE		400
#define GC_PASS_DESTRUCT	500
#define GC_PASS_POSTTOUCH	600

#define GC_PASS_LOCATE -1

#ifdef PIKE_DEBUG
#define gc_is_referenced(X) debug_gc_is_referenced(debug_malloc_pass(X))
#define gc_do_free(X) debug_gc_do_free(debug_malloc_pass(X))
#else
#define gc_is_referenced(X) (get_marker(X)->refs < *(INT32 *)(X))
#define gc_do_free(X) ( (get_marker(X)->flags & (GC_REFERENCED|GC_CHECKED)) == GC_CHECKED )
#endif

#endif
