/*
|| This file is part of Pike. For copyright information see COPYRIGHT.
|| Pike is distributed under GPL, LGPL and MPL. See the file COPYING
|| for more information.
*/

/*
 * Pike interface to ODBC compliant databases
 *
 * Henrik Grubbström
 */

/* FIXME: ODBC allows for multiple result sets from the same query.
 * Support for SQLMoreResults should be added to support that (and it
 * needs to work also in the case when the first result set has no
 * columns). */

/*
 * Includes
 */

#include "global.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

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
#include "pike_types.h"

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
struct program *odbc_typed_result_program = NULL;

int odbc_result_fun_num = -1;
int odbc_typed_result_fun_num = -1;

static int scale_numeric_fun_num = -1;
static int time_factory_fun_num = -1;
static int timestamp_factory_fun_num = -1;
static int user_defined_factory_fun_num = -1;
static int uuid_factory_fun_num = -1;

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
 
static void init_res_struct(struct object *UNUSED(o))
{
  memset(PIKE_ODBC_RES, 0, sizeof(struct precompiled_odbc_result));
  PIKE_ODBC_RES->hstmt = SQL_NULL_HSTMT;
  SET_SVAL(PIKE_ODBC_RES->null_value, PIKE_T_INT, NUMBER_UNDEFINED,
	   integer, 0);
}
 
static void exit_res_struct(struct object *UNUSED(o))
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

/* NB: The field number argument (i) in these callbacks
 *     is currently unused in all of the functions, but
 *     it will in the future be used when handling the
 *     used defined types.
 */

static void push_sql_float(int UNUSED(i))
{
  struct pike_string *data = Pike_sp[-1].u.string;
  SQLDOUBLE *res = (SQLDOUBLE *)data->str;

  if (data->len != sizeof(SQLDOUBLE)) {
    Pike_error("Invalid floating point field length: %d\n", data->len);
  }

  Pike_sp--;
  push_float(*res);
  free_string(data);
}

static void push_sql_int(int UNUSED(i))
{
  struct pike_string *data = Pike_sp[-1].u.string;
  void *bytes = data->str;
  Pike_sp--;
  switch(data->len) {
  case 0:
    push_int(0);
    break;
  case 1:
    push_int(data->str[0]);
    break;
  case 2:
    push_int(*((INT16 *)bytes));
    break;
  case 4:
    push_int(*((INT32 *)bytes));
    break;
  case 8:
    push_int64(*((INT64 *)bytes));
    break;
  default:
    Pike_sp++;
    Pike_error("Invalid integer field length: %d\n", data->len);
    break;
  }
  free_string(data);
}

static void push_numeric(int UNUSED(i))
{
  struct pike_string *data = Pike_sp[-1].u.string;
  SQL_NUMERIC_STRUCT *numeric = (SQL_NUMERIC_STRUCT *)data->str;
  struct object *res;
  MP_INT *mp;

  if (data->len != sizeof(SQL_NUMERIC_STRUCT)) {
    Pike_error("Invalid numeric field length: %d\n", data->len);
  }

  res = fast_clone_object(bignum_program);
  Pike_sp--;
  push_object(res);
  mp = (MP_INT *)res->storage;
  mpz_import(mp, SQL_MAX_NUMERIC_LEN, -1, 1, 0, 0, numeric->val);
  if (!numeric->sign) {
    mpz_neg(mp, mp);
  }
  if (numeric->scale) {
    push_int(-numeric->scale);
    free_string(data);
    apply_current(scale_numeric_fun_num, 2);
    return;
  }
  free_string(data);
}

static void push_time(int UNUSED(i))
{
  struct pike_string *data = Pike_sp[-1].u.string;
  TIME_STRUCT *time = (TIME_STRUCT *)(data->str);
  if (data->len < ((ptrdiff_t)sizeof(TIME_STRUCT))) {
    return;
  }
  Pike_sp--;
  push_int(time->hour);
  push_int(time->minute);
  push_int(time->second);
#if 0
  /* New in SS_TIME2_STRUCT. */
  push_int(time->fraction);	/* ns */
#endif
  free_string(data);
  apply_current(time_factory_fun_num, 3);
}

static void push_date(int UNUSED(i))
{
  struct pike_string *data = Pike_sp[-1].u.string;
  DATE_STRUCT *date = (DATE_STRUCT *)(data->str);
  if (data->len < ((ptrdiff_t)sizeof(DATE_STRUCT))) {
    return;
  }
  Pike_sp--;
  push_int(date->year);
  push_int(date->month);
  push_int(date->day);
  free_string(data);
  apply_current(timestamp_factory_fun_num, 3);
}

static void push_timestamp(int UNUSED(i))
{
  struct pike_string *data = Pike_sp[-1].u.string;
  TIMESTAMP_STRUCT *date = (TIMESTAMP_STRUCT *)(data->str);
  if (data->len < ((ptrdiff_t)sizeof(TIMESTAMP_STRUCT))) {
    return;
  }
  Pike_sp--;
  push_int(date->year);
  push_int(date->month);
  push_int(date->day);
  push_int(date->hour);
  push_int(date->minute);
  push_int(date->second);
  push_int(date->fraction);	/* ns */
#if 0
  /* New in struct SS_TIMESTAMPOFFSET_STRUCT. */
  push_int(time->timezone_hour);
  push_int(time->timezone_minute);
#endif
  free_string(data);
  apply_current(timestamp_factory_fun_num, 7);
}

static void push_uuid(int UNUSED(i))
{
  apply_current(uuid_factory_fun_num, 1);
}

static void push_user_defined(int i)
{
  push_svalue(PIKE_ODBC_RES->fields->item + i);
  push_int(i);
  apply_current(user_defined_factory_fun_num, 3);
}

#ifndef SQL_SS_VARIANT
#define SQL_SS_VARIANT		(-150)
#endif
#ifndef SQL_SS_UDT
#define SQL_SS_UDT		(-151)
#endif
#ifndef SQL_SS_XML
#define SQL_SS_XML		(-152)
#endif
#ifndef SQL_SS_TABLE
#define SQL_SS_TABLE		(-153)
#endif
#ifndef SQL_SS_TIME2
#define SQL_SS_TIME2		(-154)
#endif
#ifndef SQL_SS_TIMESTAMPOFFSET
#define SQL_SS_TIMESTAMPOFFSET	(-155)
#endif

#ifndef SQL_CA_SS_BASE
#define SQL_CA_SS_BASE		1200
#endif
#ifndef SQL_CA_SS_UDT_ASSEMBLY_TYPE_NAME
#define SQL_CA_SS_UDT_ASSEMBLY_TYPE_NAME	(SQL_CA_SS_BASE + 21)
#endif

static void odbc_fix_fields(void)
{
  SQLHSTMT hstmt = PIKE_ODBC_RES->hstmt;
  int i;
  struct field_info *field_info;
  size_t buf_size = 1024;
  SQLWCHAR *buf = alloca(buf_size * sizeof(SQLWCHAR));

  if (!buf) {
    Pike_error("odbc_fix_fields(): Out of memory\n");
  }

  if (PIKE_ODBC_RES->field_info) {
    free(PIKE_ODBC_RES->field_info);
    PIKE_ODBC_RES->field_info = NULL;
  }
  PIKE_ODBC_RES->field_info = field_info = (struct field_info *)
    xalloc(sizeof(struct field_info) * PIKE_ODBC_RES->num_fields);

  /*
   * First build the fields-array;
   */
  for (i=0; i < PIKE_ODBC_RES->num_fields; i++) {
    struct svalue *save_sp = Pike_sp;
    int nbits;
    SWORD name_len = 0;
    SWORD sql_type;
    SQLULEN precision;
    SWORD scale;
    SWORD nullable = 0;
    SQLHDESC hdesc = NULL;

    while (1) {
      RETCODE code;
      ODBC_ALLOW();
      code = SQLDescribeColW(hstmt, i+1,
			     buf,
			     DO_NOT_WARN((SQLSMALLINT)buf_size),
			     &name_len,
			     &sql_type, &precision, &scale, &nullable);
      ODBC_DISALLOW();
      odbc_check_error("odbc_fix_fields", "Failed to fetch field info",
		       code, NULL, NULL);
      if ((name_len * (ptrdiff_t)sizeof(SQLWCHAR)) < (ptrdiff_t)buf_size) {
	break;
      }
      do {
	buf_size *= 2;
      } while ((name_len * (ptrdiff_t)sizeof(SQLWCHAR)) >= (ptrdiff_t)buf_size);
      if (!(buf = alloca(buf_size * sizeof(SQLWCHAR)))) {
	Pike_error("odbc_fix_fields(): Out of memory\n");
      }
    }
#ifdef ODBC_DEBUG
    fprintf(stderr,
	    "ODBC:odbc_fix_fields():\n"
	    "name:%ls\n",
	    buf);
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
    push_sqlwchar(buf, name_len);
    ref_push_string(literal_type_string);
#ifdef ODBC_DEBUG
    fprintf(stderr, "SQL_C_WCHAR\n");
#endif /* ODBC_DEBUG */
    field_info[i].factory = NULL;
    field_info[i].type = SQL_C_CHAR;
    field_info[i].size = precision;
    field_info[i].bin_type = SQL_C_BINARY;
    field_info[i].bin_size = precision;
    field_info[i].scale = scale;
    switch(sql_type) {
    case SQL_CHAR:
    case SQL_WCHAR:
      field_info[i].type = SQL_C_WCHAR;
      /* NOTE: Field size is in characters, while SQLGetData()
       *       wants bytes.
       */
      field_info[i].size *= (ptrdiff_t)sizeof(SQLWCHAR);
      ref_push_string(literal_string_string);
      break;
    case SQL_NUMERIC:	/* INT128 + scale + sign */
      push_text("numeric");
      field_info[i].size += 3;	/* Sign, leading zero and decimal characters. */
      field_info[i].bin_type = SQL_C_NUMERIC;
      field_info[i].bin_size = sizeof(SQL_NUMERIC_STRUCT);
      field_info[i].factory = push_numeric;
      break;
    case SQL_DECIMAL:	/* INT128 + scale + sign */
      push_text("decimal");
      field_info[i].size += 3;	/* Sign, leading zero and decimal characters. */
      field_info[i].bin_type = SQL_C_NUMERIC;
      field_info[i].bin_size = sizeof(SQL_NUMERIC_STRUCT);
      field_info[i].factory = push_numeric;
      break;
    case SQL_INTEGER:	/* INT32 */
      push_text("integer");
      field_info[i].size++;	/* Allow for a sign character. */
      field_info[i].bin_size = 4;
      field_info[i].factory = push_sql_int;
      break;
    case SQL_SMALLINT:	/* INT16 */
      push_text("short");
      field_info[i].size++;	/* Allow for a sign character. */
      field_info[i].bin_size = 2;
      field_info[i].factory = push_sql_int;
      break;
    case SQL_FLOAT:	/* float or double */
      ref_push_string(literal_float_string);
      field_info[i].size += 3;	/* Sign, leading zero and decimal characters. */
      field_info[i].bin_type = SQL_C_DOUBLE;
      field_info[i].bin_size = sizeof(SQLDOUBLE);
      field_info[i].factory = push_sql_float;
      break;
    case SQL_REAL:	/* float */
      push_text("real");
      field_info[i].size += 3;	/* Sign, leading zero and decimal characters. */
      field_info[i].bin_type = SQL_C_DOUBLE;
      field_info[i].bin_size = sizeof(SQLDOUBLE);
      field_info[i].factory = push_sql_float;
      break;
    case SQL_DOUBLE:	/* double */
      push_text("double");
      field_info[i].size += 3;	/* Sign, leading zero and decimal characters. */
      field_info[i].bin_type = SQL_C_DOUBLE;
      field_info[i].bin_size = sizeof(SQLDOUBLE);
      field_info[i].factory = push_sql_float;
      break;
    case SQL_VARCHAR:
#ifdef SQL_WVARCHAR
    case SQL_WVARCHAR:
#endif
      push_text("var string");
      field_info[i].type = SQL_C_WCHAR;
      field_info[i].size = 0;	/* Variable length */
      break;
#ifdef SQL_GUID
    case SQL_GUID:
      push_text("uuid");
      field_info[i].bin_size = sizeof(SQLGUID);
      field_info[i].factory = push_uuid;
      break;
#endif
    case SQL_DATE:
      push_text("date");
      field_info[i].type = SQL_C_WCHAR;
      /* NOTE: Field size is in characters, while SQLGetData()
       *       wants bytes.
       */
      field_info[i].size = 32 * (ptrdiff_t)sizeof(SQLWCHAR);
      field_info[i].bin_size = sizeof(DATE_STRUCT);
      field_info[i].factory = push_date;
      break;
    case SQL_SS_TIME2:
      /* This corresponds to MSSQL time, and is time of day
       * with nanosecond precision.
       *
       * FIXME: We just convert it to SQL_C_TYPE_TIME for now.
       */
      /* FALL_THROUGH */
    case SQL_TIME:
      push_text("time");
      field_info[i].type = SQL_C_WCHAR;
      /* NOTE: Field size is in characters, while SQLGetData()
       *       wants bytes.
       */
      field_info[i].size = 32 * (ptrdiff_t)sizeof(SQLWCHAR);
      field_info[i].bin_type = SQL_C_TYPE_TIME;
      field_info[i].bin_size = sizeof(TIME_STRUCT);
      field_info[i].factory = push_time;
      break;
    case SQL_SS_TIMESTAMPOFFSET:
      /* This corresponds to MSSQL datetimeoffset, and is
       * a timestamp followed by timestamp UTC offset.
       *
       * FIXME: We just convert it to SQL_C_TIMESTAMP for now.
       */
      /* FALL_THROUGH */
    case SQL_TIMESTAMP:
      push_text("timestamp");
      field_info[i].type = SQL_C_WCHAR;
      /* NOTE: Field size is in characters, while SQLGetData()
       *       wants bytes.
       */
      field_info[i].size = 32 * (ptrdiff_t)sizeof(SQLWCHAR);
      /* NB: This is the old timestamp format (datetime) when
       *     precision:scale is 23:3, and the new (datetime2)
       *     when they are 27:7, but unixODBC 2.3.2 doesn't
       *     seem to have the struct corresponding to datetime,
       *     so we force the driver convert it to the current
       *     timestamp format.
       */
      field_info[i].bin_type = SQL_C_TYPE_TIMESTAMP;
      field_info[i].bin_size = sizeof(TIMESTAMP_STRUCT);
      field_info[i].factory = push_timestamp;
      break;
    case SQL_SS_XML:
      /* This corresponds to MSSQL xml. */
      push_text("xml");
      field_info[i].type = SQL_C_WCHAR;
      field_info[i].size = 0;	/* Variable length */
      break;
    case SQL_LONGVARCHAR:
#ifdef SQL_WLONGVARCHAR
    case SQL_WLONGVARCHAR:
#endif
      push_text("var string");
      field_info[i].type = SQL_C_WCHAR;
      field_info[i].size = 0;	/* Variable length */
      break;
    case SQL_BINARY:
      push_text("binary");
#ifdef ODBC_DEBUG
      fprintf(stderr, "SQL_C_BINARY\n");
#endif /* ODBC_DEBUG */
      field_info[i].type = SQL_C_BINARY;
      break;
    case SQL_VARBINARY:
      push_text("blob");
#ifdef ODBC_DEBUG
      fprintf(stderr, "SQL_C_BINARY\n");
#endif /* ODBC_DEBUG */
      field_info[i].type = SQL_C_BINARY;
      field_info[i].size = 0;	/* Variable length */
      break;
    case SQL_LONGVARBINARY:
      push_text("long blob");
#ifdef ODBC_DEBUG
      fprintf(stderr, "SQL_C_BINARY\n");
#endif /* ODBC_DEBUG */
      field_info[i].type = SQL_C_BINARY;
      field_info[i].size = 0;	/* Variable length */
      break;
    case SQL_BIGINT:	/* INT64 */
      push_text("long integer");
      field_info[i].size++;	/* Allow for a sign character. */
      field_info[i].bin_type = SQL_C_SBIGINT;
      field_info[i].bin_size = sizeof(SQLBIGINT);
      field_info[i].factory = push_sql_int;
      break;
    case SQL_TINYINT:	/* INT8 */
      push_text("tiny integer");
      field_info[i].size++;	/* Allow for a sign character. */
      field_info[i].bin_type = SQL_C_SLONG;
      field_info[i].bin_size = sizeof(SQLINTEGER);
      field_info[i].factory = push_sql_int;
      break;
    case SQL_BIT:	/* INT1 */
      push_text("bit");
      field_info[i].bin_type = SQL_C_SLONG;
      field_info[i].bin_size = sizeof(SQLINTEGER);
      field_info[i].factory = push_sql_int;
      break;
    case SQL_SS_UDT:
      /* User-defined data type. */
      /* FIXME: Use SQLGetDescFieldW(hstmt, i+1,
       *                             SQL_CA_SS_UDT_ASSEMBLY_TYPE_NAME,
       *                             &wstr_buf, wstr_buf_bytes,
       *                             &out_bytes)
       *        to get the actual type.
       */
      push_text("user-defined");
      field_info[i].type = SQL_C_BINARY;
      field_info[i].factory = push_user_defined;
      {
	SQLINTEGER user_type_len = 0;
	struct pike_string *user_type = NULL;
	RETCODE code;

	if (!hdesc) {
	  odbc_check_error("odbc->fetch_row", "SQLGetStmtAttr() failed",
			   SQLGetStmtAttr(hstmt, SQL_ATTR_APP_ROW_DESC,
					  &hdesc, 0, NULL),
			   NULL, NULL);
	  /* NB: hdesc is an implicit descriptor handle, and is
	   *     freed when hstmt is freed.
	   */
	}
	SQLGetDescFieldW(hdesc, i + 1,
			 SQL_CA_SS_UDT_ASSEMBLY_TYPE_NAME,
			 NULL, 0, &user_type_len);
	if (user_type_len) {
	  push_text("user_type");
	  if (sizeof(SQLWCHAR) == 4) {
	    user_type = begin_wide_shared_string(user_type_len, 2);
	  } else {
	    user_type = begin_wide_shared_string(user_type_len, 1);
	  }
	  code = SQLGetDescFieldW(hdesc, i + 1,
				  SQL_CA_SS_UDT_ASSEMBLY_TYPE_NAME,
				  user_type->str,
				  (user_type_len + 1) * sizeof(SQLWCHAR),
				  &user_type_len);
	  push_string(end_shared_string(user_type));
	  odbc_check_error("odbc->fetch_row", "SQLGetDescField() failed",
			   code, NULL, NULL);
	}
      }

      break;
    case SQL_SS_VARIANT:
      push_text("variant");
      field_info[i].type = SQL_C_BINARY;
      break;
    case SQL_SS_TABLE:
      push_text("table");
      field_info[i].type = SQL_C_BINARY;
      break;
    default:
      push_text("unknown");
      field_info[i].type = SQL_C_WCHAR;
      /* NOTE: Field size is in characters, while SQLGetData()
       *       wants bytes.
       */
      field_info[i].size *= (ptrdiff_t)sizeof(SQLWCHAR);
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

    f_aggregate_mapping(Pike_sp - save_sp);
  }
  f_aggregate(PIKE_ODBC_RES->num_fields);

  add_ref(PIKE_ODBC_RES->fields = Pike_sp[-1].u.array);
  pop_stack();
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
  if ((TYPEOF(Pike_sp[-args]) != T_OBJECT) ||
      (!(PIKE_ODBC_RES->odbc =
	 get_storage(Pike_sp[-args].u.object, odbc_program)))) {
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
  char *to_free = NULL;
  SQLWCHAR *wq = NULL;

  get_all_args("execute", args, "%W", &q);
  if ((q->size_shift > 1) && (sizeof(SQLWCHAR) == 2)) {
    SIMPLE_ARG_TYPE_ERROR("execute", 1, "string(16bit)");
  }
  if (q->size_shift) {
    if ((sizeof(SQLWCHAR) == 4) && (q->size_shift == 1)) {
      wq = (SQLWCHAR *)require_wstring2(q, &to_free);
    } else {
      wq = (SQLWCHAR *)q->str;
    }
  }

  ODBC_ALLOW();

  if (wq)
    code = SQLExecDirectW(hstmt, wq, DO_NOT_WARN((SQLINTEGER)(q->len)));
  else
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

  if (to_free) free (to_free);

#ifdef ODBC_DEBUG
  fprintf (stderr, "ODBC:execute: SQLExecDirect returned %d, "
	   "cols %d, rows %ld\n", code, num_fields, (long) num_rows);
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
    push_text("%");
    args = 1;
  } else if ((TYPEOF(Pike_sp[-args]) != T_STRING) ||
	     (Pike_sp[-args].u.string->size_shift)) {
    Pike_error("odbc_result->list_tables(): "
	       "Bad argument 1. Expected 8-bit string.\n");
  }

  table_name_pattern = Pike_sp[-args].u.string;

  ODBC_ALLOW();
  code = SQLTables(hstmt, (SQLCHAR *) "%", 1, (SQLCHAR *) "%", 1,
		   (SQLCHAR *) table_name_pattern->str,
		   DO_NOT_WARN((SQLSMALLINT)table_name_pattern->len),
		   (SQLCHAR *) "%", 1);
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
  unsigned int old_tds_kludge = PIKE_ODBC_RES->odbc->flags & PIKE_ODBC_OLD_TDS_KLUDGE;
  RETCODE code;
 
  pop_n_elems(args);

  ODBC_ALLOW();
  code = SQLFetch(hstmt);
  ODBC_DISALLOW();
  
  if (code == SQL_NO_DATA_FOUND) {
    /* No rows left in result */
    push_undefined();
  } else {
    odbc_check_error("odbc->fetch_row", "Couldn't fetch row",
		     code, NULL, NULL);
 
    for (i=0; i < PIKE_ODBC_RES->num_fields; i++) {
      SQLLEN len = PIKE_ODBC_RES->field_info[i].size;
      SWORD field_type = PIKE_ODBC_RES->field_info[i].type;
      static char dummy_buf[4];

      /* First get the size of the data.
       *
       * Note that this method of getting the size apparently isn't
       * always supported for all data types (eg for INTEGER/MSSQL),
       * where we instead use the width returned by SQLDescribeCol()
       * earlier.
       *
       * Note also that FreeTDS/UnixODBC apparently has both a broken
       * SQLDescribeCol() (which returns the SQL_C_BINARY size for
       * eg INTEGER even if SQL_C_CHAR has been requested, and a
       * SQLGetData() which doesn't properly support streamed fetching.
       *
       * We tried solving this by performing SQLGetData with zero size,
       * and using the result if it succeeded, using the SQLDescribeCol()
       * size if not zero, and otherwise throw an error. Unfortunately
       * this didn't work, since MSSQL believes that it returned all the
       * data in the first call (which failed with code "22003")...
       *
       * On unbroken ODBC implementations the UnixODBC method is
       * preferred, since less buffer space is wasted.
       */

      if (old_tds_kludge || !len) {

	ODBC_ALLOW();

#ifdef ODBC_DEBUG
	fprintf(stderr, "ODBC:fetch_row(): Field %d: "
		"SQLGetData(X, %d, %d, X, 0, X)...\n",
		i + 1, i+1, field_type);
#endif /* ODBC_DEBUG */
	code = SQLGetData(hstmt, (SQLUSMALLINT)(i+1),
			  field_type, dummy_buf, 0, &len);

	if (code == SQL_NULL_DATA && (field_type == SQL_C_WCHAR)) {
	  /* Kludge for FreeTDS which doesn't support WCHAR.
	   * Refetch as a normal char.
	   */
#ifdef ODBC_DEBUG
	  fprintf(stderr, "ODBC:fetch_row(): Field %d: WCHAR not supported.\n",
		  i + 1);
#endif /* ODBC_DEBUG */
	  field_type = SQL_C_CHAR;
#ifdef ODBC_DEBUG
	  fprintf(stderr, "ODBC:fetch_row(): Field %d: "
		  "SQLGetData(X, %d, %d, X, 0, X)...\n",
		  i + 1, i+1, field_type);
#endif /* ODBC_DEBUG */
	  code = SQLGetData(hstmt, (SQLUSMALLINT)(i+1),
			    field_type, dummy_buf, 0, &len);
	}

#ifdef ODBC_DEBUG
	fprintf(stderr, "ODBC:fetch_row(): Field %d: "
		"SQLGetData(X, %d, %d, X, 0, X, X) ==> code: %d, len: %ld.\n",
		i + 1,
		i+1, field_type, code, len);
#endif /* ODBC_DEBUG */

	ODBC_DISALLOW();

	/* In case the type got changed in the kludge above. */
	PIKE_ODBC_RES->field_info[i].type = field_type;
      }

#ifdef ODBC_DEBUG
      fprintf(stderr, "ODBC:fetch_row(): Field %d: "
	      "field_type: %d, len: %ld, code: %d.\n",
	      i + 1, field_type, len, code);
#endif /* ODBC_DEBUG */

      if (code == SQL_NO_DATA_FOUND) {
	/* All data already returned. */
#ifdef ODBC_DEBUG
	fprintf(stderr, "ODBC:fetch_row(): NO DATA FOUND\n");
#endif /* ODBC_DEBUG */
	push_empty_string();
	continue;
      }
      if (!len) {
	odbc_check_error("odbc->fetch_row", "SQLGetData() failed",
			 code, NULL, NULL);
      }
      if (len == SQL_NULL_DATA) {
#ifdef ODBC_DEBUG
	fprintf(stderr, "ODBC:fetch_row(): NULL\n");
#endif /* ODBC_DEBUG */
	/* NULL */
	push_undefined();
      } else if (len == 0) {
	push_empty_string();
      } else {
	struct pike_string *s;
	SQLLEN bytes;
	SQLLEN pad = 0;
	int num_strings = 0;

	switch(field_type) {
	case SQL_C_CHAR:
	  /* Adjust for NUL. */
	  pad = 1;
	  break;
	default:
	case SQL_C_BINARY:
	  break;
	case SQL_C_WCHAR:
	  /* Adjust for NUL. */
	  pad = sizeof(SQLWCHAR);
	  break;
	}

	while((bytes = len)) {
#ifdef SQL_NO_TOTAL
	  if (bytes == SQL_NO_TOTAL) {
	    bytes = BLOB_BUFSIZ;
	  }
#endif /* SQL_NO_TOTAL */
	  switch(field_type) {
	  case SQL_C_CHAR:
	  default:
	  case SQL_C_BINARY:
	    s = begin_shared_string(bytes);
	    break;
	  case SQL_C_WCHAR:
	    s = begin_wide_shared_string(bytes/sizeof(SQLWCHAR),
					 sizeof(SQLWCHAR)>2?2:1);
	    break;
	  }

#ifdef ODBC_DEBUG
	  fprintf(stderr, "ODBC:fetch_row(): Field %d: "
		  "SQLGetData(X, %d, %d, X, %d, X)...\n",
		  i + 1, i+1, field_type, bytes+pad);
#endif /* ODBC_DEBUG */

	  ODBC_ALLOW();

	  code = SQLGetData(hstmt, (SQLUSMALLINT)(i+1),
			    field_type, s->str, bytes + pad, &len);

	  ODBC_DISALLOW();

	  num_strings++;
#ifdef ODBC_DEBUG
	  fprintf(stderr,
		  "ODBC:fetch_row(): %d:%d: Got %ld/%ld bytes (pad: %ld).\n"
		  "     Code: %d\n",
		  i+1, num_strings, (long)bytes, (long)len, (long)pad,
		  code);
#endif /* ODBC_DEBUG */
	  if (code == SQL_NO_DATA_FOUND) {
	    /* No data or end marker. */
	    free_string(s);
	    push_empty_string();
	    break;
	  } else {
	    odbc_check_error("odbc->fetch_row", "SQLGetData() failed",
			     code, NULL, NULL);
	    if (!len) {
	      free_string(s);
	      push_empty_string();
	      break;
	    } else if (len == SQL_NULL_DATA) {
	      free_string(s);
	      if (num_strings > 1) {
		num_strings--;
	      } else {
		push_undefined();
	      }
	      break;
#ifdef SQL_NO_TOTAL
	    } else if (len == SQL_NO_TOTAL) {
	      /* More data remaining... */
	      push_string(end_shared_string(s));
#ifdef ODBC_DEBUG
	      fprintf(stderr, "ODBC:fetch_row(): More data remaining.\n");
#endif /* ODBC_DEBUG */
#endif /* SQL_NO_TOTAL */
	    } else {
	      SQLLEN str_len = len;
	      if (len > bytes) {
		/* Possibly truncated result. */
		str_len = bytes;
		len -= bytes;
#ifdef ODBC_DEBUG
		fprintf(stderr, "ODBC:fetch_row(): %ld bytes remaining.\n",
			(long)len);
#endif /* ODBC_DEBUG */
	      } else len = 0;

	      str_len = str_len>>s->size_shift;
	      push_string(end_and_resize_shared_string(s, str_len));
	    }
	  }
	}
	if (!num_strings) {
	  push_empty_string();
	} else if (num_strings > 1) {
	  f_add(num_strings);
	}
      }
    }
    f_aggregate(PIKE_ODBC_RES->num_fields);
  }
}
 
/* int|array(string|float|int|object) fetch_typed_row() */
static void f_fetch_typed_row(INT32 args)
{
  SQLHSTMT hstmt = PIKE_ODBC_RES->hstmt;
  int i;
  unsigned int old_tds_kludge = PIKE_ODBC_RES->odbc->flags & PIKE_ODBC_OLD_TDS_KLUDGE;
  RETCODE code;

  pop_n_elems(args);

  ODBC_ALLOW();
  code = SQLFetch(hstmt);
  ODBC_DISALLOW();

  if (code == SQL_NO_DATA_FOUND) {
    /* No rows left in result */
    push_undefined();
  } else {
    struct svalue *null_value = &PIKE_ODBC_RES->null_value;
    SQLHDESC hdesc = NULL;

    odbc_check_error("odbc->fetch_row", "Couldn't fetch row",
		     code, NULL, NULL);

    for (i=0; i < PIKE_ODBC_RES->num_fields; i++) {
      struct field_info *field_info = &PIKE_ODBC_RES->field_info[i];
      SQLLEN len = field_info->size;
      SWORD field_type = field_info->type;
      field_factory_func factory = field_info->factory;
      static char dummy_buf[4];

      /* Read fields with factories as binary. */
      if (factory) {
	field_type = field_info->bin_type;
	len = field_info->bin_size;
      }

      /* First get the size of the data.
       *
       * Note that this method of getting the size apparently isn't
       * always supported for all data types (eg for INTEGER/MSSQL),
       * where we instead use the width returned by SQLDescribeCol()
       * earlier.
       *
       * Note also that FreeTDS/UnixODBC apparently has both a broken
       * SQLDescribeCol() (which returns the SQL_C_BINARY size for
       * eg INTEGER even if SQL_C_CHAR has been requested, and a
       * SQLGetData() which doesn't properly support streamed fetching.
       *
       * We tried solving this by performing SQLGetData with zero size,
       * and using the result if it succeeded, using the SQLDescribeCol()
       * size if not zero, and otherwise throw an error. Unfortunately
       * this didn't work, since MSSQL believes that it returned all the
       * data in the first call (which failed with code "22003")...
       *
       * On unbroken ODBC implementations the UnixODBC method is
       * preferred, since less buffer space is wasted.
       */

      if (old_tds_kludge || !len) {

	ODBC_ALLOW();

#ifdef ODBC_DEBUG
	fprintf(stderr, "ODBC:fetch_row(): Field %d: "
		"SQLGetData(X, %d, %d, X, 0, X)...\n",
		i + 1, i+1, field_type);
#endif /* ODBC_DEBUG */
	code = SQLGetData(hstmt, (SQLUSMALLINT)(i+1),
			  field_type, dummy_buf, 0, &len);

	if (code == SQL_NULL_DATA && (field_type == SQL_C_WCHAR)) {
	  /* Kludge for FreeTDS which doesn't support WCHAR.
	   * Refetch as a normal char.
	   */
#ifdef ODBC_DEBUG
	  fprintf(stderr, "ODBC:fetch_row(): Field %d: WCHAR not supported.\n",
		  i + 1);
#endif /* ODBC_DEBUG */
	  field_type = SQL_C_CHAR;
#ifdef ODBC_DEBUG
	  fprintf(stderr, "ODBC:fetch_row(): Field %d: "
		  "SQLGetData(X, %d, %d, X, 0, X)...\n",
		  i + 1, i+1, field_type);
#endif /* ODBC_DEBUG */
	  code = SQLGetData(hstmt, (SQLUSMALLINT)(i+1),
			    field_type, dummy_buf, 0, &len);
	}

#ifdef ODBC_DEBUG
	fprintf(stderr, "ODBC:fetch_row(): Field %d: "
		"SQLGetData(X, %d, %d, X, 0, X, X) ==> code: %d, len: %ld.\n",
		i + 1,
		i+1, field_type, code, len);
#endif /* ODBC_DEBUG */

	ODBC_DISALLOW();

	/* In case the type got changed in the kludge above. */
	field_info->type = field_type;
      }

#ifdef ODBC_DEBUG
      fprintf(stderr, "ODBC:fetch_row(): Field %d: "
	      "field_type: %d, len: %ld, code: %d.\n",
	      i + 1, field_type, len, code);
#endif /* ODBC_DEBUG */

      if (code == SQL_NO_DATA_FOUND) {
	/* All data already returned. */
#ifdef ODBC_DEBUG
	fprintf(stderr, "ODBC:fetch_row(): NO DATA FOUND\n");
#endif /* ODBC_DEBUG */
	push_empty_string();
	continue;
      }
      if (!len) {
	odbc_check_error("odbc->fetch_row", "SQLGetData() failed",
			 code, NULL, NULL);
      }
      if (len == SQL_NULL_DATA) {
#ifdef ODBC_DEBUG
	fprintf(stderr, "ODBC:fetch_row(): NULL\n");
#endif /* ODBC_DEBUG */
	/* NULL */
	push_svalue(null_value);
      } else if (len == 0) {
	push_empty_string();
      } else {
	struct pike_string *s;
	SQLLEN bytes;
	SQLLEN pad = 0;
	int num_strings = 0;

	switch(field_type) {
	case SQL_C_CHAR:
	  /* Adjust for NUL. */
	  pad = 1;
	  break;
	case SQL_C_NUMERIC:
	  /* NOTE: API stupidity.
	   *
	   *   When using SQLGetData() with field type SQL_NUMERIC the
	   *   values returned by MSSQL are normalized to scale == 0,
	   *   which means that any decimals will be truncated.
	   *
	   *   We work around this by using setting the field scaling
	   *   parameters by hand and using the virtual field type
	   *   SQL_ARD_TYPE.
	   */
	  if (!field_info->scale) break;
	  if (!hdesc) {
	    odbc_check_error("odbc->fetch_row", "SQLGetStmtAttr() failed",
			     SQLGetStmtAttr(hstmt, SQL_ATTR_APP_ROW_DESC,
					    &hdesc, 0, NULL),
			     NULL, NULL);
	    /* NB: hdesc is an implicit descriptor handle, and is
	     *     freed when hstmt is freed.
	     */
	  }
	  odbc_check_error("odbc->fetch_row", "SQLSetDescField() failed",
			   SQLSetDescField(hdesc, i + 1, SQL_DESC_TYPE,
					   (void*)SQL_C_NUMERIC, 0),
			   NULL, NULL);
	  odbc_check_error("odbc->fetch_row", "SQLSetDescField() failed",
			   SQLSetDescField(hdesc, i + 1, SQL_DESC_PRECISION,
					   (void*)(field_info->size-1), 0),
			   NULL, NULL);
	  odbc_check_error("odbc->fetch_row", "SQLSetDescField() failed",
			   SQLSetDescField(hdesc, i + 1, SQL_DESC_SCALE,
					   (void*)(ptrdiff_t)field_info->scale,
					   0),
			   NULL, NULL);
	  field_info->bin_type = field_type = SQL_ARD_TYPE;
	  break;
	default:
	case SQL_C_BINARY:
	  break;
	case SQL_C_WCHAR:
	  /* Adjust for NUL. */
	  pad = sizeof(SQLWCHAR);
	  break;
	}

	while((bytes = len)) {
#ifdef SQL_NO_TOTAL
	  if (bytes == SQL_NO_TOTAL) {
	    bytes = BLOB_BUFSIZ;
	  }
#endif /* SQL_NO_TOTAL */
	  switch(field_type) {
	  case SQL_C_CHAR:
	  default:
	  case SQL_C_BINARY:
	    s = begin_shared_string(bytes);
	    break;
	  case SQL_C_WCHAR:
	    s = begin_wide_shared_string(bytes/sizeof(SQLWCHAR),
					 sizeof(SQLWCHAR)>2?2:1);
	    break;
	  }

#ifdef ODBC_DEBUG
	  fprintf(stderr, "ODBC:fetch_row(): Field %d: "
		  "SQLGetData(X, %d, %d, X, %d, X)...\n",
		  i + 1, i+1, field_type, bytes+pad);
#endif /* ODBC_DEBUG */

	  ODBC_ALLOW();

	  code = SQLGetData(hstmt, (SQLUSMALLINT)(i+1),
			    field_type, s->str, bytes + pad, &len);

	  ODBC_DISALLOW();

	  num_strings++;
#ifdef ODBC_DEBUG
	  fprintf(stderr,
		  "ODBC:fetch_row(): %d:%d: Got %ld/%ld bytes (pad: %ld).\n"
		  "     Code: %d\n",
		  i+1, num_strings, (long)bytes, (long)len, (long)pad,
		  code);
#endif /* ODBC_DEBUG */
	  if (code == SQL_NO_DATA_FOUND) {
	    /* No data or end marker. */
	    free_string(s);
	    push_empty_string();
	    break;
	  } else {
	    odbc_check_error("odbc->fetch_row", "SQLGetData() failed",
			     code, NULL, NULL);
	    if (!len) {
	      free_string(s);
	      push_empty_string();
	      break;
	    } else if (len == SQL_NULL_DATA) {
	      free_string(s);
	      if (num_strings > 1) {
		num_strings--;
	      } else {
		push_svalue(null_value);
		num_strings = -1;
	      }
	      break;
#ifdef SQL_NO_TOTAL
	    } else if (len == SQL_NO_TOTAL) {
	      /* More data remaining... */
	      push_string(end_shared_string(s));
#ifdef ODBC_DEBUG
	      fprintf(stderr, "ODBC:fetch_row(): More data remaining.\n");
#endif /* ODBC_DEBUG */
#endif /* SQL_NO_TOTAL */
	    } else {
	      SQLLEN str_len = len;
	      if (len > bytes) {
		/* Possibly truncated result. */
		str_len = bytes;
		len -= bytes;
#ifdef ODBC_DEBUG
		fprintf(stderr, "ODBC:fetch_row(): %ld bytes remaining.\n",
			(long)len);
#endif /* ODBC_DEBUG */
	      } else len = 0;

	      str_len = str_len>>s->size_shift;
	      push_string(end_and_resize_shared_string(s, str_len));
	    }
	  }
	}
	if (num_strings >= 0) {
	  if (!num_strings) {
	    push_empty_string();
	  } else if (num_strings > 1) {
	    f_add(num_strings);
	  }
	  if (factory) {
	    factory(i);
	  }
	}
      }
    }
    f_aggregate(PIKE_ODBC_RES->num_fields);
  }
}

/* int eof() */
static void f_eof(INT32 UNUSED(args))
{
  Pike_error("odbc->eof(): Not implemented yet!\n");
}

/* void seek() */
static void f_seek(INT32 UNUSED(args))
{
  Pike_error("odbc->seek(): Not implemented yet!\n");
}
 
/*
 * Module linkage
 */
 
void init_odbc_res_programs(void)
{
  start_new_program();
  ADD_STORAGE(struct precompiled_odbc_result);

  map_variable("_odbc", "object", 0,
	       OFFSETOF(precompiled_odbc_result, obj), T_OBJECT);
  map_variable("_fields", "array(mapping(string:mixed))", 0,
	       OFFSETOF(precompiled_odbc_result, fields), T_ARRAY);
  map_variable("_null_value", "mixed", 0,
	       OFFSETOF(precompiled_odbc_result, null_value), T_MIXED);
 
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
  odbc_result_fun_num =
    add_program_constant("result", odbc_result_program, 0);

  start_new_program();
  low_inherit(odbc_result_program, NULL, -1, 0, 0, NULL);
  ADD_FUNCTION("fetch_row", f_fetch_typed_row,
	       tFunc(tVoid,tOr(tInt,tArr(tOr4(tStr,tInt,tFlt,tObj)))),
	       ID_PUBLIC);

  /* Prototypes for functions implemented in Pike. */
  scale_numeric_fun_num =
    ADD_FUNCTION("scale_numeric", NULL,
		 tFunc(tInt tInt, tOr(tInt, tObj)),
		 ID_PUBLIC);
  time_factory_fun_num =
    ADD_FUNCTION("time_factory", NULL,
		 tFunc(tInt tInt tInt tOr(tInt, tVoid), tMix),
		 ID_PUBLIC);
  timestamp_factory_fun_num =
    ADD_FUNCTION("timestamp_factory", NULL,
		 tFunc(tInt tInt tInt
		       tOr(tInt, tVoid) tOr(tInt, tVoid)
		       tOr(tInt, tVoid) tOr(tInt, tVoid)
		       tOr(tInt, tVoid) tOr(tInt, tVoid), tMix),
		 ID_PUBLIC);
  user_defined_factory_fun_num =
    ADD_FUNCTION("user_defined_factory", NULL,
		 tFunc(tStr8 tMap(tMix, tMix), tMix),
		 ID_PUBLIC);
  uuid_factory_fun_num =
    ADD_FUNCTION("uuid_factory", NULL, tFunc(tStr8, tObj), ID_PUBLIC);

  odbc_typed_result_program = end_program();
  odbc_typed_result_fun_num =
    add_program_constant("typed_result", odbc_typed_result_program, 0);
}
 
void exit_odbc_res(void)
{
  if (odbc_typed_result_program) {
    free_program(odbc_typed_result_program);
    odbc_typed_result_program = NULL;
  }
  if (odbc_result_program) {
    free_program(odbc_result_program);
    odbc_result_program = NULL;
  }
}

#else
static int place_holder;	/* Keep the compiler happy */
#endif /* HAVE_ODBC */
