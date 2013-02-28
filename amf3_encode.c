/*
** Copyright (C) 2010, 2013 Arseny Vakhrushev <arseny.vakhrushev at gmail dot com>
** Please read the LICENSE file for license details
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_amf3.h"
#include "ext/standard/php_smart_str.h"
#include "amf3.h"


static void encodeValue(smart_str *ss, zval *val, HashTable *sht, HashTable *oht, HashTable *tht TSRMLS_DC);

static void encodeU29(smart_str *ss, int val) {
	char buf[4];
	int size;
	val &= 0x1fffffff;
	if (val <= 0x7f) {
		buf[0] = val;
		size = 1;
	} else if (val <= 0x3fff) {
		buf[1] = val & 0x7f;
		val >>= 7;
		buf[0] = val | 0x80;
		size = 2;
	} else if (val <= 0x1fffff) {
		buf[2] = val & 0x7f;
		val >>= 7;
		buf[1] = val | 0x80;
		val >>= 7;
		buf[0] = val | 0x80;
		size = 3;
	} else {
		buf[3] = val;
		val >>= 8;
		buf[2] = val | 0x80;
		val >>= 7;
		buf[1] = val | 0x80;
		val >>= 7;
		buf[0] = val | 0x80;
		size = 4;
	}
	smart_str_appendl(ss, buf, size);
}

static void encodeDouble(smart_str *ss, double val) {
	union { int i; char c; } t;
	union { double d; char c[8]; } u;
	char buf[8];
	t.i = 1;
	u.d = val;
	if (!t.c) memcpy(buf, u.c, 8);
	else { /* little-endian machine */
		int i;
		for (i = 0; i < 8; ++i) buf[7 - i] = u.c[i];
	}
	smart_str_appendl(ss, buf, 8);
}

static void encodeStr(smart_str *ss, const char *str, int len, HashTable *ht TSRMLS_DC) {
	if (len > AMF3_MAX_INT) len = AMF3_MAX_INT;
	if (len) { /* empty string is never sent by reference */
		int *oidx, nidx;
		if (zend_hash_find(ht, str, len, (void **)&oidx) == SUCCESS) {
			encodeU29(ss, *oidx << 1);
			return;
		}
		nidx = zend_hash_num_elements(ht);
		if (nidx <= AMF3_MAX_INT) zend_hash_add(ht, str, len, &nidx, sizeof(nidx), NULL);
	}
	encodeU29(ss, (len << 1) | 1);
	smart_str_appendl(ss, str, len);
}

static int encodeRef(smart_str *ss, zval *val, HashTable *ht TSRMLS_DC) {
	int *oidx, nidx;
	if (zend_hash_find(ht, (char *)&val, sizeof(val), (void **)&oidx) == SUCCESS) {
		encodeU29(ss, *oidx << 1);
		return 1;
	}
	nidx = zend_hash_num_elements(ht);
	if (nidx <= AMF3_MAX_INT) zend_hash_add(ht, (char *)&val, sizeof(val), &nidx, sizeof(nidx), NULL);
	return 0;
}

static void encodeArray(smart_str *ss, zval *val, HashTable *sht, HashTable *oht, HashTable *tht TSRMLS_DC) {
	HashTable *ht = Z_ARRVAL_P(val);
	HashPosition hp;
	zval **hv;
	char *key, keyBuf[22];
	uint keyLen;
	ulong idx;
	int len = 0;
	if (encodeRef(ss, val, oht TSRMLS_CC)) return;
	for (zend_hash_internal_pointer_reset_ex(ht, &hp) ;; zend_hash_move_forward_ex(ht, &hp)) {
		if (zend_hash_get_current_key_ex(ht, &key, &keyLen, &idx, 0, &hp) != HASH_KEY_IS_LONG) break;
		if (idx != len) break;
		++len;
	}
	if (len == zend_hash_num_elements(ht)) { /* dense array */
		if (len > AMF3_MAX_INT) len = AMF3_MAX_INT;
		encodeU29(ss, (len << 1) | 1);
		smart_str_appendc(ss, 0x01);
		for (zend_hash_internal_pointer_reset_ex(ht, &hp) ;; zend_hash_move_forward_ex(ht, &hp)) {
			if (zend_hash_get_current_data_ex(ht, (void **)&hv, &hp) != SUCCESS) break;
			if (!len--) break;
			encodeValue(ss, *hv, sht, oht, tht TSRMLS_CC);
		}
	} else { /* associative array */
		smart_str_appendc(ss, 0x01);
		for (zend_hash_internal_pointer_reset_ex(ht, &hp) ;; zend_hash_move_forward_ex(ht, &hp)) {
			if (zend_hash_get_current_data_ex(ht, (void **)&hv, &hp) != SUCCESS) break;
			switch (zend_hash_get_current_key_ex(ht, &key, &keyLen, &idx, 0, &hp)) {
				case HASH_KEY_IS_LONG:
					encodeStr(ss, keyBuf, sprintf(keyBuf, "%ld", idx), sht TSRMLS_CC);
					break;
				case HASH_KEY_IS_STRING:
					if (keyLen <= 1) continue; /* empty key can't be represented in AMF3 */
					encodeStr(ss, key, keyLen - 1, sht TSRMLS_CC);
					break;
				default:
					continue;
			}
			encodeValue(ss, *hv, sht, oht, tht TSRMLS_CC);
		}
		smart_str_appendc(ss, 0x01);
	}
}

static void encodeObject(smart_str *ss, zval *val, HashTable *sht, HashTable *oht, HashTable *tht TSRMLS_DC) {
	zend_class_entry *ce = Z_OBJCE_P(val);
	int *oidx, nidx;
	HashTable *ht = Z_OBJPROP_P(val);
	HashPosition hp;
	zval **hv;
	char *key;
	uint keyLen;
	ulong idx;
	if (encodeRef(ss, val, oht TSRMLS_CC)) return;
	if (zend_hash_find(tht, (char *)&ce, sizeof(ce), (void **)&oidx) == SUCCESS) encodeU29(ss, (*oidx << 2) | 1);
	else {
		nidx = zend_hash_num_elements(tht);
		if (nidx <= AMF3_MAX_INT) zend_hash_add(tht, (char *)&ce, sizeof(ce), &nidx, sizeof(nidx), NULL);
		smart_str_appendc(ss, 0x0b);
		if (ce == zend_standard_class_def) smart_str_appendc(ss, 0x01); /* anonymous object */
		else encodeStr(ss, ce->name, ce->name_length, sht TSRMLS_CC); /* typed object */
	}
	for (zend_hash_internal_pointer_reset_ex(ht, &hp) ;; zend_hash_move_forward_ex(ht, &hp)) {
		if (zend_hash_get_current_data_ex(ht, (void **)&hv, &hp) != SUCCESS) break;
		if (zend_hash_get_current_key_ex(ht, &key, &keyLen, &idx, 0, &hp) != HASH_KEY_IS_STRING) continue;
		if (keyLen <= 1) continue; /* empty key can't be represented in AMF3 */
		if (key[0] == 0) continue; /* skip private/protected property */
		encodeStr(ss, key, keyLen - 1, sht TSRMLS_CC);
		encodeValue(ss, *hv, sht, oht, tht TSRMLS_CC);
	}
	smart_str_appendc(ss, 0x01);
}

static void encodeValue(smart_str *ss, zval *val, HashTable *sht, HashTable *oht, HashTable *tht TSRMLS_DC) {
	switch (Z_TYPE_P(val)) {
		default:
			smart_str_appendc(ss, AMF3_UNDEFINED);
			break;
		case IS_NULL:
			smart_str_appendc(ss, AMF3_NULL);
			break;
		case IS_BOOL:
			smart_str_appendc(ss, Z_LVAL_P(val) ? AMF3_TRUE : AMF3_FALSE);
			break;
		case IS_LONG: {
			int i = Z_LVAL_P(val);
			if ((i >= AMF3_MIN_INT) && (i <= AMF3_MAX_INT)) {
				smart_str_appendc(ss, AMF3_INTEGER);
				encodeU29(ss, i);
			} else {
				smart_str_appendc(ss, AMF3_DOUBLE);
				encodeDouble(ss, i);
			}
			break;
		}
		case IS_DOUBLE:
			smart_str_appendc(ss, AMF3_DOUBLE);
			encodeDouble(ss, Z_DVAL_P(val));
			break;
		case IS_STRING:
			smart_str_appendc(ss, AMF3_STRING);
			encodeStr(ss, Z_STRVAL_P(val), Z_STRLEN_P(val), sht TSRMLS_CC);
			break;
		case IS_ARRAY: {
			smart_str_appendc(ss, AMF3_ARRAY);
			encodeArray(ss, val, sht, oht, tht TSRMLS_CC);
			break;
		}
		case IS_OBJECT: {
			smart_str_appendc(ss, AMF3_OBJECT);
			encodeObject(ss, val, sht, oht, tht TSRMLS_CC);
			break;
		}
	}
}

PHP_FUNCTION(amf3_encode) {
	smart_str ss = { 0 };
	zval *val;
	HashTable sht, oht, tht;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &val) == FAILURE) return;
	zend_hash_init(&sht, 0, NULL, NULL, 0);
	zend_hash_init(&oht, 0, NULL, NULL, 0);
	zend_hash_init(&tht, 0, NULL, NULL, 0);
	encodeValue(&ss, val, &sht, &oht, &tht TSRMLS_CC);
	zend_hash_destroy(&sht);
	zend_hash_destroy(&oht);
	zend_hash_destroy(&tht);
	RETVAL_STRINGL(ss.c, ss.len, 1);
	smart_str_free(&ss);
}
