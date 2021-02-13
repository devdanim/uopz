/*
  +----------------------------------------------------------------------+
  | uopz                                                                 |
  +----------------------------------------------------------------------+
  | Copyright (c) Joe Watkins 2016-2020                                  |
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

#ifndef UOPZ_CLASS
#define UOPZ_CLASS

#include "php.h"
#include "uopz.h"

#include "util.h"
#include "class.h"

ZEND_EXTERN_MODULE_GLOBALS(uopz);

#if PHP_VERSION_ID >= 70100
#	define uopz_get_scope(e) ((e) ? zend_get_executed_scope() : EG(fake_scope))
#	define uopz_set_scope(s) EG(fake_scope) = (s)
#else
#	define uopz_get_scope(e) EG(scope)
#	define uopz_set_scope(s) EG(scope) = (s)
#endif

void uopz_set_mock(zend_string *clazz, zval *mock) { /* {{{ */
	zend_string *key = zend_string_tolower(clazz);

	if (zend_hash_update(&UOPZ(mocks), key, mock)) {
		zval_copy_ctor(mock);
	}

	/*zend_string_release(key);*/
} /* }}} */

void uopz_unset_mock(zend_string *clazz) { /* {{{ */
	zend_string *key = zend_string_tolower(clazz);
	
	if (!zend_hash_exists(&UOPZ(mocks), key)) {
		uopz_exception(
			"the class provided (%s) has no mock set",
			ZSTR_VAL(clazz));
		/*zend_string_release(key);*/
		return;
	}

	zend_hash_del(&UOPZ(mocks), key);
	/*zend_string_release(key);*/
} /* }}} */

int uopz_get_mock(zend_string *clazz, zval *return_value) { /* {{{ */
	zval *mock = NULL;
	zend_string *key = zend_string_tolower(clazz);
	
	if (!(mock = zend_hash_find(&UOPZ(mocks), key))) {
		/*zend_string_release(key);*/
		return FAILURE;
	}

	ZVAL_COPY(return_value, mock);
	/*zend_string_release(key);*/

	return SUCCESS;
} /* }}} */

int uopz_find_mock(zend_string *clazz, zend_object **object, zend_class_entry **mock) { /* {{{ */
	zend_string *key = zend_string_tolower(clazz);
	zval *found = zend_hash_find(&UOPZ(mocks), key);

	/*zend_string_release(key);*/

	if (!found) {
		return FAILURE;
	}

	if (Z_TYPE_P(found) == IS_STRING) {
		*mock = zend_lookup_class(Z_STR_P(found));
	} else {
		*mock = Z_OBJCE_P(found);
		if (object) {
			*object = Z_OBJ_P(found);
		}
	}

	return SUCCESS;
} /* }}} */

/* {{{ */
zend_bool uopz_extend(zend_class_entry *clazz, zend_class_entry *parent) {
	zend_bool is_final, is_trait;

	if (instanceof_function(clazz, parent)) {
		uopz_exception(
			"the class provided (%s) already extends %s",
			ZSTR_VAL(clazz->name), ZSTR_VAL(parent->name));
		return 0;
	}

	if ((clazz->ce_flags & ZEND_ACC_TRAIT) &&
		!(parent->ce_flags & ZEND_ACC_TRAIT)) {
		uopz_exception(
		    "the trait provided (%s) cannot extend %s, because %s is not a trait",
		     ZSTR_VAL(clazz->name), ZSTR_VAL(parent->name), ZSTR_VAL(parent->name));
		return 0;
	}

	if ((clazz->ce_flags & ZEND_ACC_INTERFACE) &&
		!(parent->ce_flags & ZEND_ACC_INTERFACE)) {
		uopz_exception(
		    "the interface provided (%s) cannot extend %s, because %s is not an interface",
		     ZSTR_VAL(clazz->name), ZSTR_VAL(parent->name), ZSTR_VAL(parent->name));
		return 0;
	}

#if PHP_VERSION_ID >= 70400
	if ((clazz->ce_flags & ZEND_ACC_IMMUTABLE)) {
		uopz_exception(
		    "cannot change the class provided (%s), because it is immutable",
		     ZSTR_VAL(clazz->name));
		return 0;
	}
#endif

	is_final = clazz->ce_flags & ZEND_ACC_FINAL;
	is_trait = (clazz->ce_flags & ZEND_ACC_TRAIT) == ZEND_ACC_TRAIT;

#if PHP_VERSION_ID >= 70400
    if (is_trait && (parent->ce_flags & ZEND_ACC_IMMUTABLE)) {
		uopz_exception(
		    "the parent trait provided (%s) cannot be extended by %s, because it is immutable",
		     ZSTR_VAL(parent->name), ZSTR_VAL(clazz->name));
		return 0;
    }
#endif

	clazz->ce_flags &= ~ZEND_ACC_FINAL;

	if (clazz->parent) {
		zend_function *method;
		zend_string   *name;
		dtor_func_t    dtor = clazz->function_table.pDestructor;

		clazz->function_table.pDestructor = NULL;

		ZEND_HASH_FOREACH_STR_KEY_PTR(&clazz->function_table, name, method) {
			if (method->common.fn_flags & ZEND_ACC_ABSTRACT) {
				continue;
			}

			if (zend_hash_exists(&parent->function_table, name)) {
				zend_hash_del(&clazz->function_table, name);
			}
		} ZEND_HASH_FOREACH_END();

		clazz->function_table.pDestructor = dtor;
        clazz->parent = NULL;
	}

#if PHP_VERSION_ID >= 70400
    if (is_trait) {
        clazz->ce_flags &= ~ZEND_ACC_TRAIT;
        parent->ce_flags &= ~ZEND_ACC_TRAIT;
    }
    zend_do_inheritance(clazz, parent);
    if (is_trait) {
        clazz->ce_flags |= ZEND_ACC_TRAIT;
        parent->ce_flags |= ZEND_ACC_TRAIT;
    }
#else
	if ((parent->ce_flags & ZEND_ACC_TRAIT) == ZEND_ACC_TRAIT) {
		zend_do_implement_trait(clazz, parent);
		zend_do_bind_traits(clazz);
	} else zend_do_inheritance(clazz, parent);
#endif

	if (is_final)
		clazz->ce_flags |= ZEND_ACC_FINAL;

	return is_trait ? 1 : instanceof_function(clazz, parent);
} /* }}} */

/* {{{ */
zend_bool uopz_implement(zend_class_entry *clazz, zend_class_entry *interface) {
	if (!(interface->ce_flags & ZEND_ACC_INTERFACE)) {
		uopz_exception(
			"the class provided (%s) is not an interface", 
			ZSTR_VAL(interface->name));
		return 0;
	}

	if (instanceof_function(clazz, interface)) {
		uopz_exception(
			"the class provided (%s) already has the interface %s",
			ZSTR_VAL(clazz->name),
			ZSTR_VAL(interface->name));
		return 0;
	}

#if PHP_VERSION_ID >= 70400
    if (clazz->ce_flags & ZEND_ACC_IMMUTABLE) {
		uopz_exception(
			"the class provided (%s) cannot implement %s, it is immutable",
			ZSTR_VAL(clazz->name),
			ZSTR_VAL(interface->name));
		return 0; 
    }
#endif

	zend_do_implement_interface(clazz, interface);

#if PHP_VERSION_ID >= 80000
	clazz->ce_flags |= ZEND_ACC_RESOLVED_INTERFACES;
#endif

	return instanceof_function(clazz, interface);
} /* }}} */

void uopz_set_property(zval *object, zval *member, zval *value) { /* {{{ */
	zend_class_entry *scope = uopz_get_scope(0);
	zend_class_entry *ce = Z_OBJCE_P(object);
	zend_property_info *info;

	do {
		uopz_set_scope(ce);

		info = zend_get_property_info(ce, Z_STR_P(member), 1);
	
		if (info && info != ZEND_WRONG_PROPERTY_INFO) {
			break;
		}

		ce = ce->parent;
	} while (ce);

	if (info && info != ZEND_WRONG_PROPERTY_INFO) {
		uopz_set_scope(info->ce);
	} else {
		uopz_set_scope(Z_OBJCE_P(object));
	}

#if PHP_VERSION_ID >= 80000
	Z_OBJ_HT_P(object)
		->write_property(Z_OBJ_P(object), Z_STR_P(member), value, NULL);
#else
	Z_OBJ_HT_P(object)
		->write_property(object, member, value, NULL);
#endif

	uopz_set_scope(scope);
} /* }}} */

void uopz_get_property(zval *object, zval *member, zval *value) { /* {{{ */
	zend_class_entry *scope = uopz_get_scope(0);
	zend_class_entry *ce = Z_OBJCE_P(object);
	zend_property_info *info;
	zval *prop, rv;

	do {
		uopz_set_scope(ce);

		info = zend_get_property_info(ce, Z_STR_P(member), 1);
	
		if (info && info != ZEND_WRONG_PROPERTY_INFO) {
			break;
		}

		ce = ce->parent;
	} while (ce);

	if (info && info != ZEND_WRONG_PROPERTY_INFO) {
		uopz_set_scope(info->ce);
	} else {
		uopz_set_scope(Z_OBJCE_P(object));
	}

#if PHP_VERSION_ID >= 80000
	prop = Z_OBJ_HT_P(object)->read_property(
		Z_OBJ_P(object), Z_STR_P(member), BP_VAR_R, NULL, &rv);
#else
	prop = Z_OBJ_HT_P(object)->read_property(
		object, member, BP_VAR_R, NULL, &rv);
#endif

	uopz_set_scope(scope);

	ZVAL_COPY(value, prop);
} /* }}} */

void uopz_set_static_property(zend_class_entry *ce, zend_string *property, zval *value) { /* {{{ */
	zend_class_entry *scope = uopz_get_scope(0);
	zend_class_entry *seek = ce;
	zend_property_info *info;
	zval *prop;

	do {
		uopz_set_scope(seek);

		info = zend_get_property_info(seek, property, 1);
	
		if (info && info != ZEND_WRONG_PROPERTY_INFO) {
			break;
		}

		seek = seek->parent;
	} while (seek);

	if (info && info != ZEND_WRONG_PROPERTY_INFO) {
		uopz_set_scope(info->ce);
	} else {
		uopz_set_scope(ce);
	}

	prop = zend_std_get_static_property(uopz_get_scope(0), property, 1);
	
	uopz_set_scope(scope);

	if (!prop) {
		uopz_exception(
			"cannot set non-existent static property %s::%s", 
			ZSTR_VAL(ce->name),
			ZSTR_VAL(property));
		return;
	}

	zval_ptr_dtor(prop);
	ZVAL_COPY(prop, value);
} /* }}} */

void uopz_get_static_property(zend_class_entry *ce, zend_string *property, zval *value) { /* {{{ */
	zend_class_entry *scope = uopz_get_scope(0);
	zend_class_entry *seek = ce;
	zend_property_info *info;
	zval *prop;

	do {
		uopz_set_scope(seek);

		info = zend_get_property_info(seek, property, 1);
	
		if (info && info != ZEND_WRONG_PROPERTY_INFO) {
			break;
		}

		seek = seek->parent;
	} while (seek);

	if (info && info != ZEND_WRONG_PROPERTY_INFO) {
		uopz_set_scope(info->ce);
	} else {
		uopz_set_scope(ce);
	}

	prop = zend_std_get_static_property(uopz_get_scope(0), property, 1);
	
	uopz_set_scope(scope);

	if (!prop) {
		return;
	}
	
	ZVAL_COPY(value, prop);
} /* }}} */

#endif	/* UOPZ_CLASS */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
