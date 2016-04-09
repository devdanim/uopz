/*
  +----------------------------------------------------------------------+
  | uopz                                                                 |
  +----------------------------------------------------------------------+
  | Copyright (c) Joe Watkins 2015                                       |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Joe Watkins <krakjoe@php.net>                                |
  +----------------------------------------------------------------------+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "Zend/zend_closures.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_string.h"
#include "Zend/zend_inheritance.h"
#include "Zend/zend_compile.h"
#include "Zend/zend_vm_opcodes.h"

#ifdef HAVE_SPL
#include "ext/spl/spl_exceptions.h"
#else
/* {{{ */
zend_class_entry *spl_ce_RuntimeException;
zend_class_entry *spl_ce_InvalidArgumentException; /* }}} */
#endif

#include "uopz.h"
#include "copy.h"

ZEND_DECLARE_MODULE_GLOBALS(uopz)

#define UOPZ_RETURN_EXECUTE 0x00000001
#define UOPZ_RETURN_BUSY	0x00000010

typedef struct _uopz_return_t {
	zval value;
	zend_uchar flags;
	zend_class_entry *clazz;
	zend_string *function;
} uopz_return_t;

#define UOPZ_RETURN_IS_EXECUTABLE(u) (((u)->flags & UOPZ_RETURN_EXECUTE) == UOPZ_RETURN_EXECUTE)
#define UOPZ_RETURN_IS_BUSY(u) (((u)->flags & UOPZ_RETURN_BUSY) == UOPZ_RETURN_BUSY)

typedef struct _uopz_hook_t {
	zval closure;
	zend_class_entry *clazz;
	zend_string *function;
	zend_bool busy;
} uopz_hook_t;

typedef void (*zend_execute_internal_f) (zend_execute_data *, zval *);
typedef void (*zend_execute_f) (zend_execute_data *);

zend_execute_internal_f zend_execute_internal_function;
zend_execute_f zend_execute_function;

#define uopz_parse_parameters(spec, ...) zend_parse_parameters_ex\
	(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS(), spec, ##__VA_ARGS__)
#define uopz_refuse_parameters(message, ...) zend_throw_exception_ex\
	(spl_ce_InvalidArgumentException, 0, message, ##__VA_ARGS__)
#define uopz_exception(message, ...) zend_throw_exception_ex\
	(spl_ce_RuntimeException, 0, message, ##__VA_ARGS__)

/* {{{ */
typedef struct _uopz_magic_t {
	const char *name;
	size_t      length;
	int         id;
} uopz_magic_t;

#define UOPZ_MAGIC(name, id) {name, sizeof(name)-1, id}
#define UOPZ_MAGIC_END	     {NULL, 0, 0L}

static const uopz_magic_t umagic[] = {
	UOPZ_MAGIC(ZEND_CONSTRUCTOR_FUNC_NAME, 0),
	UOPZ_MAGIC(ZEND_DESTRUCTOR_FUNC_NAME, 1),
	UOPZ_MAGIC(ZEND_CLONE_FUNC_NAME, 2),
	UOPZ_MAGIC(ZEND_GET_FUNC_NAME, 3),
	UOPZ_MAGIC(ZEND_SET_FUNC_NAME, 4),
	UOPZ_MAGIC(ZEND_UNSET_FUNC_NAME, 5),
	UOPZ_MAGIC(ZEND_ISSET_FUNC_NAME, 6),
	UOPZ_MAGIC(ZEND_CALL_FUNC_NAME, 7),
	UOPZ_MAGIC(ZEND_CALLSTATIC_FUNC_NAME, 8),
	UOPZ_MAGIC(ZEND_TOSTRING_FUNC_NAME, 9),
	UOPZ_MAGIC("serialize", 10),
	UOPZ_MAGIC("unserialize", 11),
	UOPZ_MAGIC(ZEND_DEBUGINFO_FUNC_NAME, 12),
	UOPZ_MAGIC_END
};

static inline void uopz_handle_magic(zend_class_entry *clazz, zend_string *name, zend_function *function) {
	uopz_magic_t *magic;

	for (magic = (uopz_magic_t*) umagic; magic->name; magic++) {
		if (ZSTR_LEN(name) == magic->length &&
				strncasecmp(ZSTR_VAL(name), magic->name, magic->length) == SUCCESS) {

			switch (magic->id) {
				case 0: clazz->constructor = function; break;
				case 1: clazz->destructor = function; break;
				case 2: clazz->clone = function; break;
				case 3: clazz->__get = function; break;
				case 4: clazz->__set = function; break;
				case 5: clazz->__unset = function; break;
				case 6: clazz->__isset = function; break;
				case 7: clazz->__call = function; break;
				case 8: clazz->__callstatic = function; break;
				case 9: clazz->__tostring = function; break;
				case 10: clazz->serialize_func = function; break;
				case 11: clazz->unserialize_func = function; break;
				case 12: clazz->__debugInfo = function; break;
			}
			return;
		}
	}
}
/* }}} */

typedef struct _uopz_backup_t {
	HashTable *table;
	zend_string *name;
	zend_function *function;
} uopz_backup_t;

/* {{{ this is awkward, but finds private functions ... so don't "fix" it ... */
static int uopz_find_function(HashTable *table, zend_string *name, zend_function **function) {
	Bucket *bucket;

	ZEND_HASH_FOREACH_BUCKET(table, bucket) {
		if (zend_string_equals_ci(bucket->key, name)) {
			if (function) {
				*function = (zend_function*) Z_PTR(bucket->val);
			}
			return SUCCESS;
		}
	} ZEND_HASH_FOREACH_END();

	return FAILURE;
} /* }}} */

/* {{{ */
static void uopz_backup_table_dtor(zval *zv) {
	zend_hash_destroy(Z_PTR_P(zv));
	efree(Z_PTR_P(zv));
} /* }}} */

/* {{{ */
static inline zend_bool uopz_replace_function(HashTable *table, zend_string *name, zend_function *function, zend_bool is_shutdown) {
	zend_bool result;
	dtor_func_t dtor;
	zend_bool keep_current;
	zend_function *current = zend_hash_find_ptr(table, name);

	if (function == current) {
		/* nothing to do actually */
		return 1;
	}

	keep_current = 0;

	if (current && !is_shutdown) {
		/* see, if we have a backed up function and if it matches one being replaced, and if so - don't touch it */
		HashTable *backups = zend_hash_index_find_ptr(&UOPZ(backup), (zend_long) table);
		if (backups) {
			uopz_backup_t *backup = zend_hash_find_ptr(backups, name);
			keep_current = backup && backup->function == current;
		}
	}
	/* in shutdown passed replacement is really a backed up function, so no need to keep current */

	if (keep_current) {
		dtor = table->pDestructor;
		table->pDestructor = NULL;
	}

	if (function) {
		result = zend_hash_update_ptr(table, name, function) != NULL;
	} else {
		result = zend_hash_del(table, name) == SUCCESS;
	}

	if (keep_current) {
		table->pDestructor = dtor;
	}

	return result;
} /* }}} */

/* {{{ */
static void uopz_backup_dtor(zval *zv) {
	uopz_backup_t *backup = (uopz_backup_t*) Z_PTR_P(zv);
	uopz_replace_function(backup->table, backup->name, backup->function, 1);
	zend_string_release(backup->name);
	efree(backup);
} /* }}} */

/* {{{ */
static zend_bool uopz_backup(zend_class_entry *clazz, zend_string *name) {
	HashTable *table = clazz ? &clazz->function_table : CG(function_table);
	HashTable *backups = zend_hash_index_find_ptr(&UOPZ(backup), (zend_long) table);
	uopz_backup_t backup;
	zend_function *function;
	zend_string *lower = zend_string_tolower(name);

	if (!backups) {
		ALLOC_HASHTABLE(backups);
		zend_hash_init(backups, 8, NULL, uopz_backup_dtor, 0);
		if (!zend_hash_index_add_ptr(
			&UOPZ(backup), (zend_long) table, backups)) {
			zend_hash_destroy(backups);
			FREE_HASHTABLE(backups);
			return 0;
		}
	}

	if (zend_hash_exists(backups, lower)) {
		zend_string_release(lower);
		return 0;
	}

	backup.table = table;
	backup.name  = lower;
	backup.function = uopz_find_function(table, lower, &function) == SUCCESS ? function : NULL;

	if (!zend_hash_add_mem(backups, lower, &backup, sizeof(uopz_backup_t))) {
		zend_string_release(lower);
		return 0;
	}
	return 1;
} /* }}} */

/* {{{ proto bool uopz_backup(string function)
	   proto bool uopz_backup(string class, string method) */
PHP_FUNCTION(uopz_backup) {
	zend_string *name = NULL;
	zend_class_entry *clazz = NULL;
	
	if (uopz_parse_parameters("S", &name) != SUCCESS &&
		uopz_parse_parameters("CS", &clazz, &name) != SUCCESS) {
		uopz_refuse_parameters(
			"unexpected parameter combination, "
			"expected "
			"(class, name) or (name)");
		return;
	}

	RETVAL_BOOL(uopz_backup(clazz, name));
} /* }}} */

/* {{{ */
static void php_uopz_init_globals(zend_uopz_globals *ng) {

} /* }}} */

/* {{{ */
static void php_uopz_table_dtor(zval *zv) {
	zend_hash_destroy((HashTable*) Z_PTR_P(zv));
	efree(Z_PTR_P(zv));
} /* }}} */

/* {{{ */
static inline void php_uopz_function_dtor(zval *zv) {
	destroy_op_array((zend_op_array*) Z_PTR_P(zv));
} /* }}} */

static uopz_hook_t* uopz_find_hook(zend_function *function) {
	HashTable *returns = function->common.scope ? zend_hash_find_ptr(&UOPZ(hooks), function->common.scope->name) :
												  zend_hash_index_find_ptr(&UOPZ(hooks), 0);

	if (returns && function->common.function_name) {
		Bucket *bucket;

		ZEND_HASH_FOREACH_BUCKET(returns, bucket) {
			if (zend_string_equals_ci(function->common.function_name, bucket->key)) {
				return Z_PTR(bucket->val);
			}
		} ZEND_HASH_FOREACH_END();
	}

	return NULL;
}

static uopz_return_t* uopz_find_return(zend_function *function) {
	HashTable *returns = function->common.scope ? zend_hash_find_ptr(&UOPZ(returns), function->common.scope->name) :
												  zend_hash_index_find_ptr(&UOPZ(returns), 0);

	if (returns && function->common.function_name) {
		Bucket *bucket;

		ZEND_HASH_FOREACH_BUCKET(returns, bucket) {
			if (zend_string_equals_ci(function->common.function_name, bucket->key)) {
				return Z_PTR(bucket->val);
			}
		} ZEND_HASH_FOREACH_END();
	}

	return NULL;
}

static void php_uopz_execute_return(uopz_return_t *ureturn, zend_execute_data *execute_data, zval *return_value) { /* {{{ */
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	char *error = NULL;
	zval closure, 
		 rv,
		 *result = return_value ? return_value : &rv;
	const zend_function *overload = zend_get_closure_method_def(&ureturn->value);

	zend_execute_data *prev_execute_data = execute_data;

	ZVAL_UNDEF(&rv);

	ureturn->flags ^= UOPZ_RETURN_BUSY;

	zend_create_closure(&closure, (zend_function*) overload, 
		ureturn->clazz, ureturn->clazz, Z_OBJ(EX(This)) ? &EX(This) : NULL);

	if (zend_fcall_info_init(&closure, 0, &fci, &fcc, NULL, &error) != SUCCESS) {
		uopz_exception("cannot use return value set for %s as function: %s",
			ZSTR_VAL(EX(func)->common.function_name), error);
		if (error) {
			efree(error);
		}
		goto _exit_php_uopz_execute_return;
	}

	if (zend_fcall_info_argp(&fci, EX_NUM_ARGS(), EX_VAR_NUM(0)) != SUCCESS) {
		uopz_exception("cannot set arguments for %s",
			ZSTR_VAL(EX(func)->common.function_name));
		goto _exit_php_uopz_execute_return;
	}

	fci.retval= result;
	
	if (zend_call_function(&fci, &fcc) == SUCCESS) {
		zend_fcall_info_args_clear(&fci, 1);

		if (!return_value) {
			if (!Z_ISUNDEF(rv)) {
				zval_ptr_dtor(&rv);
			}
		}
	}

_exit_php_uopz_execute_return:
	zval_ptr_dtor(&closure);

	ureturn->flags ^= UOPZ_RETURN_BUSY;

	EG(current_execute_data) = prev_execute_data;
} /* }}} */

static void php_uopz_execute_hook(uopz_hook_t *uhook, zend_execute_data *execute_data) { /* {{{ */
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	char *error = NULL;
	zval closure, rv;
	const zend_function *overload = zend_get_closure_method_def(&uhook->closure);

	zend_execute_data *prev_execute_data = execute_data;

	ZVAL_UNDEF(&rv);

	uhook->busy = 1;

	zend_create_closure(&closure, (zend_function*) overload, 
		uhook->clazz, uhook->clazz, Z_OBJ(EX(This)) ? &EX(This) : NULL);

	if (zend_fcall_info_init(&closure, 0, &fci, &fcc, NULL, &error) != SUCCESS) {
		uopz_exception("cannot use hook set for %s as function: %s",
			ZSTR_VAL(EX(func)->common.function_name), error);
		if (error) {
			efree(error);
		}
		goto _exit_php_uopz_execute_hook;
	}

	if (zend_fcall_info_argp(&fci, EX_NUM_ARGS(), EX_VAR_NUM(0)) != SUCCESS) {
		uopz_exception("cannot set arguments for %s hook",
			ZSTR_VAL(EX(func)->common.function_name));
		goto _exit_php_uopz_execute_hook;
	}

	fci.retval= &rv;
	
	if (zend_call_function(&fci, &fcc) == SUCCESS) {
		zend_fcall_info_args_clear(&fci, 1);
		if (!Z_ISUNDEF(rv)) {
			zval_ptr_dtor(&rv);
		}
	}

_exit_php_uopz_execute_hook:
	zval_ptr_dtor(&closure);

	uhook->busy = 0;

	EG(current_execute_data) = prev_execute_data;
} /* }}} */

static inline void uopz_run_hooks(zend_execute_data *execute_data) { /* {{{ */
	if (EX(func)) {
		uopz_hook_t *uhook = uopz_find_hook(EX(func));

		if (uhook && !uhook->busy) {
			php_uopz_execute_hook(uhook, execute_data);
		}
	}
} /* }}} */

static void php_uopz_execute_internal(zend_execute_data *execute_data, zval *return_value) { /* {{{ */
	uopz_run_hooks(execute_data);

	if (EX(func) ) {
		uopz_return_t *ureturn = uopz_find_return(EX(func));

		if (ureturn) {
			if (UOPZ_RETURN_IS_EXECUTABLE(ureturn)) {
				if (UOPZ_RETURN_IS_BUSY(ureturn)) {
					goto _php_uopz_execute_internal;
				}

				php_uopz_execute_return(ureturn, execute_data, return_value);
				return;
			}

			if (return_value) {
				ZVAL_COPY(return_value, &ureturn->value);
			}
			return;
		}
	}

_php_uopz_execute_internal:
	if (zend_execute_internal_function) {
		zend_execute_internal_function(execute_data, return_value);
	} else execute_internal(execute_data, return_value);
} /* }}} */

static void php_uopz_execute(zend_execute_data *execute_data) { /* {{{ */
	uopz_run_hooks(execute_data);

	if (EX(func)) {
		uopz_return_t *ureturn = uopz_find_return(EX(func));

		if (ureturn) {
			if (UOPZ_RETURN_IS_EXECUTABLE(ureturn)) {
				if (UOPZ_RETURN_IS_BUSY(ureturn)) {
					goto _php_uopz_execute;
				}

				php_uopz_execute_return(ureturn, execute_data, EX(return_value));
				return;
			}

			if (EX(return_value)) {
				ZVAL_COPY(EX(return_value), &ureturn->value);
			}
			return;
		}
	}

_php_uopz_execute:
	if (zend_execute_function) {
		zend_execute_function(execute_data);
	} else execute_ex(execute_data);
} /* }}} */

/* {{{ init call hooks */
static int uopz_init_call_hook(zend_execute_data *execute_data) {
	switch (EX(opline)->opcode) {
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_FCALL:
		case ZEND_INIT_NS_FCALL_BY_NAME: {
			zval *function_name = EX_CONSTANT(EX(opline)->op2);
			CACHE_PTR(Z_CACHE_SLOT_P(function_name), NULL);
		} break;

		case ZEND_INIT_METHOD_CALL: {
			if (EX(opline)->op2_type == IS_CONST) {
				zval *function_name = EX_CONSTANT(EX(opline)->op2);
				CACHE_POLYMORPHIC_PTR(Z_CACHE_SLOT_P(function_name), NULL, NULL);
			}
		} break;

		case ZEND_INIT_STATIC_METHOD_CALL: {
			if (EX(opline)->op2_type == IS_CONST) {
				zval *function_name = EX_CONSTANT(EX(opline)->op2);
				if (EX(opline)->op1_type == IS_CONST) {
					CACHE_PTR(Z_CACHE_SLOT_P(function_name), NULL);
				} else {
					CACHE_POLYMORPHIC_PTR(Z_CACHE_SLOT_P(function_name), NULL, NULL);
				}
			}
		} break;
	}

	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static inline void uopz_register_init_call_hooks() {
	zend_set_user_opcode_handler(ZEND_INIT_FCALL_BY_NAME, uopz_init_call_hook);
	zend_set_user_opcode_handler(ZEND_INIT_FCALL, uopz_init_call_hook);
	zend_set_user_opcode_handler(ZEND_INIT_NS_FCALL_BY_NAME, uopz_init_call_hook);
	zend_set_user_opcode_handler(ZEND_INIT_METHOD_CALL, uopz_init_call_hook);
	zend_set_user_opcode_handler(ZEND_INIT_STATIC_METHOD_CALL, uopz_init_call_hook);
} /* }}} */

static int uopz_mock_new_handler(zend_execute_data *execute_data) { /* {{{ */
	zend_execute_data *prev_execute_data = execute_data;
	int UOPZ_VM_ACTION = ZEND_USER_OPCODE_DISPATCH;
	
	if (EXPECTED(EX(opline)->op1_type == IS_CONST)) {
		zend_string *key;
		zend_string *clazz = NULL;
		zval *mock = NULL;
		zend_class_entry *ce = CACHED_PTR(Z_CACHE_SLOT_P(EX_CONSTANT(EX(opline)->op1)));

		if (UNEXPECTED(ce == NULL)) {
			clazz = Z_STR_P(EX_CONSTANT(EX(opline)->op1));
		} else {
			clazz = ce->name;
		}

		key = zend_string_tolower(clazz);

		if (UNEXPECTED((mock = zend_hash_find(&UOPZ(mocks), key)))) {
			switch (Z_TYPE_P(mock)) {
				case IS_OBJECT:
					ZVAL_COPY(
						EX_VAR(EX(opline)->result.var), mock);
#if PHP_VERSION_ID < 70100
					EX(opline) = 
						OP_JMP_ADDR(EX(opline), EX(opline)->op2);
#else
					if (EX(opline)->extended_value == 0 && (EX(opline)+1)->opcode == ZEND_DO_FCALL) {
						EX(opline) += 2;	
					}
#endif
					UOPZ_VM_ACTION = ZEND_USER_OPCODE_CONTINUE;
				break;

				case IS_STRING:
					ce = zend_lookup_class(Z_STR_P(mock));
					if (EXPECTED(ce))	 {
						CACHE_PTR(Z_CACHE_SLOT_P(EX_CONSTANT(EX(opline)->op1)), ce);
					}
				break;
			}
		}

		zend_string_release(key);
	}

	EG(current_execute_data) = prev_execute_data;

	return UOPZ_VM_ACTION;
} /* }}} */

static inline void uopz_register_mock_handler(void) { /* {{{ */
	zend_set_user_opcode_handler(ZEND_NEW, uopz_mock_new_handler);
} /* }}} */

static int uopz_constant_handler(zend_execute_data *execute_data) { /* {{{ */
#if PHP_VERSION_ID >= 70100
	if (CACHED_PTR(Z_CACHE_SLOT_P(EX_CONSTANT(EX(opline)->op2)))) {
		CACHE_PTR(Z_CACHE_SLOT_P(EX_CONSTANT(EX(opline)->op2)), NULL);
	}
#else
	if (EX(opline)->op1_type == IS_UNUSED) {
		if (CACHED_PTR(Z_CACHE_SLOT_P(EX_CONSTANT(EX(opline)->op2)))) {
			CACHE_PTR(Z_CACHE_SLOT_P(EX_CONSTANT(EX(opline)->op2)), NULL);
		}
	} else {
		if (!EX(opline)->op2.var) {
			return ZEND_USER_OPCODE_DISPATCH;
		}

		if (EX(opline)->op1_type == IS_CONST) {
			if (CACHED_PTR(Z_CACHE_SLOT_P(EX_CONSTANT(EX(opline)->op2)))) {
				CACHE_PTR(Z_CACHE_SLOT_P(EX_CONSTANT(EX(opline)->op2)), NULL);
			}
		} else {
			CACHE_POLYMORPHIC_PTR(Z_CACHE_SLOT_P(EX_CONSTANT(EX(opline)->op2)), 
								  Z_CE_P(EX_VAR(EX(opline)->op1.var)), NULL);
		}
	}
#endif

	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static inline void uopz_register_constant_hook(void) { /* {{{ */
	zend_set_user_opcode_handler(ZEND_FETCH_CONSTANT, uopz_constant_handler);
} /* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
static PHP_MINIT_FUNCTION(uopz)
{
	ZEND_INIT_MODULE_GLOBALS(uopz, php_uopz_init_globals, NULL);

	zend_execute_internal_function = zend_execute_internal;
	zend_execute_internal = php_uopz_execute_internal;
	zend_execute_function = zend_execute_ex;
	zend_execute_ex = php_uopz_execute;

	REGISTER_LONG_CONSTANT("ZEND_USER_OPCODE_CONTINUE",		ZEND_USER_OPCODE_CONTINUE,		CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_USER_OPCODE_ENTER",		ZEND_USER_OPCODE_ENTER,			CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_USER_OPCODE_LEAVE", 		ZEND_USER_OPCODE_LEAVE,			CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_USER_OPCODE_DISPATCH", 	ZEND_USER_OPCODE_DISPATCH,		CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_USER_OPCODE_DISPATCH_TO", 	ZEND_USER_OPCODE_DISPATCH_TO,	CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_USER_OPCODE_RETURN", 		ZEND_USER_OPCODE_RETURN, 		CONST_CS|CONST_PERSISTENT);

	REGISTER_LONG_CONSTANT("ZEND_ACC_PUBLIC", 				ZEND_ACC_PUBLIC, 				CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_ACC_PRIVATE", 				ZEND_ACC_PRIVATE,				CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_ACC_PROTECTED", 			ZEND_ACC_PROTECTED,				CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_ACC_PPP_MASK", 			ZEND_ACC_PPP_MASK,				CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_ACC_STATIC", 				ZEND_ACC_STATIC,				CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_ACC_FINAL", 				ZEND_ACC_FINAL,					CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_ACC_ABSTRACT", 			ZEND_ACC_ABSTRACT,				CONST_CS|CONST_PERSISTENT);

	/* just for consistency */
	REGISTER_LONG_CONSTANT("ZEND_ACC_CLASS",     			0,  							CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_ACC_INTERFACE", 			ZEND_ACC_INTERFACE, 			CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_ACC_TRAIT",    			ZEND_ACC_TRAIT,     			CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_ACC_FETCH",				LONG_MAX,						CONST_CS|CONST_PERSISTENT);

	REGISTER_LONG_CONSTANT("ZEND_USER_FUNCTION",			ZEND_USER_FUNCTION,				CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("ZEND_INTERNAL_FUNCTION",		ZEND_INTERNAL_FUNCTION,			CONST_CS|CONST_PERSISTENT);

	uopz_register_init_call_hooks();
	uopz_register_mock_handler();
	uopz_register_constant_hook();

	return SUCCESS;
}
/* }}} */

/* {{{ */
static PHP_MSHUTDOWN_FUNCTION(uopz)
{
	zend_execute_internal = zend_execute_internal_function;
	zend_execute_ex = zend_execute_function;

	return SUCCESS;
} /* }}} */

static inline void uopz_return_dtor(zval *zv) {
	uopz_return_t *ret = Z_PTR_P(zv);
	
	zend_string_release(ret->function);
	zval_ptr_dtor(&ret->value);
}

static inline void uopz_hook_dtor(zval *zv) {
	uopz_hook_t *hook = Z_PTR_P(zv);
	
	zend_string_release(hook->function);
	zval_ptr_dtor(&hook->closure);
}

static inline void uopz_table_dtor(zval *zv) {
	zend_hash_destroy(Z_PTR_P(zv));
	efree(Z_PTR_P(zv));
}

static inline void uopz_mock_table_dtor(zval *zv) {
	zval_ptr_dtor(zv);
}

/* {{{ PHP_RINIT_FUNCTION
 */
static PHP_RINIT_FUNCTION(uopz)
{
	zend_class_entry *ce = NULL;
	zend_string *spl;

#ifdef ZTS
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	spl = zend_string_init(ZEND_STRL("RuntimeException"), 0);
	spl_ce_RuntimeException =
			(ce = zend_lookup_class(spl)) ?
				ce : zend_exception_get_default();
	zend_string_release(spl);

	spl = zend_string_init(ZEND_STRL("InvalidArgumentException"), 0);
	spl_ce_InvalidArgumentException =
			(ce = zend_lookup_class(spl)) ?
				ce : zend_exception_get_default();
	zend_string_release(spl);

	UOPZ(copts) = CG(compiler_options);

	CG(compiler_options) |= ZEND_COMPILE_HANDLE_OP_ARRAY | 
							ZEND_COMPILE_NO_CONSTANT_SUBSTITUTION | 
#ifdef ZEND_COMPILE_NO_PERSISTENT_CONSTANT_SUBSTITUTION
							ZEND_COMPILE_NO_PERSISTENT_CONSTANT_SUBSTITUTION |
#endif
							ZEND_COMPILE_IGNORE_INTERNAL_FUNCTIONS | 
							ZEND_COMPILE_IGNORE_USER_FUNCTIONS | 
							ZEND_COMPILE_GUARDS;
	/*
		We are hacking, horribly ... we can just ignore leaks ...
	*/
	PG(report_memleaks)=0;

	zend_hash_init(&UOPZ(backup), 8, NULL, uopz_backup_table_dtor, 0);
	zend_hash_init(&UOPZ(returns), 8, NULL, uopz_table_dtor, 0);
	zend_hash_init(&UOPZ(mocks), 8, NULL, uopz_mock_table_dtor, 0);
	zend_hash_init(&UOPZ(hooks), 8, NULL, uopz_table_dtor, 0);

	return SUCCESS;
} /* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
static PHP_RSHUTDOWN_FUNCTION(uopz)
{
	CG(compiler_options) = UOPZ(copts);

	zend_hash_destroy(&UOPZ(backup));
	zend_hash_destroy(&UOPZ(mocks));
	zend_hash_destroy(&UOPZ(returns));
	zend_hash_destroy(&UOPZ(hooks));

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
static PHP_MINFO_FUNCTION(uopz)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "uopz support", "enabled");
	php_info_print_table_end();
}
/* }}} */

/* {{{ */
static inline void uopz_copy(zend_class_entry *clazz, zend_string *name, zval **return_value, zval *this_ptr) {
	HashTable *table = (clazz) ? &clazz->function_table : CG(function_table);
	zend_function *function = NULL, *closure = NULL;
	zend_class_entry *scope = EG(scope);
	zend_bool staticify = 0;

	if (uopz_find_function(table, name, &function) != SUCCESS) {
		if (clazz) {
			uopz_exception(
				"could not find the requested function (%s::%s)",
				ZSTR_VAL(clazz->name), ZSTR_VAL(name));
		} else {
			uopz_exception("could not find the requested function (%s)", ZSTR_VAL(name));
		}
		return;
	}

	staticify = function->common.fn_flags & ZEND_ACC_STATIC;
	EG(scope)=function->common.scope;

	zend_create_closure(
	    *return_value,
	    function, function->common.scope, function->common.scope, 
	    this_ptr ? this_ptr : NULL);
	{
		closure = (zend_function *)zend_get_closure_method_def(*return_value);
		if (staticify) {
			closure->common.fn_flags |= ZEND_ACC_STATIC;
		} else closure->common.fn_flags &= ~ZEND_ACC_STATIC;
	}
	EG(scope)=scope;
} /* }}} */

/* {{{ proto Closure uopz_copy(string class, string function)
	   proto Closure uopz_copy(string function) */
PHP_FUNCTION(uopz_copy) {
	zend_string *name = NULL;
	zend_class_entry *clazz = NULL;

	switch (ZEND_NUM_ARGS()) {
		case 2: if (uopz_parse_parameters("CS", &clazz, &name) != SUCCESS) {
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, function)");
			return;
		} break;

		case 1: if (uopz_parse_parameters("S", &name) != SUCCESS) {
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (function)");
			return;
		} break;

		default:
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, function) or (function)");
			return;
	}

	uopz_copy(clazz, name, &return_value, getThis());
} /* }}} */

/* {{{ */
static inline zend_bool uopz_delete(zend_class_entry *clazz, zend_string *name) {
	HashTable *table = clazz ? &clazz->function_table : CG(function_table);
	zend_string *lower = zend_string_tolower(name);
	
	if (!table || !zend_hash_exists(table, lower)) {
		if (clazz) {
			uopz_exception(
				"failed to delete the function %s::%s, no overload", ZSTR_VAL(clazz->name), ZSTR_VAL(name));
		} else {
			uopz_exception(
				"failed to delete the function %s, no overload", ZSTR_VAL(name));
		}
		zend_string_release(lower);
		return 0;
	}

	uopz_backup(clazz, lower);

	if (!uopz_replace_function(table, lower, NULL, 0)) {
		if (clazz) {
			uopz_exception(
				"failed to delete the function %s::%s, delete failed", ZSTR_VAL(clazz->name), ZSTR_VAL(name));
		} else {
			uopz_exception(
				"failed to delete the function %s, delete failed", ZSTR_VAL(name));
		}
		zend_string_release(lower);
		return 0;
	}

	if (clazz) {
		uopz_handle_magic(clazz, lower, NULL);
	}

	zend_string_release(lower);

	return 1;
} /* }}} */

/* {{{ proto bool uopz_delete(mixed function)
	   proto bool uopz_delete(string class, string function) */
PHP_FUNCTION(uopz_delete) {
	zend_string *name = NULL;
	zend_class_entry *clazz = NULL;

	switch (ZEND_NUM_ARGS()) {
		case 2: if (uopz_parse_parameters("CS", &clazz, &name) != SUCCESS) {
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, function)");
			return;
		} break;

		case 1: if (uopz_parse_parameters("S", &name) != SUCCESS) {
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (function)");
			return;
		} break;

		default:
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, function) or (function)");
			return;
	}

	RETVAL_BOOL(uopz_delete(clazz, name));
} /* }}} */

/* {{{ */
static inline zend_bool uopz_restore(zend_class_entry *clazz, zend_string *name) {
	HashTable *table = clazz ? &clazz->function_table : CG(function_table),
			  *backups;
	zend_bool result = 0;
	
	backups = zend_hash_index_find_ptr(&UOPZ(backup), (zend_long) table);

	if (backups) {
		zend_string *lower = zend_string_tolower(name);
		uopz_backup_t *backup = zend_hash_find_ptr(backups, lower);
		
		if (backup) {
			result = uopz_replace_function(table, lower, backup->function, 0);
			if (result && clazz) {
				uopz_handle_magic(clazz, lower, backup->function);
			}
		}
		
		zend_string_release(lower);
	}

	return result;
} /* }}} */

/* {{{ proto bool uopz_restore(mixed function)
	   proto bool uopz_restore(string class, string function) */
PHP_FUNCTION(uopz_restore) {
	zend_string *name = NULL;
	zend_class_entry *clazz = NULL;

	switch (ZEND_NUM_ARGS()) {
		case 2: if (uopz_parse_parameters("CS", &clazz, &name) != SUCCESS) {
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, function)");
			return;
		} break;

		case 1: if (uopz_parse_parameters("S", &name) != SUCCESS) {
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (function)");
			return;
		} break;

		default:
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, function) or (function)");
			return;
	}

	RETVAL_BOOL(uopz_restore(clazz, name));
} /* }}} */

/* {{{ */
static inline zend_bool uopz_redefine(zend_class_entry *clazz, zend_string *name, zval *variable) {
	zend_constant *zconstant;
	HashTable *table = clazz ? &clazz->constants_table : EG(zend_constants);

	switch (Z_TYPE_P(variable)) {
		case IS_LONG:
		case IS_DOUBLE:
		case IS_STRING:
		case IS_TRUE:
		case IS_FALSE:
		case IS_RESOURCE:
		case IS_NULL:
			break;

		default:
			if (clazz) {
				uopz_exception(
					"failed to redefine the constant %s::%s, type not allowed", ZSTR_VAL(clazz->name), ZSTR_VAL(name));
			} else {
				uopz_exception(
					"failed to redefine the constant %s, type not allowed", ZSTR_VAL(name));
			}
			return 0;
	}

	if (!(zconstant = zend_hash_find_ptr(table, name))) {
		if (!clazz) {
			zend_constant create;

			ZVAL_COPY(&create.value, variable);
			create.flags = CONST_CS;
			create.name = zend_string_copy(name);
			create.module_number = PHP_USER_CONSTANT;

			if (zend_register_constant(&create) != SUCCESS) {
				uopz_exception(
					"failed to redefine the constant %s, operation failed", ZSTR_VAL(name));
				zval_dtor(&create.value);
				return 0;
			}
		} else {
			if (zend_declare_class_constant(clazz, ZSTR_VAL(name), ZSTR_LEN(name), variable) == FAILURE) {
				uopz_exception(
					"failed to redefine the constant %s::%s, update failed", ZSTR_VAL(clazz->name), ZSTR_VAL(name));
				return 0;
			}
			Z_TRY_ADDREF_P(variable);
		}

		return 1;
	}

	if (!clazz) {
		if (zconstant->module_number == PHP_USER_CONSTANT) {
			zval_dtor(&zconstant->value);
			ZVAL_COPY(&zconstant->value, variable);
		} else {
			uopz_exception(
				"failed to redefine the internal %s, not allowed", ZSTR_VAL(name));
			return 0;
		}
	} else {
		zend_hash_del(table, name);
		
		if (zend_declare_class_constant(clazz, ZSTR_VAL(name), ZSTR_LEN(name), variable) == FAILURE) {
			uopz_exception(
				"failed to redefine the constant %s::%s, update failed", ZSTR_VAL(clazz->name), ZSTR_VAL(name));
			return 0;
		}
		Z_TRY_ADDREF_P(variable);
	}

	return 1;
} /* }}} */

static inline void uopz_set_return(zend_class_entry *clazz, zend_string *name, zval *value, zend_bool execute) { /* {{{ */
	HashTable *returns;
	uopz_return_t ret;
	zend_string *key = zend_string_tolower(name);

	if (clazz && uopz_find_function(&clazz->function_table, key, NULL) != SUCCESS) {
		uopz_exception(
			"failed to set return for %s::%s, the method does not exist",
			ZSTR_VAL(clazz->name),
			ZSTR_VAL(name));
		zend_string_release(key);
		return;
	}

	if (clazz) {
		returns = zend_hash_find_ptr(&UOPZ(returns), clazz->name);
	} else returns = zend_hash_index_find_ptr(&UOPZ(returns), 0);
	
	if (!returns) {
		ALLOC_HASHTABLE(returns);
		zend_hash_init(returns, 8, NULL, uopz_return_dtor, 0);
		if (clazz) {
			zend_hash_update_ptr(&UOPZ(returns), clazz->name, returns);
		} else zend_hash_index_update_ptr(&UOPZ(returns), 0, returns);
	}

	memset(&ret, 0, sizeof(uopz_return_t));
	
	ret.clazz = clazz;
	ret.function = zend_string_copy(name);
	ZVAL_COPY(&ret.value, value);
	ret.flags = execute ? UOPZ_RETURN_EXECUTE : 0;
	
	zend_hash_update_mem(returns, key, &ret, sizeof(uopz_return_t));
	zend_string_release(key);
} /* }}} */

static inline zend_bool uopz_is_magic_method(zend_class_entry *clazz, zend_string *function) /* {{{ */
{ 
	if (!clazz) {
		return 0;
	}

	if (zend_string_equals_literal_ci(function, "__construct") ||
		zend_string_equals_literal_ci(function, "__destruct") ||
		zend_string_equals_literal_ci(function, "__clone") ||
		zend_string_equals_literal_ci(function, "__get") ||
		zend_string_equals_literal_ci(function, "__set") ||
		zend_string_equals_literal_ci(function, "__unset") ||
		zend_string_equals_literal_ci(function, "__isset") ||
		zend_string_equals_literal_ci(function, "__call") ||
		zend_string_equals_literal_ci(function, "__callstatic") ||
		zend_string_equals_literal_ci(function, "__tostring") ||
		zend_string_equals_literal_ci(function, "__debuginfo") ||
		zend_string_equals_literal_ci(function, "__serialize") ||
		zend_string_equals_literal_ci(function, "__unserialize") ||
		zend_string_equals_literal_ci(function, "__sleep") ||
		zend_string_equals_literal_ci(function, "__wakeup")) {
		return 1;
	}

	return 0;
} /* }}} */

/* {{{ proto void uopz_set_return(string class, string function, mixed value)
	   proto void uopz_set_return(function, mixed value) */
PHP_FUNCTION(uopz_set_return) 
{
	zend_string *function = NULL;
	zval *variable = NULL;
	zend_class_entry *clazz = NULL;
	zend_long execute = 0;

	if (uopz_parse_parameters("CSz|l", &clazz, &function, &variable, &execute) != SUCCESS &&
		uopz_parse_parameters("Sz|l", &function, &variable, &execute) != SUCCESS) {
		uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, function, variable [, execute]) or (function, variable [, execute])");
		return;
	}

	if (execute && !instanceof_function(Z_OBJCE_P(variable), zend_ce_closure)) {
		uopz_refuse_parameters(
			"only closures are accepted as executable return values");
		return;
	}

	if (uopz_is_magic_method(clazz, function)) {
		uopz_refuse_parameters(
			"will not override magic methods, too magical");
		return;
	}

	uopz_set_return(clazz, function, variable, execute);
} /* }}} */

static inline void uopz_unset_return(zend_class_entry *clazz, zend_string *function) { /* {{{ */
	HashTable *returns;

	if (clazz) {
		returns = zend_hash_find_ptr(&UOPZ(returns), clazz->name);
	} else returns = zend_hash_index_find_ptr(&UOPZ(returns), 0);

	if (!returns) {
		return;
	}

	{
		zend_string *key = zend_string_tolower(function);

		zend_hash_del(returns, key);

		zend_string_release(key);
	}
} /* }}} */

/* {{{ proto void uopz_unset_return(string class, string function)
	   proto void uopz_unset_return(string function) */
PHP_FUNCTION(uopz_unset_return) 
{
	zend_string *function = NULL;
	zend_class_entry *clazz = NULL;

	switch (ZEND_NUM_ARGS()) {
		case 2: {
			if (uopz_parse_parameters("CS", &clazz, &function) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (class, function)");
				return;
			}
		} break;

		case 1: {
			if (uopz_parse_parameters("S", &function) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (function)");
				return;
			}
		} break;

		default:
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, function) or (function)");
			return;
	}

	uopz_unset_return(clazz, function);
} /* }}} */

static inline void uopz_get_return(zend_class_entry *clazz, zend_string *function, zval *return_value) { /* {{{ */
	HashTable *returns;
	uopz_return_t *ureturn;

	if (clazz) {
		returns = zend_hash_find_ptr(&UOPZ(returns), clazz->name);
	} else returns = zend_hash_index_find_ptr(&UOPZ(returns), 0);

	if (!returns) {
		return;
	}

	ureturn = zend_hash_find_ptr(returns, function);

	if (!ureturn) {
		return;
	}
	
	ZVAL_COPY(return_value, &ureturn->value);
} /* }}} */

/* {{{ proto mixed uopz_get_return(string class, string function)
	   proto mixed uopz_get_return(string function) */
PHP_FUNCTION(uopz_get_return) 
{
	zend_string *function = NULL;
	zend_class_entry *clazz = NULL;

	switch (ZEND_NUM_ARGS()) {
		case 2: {
			if (uopz_parse_parameters("CS", &clazz, &function) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (class, function)");
				return;
			}
		} break;

		case 1: {
			if (uopz_parse_parameters("S", &function) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (function)");
				return;
			}
		} break;

		default:
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, function) or (function)");
			return;
	}

	uopz_get_return(clazz, function, return_value);
} /* }}} */

static inline void uopz_set_mock(zend_string *clazz, zval *mock) { /* {{{ */
	zend_string *key = zend_string_tolower(clazz);
	
	if (zend_hash_exists(&UOPZ(mocks), key)) {
		uopz_exception(
			"cannot create mock for %s",
			ZSTR_VAL(clazz));
		zend_string_release(key);
		return;
	}

	if (zend_hash_update(&UOPZ(mocks), key, mock)) {
		zval_copy_ctor(mock);
	}

	zend_string_release(key);
} /* }}} */

/* {{{ proto void uopz_set_mock(string class, mixed mock) */
PHP_FUNCTION(uopz_set_mock) 
{
	zend_string *clazz = NULL;
	zval *mock = NULL;

	if (uopz_parse_parameters("Sz", &clazz, &mock) != SUCCESS) {
		uopz_refuse_parameters(
			"unexpected parameter combination, expected (class, mock), classes not found ?");
		return;
	}

	if (!mock || (Z_TYPE_P(mock) != IS_STRING && Z_TYPE_P(mock) != IS_OBJECT)) {
		uopz_refuse_parameters(
			"unexpected parameter combination, mock is expected to be a string, or an object");
		return;
	}

	uopz_set_mock(clazz, mock);
} /* }}} */

static inline void uopz_unset_mock(zend_string *clazz) { /* {{{ */
	zend_string *key = zend_string_tolower(clazz);
	
	if (!zend_hash_exists(&UOPZ(mocks), key)) {
		uopz_exception(
			"cannot delete mock %s, does not exists",
			ZSTR_VAL(clazz));
		zend_string_release(key);
		return;
	}

	zend_hash_del(&UOPZ(mocks), key);
	zend_string_release(key);
} /* }}} */

/* {{{ proto void uopz_unset_mock(string mock) */
PHP_FUNCTION(uopz_unset_mock) 
{
	zend_string *clazz = NULL;

	if (uopz_parse_parameters("S", &clazz) != SUCCESS) {
		uopz_refuse_parameters(
			"unexpected parameter combination, expected (clazz), class not found ?");
		return;
	}

	uopz_unset_mock(clazz);
} /* }}} */

static inline void uopz_get_mock(zend_string *clazz, zval *return_value) { /* {{{ */
	zval *mock = NULL;
	zend_string *key = zend_string_tolower(clazz);
	
	if (!(mock = zend_hash_find(&UOPZ(mocks), key))) {
		zend_string_release(key);
		return;
	}

	ZVAL_COPY(return_value, mock);
	zend_string_release(key);
} /* }}} */

/* {{{ proto void uopz_get_mock(string mock) */
PHP_FUNCTION(uopz_get_mock) 
{
	zend_string *clazz = NULL;

	if (uopz_parse_parameters("S", &clazz) != SUCCESS) {
		uopz_refuse_parameters(
			"unexpected parameter combination, expected (clazz), class not found ?");
		return;
	}

	uopz_get_mock(clazz, return_value);
} /* }}} */

static inline void uopz_get_static(zend_class_entry *clazz, zend_string *function, zval *return_value) { /* {{{ */
	zend_function *entry;
	
	if (clazz) {
		if (uopz_find_function(&clazz->function_table, function, &entry) != SUCCESS) {
			return;
		}
	} else {
		if (uopz_find_function(CG(function_table), function, &entry) != SUCCESS) {
			return;
		}
	}

	if (entry->type != ZEND_USER_FUNCTION) {
		return;
	}

	if (!entry->op_array.static_variables) {
		return;
	}

	array_init(return_value);
	zend_hash_copy(Z_ARRVAL_P(return_value), 
		entry->op_array.static_variables, 
		(copy_ctor_func_t) zval_addref_p);
} /* }}} */

/* {{{ proto array uopz_get_static(string class, string method)
			 array uopz_get_static(string function) */
PHP_FUNCTION(uopz_get_static) 
{
	zend_string *function = NULL;
	zend_class_entry *clazz = NULL;
	
	switch (ZEND_NUM_ARGS()) {
		case 2: {
			if (uopz_parse_parameters("CS", &clazz, &function) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (class, function)");
				return;
			}
		} break;

		case 1: {
			if (uopz_parse_parameters("S", &function) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (function)");
				return;
			}
		} break;

		default:
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, function) or (function)");
			return;
	}

	uopz_get_static(clazz, function, return_value);
} /* }}} */

static inline void uopz_set_static(zend_class_entry *clazz, zend_string *function, zval *statics) { /* {{{ */
	zend_function *entry;
	
	if (clazz) {
		if (uopz_find_function(&clazz->function_table, function, &entry) != SUCCESS) {
			return;
		}
	} else {
		if (uopz_find_function(CG(function_table), function, &entry) != SUCCESS) {
			return;
		}
	}

	if (entry->type != ZEND_USER_FUNCTION) {
		return;
	}

	if (!entry->op_array.static_variables) {
		return;
	}

	zend_hash_clean(entry->op_array.static_variables);
	
	zend_hash_copy(
		entry->op_array.static_variables, 
		Z_ARRVAL_P(statics),
		(copy_ctor_func_t) zval_addref_p);
} /* }}} */

/* {{{ proto array uopz_set_static(string class, string method, array statics)
			 array uopz_set_static(string function, array statics) */
PHP_FUNCTION(uopz_set_static) 
{
	zend_string *function = NULL;
	zend_class_entry *clazz = NULL;
	zval *statics = NULL;
	
	switch (ZEND_NUM_ARGS()) {
		case 3: {
			if (uopz_parse_parameters("CSz", &clazz, &function, &statics) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (class, function, statics)");
				return;
			}
		} break;

		case 2: {
			if (uopz_parse_parameters("Sz", &function, &statics) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (function, statics)");
				return;
			}
		} break;

		default:
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, function, statics) or (function, statics)");
			return;
	}

	uopz_set_static(clazz, function, statics);
} /* }}} */

static inline void uopz_set_hook(zend_class_entry *clazz, zend_string *name, zval *closure) { /* {{{ */
	HashTable *hooks;
	uopz_hook_t hook;
	zend_string *key = zend_string_tolower(name);

	if (clazz && uopz_find_function(&clazz->function_table, key, NULL) != SUCCESS) {
		uopz_exception(
			"failed to set hook for %s::%s, the method does not exist",
			ZSTR_VAL(clazz->name),
			ZSTR_VAL(name));
		zend_string_release(key);
		return;
	}

	if (clazz) {
		hooks = zend_hash_find_ptr(&UOPZ(hooks), clazz->name);
	} else hooks = zend_hash_index_find_ptr(&UOPZ(hooks), 0);
	
	if (!hooks) {
		ALLOC_HASHTABLE(hooks);
		zend_hash_init(hooks, 8, NULL, uopz_hook_dtor, 0);
		if (clazz) {
			zend_hash_update_ptr(&UOPZ(hooks), clazz->name, hooks);
		} else zend_hash_index_update_ptr(&UOPZ(hooks), 0, hooks);
	}

	memset(&hook, 0, sizeof(uopz_hook_t));
	
	hook.clazz = clazz;
	hook.function = zend_string_copy(name);
	ZVAL_COPY(&hook.closure, closure);

	zend_hash_update_mem(hooks, key, &hook, sizeof(uopz_hook_t));
	zend_string_release(key);
} /* }}} */

/* {{{ proto void uopz_set_hook(string class, string function, Closure hook)
			 void uopz_set_hook(string function, Closure hook) */
PHP_FUNCTION(uopz_set_hook) 
{
	zend_string *function = NULL;
	zend_class_entry *clazz = NULL;
	zval *hook = NULL;
	
	switch (ZEND_NUM_ARGS()) {
		case 3: {
			if (uopz_parse_parameters("CSO", &clazz, &function, &hook, zend_ce_closure) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (class, function, hook)");
				return;
			}
		} break;

		case 2: {
			if (uopz_parse_parameters("SO", &function, &hook, zend_ce_closure) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (function, hook)");
				return;
			}
		} break;

		default:
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, function, hook) or (function, hook)");
			return;
	}

	uopz_set_hook(clazz, function, hook);
} /* }}} */

static inline void uopz_unset_hook(zend_class_entry *clazz, zend_string *function) { /* {{{ */
	HashTable *hooks;

	if (clazz) {
		hooks = zend_hash_find_ptr(&UOPZ(hooks), clazz->name);
	} else hooks = zend_hash_index_find_ptr(&UOPZ(hooks), 0);

	if (!hooks) {
		return;
	}

	{
		zend_string *key = zend_string_tolower(function);
		
		zend_hash_del(hooks, key);

		zend_string_release(key);
	}
} /* }}} */

/* {{{ proto void uopz_set_hook(string class, string function)
			 void uopz_set_hook(string function) */
PHP_FUNCTION(uopz_unset_hook) 
{
	zend_string *function = NULL;
	zend_class_entry *clazz = NULL;
	
	switch (ZEND_NUM_ARGS()) {
		case 2: {
			if (uopz_parse_parameters("CS", &clazz, &function) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (class, function)");
				return;
			}
		} break;

		case 1: {
			if (uopz_parse_parameters("S", &function) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (function)");
				return;
			}
		} break;

		default:
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, function) or (function)");
			return;
	}

	uopz_unset_hook(clazz, function);
} /* }}} */

static inline void uopz_get_hook(zend_class_entry *clazz, zend_string *function, zval *return_value) { /* {{{ */
	HashTable *hooks;
	uopz_hook_t *uhook;
	
	if (clazz) {
		hooks = zend_hash_find_ptr(&UOPZ(hooks), clazz->name);
	} else hooks = zend_hash_index_find_ptr(&UOPZ(hooks), 0);

	if (!hooks) {
		return;
	}

	uhook = zend_hash_find_ptr(hooks, function);

	if (!uhook) {
		return;
	}

	ZVAL_COPY(return_value, &uhook->closure);
} /* }}} */

/* {{{ proto Closure uopz_get_hook(string class, string function)
			 Closure uopz_get_hook(string function) */
PHP_FUNCTION(uopz_get_hook) 
{
	zend_string *function = NULL;
	zend_class_entry *clazz = NULL;
	
	switch (ZEND_NUM_ARGS()) {
		case 2: {
			if (uopz_parse_parameters("CS", &clazz, &function) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (class, function)");
				return;
			}
		} break;

		case 1: {
			if (uopz_parse_parameters("S", &function) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (function)");
				return;
			}
		} break;

		default:
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, function) or (function)");
			return;
	}

	uopz_get_hook(clazz, function, return_value);
} /* }}} */

/* {{{ proto bool uopz_redefine(string constant, mixed variable)
	   proto bool uopz_redefine(string class, string constant, mixed variable) */
PHP_FUNCTION(uopz_redefine)
{
	zend_string *name = NULL;
	zval *variable = NULL;
	zend_class_entry *clazz = NULL;

	switch (ZEND_NUM_ARGS()) {
		case 3: {
			if (uopz_parse_parameters("CSz", &clazz, &name, &variable) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (class, constant, variable)");
				return;
			}
		} break;

		case 2: if (uopz_parse_parameters("Sz", &name, &variable) != SUCCESS) {
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (constant, variable)");
			return;
		} break;

		default: {
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, constant, variable) or (constant, variable)");
			return;
		}
	}

	if (uopz_redefine(clazz, name, variable)) {
		if (clazz) {
			while ((clazz = clazz->parent)) {
				uopz_redefine(
					clazz, name, variable);
			}
		}
		RETURN_TRUE;
	} else {
		RETURN_FALSE;
	}
} /* }}} */

/* {{{ */
static inline zend_bool uopz_undefine(zend_class_entry *clazz, zend_string *name) {
	zend_constant *zconstant;
	HashTable *table = clazz ? &clazz->constants_table : EG(zend_constants);

	if (!(zconstant = zend_hash_find_ptr(table, name))) {
		return 0;
	}

	if (!clazz) {
		if (zconstant->module_number != PHP_USER_CONSTANT) {
			uopz_exception(
				"failed to undefine the internal constant %s, not allowed", ZSTR_VAL(name));
			return 0;
		}

		if (zend_hash_del(table, name) != SUCCESS) {
			uopz_exception(
				"failed to undefine the constant %s, delete failed", ZSTR_VAL(name));
			return 0;
		}

		return 1;
	}

	if (zend_hash_del(table, name) != SUCCESS) {
		uopz_exception(
			"failed to undefine the constant %s::%s, delete failed", ZSTR_VAL(clazz->name), ZSTR_VAL(name));
		return 0;
	}

	return 1;
} /* }}} */

/* {{{ proto bool uopz_undefine(string constant)
	   proto bool uopz_undefine(string class, string constant) */
PHP_FUNCTION(uopz_undefine)
{
	zend_string *name = NULL;
	zend_class_entry *clazz = NULL;

	switch (ZEND_NUM_ARGS()) {
		case 2: {
			if (uopz_parse_parameters("CS", &clazz, &name) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (class, constant)");
				return;
			}
		} break;

		case 1: {
			if (uopz_parse_parameters("S", &name) != SUCCESS) {
				uopz_refuse_parameters(
					"unexpected parameter combination, expected (constant)");
				return;
			}
		} break;

		default:
			uopz_refuse_parameters(
				"unexpected parameter combination, expected (class, constant) or (constant)");
			return;
	}

	if (uopz_undefine(clazz, name)) {
		if (clazz) {
			while ((clazz = clazz->parent)) {
				uopz_undefine(clazz, name);
			}
		}
		RETURN_TRUE;
	} else {
		RETURN_FALSE;
	}
} /* }}} */

/* {{{ */
static zend_bool uopz_function(zend_class_entry *clazz, zend_string *name, zval *closure, zend_long flags, zend_bool ancestry) {
	HashTable *table = clazz ? &clazz->function_table : CG(function_table);
	zend_function *destination = NULL;
	zend_function *function =  (zend_function*) zend_get_closure_method_def(closure);
	zend_string *lower = zend_string_tolower(name);

	if (!flags) {
		/* get flags from original function */
		if (uopz_find_function(table, lower, &destination) == SUCCESS) {
			flags = 
				destination->common.fn_flags;
		} else {
			/* set flags to sensible default */
			flags = ZEND_ACC_PUBLIC;
		}
	}

	uopz_backup(clazz, lower);

	function = uopz_copy_function(clazz, function);

	if (!uopz_replace_function(table, lower, function, 0)) {
		destroy_zend_function(function);
		zend_arena_release(&CG(arena), function);
		zend_string_release(lower);

		if (clazz) {
			uopz_exception("failed to create function %s::%s, update failed", ZSTR_VAL(clazz->name), ZSTR_VAL(name));
		} else {
			uopz_exception("failed to create function %s, update failed", ZSTR_VAL(name));
		}

		return 0;
	}

	function->common.prototype = NULL;
	function->common.fn_flags |= flags & ZEND_ACC_PPP_MASK;

	if (flags & ZEND_ACC_STATIC) {
		function->common.fn_flags |= ZEND_ACC_STATIC;
	}

	if (flags & ZEND_ACC_ABSTRACT) {
		function->common.fn_flags |= ZEND_ACC_ABSTRACT;
	}

	if (clazz) {
		uopz_handle_magic(clazz, lower, function);
		function->common.scope = clazz;
	} else {
		function->common.scope = NULL;
	}

	if (clazz && ancestry) {
		zend_class_entry *ce;
		ZEND_HASH_FOREACH_PTR(EG(class_table), ce) {
			if (ce->parent == clazz) {
				uopz_function(ce, name, closure, flags, ancestry);
			}
		} ZEND_HASH_FOREACH_END();
	}

	zend_string_release(lower);

	return 1;
} /* }}} */

/* {{{ proto bool uopz_function(string function, Closure handler [, int flags = 0])
	   proto bool uopz_function(string class, string method, Closure handler [, int flags = 0 [, bool ancestors = true]]) */
PHP_FUNCTION(uopz_function) {
	zend_string *name = NULL;
	zval *closure = NULL;
	zend_class_entry *clazz = NULL;
	zend_long flags = 0;
	zend_bool ancestors = 1;
	
	if (uopz_parse_parameters("SO|l", &name, &closure, zend_ce_closure, &flags) != SUCCESS &&
		uopz_parse_parameters("CSO|lb", &clazz, &name, &closure, zend_ce_closure, &flags, &ancestors) != SUCCESS) {
		uopz_refuse_parameters(
			"unexpected parameter combination, "
			"expected "
			"(class, name, closure [, flags [, ancestors]]) or (name, closure [, flags])");
		return;
	}

	RETVAL_BOOL(uopz_function(clazz, name, closure, flags, ancestors));
} /* }}} */

/* {{{ */
static inline zend_bool uopz_implement(zend_class_entry *clazz, zend_class_entry *interface) {
	zend_bool is_final =
		(clazz->ce_flags & ZEND_ACC_FINAL);

	if (!(interface->ce_flags & ZEND_ACC_INTERFACE)) {
		uopz_exception(
			"the class provided (%s) is not an interface", interface->name);
		return 0;
	}

	if (instanceof_function(clazz, interface)) {
		uopz_exception(
			"the class provided (%s) already has the interface interface", clazz->name);
		return 0;
	}

	clazz->ce_flags &= ~ZEND_ACC_FINAL;

	zend_do_implement_interface
		(clazz, interface);

	if (is_final)
		clazz->ce_flags |= ZEND_ACC_FINAL;

	return instanceof_function(clazz, interface);
} /* }}} */

/* {{{ proto bool uopz_implement(string class, string interface) */
PHP_FUNCTION(uopz_implement)
{
	zend_class_entry *clazz = NULL;
	zend_class_entry *interface = NULL;

	if (uopz_parse_parameters("CC", &clazz, &interface) != SUCCESS) {
		uopz_refuse_parameters(
			"unexpected parameter combination, expected (class, interface)");
		return;
	}

	RETURN_BOOL(uopz_implement(clazz, interface));
} /* }}} */

/* {{{ */
static inline zend_bool uopz_extend(zend_class_entry *clazz, zend_class_entry *parent) {
	zend_bool is_final = clazz->ce_flags & ZEND_ACC_FINAL;

	clazz->ce_flags &= ~ZEND_ACC_FINAL;

	if ((clazz->ce_flags & ZEND_ACC_INTERFACE) &&
		!(parent->ce_flags & ZEND_ACC_INTERFACE)) {
		uopz_exception(
		    "%s cannot extend %s, because %s is not an interface",
		     ZSTR_VAL(clazz->name), ZSTR_VAL(parent->name), ZSTR_VAL(parent->name));
		return 0;
	}

	if (instanceof_function(clazz, parent)) {
		uopz_exception(
			"class %s already extends %s",
			ZSTR_VAL(clazz->name), ZSTR_VAL(parent->name));
		return 0;
	}

	if (parent->ce_flags & ZEND_ACC_TRAIT) {
		zend_do_implement_trait(clazz, parent);
	} else zend_do_inheritance(clazz, parent);

	if (parent->ce_flags & ZEND_ACC_TRAIT)
		zend_do_bind_traits(clazz);

	if (is_final)
		clazz->ce_flags |= ZEND_ACC_FINAL;

	return instanceof_function(clazz, parent);
} /* }}} */

/* {{{ proto bool uopz_extend(string class, string parent) */
PHP_FUNCTION(uopz_extend)
{
	zend_class_entry *clazz = NULL;
	zend_class_entry *parent = NULL;

	if (uopz_parse_parameters("CC", &clazz, &parent) != SUCCESS) {
		uopz_refuse_parameters(
			"unexpected parameter combination, expected (class, parent)");
		return;
	}

	RETURN_BOOL(uopz_extend(clazz, parent));
} /* }}} */

/* {{{ */
static inline zend_bool uopz_compose(zend_string *name, HashTable *classes, HashTable *methods, HashTable *properties, zend_long flags) {
	HashPosition position[2];
	zend_class_entry *entry;
	zval *member = NULL;
	zend_ulong idx;
	zend_string *lower, *key;
	
	if ((flags & ZEND_ACC_INTERFACE)) {
		if ((properties && zend_hash_num_elements(properties))) {
			uopz_exception(
				"interfaces can not have properties");
			return 0;
		}
	}

	lower = zend_string_tolower(name);

	if (zend_hash_exists(CG(class_table), lower)) {
		uopz_exception(
			"cannot compose existing class (%s)", ZSTR_VAL(name));
		zend_string_release(lower);
		return 0;
	}

	entry = (zend_class_entry*) zend_arena_alloc(&CG(arena), sizeof(zend_class_entry));
	entry->name = zend_string_copy(name);
	entry->type = ZEND_USER_CLASS;
	
	zend_initialize_class_data(entry, 1);

	entry->ce_flags |= flags;
	
	if (!zend_hash_update_ptr(CG(class_table), lower, entry)) {
		uopz_exception(
			"cannot compose class (%s), update failed", ZSTR_VAL(name));
		zend_string_release(lower);
		return 0;
	}

#define uopz_compose_bail(s, ...) do {\
	uopz_exception(s, ##__VA_ARGS__);\
	zend_hash_del(CG(class_table), lower); \
	zend_string_release(lower); \
	return 0; \
} while(0)
	
	if (methods) {
		ZEND_HASH_FOREACH_KEY_VAL(methods, idx, key, member) {
			switch (Z_TYPE_P(member)) {
				 case IS_ARRAY:
					 if (zend_hash_num_elements(Z_ARRVAL_P(member)) == 1)
						 break;

				 case IS_OBJECT:
					 if (instanceof_function(Z_OBJCE_P(member), zend_ce_closure))
						 break;

				 default:
					 uopz_compose_bail("invalid member found in methods array, expects [modifiers => closure], or closure");
			 }
			
			 if (!key) {
				uopz_compose_bail("invalid key found in methods array, expect string keys to be legal function names");
			 }
			
			if (Z_TYPE_P(member) == IS_ARRAY) {
				zend_string *ignored;
				zval *closure;

				ZEND_HASH_FOREACH_NUM_KEY_VAL(Z_ARRVAL_P(member), flags, closure) {
					if (Z_TYPE_P(closure) != IS_OBJECT || !instanceof_function(Z_OBJCE_P(closure), zend_ce_closure)) {
						uopz_compose_bail(
							"invalid member found in methods array, "
							"expects [int => closure], got [int => other]");
					}

					if (!uopz_function(entry, key, closure, flags, 0)) {
						uopz_compose_bail(
							"failed to add method %s to class %s, "
							"previous exceptions occured", ZSTR_VAL(key), ZSTR_VAL(name));
					}
				} ZEND_HASH_FOREACH_END();
			 } else {
				if (!uopz_function(entry, key, member, ZEND_ACC_PUBLIC, 0)) {
			 		uopz_compose_bail(
				 		"failed to add method %s to class %s, "
				 		"previous exceptions occured", ZSTR_VAL(key), ZSTR_VAL(name));
				}
				zend_string_release(key);
			 }
		} ZEND_HASH_FOREACH_END();
	} 

 	if (properties) {
		ZEND_HASH_FOREACH_KEY_VAL(properties, idx, key, member) {
			if (Z_TYPE_P(member) != IS_LONG || !key) {
				uopz_compose_bail(
					"invalid member found in properties array, expects [string => int]");
				break;
			}

			if (zend_declare_property_null(entry, ZSTR_VAL(key), ZSTR_LEN(key), Z_LVAL_P(member)) != SUCCESS) {
				uopz_compose_bail(
					"failed to declare property %s::$%s, engine failure", ZSTR_VAL(entry->name), ZSTR_VAL(key));
				break;
			}
		} ZEND_HASH_FOREACH_END();
	}

	ZEND_HASH_FOREACH_VAL(classes, member) {
		zend_class_entry *parent;
		
		if (Z_TYPE_P(member) != IS_STRING) {
			continue;
		}

		if ((parent = zend_lookup_class(Z_STR_P(member)))) {

			if (entry->ce_flags & ZEND_ACC_TRAIT) {
				if (parent->ce_flags & ZEND_ACC_INTERFACE) {
					uopz_compose_bail(
						"trait %s can not implement interface %s, not allowed",
						ZSTR_VAL(entry->name), ZSTR_VAL(parent->name));
				}
			}

			if (parent->ce_flags & ZEND_ACC_INTERFACE) {
				if (entry->ce_flags & ZEND_ACC_INTERFACE) {
					if (!entry->parent) {
						zend_do_inheritance(entry, parent);
					} else {
						uopz_compose_bail(
							"interface %s may not extend %s, parent of %s already set to %s",
							ZSTR_VAL(entry->name),
							ZSTR_VAL(parent->name),
							ZSTR_VAL(entry->name),
							ZSTR_VAL(entry->parent->name));
					}
				} else zend_do_implement_interface(entry, parent);
			} else if (parent->ce_flags & ZEND_ACC_TRAIT) {
				zend_do_implement_trait(entry, parent);
			} else {
				if (!entry->parent) {
					zend_do_inheritance(entry, parent);
				} else {
					uopz_compose_bail(
						"class %s may not extend %s, parent of %s already set to %s",
						ZSTR_VAL(entry->name),
						ZSTR_VAL(parent->name),
						ZSTR_VAL(entry->name),
						ZSTR_VAL(entry->parent->name));
				}
			}
		}
	} ZEND_HASH_FOREACH_END();

	zend_do_bind_traits(entry);
	zend_string_release(lower);

	return 1;
} /* }}} */

/* {{{ proto bool uopz_compose(string name, array classes [, array methods [, array properties [, int flags = ZEND_ACC_CLASS]]]) */
PHP_FUNCTION(uopz_compose)
{
	zend_string *name = NULL;
	HashTable *classes = NULL;
	HashTable *methods = NULL;
	HashTable *properties = NULL;
	zend_long flags = 0L;

	if (uopz_parse_parameters("Sh|hhl", &name, &classes, &methods, &properties, &flags) != SUCCESS) {
		uopz_refuse_parameters(
			"unexpected parameter combination, expected (string name, array classes [, array methods [, int flags]])");
		return;
	}

	RETURN_BOOL(uopz_compose(name, classes, methods, properties, flags));
} /* }}} */

/* {{{ */
static inline void uopz_flags(zend_class_entry *clazz, zend_string *name, zend_long flags, zval *return_value) {
	HashTable *table = clazz ? &clazz->function_table : CG(function_table);
	zend_function *function = NULL;
	zend_long current = 0;

	if (!name || !ZSTR_LEN(name) || !ZSTR_VAL(name)) {
		if (flags == LONG_MAX) {
			RETURN_LONG(clazz->ce_flags);
		}

		if (flags & ZEND_ACC_PPP_MASK) {
			uopz_exception(
				"attempt to set public, private or protected on class entry, not allowed");
			return;
		}

		if (flags & ZEND_ACC_STATIC) {
			uopz_exception(
				"attempt to set static on class entry, not allowed");
			return;
		}

		current = clazz->ce_flags;
		clazz->ce_flags = flags;
		RETURN_LONG(current);
	}

	if (uopz_find_function(table, name, &function) != SUCCESS) {
		if (clazz) {
			uopz_exception(
			"failed to set or get flags of %s::%s, function does not exist",
			ZSTR_VAL(clazz->name), ZSTR_VAL(name));
		} else {
			uopz_exception(
				"failed to set or get flags of %s, function does not exist",
				ZSTR_VAL(name));
		}
		return;
	}

	if (flags == LONG_MAX) {
		RETURN_LONG(function->common.fn_flags);
	}

	current = function->common.fn_flags;
	function->common.fn_flags = flags;
	RETURN_LONG(current);
} /* }}} */

/* {{{ proto int uopz_flags(string function, int flags)
       proto int uopz_flags(string class, string function, int flags) */
PHP_FUNCTION(uopz_flags)
{
	zend_string *name = NULL;
	zend_class_entry *clazz = NULL;
	zend_long flags = LONG_MAX;
	
	switch (ZEND_NUM_ARGS()) {
		case 3: if (uopz_parse_parameters("CSl", &clazz, &name, &flags) != SUCCESS) {
			uopz_refuse_parameters(
				"unexpected parameter combination, expected "
				"(string class, string function, int flags)");
			return;
		} break;

		case 2: if (uopz_parse_parameters("Sl", &name, &flags) != SUCCESS) {
			uopz_refuse_parameters(
				"unexpected parameter combination, expected "
				"(string function, int flags)");
			return;
		} break;

		default:
			uopz_refuse_parameters(
				"unexpected parameter combination, expected "
				"(string class, string function, int flags) or (string function, int flags)");
			return;
	}

	uopz_flags(clazz, name, flags, return_value);
} /* }}} */

/* {{{ uopz */
ZEND_BEGIN_ARG_INFO(uopz_copy__arginfo, 1)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, function)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_delete_arginfo, 1)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, function)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_restore_arginfo, 1)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, function)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_redefine_arginfo, 2)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, constant)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_undefine_arginfo, 1)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, constant)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_function_arginfo, 2)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, function)
	ZEND_ARG_INFO(0, handler)
	ZEND_ARG_INFO(0, modifiers)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_backup_arginfo, 2)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, function)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_implement_arginfo, 2)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, interface)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_extend_arginfo, 2)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, parent)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_compose_arginfo, 2)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, classes)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_flags_arginfo, 2)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, function)
	ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_set_return_arginfo, 2)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, function)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_unset_return_arginfo, 2)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, function)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_get_return_arginfo, 2)
	ZEND_ARG_INFO(0, class)
	ZEND_ARG_INFO(0, function)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_set_mock_arginfo, 2)
	ZEND_ARG_INFO(0, clazz)
	ZEND_ARG_INFO(0, mock)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_unset_mock_arginfo, 2)
	ZEND_ARG_INFO(0, clazz)
	ZEND_ARG_INFO(0, mock)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_get_mock_arginfo, 2)
	ZEND_ARG_INFO(0, clazz)
	ZEND_ARG_INFO(0, mock)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_set_static_arginfo, 3)
	ZEND_ARG_INFO(0, clazz)
	ZEND_ARG_INFO(0, function)
	ZEND_ARG_INFO(0, statics)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_get_static_arginfo, 2)
	ZEND_ARG_INFO(0, clazz)
	ZEND_ARG_INFO(0, function)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_set_hook_arginfo, 3)
	ZEND_ARG_INFO(0, clazz)
	ZEND_ARG_INFO(0, function)
	ZEND_ARG_INFO(0, statics)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_unset_hook_arginfo, 2)
	ZEND_ARG_INFO(0, clazz)
	ZEND_ARG_INFO(0, function)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO(uopz_get_hook_arginfo, 2)
	ZEND_ARG_INFO(0, clazz)
	ZEND_ARG_INFO(0, function)
ZEND_END_ARG_INFO()
/* }}} */

/* {{{ uopz_functions[]
 */
static const zend_function_entry uopz_functions[] = {
	PHP_FE(uopz_copy, uopz_copy__arginfo)
	PHP_FE(uopz_delete, uopz_delete_arginfo)
	PHP_FE(uopz_redefine, uopz_redefine_arginfo)
	PHP_FE(uopz_undefine, uopz_undefine_arginfo)
	PHP_FE(uopz_function, uopz_function_arginfo)
	PHP_FE(uopz_backup, uopz_backup_arginfo)
	PHP_FE(uopz_flags, uopz_flags_arginfo)
	PHP_FE(uopz_implement, uopz_implement_arginfo)
	PHP_FE(uopz_extend, uopz_extend_arginfo)
	PHP_FE(uopz_compose, uopz_compose_arginfo)
	PHP_FE(uopz_restore, uopz_restore_arginfo)
	PHP_FE(uopz_set_return, uopz_set_return_arginfo)
	PHP_FE(uopz_unset_return, uopz_unset_return_arginfo)
	PHP_FE(uopz_get_return, uopz_get_return_arginfo)
	PHP_FE(uopz_set_mock, uopz_set_mock_arginfo)
	PHP_FE(uopz_unset_mock, uopz_unset_mock_arginfo)
	PHP_FE(uopz_get_mock, uopz_get_mock_arginfo)
	PHP_FE(uopz_set_static, uopz_set_static_arginfo)
	PHP_FE(uopz_get_static, uopz_get_static_arginfo)
	PHP_FE(uopz_set_hook, uopz_set_hook_arginfo)
	PHP_FE(uopz_unset_hook, uopz_unset_hook_arginfo)
	PHP_FE(uopz_get_hook, uopz_get_hook_arginfo)
	{NULL, NULL, NULL}
};
/* }}} */

/* {{{ uopz_module_entry
 */
zend_module_entry uopz_module_entry = {
	STANDARD_MODULE_HEADER,
	PHP_UOPZ_EXTNAME,
	uopz_functions,
	PHP_MINIT(uopz),
	PHP_MSHUTDOWN(uopz),
	PHP_RINIT(uopz),
	PHP_RSHUTDOWN(uopz),
	PHP_MINFO(uopz),
	PHP_UOPZ_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_UOPZ
ZEND_GET_MODULE(uopz)
#ifdef ZTS
	ZEND_TSRMLS_CACHE_DEFINE();
#endif
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
