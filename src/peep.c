#include "global.h"
#include "types.h"
#include "language.h"
#include "stralloc.h"
#include "dynamic_buffer.h"
#include "program.h"
#include "las.h"
#include "docode.h"
#include "main.h"
#include "error.h"
#include "lex.h"
#include "peep.h"

struct p_instr_s
{
  short opcode;
  short line;
  struct pike_string *file;
  INT32 arg;
};

typedef struct p_instr_s p_instr;
static void asm_opt(void);

dynamic_buffer instrbuf;

static int hasarg(int opcode) { return instrs[opcode-F_OFFSET].hasarg; }

void init_bytecode()
{
  low_reinit_buf(&instrbuf);
}

void exit_bytecode()
{
  INT32 e,length;
  p_instr *c;

  c=(p_instr *)instrbuf.s.str;
  length=instrbuf.s.len / sizeof(p_instr);

  for(e=0;e<length;e++) free_string(c->file);
  
  toss_buffer(&instrbuf);
}

int insert_opcode(unsigned int f,
		  INT32 b,
		  INT32 current_line,
		  struct pike_string *current_file)
{
  p_instr *p;

#ifdef DEBUG
  if(!hasarg(f) && b)
    fatal("hasarg() is wrong!\n");
#endif

  p=(p_instr *)low_make_buf_space(sizeof(p_instr), &instrbuf);


#ifdef DEBUG
  if(!instrbuf.s.len)
    fatal("Low make buf space failed!!!!!!\n");
#endif

  p->opcode=f;
  p->line=current_line;
  copy_shared_string(p->file, current_file);
  p->arg=b;

  return p - (p_instr *)instrbuf.s.str;
}

int insert_opcode2(int f,int current_line, struct pike_string *current_file)
{
#ifdef DEBUG
  if(hasarg(f))
    fatal("hasarg() is wrong!\n");
#endif
  return insert_opcode(f,0,current_line, current_file);
}

void update_arg(int instr,INT32 arg)
{
  p_instr *p;
#ifdef DEBUG
  if(instr > (long)instrbuf.s.len / (long)sizeof(p_instr) || instr < 0)
    fatal("update_arg outside known space.\n");
#endif  
  p=(p_instr *)instrbuf.s.str;
  p[instr].arg=arg;
}


/**** Bytecode Generator *****/

void ins_f_byte(unsigned int b)
{
  if(store_linenumbers && b<F_MAX_OPCODE)
    ADD_COMPILED(b);

  b-=F_OFFSET;
#ifdef DEBUG
  if(b>255)
    error("Instruction too big %d\n",b);
#endif
  ins_byte((unsigned char)b,A_PROGRAM);
}

static void ins_f_byte_with_arg(unsigned int a,unsigned INT32 b)
{
  switch(b >> 8)
  {
  case 0 : break;
  case 1 : ins_f_byte(F_PREFIX_256); break;
  case 2 : ins_f_byte(F_PREFIX_512); break;
  case 3 : ins_f_byte(F_PREFIX_768); break;
  case 4 : ins_f_byte(F_PREFIX_1024); break;
  default:
    if( b < 256*256)
    {
      ins_f_byte(F_PREFIX_CHARX256);
      ins_byte(b>>8, A_PROGRAM);
    }else if(b < 256*256*256) {
      ins_f_byte(F_PREFIX_WORDX256);
      ins_byte(b >> 16, A_PROGRAM);
      ins_byte(b >> 8, A_PROGRAM);
    }else{
      ins_f_byte(F_PREFIX_24BITX256);
      ins_byte(b >> 24, A_PROGRAM);
      ins_byte(b >> 16, A_PROGRAM);
      ins_byte(b >> 8, A_PROGRAM);
    }
  }
  ins_f_byte(a);
  ins_byte(b, A_PROGRAM);
}

#define BRANCH_CASES \
         F_BRANCH_WHEN_EQ: \
    case F_BRANCH_WHEN_NE: \
    case F_BRANCH_WHEN_LT: \
    case F_BRANCH_WHEN_LE: \
    case F_BRANCH_WHEN_GT: \
    case F_BRANCH_WHEN_GE: \
    case F_BRANCH_WHEN_ZERO: \
    case F_BRANCH_WHEN_NON_ZERO: \
    case F_BRANCH: \
    case F_INC_LOOP: \
    case F_DEC_LOOP: \
    case F_INC_NEQ_LOOP: \
    case F_DEC_NEQ_LOOP: \
    case F_LAND: \
    case F_LOR: \
    case F_CATCH: \
    case F_FOREACH


void assemble(void)
{
  INT32 e,d,length,max_label,tmp;
  INT32 *labels, *jumps, *uses;
  p_instr *c;

  c=(p_instr *)instrbuf.s.str;
  length=instrbuf.s.len / sizeof(p_instr);

  max_label=-1;
  for(e=0;e<length;e++,c++)
    if(c->opcode == F_LABEL)
      if(c->arg > max_label)
	max_label = c->arg;



  labels=(INT32 *)xalloc(sizeof(INT32) * (max_label+1));
  jumps=(INT32 *)xalloc(sizeof(INT32) * (max_label+1));
  uses=(INT32 *)xalloc(sizeof(INT32) * (max_label+1));

  for(e=0;e<=max_label;e++)
  {
    labels[e]=jumps[e]=-1;
    uses[e]=0;
  }

  c=(p_instr *)instrbuf.s.str;
  for(e=0;e<length;e++)
    if(c[e].opcode == F_LABEL)
      labels[c[e].arg]=e;

  for(e=0;e<length;e++)
  {
    int tmp;
    switch(c[e].opcode)
    {
    case BRANCH_CASES:
    case F_POINTER:
      while(1)
      {
	tmp=labels[c[e].arg];
	
	while(c[tmp].opcode == F_LABEL ||
	      c[tmp].opcode == F_NOP) tmp++;
	
	if(c[tmp].opcode!=F_BRANCH) break;
	c[e].arg=c[tmp].arg;
      }
      uses[c[e].arg]++;
    }
  }

  for(e=0;e<=max_label;e++)
    if(!uses[e] && labels[e]>=0)
      c[labels[e]].opcode=F_NOP;

  asm_opt();

  c=(p_instr *)instrbuf.s.str;
  length=instrbuf.s.len / sizeof(p_instr);

  for(e=0;e<=max_label;e++) labels[e]=jumps[e]=-1;
  
  c=(p_instr *)instrbuf.s.str;
  for(e=0;e<length;e++)
  {
#ifdef DEBUG
    if(a_flag > 2 && store_linenumbers)
    {
      if(hasarg(c->opcode))
	fprintf(stderr,"===%3d %4x %s(%d)\n",c->line,PC,get_token_name(c->opcode),c->arg);
      else
	fprintf(stderr,"===%3d %4x %s\n",c->line,PC,get_token_name(c->opcode));
    }
#endif

    if(store_linenumbers)
      store_linenumber(c->line, c->file);

    switch(c->opcode)
    {
    case F_NOP: break;
    case F_ALIGN:
      while(PC % c->arg) ins_byte(0, A_PROGRAM);
      break;

    case F_BYTE:
      ins_byte(c->arg, A_PROGRAM);
      break;

    case F_LABEL:
#ifdef DEBUG
      if(c->arg > max_label || c->arg < 0)
	fatal("max_label calculation failed!\n");

      if(labels[c->arg] != -1)
	fatal("Duplicate label!\n");
#endif
      labels[c->arg]=PC;
      break;

    case BRANCH_CASES:
      ins_f_byte(c->opcode);

    case F_POINTER:
#ifdef DEBUG
      if(c->arg > max_label || c->arg < 0) fatal("Jump to unknown label?\n");
#endif
      tmp=PC;
      ins_int(jumps[c->arg],A_PROGRAM);
      jumps[c->arg]=tmp;
      break;

    default:
      if(hasarg(c->opcode))
	ins_f_byte_with_arg(c->opcode, c->arg);
      else
	ins_f_byte(c->opcode);
      break;
    }
    
    c++;
  }

  for(e=0;e<=max_label;e++)
  {
    int tmp2=labels[e];

    while(jumps[e]!=-1)
    {
#ifdef DEBUG
      if(labels[e]==-1)
	fatal("Hyperspace error: unknown jump point.\n");
#endif
      tmp=read_int(jumps[e]);
      upd_int(jumps[e], tmp2 - jumps[e]);
      jumps[e]=tmp;
    }
  }

  free((char *)labels);
  free((char *)jumps);
  free((char *)uses);


  exit_bytecode();
}

/**** Peephole optimizer ****/

static int fifo_len, eye,len;
static p_instr *instructions;

static void debug()
{
  if(fifo_len > (long)instrbuf.s.len / (long)sizeof(p_instr))
    fifo_len=(long)instrbuf.s.len / (long)sizeof(p_instr);
#ifdef DEBUG
  if(eye < 0)
    fatal("Popped beyond start of code.\n");

  if(instrbuf.s.len)
  {
    p_instr *p;
    p=(p_instr *)low_make_buf_space(0, &instrbuf);
    if(!p[-1].file)
      fatal("No file name on last instruction!\n");
  }
#endif
}


static p_instr *instr(int offset)
{
  p_instr *p;

  debug();

  if(offset >= 0)
  {
    if(offset < fifo_len)
    {
      p=(p_instr *)low_make_buf_space(0, &instrbuf);
      p-=fifo_len;
      p+=offset;
      return p;
    }else{
      offset-=fifo_len;
      offset+=eye;
      if(offset >= len) return 0;
      return instructions+offset;
    }
  }else{
    fatal("Can't handle negative offsets in peephole optimizer!\n");
    return 0; /* Make GCC happy */
  }
}

static int opcode(int offset)
{
  p_instr *a;
  a=instr(offset);
  if(a) return a->opcode;
  return -1;
}

static int argument(int offset)
{
  p_instr *a;
  a=instr(offset);
  if(a) return a->arg;
  return -1;
}

static void advance()
{
  if(fifo_len)
  {
    fifo_len--;
  }else{
    p_instr *p;
    if(p=instr(0))
      insert_opcode(p->opcode, p->arg, p->line, p->file);
    eye++;
  }
  debug();
}

static void pop_n_opcodes(int n)
{
  int e,d;
  if(fifo_len)
  {
    p_instr *p;

    d=fifo_len;
    if(d>n) d=n;
#ifdef DEBUG
    if((long)d > (long)instrbuf.s.len / (long)sizeof(p_instr))
      fatal("Popping out of instructions.\n");
#endif

    low_make_buf_space(-((INT32)sizeof(p_instr))*fifo_len, &instrbuf);
    p=(p_instr *)low_make_buf_space(0, &instrbuf);
    for(e=0;e<d;e++) free_string(p[e].file);
    fifo_len-=d;
    if(fifo_len) MEMMOVE(p,p+d,fifo_len*sizeof(p_instr));
    n-=d;
  }
  eye+=n;
}

static void dofix()
{
  p_instr *p,tmp;
  int e;

  if(fifo_len)
  {
    p=(p_instr *)low_make_buf_space(0, &instrbuf);
    tmp=p[-1];
    for(e=0;e<fifo_len;e++)
      p[-1-e]=p[-2-e];
    p[-1-e]=tmp;
  }
}

static void asm_opt(void)
{
#ifdef DEBUG
  if(a_flag > 3)
  {
    p_instr *c;
    INT32 e,length;
    c=(p_instr *)instrbuf.s.str;
    length=instrbuf.s.len / sizeof(p_instr);

    fprintf(stderr,"Optimization begins: \n");
    for(e=0;e<length;e++,c++)
    {
      if(hasarg(c->opcode))
	fprintf(stderr,"---%3d: %s(%d)\n",c->line,get_token_name(c->opcode),c->arg);
      else
	fprintf(stderr,"---%3d: %s\n",c->line,get_token_name(c->opcode));
    }
  }
#endif

#include "peep_engine.c"


#ifdef DEBUG
  if(a_flag > 4)
  {
    p_instr *c;
    INT32 e,length;
    c=(p_instr *)instrbuf.s.str;
    length=instrbuf.s.len / sizeof(p_instr);

    fprintf(stderr,"Optimization begins: \n");
    for(e=0;e<length;e++,c++)
    {
      if(hasarg(c->opcode))
	fprintf(stderr,">>>%3d: %s(%d)\n",c->line,get_token_name(c->opcode),c->arg);
      else
	fprintf(stderr,">>>%3d: %s\n",c->line,get_token_name(c->opcode));
    }
  }
#endif
}

