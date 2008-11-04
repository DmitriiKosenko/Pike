/*
|| This file is part of Pike. For copyright information see COPYRIGHT.
|| Pike is distributed under GPL, LGPL and MPL. See the file COPYING
|| for more information.
|| $Id: odbc_result.c,v 1.49 2008/11/04 13:55:09 grubba Exp $
*/

/*
 * Pike interface to ODBC compliant databases
 *
 * Henrik Grubbström
 */

/*
 * Includes
 */

#include "global.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

RCSID("$Id: odbc_result.c,v 1.49 2008/11/04 13:55:09 grubba Exp $");

#include "interpret.h"
#include "object.h"
#include "threads.h"
#include "stralloc.h"
#include "mapping.h"
#include "array.h"
#include "multiset.h"
#include "program.h"
#include "array.h"
#include "operators.h"
#include "builtin_functions.h"
#include "pike_memory.h"
#include "pike_macros.h"
#include "module_support.h"
#include "bignum.h"

#include "precompiled_odbc.h"

#ifdef HAVE_ODBC

/* #define ODBC_DEBUG */

/*
 * Constants
 */

/* Buffer size used when retrieving BLOBs
 *
 * Allow it to be specified from the command line.
 */
#ifndef BLOB_BUFSIZ
#define BLOB_BUFSIZ	1024
#endif /* !BLOB_BUFSIZ */

/*
 * Globals
 */

struct program *odbc_result_program = NULL;

/*
 * Functions
 */

/*
 * Help functions
 */

static void clean_sql_res(void)
{
  if (PIKE_ODBC_RES->field_info) {
    free(PIKE_ODBC_RES->field_info);
    PIKE_ODBC_RES->field_info = NULL;
  }
  if (PIKE_ODBC_RES->fields) {
    free_array(PIKE_ODBC_RES->fields);
    PIKE_ODBC_RES->fields = NULL;
  }
  if (PIKE_ODBC_RES->obj) {
    free_object(PIKE_ODBC_RES->obj);
    PIKE_ODBC_RES->obj = NULL;
    PIKE_ODBC_RES->odbc = NULL;
  }
  PIKE_ODBC_RES->hstmt = SQL_NULL_HSTMT;
}

static INLINE void odbc_check_error(const char *fun, const char *msg,
				    RETCODE code,
				    void (*clean)(void *), void *clean_arg)
{
  if ((code != SQL_SUCCESS) && (code != SQL_SUCCESS_WITH_INFO)) {
    odbc_error(fun, msg, PIKE_ODBC_RES->odbc, PIKE_ODBC_RES->hstmt,
	       code, clean, clean_arg);
  }
}

/*
 * State maintenance
 */
 
static void init_res_struct(struct object *o)
{
  memset(PIKE_ODBC_RES, 0, sizeof(struct precompiled_odbc_result));
  PIKE_ODBC_RES->hstmt = SQL_NULL_HSTMT;
}
 
static void exit_res_struct(struct object *o)
{
  if (PIKE_ODBC_RES->hstmt != SQL_NULL_HSTMT) {
    SQLHSTMT hstmt = PIKE_ODBC_RES->hstmt;
    RETCODE code;
    PIKE_ODBC_RES->hstmt = SQL_NULL_HSTMT;
    ODBC_ALLOW();
    code = SQLFreeStmt(hstmt, SQL_DROP);
    ODBC_DISALLOW();
    odbc_check_error("exit_res_struct", "Freeing of HSTMT failed",
		     code, (void (*)(void *))clean_sql_res, NULL);
  }
  clean_sql_res();
}

/*
 * More help functions
 */

static void odbc_fix_fields(void)
{
  SQLHSTMT hstmt = PIKE_ODBC_RES->hstmt;
  int i;
  SWORD *odbc_field_types = alloca(sizeof(SWORD) * PIKE_ODBC_RES->num_fields);
  size_t buf_size = 1024;
#ifdef SQL_WCHAR
  SQLWCHAR *buf = alloca(buf_size * sizeof(SQLWCHAR));
#else
  unsigned char *buf = alloca(buf_size);
#endif

  if ((!buf)||(!odbc_field_types)) {
    Pike_error("odbc_fix_fields(): Out of memory\n");
  }

  /*
   * First build the fields-array;
   */
  for (i=0; i < PIKE_ODBC_RES->num_fields; i++) {
    int nbits;
    SWORD name_len = 0;
    SWORD sql_type;
    SQLULEN precision;
    SWORD scale;
    SWORD nullable = 0;

    while (1) {
      RETCODE code;
      ODBC_ALLOW();
      code =
#ifdef SQL_WCHAR
	SQLDescribeColW
#else
	SQLDescribeCol
#endif
	(hstmt, i+1,
	 buf,
	 DO_NOT_WARN((SQLSMALLINT)buf_size),
	 &name_len,
	 &sql_type, &precision, &scale, &nullable);
      ODBC_DISALLOW();
      odbc_check_error("odbc_fix_fields", "Failed to fetch field info",
		       code, NULL, NULL);
      if (name_len
#ifdef SQL_WCHAR
	  * (ptrdiff_t)sizeof(SQLWCHAR)
#endif
	  < (ptrdiff_t)buf_size) {
	break;
      }
      do {
	buf_size *= 2;
      } while (name_len
#ifdef SQL_WCHAR
	       * (ptrdiff_t)sizeof(SQLWCHAR)
#endif
	       >= (ptrdiff_t)buf_size);
      if (!(buf = alloca(
			 buf_size
#ifdef SQL_WCHAR
			 * sizeof(SQLWCHAR)
#endif
			 ))) {
	Pike_error("odbc_fix_fields(): Out of memory\n");
      }
    }
#ifdef ODBC_DEBUG
    fprintf(stderr, "ODBC:odbc_fix_fields():\n"
#ifdef SQL_WCHAR
	    "name:%ls\n"
#else
	    "name:%s\n"
#endif
	    "", buf);
    fprintf(stderr,
	    "name_len:%d\n"
	    "sql_type:%d\n"
	    "precision:%ld\n"
	    "scale:%d\n"
	    "nullable:%d\n",
	    name_len, sql_type, precision, scale, nullable);
#endif /* ODBC_DEBUG */
    /* Create the mapping */
    push_text("name");
#ifdef SQL_WCHAR
    push_sqlwchar(buf, name_len);
#else
    push_string(make_shared_binary_string((char *)buf, name_len));
#endif
    push_text("type");
#ifdef SQL_WCHAR
#ifdef ODBC_DEBUG
    fprintf(stderr, "SQL_C_WCHAR\n");
#endif /* ODBC_DEBUG */
    odbc_field_types[i] = SQL_C_WCHAR;
#else
#ifdef ODBC_DEBUG
    fprintf(stderr, "SQL_C_CHAR\n");
#endif /* ODBC_DEBUG */
    odbc_field_types[i] = SQL_C_CHAR;
#endif
    switch(sql_type) {
    case SQL_CHAR:
      push_text("char");
      break;
    case SQL_NUMERIC:
      push_text("numeric");
      break;
    case SQL_DECIMAL:
      push_text("decimal");
      break;
    case SQL_INTEGER:
      push_text("integer");
      break;
    case SQL_SMALLINT:
      push_text("short");
      break;
    case SQL_FLOAT:
      push_text("float");
      break;
    case SQL_REAL:
      push_text("real");
      break;
    case SQL_DOUBLE:
      push_text("double");
      break;
    case SQL_VARCHAR:
      push_text("var string");
      break;
    case SQL_DATE:
      push_text("date");
      break;
    case SQL_TIMESTAMP:
      push_text("time");
      break;
    case SQL_LONGVARCHAR:
      push_text("var string");
      break;
    case SQL_BINARY:
      push_text("binary");
#ifdef ODBC_DEBUG
      fprintf(stderr, "SQL_C_BINARY\n");
#endif /* ODBC_DEBUG */
      odbc_field_types[i] = SQL_C_BINARY;
      break;
    case SQL_VARBINARY:
      push_text("blob");
#ifdef ODBC_DEBUG
      fprintf(stderr, "SQL_C_BINARY\n");
#endif /* ODBC_DEBUG */
      odbc_field_types[i] = SQL_C_BINARY;
      break;
    case SQL_LONGVARBINARY:
      push_text("long blob");
#ifdef ODBC_DEBUG
      fprintf(stderr, "SQL_C_BINARY\n");
#endif /* ODBC_DEBUG */
      odbc_field_types[i] = SQL_C_BINARY;
      break;
    case SQL_BIGINT:
      push_text("long integer");
      break;
    case SQL_TINYINT:
      push_text("tiny integer");
      break;
    case SQL_BIT:
      push_text("bit");
      break;
    default:
      push_text("unknown");
      break;
    }
    push_text("length"); push_int64(precision);
    push_text("decimals"); push_int(scale);
    push_text("flags");
    nbits = 0;
    if (nullable == SQL_NULLABLE) {
      nbits++;
      push_text("nullable");
    }
    if ((sql_type == SQL_LONGVARCHAR) ||
	(sql_type == SQL_LONGVARBINARY)) {
      nbits++;
      push_text("blob");
    }
    f_aggregate_multiset(nbits);

    f_aggregate_mapping(5*2);
  }
  f_aggregate(PIKE_ODBC_RES->num_fields);

  add_ref(PIKE_ODBC_RES->fields = Pike_sp[-1].u.array);
  pop_stack();

  PIKE_ODBC_RES->field_info = (struct field_info *)
    xalloc(sizeof(struct field_info) * PIKE_ODBC_RES->num_fields);

  /*
   * Now it's time to bind the columns
   */
  for (i=0; i < PIKE_ODBC_RES->num_fields; i++)
    PIKE_ODBC_RES->field_info[i].type = odbc_field_types[i];
}

/*
 * Methods
 */

/* void create(object(odbc)) */
static void f_create(INT32 args)
{
  SQLHSTMT hstmt = SQL_NULL_HSTMT;

  if (!args) {
    Pike_error("Too few arguments to odbc_result()\n");
  }
  if ((Pike_sp[-args].type != T_OBJECT) ||
      (!(PIKE_ODBC_RES->odbc =
	 (struct precompiled_odbc *)get_storage(Pike_sp[-args].u.object,
						odbc_program)))) {
    Pike_error("Bad argument 1 to odbc_result()\n");
  }
  add_ref(PIKE_ODBC_RES->obj = Pike_sp[-args].u.object);

  /* It's doubtful if this call really can block, but we play safe. /mast */
  {
    HDBC hdbc = PIKE_ODBC_RES->odbc->hdbc;
    RETCODE code;
    ODBC_ALLOW();
    code = SQLAllocStmt(hdbc, &hstmt);
    ODBC_DISALLOW();
    odbc_check_error("odbc_result", "Statement allocation failed",
		     code, NULL, NULL);
  }
  PIKE_ODBC_RES->hstmt = hstmt;
}

static void f_execute(INT32 args)
{
  struct pike_string *q = NULL;
  SQLHSTMT hstmt = PIKE_ODBC_RES->hstmt;
  RETCODE code;
  const char *err_msg = NULL;
  SWORD num_fields;
  SQLLEN num_rows;

#ifdef SQL_WCHAR
  get_all_args("odbc_result->execute", args, "%W", &q);
  if ((q->size_shift > 1) && (sizeof(SQLWCHAR) == 2)) {
    SIMPLE_ARG_TYPE_ERROR("execute", 1, "string(16bit)");
  }
#else
  get_all_args("odbc_result->execute", args, "%S", &q);
#endif

#ifdef SQL_WCHAR
  if (q->size_shift) {
    char *to_free = NULL;
    SQLWCHAR *p;
    if ((sizeof(SQLWCHAR) == 4) && (q->size_shift == 1)) {
      p = (SQLWCHAR *)require_wstring2(q, &to_free);
    } else {
      p = (SQLWCHAR *)q->str;
    }
    ODBC_ALLOW();
    code = SQLExecDirectW(hstmt, p, DO_NOT_WARN((SQLINTEGER)(q->len)));
    if (code != SQL_SUCCESS && code != SQL_SUCCESS_WITH_INFO)
      err_msg = "Query failed";
    else {
      code = SQLNumResultCols(hstmt, &num_fields);
      if (code != SQL_SUCCESS && code != SQL_SUCCESS_WITH_INFO)
	err_msg = "Couldn't get the number of fields";
      else {
	code = SQLRowCount(hstmt, &num_rows);
	if (code != SQL_SUCCESS && code != SQL_SUCCESS_WITH_INFO)
	  err_msg = "Couldn't get the number of rows";
      }
    }
    ODBC_DISALLOW();
    if (to_free) free (to_free);
  } else
#endif
  {
    ODBC_ALLOW();
    code = SQLExecDirect(hstmt, STR0(q), DO_NOT_WARN((SQLINTEGER)(q->len)));
    if (code != SQL_SUCCESS && code != SQL_SUCCESS_WITH_INFO)
      err_msg = "Query failed";
    else {
      code = SQLNumResultCols(hstmt, &num_fields);
      if (code != SQL_SUCCESS && code != SQL_SUCCESS_WITH_INFO)
	err_msg = "Couldn't get the number of fields";
      else {
	code = SQLRowCount(hstmt, &num_rows);
	if (code != SQL_SUCCESS && code != SQL_SUCCESS_WITH_INFO)
	  err_msg = "Couldn't get the number of rows";
      }
    }
    ODBC_DISALLOW();
  }

#ifdef ODBC_DEBUG
  fprintf (stderr, "ODBC:execute: SQLExecDirect returned %d, "
	   "cols %d, rows %d\n", code, num_fields, num_rows);
#endif

  if (err_msg)
    odbc_error ("odbc_result->execute", err_msg, PIKE_ODBC_RES->odbc, hstmt,
		code, NULL, NULL);
  else {
    PIKE_ODBC_RES->odbc->affected_rows = PIKE_ODBC_RES->num_rows = num_rows;
    if (num_fields) {
      PIKE_ODBC_RES->num_fields = num_fields;
      odbc_fix_fields();
    }

    pop_n_elems(args);

    /* Result */
    push_int(num_fields);
  }
}
 
static void f_list_tables(INT32 args)
{
  struct pike_string *table_name_pattern = NULL;
  SQLHSTMT hstmt = PIKE_ODBC_RES->hstmt;
  RETCODE code;
  const char *err_msg = NULL;
  SWORD num_fields;
  SQLLEN num_rows;

  if (!args) {
    push_constant_text("%");
    args = 1;
  } else if ((Pike_sp[-args].type != T_STRING) ||
	     (Pike_sp[-args].u.string->size_shift)) {
    Pike_error("odbc_result->list_tables(): "
	       "Bad argument 1. Expected 8-bit string.\n");
  }

  table_name_pattern = Pike_sp[-args].u.string;

  ODBC_ALLOW();
  code = SQLTables(hstmt, "%", 1, "%", 1,
		   table_name_pattern->str,
		   DO_NOT_WARN((SQLSMALLINT)table_name_pattern->len),
		   "%", 1);
  if (code != SQL_SUCCESS && code != SQL_SUCCESS_WITH_INFO)
    err_msg = "Query failed";
  else {
    code = SQLNumResultCols(hstmt, &num_fields);
    if (code != SQL_SUCCESS && code != SQL_SUCCESS_WITH_INFO)
      err_msg = "Couldn't get the number of fields";
    else {
      code = SQLRowCount(hstmt, &num_rows);
      if (code != SQL_SUCCESS && code != SQL_SUCCESS_WITH_INFO)
	err_msg = "Couldn't get the number of rows";
    }
  }
  ODBC_DISALLOW();

  if (err_msg)
    odbc_error ("odbc_result->list_tables", err_msg, PIKE_ODBC_RES->odbc, hstmt,
		code, NULL, NULL);
  else {
    PIKE_ODBC_RES->odbc->affected_rows = PIKE_ODBC_RES->num_rows = num_rows;
    if (num_fields) {
      PIKE_ODBC_RES->num_fields = num_fields;
      odbc_fix_fields();
    }

    pop_n_elems(args);

    /* Result */
    push_int(PIKE_ODBC_RES->num_fields);
  }
}
 
/* int num_rows() */
static void f_num_rows(INT32 args)
{
  pop_n_elems(args);
  push_int64(PIKE_ODBC_RES->num_rows);
}

/* int num_fields() */
static void f_num_fields(INT32 args)
{
  pop_n_elems(args);
  push_int(PIKE_ODBC_RES->num_fields);
}

/* array(int|mapping(string:mixed)) fetch_fields() */
static void f_fetch_fields(INT32 args)
{
  pop_n_elems(args);

  ref_push_array(PIKE_ODBC_RES->fields);
}
 
/* int|array(string|float|int) fetch_row() */
static void f_fetch_row(INT32 args)
{
  SQLHSTMT hstmt = PIKE_ODBC_RES->hstmt;
  int i;
  RETCODE code;
 
  pop_n_elems(args);

  ODBC_ALLOW();
  code = SQLFetch(hstmt);
  ODBC_DISALLOW();
  
  if (code == SQL_NO_DATA_FOUND) {
    /* No rows left in result */
    push_int(0);
  } else {
    odbc_check_error("odbc->fetch_row", "Couldn't fetch row",
		     code, NULL, NULL);
 
    for (i=0; i < PIKE_ODBC_RES->num_fields; i++) {
      SQLLEN len = 0;
      SWORD field_type = PIKE_ODBC_RES->field_info[i].type;
      static char dummy_buf[4];

      /* First get the size of the data. */

      ODBC_ALLOW();

      code = SQLGetData(hstmt, (SQLUSMALLINT)(i+1),
			field_type, dummy_buf, 0, &len);
#ifdef SQL_WCHAR
      if (code == SQL_NULL_DATA && (field_type == SQL_C_WCHAR)) {
	/* Kludge for FreeTDS which doesn't support WCHAR.
	 * Refetch as a normal char.
	 */
#ifdef ODBC_DEBUG
	fprintf(stderr, "ODBC:fetch_row(): Field %d: WCHAR not supported.\n",
		i + 1);
#endif /* ODBC_DEBUG */
	field_type = SQL_C_CHAR;
	code = SQLGetData(hstmt, (SQLUSMALLINT)(i+1),
			  field_type, dummy_buf, 0, &len);
      }
#endif

      ODBC_DISALLOW();

#ifdef SQL_WCHAR
      /* In case the type got changed in the kludge above. */
      PIKE_ODBC_RES->field_info[i].type = field_type;
#endif

      if (code == SQL_NO_DATA_FOUND) {
	/* All data already returned. */
#ifdef ODBC_DEBUG
	fprintf(stderr, "ODBC:fetch_row(): NO DATA FOUND\n");
#endif /* ODBC_DEBUG */
	push_text("");
	continue;
      }
      odbc_check_error("odbc->fetch_row", "SQLGetData() failed",
		       code, NULL, NULL);
      if (len == SQL_NULL_DATA) {
#ifdef ODBC_DEBUG
	fprintf(stderr, "ODBC:fetch_row(): NULL\n");
#endif /* ODBC_DEBUG */
	/* NULL */
	push_undefined();
      } else if (len == 0) {
	push_text("");
      } else {
	struct pike_string *s;
	SQLLEN bytes;
	int num_strings = 0;

	while((bytes = len)) {
#ifdef SQL_NO_TOTAL
	  if (len == SQL_NO_TOTAL) {
	    bytes = BLOB_BUFSIZ;
	  }
#endif /* SQL_NO_TOTAL */
	  switch(field_type) {
	  case SQL_C_CHAR:
	  default:
	  case SQL_C_BINARY:
	    s = begin_shared_string(bytes);
	    break;
#ifdef SQL_WCHAR
	  case SQL_C_WCHAR:
	    s = begin_wide_shared_string(bytes/sizeof(SQLWCHAR),
					 sizeof(SQLWCHAR)>2?2:1);
	    break;
#endif /* SQL_WCHAR */
	  }

	  ODBC_ALLOW();

	  code = SQLGetData(hstmt, (SQLUSMALLINT)(i+1),
			    field_type, s->str, bytes, &len);

	  ODBC_DISALLOW();

	  num_strings++;
#ifdef ODBC_DEBUG
	  fprintf(stderr, "ODBC:fetch_row(): %d:%d: Got %d/%d bytes.\n",
		  i+1, num_strings, bytes, len);
#endif /* ODBC_DEBUG */
	  if (code == SQL_NO_DATA_FOUND) {
	    /* No data or end marker. */
	    free_string(s);
	    push_text("");
	  } else {
	    odbc_check_error("odbc->fetch_row", "SQLGetData() failed",
			     code, NULL, NULL);
	    if (len == bytes) {
	      push_string(end_shared_string(s));
	    } else if (!len) {
	      free_string(s);
	      push_text("");
#ifdef SQL_NO_TOTAL
	    } else if (len == SQL_NO_TOTAL) {
	      push_string(end_shared_string(s));
#ifdef ODBC_DEBUG
	      fprintf(stderr, "ODBC:fetch_row(): More data remaining.\n");
#endif /* ODBC_DEBUG */
	      continue;
#endif /* SQL_NO_TOTAL */
	    } else if (len < bytes) {
	      push_string(end_and_resize_shared_string(s, len));
	    } else {
	      push_string(end_shared_string(s));
	      if (len > bytes) {
		len = len-bytes;
#ifdef ODBC_DEBUG
		fprintf(stderr, "ODBC:fetch_row(): %d bytes remaining.\n",
			len);
#endif /* ODBC_DEBUG */
		continue;
	      }
	    }
	  }
	  break;
	}
	if (!num_strings) {
	  push_text("");
	} else if (num_strings > 1) {
	  f_add(num_strings);
	}
      }
    }
    f_aggregate(PIKE_ODBC_RES->num_fields);
  }
}
 
/* int eof() */
static void f_eof(INT32 args)
{
  Pike_error("odbc->eof(): Not implemented yet!\n");
}

/* void seek() */
static void f_seek(INT32 args)
{
  Pike_error("odbc->seek(): Not implemented yet!\n");
}
 
/*
 * Module linkage
 */
 
void init_odbc_res_programs(void)
{
  /*
   * start_new_program();
   *
   * add_storage();
   *
   * add_function();
   * add_function();
   * ...
   *
   * set_init_callback();
   * set_exit_callback();
   *
   * program = end_c_program();
   * program->refs++;
   *
   */

  start_new_program();
  ADD_STORAGE(struct precompiled_odbc_result);

  map_variable("_odbc", "object", 0,
	       OFFSETOF(precompiled_odbc_result, obj), T_OBJECT);
  map_variable("_fields", "array(mapping(string:mixed))", 0,
	       OFFSETOF(precompiled_odbc_result, fields), T_ARRAY);
 
  /* function(object:void) */
  ADD_FUNCTION("create", f_create,tFunc(tObj,tVoid), ID_PUBLIC);
  /* function(string:int) */
  ADD_FUNCTION("execute", f_execute,tFunc(tStr,tInt), ID_PUBLIC);
  /* function(void|string:int) */
  ADD_FUNCTION("list_tables", f_list_tables,tFunc(tOr(tVoid,tStr),tInt), ID_PUBLIC);
  /* function(void:int) */
  ADD_FUNCTION("num_rows", f_num_rows,tFunc(tVoid,tInt), ID_PUBLIC);
  /* function(void:int) */
  ADD_FUNCTION("num_fields", f_num_fields,tFunc(tVoid,tInt), ID_PUBLIC);
#ifdef SUPPORT_FIELD_SEEK
  /* function(int:void) */
  ADD_FUNCTION("field_seek", f_field_seek,tFunc(tInt,tVoid), ID_PUBLIC);
#endif /* SUPPORT_FIELD_SEEK */
  /* function(void:int) */
  ADD_FUNCTION("eof", f_eof,tFunc(tVoid,tInt), ID_PUBLIC);
#ifdef SUPPORT_FIELD_SEEK
  /* function(void:int|mapping(string:mixed)) */
  ADD_FUNCTION("fetch_field", f_fetch_field,tFunc(tVoid,tOr(tInt,tMap(tStr,tMix))), ID_PUBLIC);
#endif /* SUPPORT_FIELD_SEEK */
  /* function(void:array(int|mapping(string:mixed))) */
  ADD_FUNCTION("fetch_fields", f_fetch_fields,tFunc(tVoid,tArr(tOr(tInt,tMap(tStr,tMix)))), ID_PUBLIC);
  /* function(int:void) */
  ADD_FUNCTION("seek", f_seek,tFunc(tInt,tVoid), ID_PUBLIC);
  /* function(void:int|array(string|int|float)) */
  ADD_FUNCTION("fetch_row", f_fetch_row,tFunc(tVoid,tOr(tInt,tArr(tOr3(tStr,tInt,tFlt)))), ID_PUBLIC);
 
  set_init_callback(init_res_struct);
  set_exit_callback(exit_res_struct);
 
  odbc_result_program = end_program();
  add_program_constant("odbc_result",odbc_result_program, 0);
}
 
void exit_odbc_res(void)
{
  if (odbc_result_program) {
    free_program(odbc_result_program);
    odbc_result_program = NULL;
  }
}

#else
static int place_holder;	/* Keep the compiler happy */
#endif /* HAVE_ODBC */
