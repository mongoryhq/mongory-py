/**
 * @file mongory_ext.c
 * @brief CPython C-API binding for mongory-core (C99).
 *
 * Bridges Python objects (dict, list, tuple, primitives, re.Pattern, custom
 * converters, and custom operators) to mongory-core's arena memory pools,
 * matcher AST, and structured explain_tree / trace_tree APIs.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "mongory-core.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct py_mongory_matcher_t py_mongory_matcher_t;
typedef struct py_mongory_memory_pool_t py_mongory_memory_pool_t;

struct py_mongory_memory_pool_t {
  mongory_memory_pool base;
  py_mongory_matcher_t *owner;
  py_mongory_memory_pool_t *next_scratch;
  PyObject *scratch_refs; /* PyList owning temporary PyObjects during a match/trace call */
};

struct py_mongory_matcher_t {
  PyObject_HEAD
  mongory_matcher *matcher;
  mongory_value *condition;
  py_mongory_memory_pool_t *pool;
  py_mongory_memory_pool_t *scratch_free_list;
  mongory_table *key_cache;
  PyObject *keep_alive_list; /* PyList owning matcher-lifetime PyObjects */
  PyObject *raw_condition;
  PyObject *context;
};

typedef struct py_mongory_table_t {
  mongory_table base;
  PyObject *py_dict;
  py_mongory_matcher_t *owner;
} py_mongory_table_t;

typedef struct py_mongory_array_t {
  mongory_array base;
  PyObject *py_seq;
  bool is_tuple;
  py_mongory_matcher_t *owner;
} py_mongory_array_t;

static PyObject *py_mongory_error = NULL;
static PyObject *py_invalid_condition_error = NULL;
static PyObject *py_re_module = NULL;
static PyObject *py_re_pattern_type = NULL;

/* Active compile context used during synchronous mongory_matcher_new / lazy array build */
static PyObject *current_compile_context = NULL;

static mongory_value *py_to_mongory_value_deep(mongory_memory_pool *pool, PyObject *obj);
static mongory_value *py_to_mongory_value_shallow(mongory_memory_pool *pool, PyObject *obj);
static mongory_value *py_mongory_table_wrap(mongory_memory_pool *pool, PyObject *dict_obj);
static mongory_value *py_mongory_array_wrap(mongory_memory_pool *pool, PyObject *seq_obj, bool is_tuple);

static py_mongory_memory_pool_t *py_mongory_memory_pool_new(py_mongory_matcher_t *owner) {
  py_mongory_memory_pool_t *pool = (py_mongory_memory_pool_t *)malloc(sizeof(py_mongory_memory_pool_t));
  if (!pool) {
    return NULL;
  }
  mongory_memory_pool *base = mongory_memory_pool_new();
  if (!base) {
    free(pool);
    return NULL;
  }
  PyObject *refs = PyList_New(0);
  if (!refs) {
    base->free(base);
    free(base);
    return NULL;
  }
  memcpy(&pool->base, base, sizeof(mongory_memory_pool));
  free(base);
  pool->owner = owner;
  pool->next_scratch = NULL;
  pool->scratch_refs = refs;
  return pool;
}

static void py_mongory_memory_pool_reset(py_mongory_memory_pool_t *pool) {
  if (!pool) return;
  if (pool->scratch_refs && PyList_GET_SIZE(pool->scratch_refs) > 0) {
    PyList_SetSlice(pool->scratch_refs, 0, PyList_GET_SIZE(pool->scratch_refs), NULL);
  }
  pool->base.error = NULL;
  pool->base.reset(&pool->base);
}

static void py_mongory_memory_pool_destroy(py_mongory_memory_pool_t *pool) {
  if (!pool) return;
  Py_CLEAR(pool->scratch_refs);
  pool->base.free(&pool->base);
}

/* Tracks a new reference `obj` (steals ownership) in `list`. */
static inline void py_track_owned_ref(PyObject *list, PyObject *obj) {
  if (!obj) return;
  if (list) {
    PyList_Append(list, obj);
  }
  Py_DECREF(obj);
}

/* Tracks a borrowed reference `obj` (increments refcount) in `list`. */
static inline void py_track_borrowed_ref(PyObject *list, PyObject *obj) {
  if (!obj || !list) return;
  PyList_Append(list, obj);
}

static char *py_mongory_value_to_cstr(mongory_value *value, mongory_memory_pool *pool) {
  if (!value || !value->origin) return "null";
  py_mongory_memory_pool_t *py_pool = (py_mongory_memory_pool_t *)pool;
  PyObject *repr_obj = PyObject_Repr((PyObject *)value->origin);
  if (!repr_obj) {
    PyErr_Clear();
    return "<object>";
  }
  const char *cstr = PyUnicode_AsUTF8(repr_obj);
  py_track_owned_ref(py_pool ? py_pool->scratch_refs : NULL, repr_obj);
  return (char *)(cstr ? cstr : "<object>");
}

static PyObject *py_mongory_cache_fetch_key(py_mongory_matcher_t *owner, const char *key) {
  if (!owner || !owner->key_cache) {
    return PyUnicode_FromString(key);
  }
  mongory_value *v = owner->key_cache->get(owner->key_cache, (char *)key);
  if (v && v->origin) {
    PyObject *cached = (PyObject *)v->origin;
    Py_INCREF(cached);
    return cached;
  }
  PyObject *py_str = PyUnicode_FromString(key);
  if (!py_str) {
    return NULL;
  }
  mongory_value *store = mongory_value_wrap_u(&owner->pool->base, NULL);
  if (store) {
    store->origin = (void *)py_str;
    owner->key_cache->set(owner->key_cache, (char *)key, store);
    py_track_borrowed_ref(owner->keep_alive_list, py_str);
  }
  return py_str;
}

static mongory_value *py_to_mongory_value_primitive(mongory_memory_pool *pool, PyObject *obj) {
  if (!obj || obj == Py_None) {
    return mongory_value_wrap_n(pool, NULL);
  }
  /* IMPORTANT: PyBool_Check MUST precede PyLong_Check because bool subclasses int in Python */
  if (PyBool_Check(obj)) {
    return mongory_value_wrap_b(pool, obj == Py_True);
  }
  if (PyLong_Check(obj)) {
    int overflow = 0;
    long long val = PyLong_AsLongLongAndOverflow(obj, &overflow);
    if (!PyErr_Occurred() && overflow == 0) {
      return mongory_value_wrap_i(pool, (int64_t)val);
    }
    PyErr_Clear();
    double d = PyLong_AsDouble(obj);
    if (!PyErr_Occurred()) {
      return mongory_value_wrap_d(pool, d);
    }
    PyErr_Clear();
    return mongory_value_wrap_u(pool, (void *)obj);
  }
  if (PyFloat_Check(obj)) {
    return mongory_value_wrap_d(pool, PyFloat_AsDouble(obj));
  }
  if (PyUnicode_Check(obj)) {
    const char *cstr = PyUnicode_AsUTF8(obj);
    if (!cstr) {
      PyErr_Clear();
      return mongory_value_wrap_u(pool, (void *)obj);
    }
    return mongory_value_wrap_s(pool, (char *)cstr);
  }
  if (py_re_pattern_type && PyObject_IsInstance(obj, py_re_pattern_type) == 1) {
    return mongory_value_wrap_regex(pool, (void *)obj);
  }
  return NULL;
}

static mongory_value *py_to_mongory_value_shallow_rec(mongory_memory_pool *pool, PyObject *obj, bool converted) {
  mongory_value *mg_val = py_to_mongory_value_primitive(pool, obj);
  if (mg_val) {
    mg_val->origin = (void *)obj;
    return mg_val;
  }

  if (PyList_Check(obj)) {
    mg_val = py_mongory_array_wrap(pool, obj, false);
    mg_val->origin = (void *)obj;
    return mg_val;
  }
  if (PyTuple_Check(obj)) {
    mg_val = py_mongory_array_wrap(pool, obj, true);
    mg_val->origin = (void *)obj;
    return mg_val;
  }
  if (PyDict_Check(obj)) {
    mg_val = py_mongory_table_wrap(pool, obj);
    mg_val->origin = (void *)obj;
    return mg_val;
  }

  py_mongory_memory_pool_t *py_pool = (py_mongory_memory_pool_t *)pool;
  py_mongory_matcher_t *owner = py_pool ? py_pool->owner : NULL;
  if (!converted && owner && owner->context && owner->context != Py_None) {
    PyObject *conv = PyObject_CallMethod(owner->context, "convert_data", "O", obj);
    if (conv) {
      if (conv != obj) {
        py_track_owned_ref(py_pool->scratch_refs, conv);
        mongory_value *res = py_to_mongory_value_shallow_rec(pool, conv, true);
        if (res) res->origin = (void *)obj;
        return res;
      }
      Py_DECREF(conv);
    } else {
      PyErr_Clear();
    }
  }

  mg_val = mongory_value_wrap_u(pool, (void *)obj);
  if (mg_val) {
    mg_val->origin = (void *)obj;
    mg_val->to_str = py_mongory_value_to_cstr;
  }
  return mg_val;
}

static mongory_value *py_to_mongory_value_shallow(mongory_memory_pool *pool, PyObject *obj) {
  return py_to_mongory_value_shallow_rec(pool, obj, false);
}

static mongory_value *py_to_mongory_value_deep_rec(mongory_memory_pool *pool, PyObject *obj, bool converted) {
  py_mongory_memory_pool_t *py_pool = (py_mongory_memory_pool_t *)pool;
  py_mongory_matcher_t *owner = py_pool ? py_pool->owner : NULL;
  PyObject *owner_refs = owner ? owner->keep_alive_list : (py_pool ? py_pool->scratch_refs : NULL);

  mongory_value *mg_val = py_to_mongory_value_primitive(pool, obj);
  if (mg_val) {
    mg_val->origin = (void *)obj;
    py_track_borrowed_ref(owner_refs, obj);
    return mg_val;
  }

  if (PyList_Check(obj) || PyTuple_Check(obj)) {
    mongory_array *arr = mongory_array_new(pool);
    Py_ssize_t len = PyList_Check(obj) ? PyList_GET_SIZE(obj) : PyTuple_GET_SIZE(obj);
    for (Py_ssize_t i = 0; i < len; i++) {
      PyObject *item = PyList_Check(obj) ? PyList_GET_ITEM(obj, i) : PyTuple_GET_ITEM(obj, i);
      arr->push(arr, py_to_mongory_value_deep(pool, item));
    }
    mg_val = mongory_value_wrap_a(pool, arr);
    if (mg_val) mg_val->origin = (void *)obj;
    py_track_borrowed_ref(owner_refs, obj);
    return mg_val;
  }

  if (PyDict_Check(obj)) {
    mongory_table *table = mongory_table_new(pool);
    PyObject *key = NULL;
    PyObject *val = NULL;
    Py_ssize_t pos = 0;
    while (PyDict_Next(obj, &pos, &key, &val)) {
      PyObject *key_str_obj = key;
      if (!PyUnicode_Check(key)) {
        key_str_obj = PyObject_Str(key);
        if (!key_str_obj) {
          PyErr_Clear();
          continue;
        }
        py_track_owned_ref(owner_refs, key_str_obj);
      } else {
        py_track_borrowed_ref(owner_refs, key_str_obj);
      }
      const char *key_cstr = PyUnicode_AsUTF8(key_str_obj);
      if (!key_cstr) {
        PyErr_Clear();
        continue;
      }
      if (owner && owner->key_cache) {
        mongory_value *store = mongory_value_wrap_u(pool, NULL);
        if (store) {
          store->origin = (void *)key_str_obj;
          owner->key_cache->set(owner->key_cache, (char *)key_cstr, store);
        }
      }
      mongory_value *cval = py_to_mongory_value_deep(pool, val);
      table->set(table, (char *)key_cstr, cval);
    }
    mg_val = mongory_value_wrap_t(pool, table);
    if (mg_val) mg_val->origin = (void *)obj;
    py_track_borrowed_ref(owner_refs, obj);
    return mg_val;
  }

  if (!converted && owner && owner->context && owner->context != Py_None) {
    PyObject *conv = PyObject_CallMethod(owner->context, "convert_data", "O", obj);
    if (conv) {
      if (conv != obj) {
        py_track_owned_ref(owner_refs, conv);
        mongory_value *res = py_to_mongory_value_deep_rec(pool, conv, true);
        if (res) res->origin = (void *)obj;
        py_track_borrowed_ref(owner_refs, obj);
        return res;
      }
      Py_DECREF(conv);
    } else {
      PyErr_Clear();
    }
  }

  mg_val = mongory_value_wrap_u(pool, (void *)obj);
  if (mg_val) {
    mg_val->origin = (void *)obj;
    mg_val->to_str = py_mongory_value_to_cstr;
  }
  py_track_borrowed_ref(owner_refs, obj);
  return mg_val;
}

static mongory_value *py_to_mongory_value_deep(mongory_memory_pool *pool, PyObject *obj) {
  return py_to_mongory_value_deep_rec(pool, obj, false);
}

static mongory_value *py_mongory_table_get(mongory_table *self, char *key) {
  py_mongory_table_t *table = (py_mongory_table_t *)self;
  PyObject *py_key = py_mongory_cache_fetch_key(table->owner, key);
  if (!py_key) {
    PyErr_Clear();
    return NULL;
  }
  PyObject *val = PyDict_GetItemWithError(table->py_dict, py_key);
  Py_DECREF(py_key);
  if (!val) {
    if (PyErr_Occurred()) PyErr_Clear();
    return NULL;
  }
  return py_to_mongory_value_shallow(table->base.pool, val);
}

static mongory_value *py_mongory_table_wrap(mongory_memory_pool *pool, PyObject *dict_obj) {
  py_mongory_table_t *table = MG_ALLOC_PTR(pool, py_mongory_table_t);
  py_mongory_memory_pool_t *py_pool = (py_mongory_memory_pool_t *)pool;
  table->base.pool = pool;
  table->base.get = py_mongory_table_get;
  table->base.set = NULL;
  table->base.del = NULL;
  table->base.each = NULL;
  table->py_dict = dict_obj;
  table->base.count = (size_t)PyDict_Size(dict_obj);
  table->owner = py_pool ? py_pool->owner : NULL;
  mongory_value *mg_val = mongory_value_wrap_t(pool, &table->base);
  if (mg_val) {
    mg_val->origin = (void *)dict_obj;
    mg_val->to_str = py_mongory_value_to_cstr;
  }
  return mg_val;
}

static mongory_value *py_mongory_array_get(mongory_array *self, size_t index) {
  py_mongory_array_t *arr = (py_mongory_array_t *)self;
  Py_ssize_t len = arr->is_tuple ? PyTuple_GET_SIZE(arr->py_seq) : PyList_GET_SIZE(arr->py_seq);
  if (index >= (size_t)len) {
    return NULL;
  }
  PyObject *item = arr->is_tuple ? PyTuple_GET_ITEM(arr->py_seq, (Py_ssize_t)index)
                                 : PyList_GET_ITEM(arr->py_seq, (Py_ssize_t)index);
  return py_to_mongory_value_shallow(self->pool, item);
}

static mongory_value *py_mongory_array_wrap(mongory_memory_pool *pool, PyObject *seq_obj, bool is_tuple) {
  py_mongory_array_t *arr = MG_ALLOC_PTR(pool, py_mongory_array_t);
  py_mongory_memory_pool_t *py_pool = (py_mongory_memory_pool_t *)pool;
  arr->base.pool = pool;
  arr->base.get = py_mongory_array_get;
  arr->base.set = NULL;
  arr->base.push = NULL;
  arr->base.each = NULL;
  arr->py_seq = seq_obj;
  arr->is_tuple = is_tuple;
  arr->base.count = (size_t)(is_tuple ? PyTuple_GET_SIZE(seq_obj) : PyList_GET_SIZE(seq_obj));
  arr->owner = py_pool ? py_pool->owner : NULL;
  mongory_value *mg_val = mongory_value_wrap_a(pool, &arr->base);
  if (mg_val) {
    mg_val->origin = (void *)seq_obj;
    mg_val->to_str = py_mongory_value_to_cstr;
  }
  return mg_val;
}

static mongory_value *py_value_converter_deep_adapter(mongory_memory_pool *pool, void *value) {
  return py_to_mongory_value_deep(pool, (PyObject *)value);
}

static mongory_value *py_value_converter_shallow_adapter(mongory_memory_pool *pool, void *value) {
  return py_to_mongory_value_shallow(pool, (PyObject *)value);
}

static void *py_value_converter_recover_adapter(mongory_memory_pool *pool, mongory_value *value) {
  (void)pool;
  return value ? value->origin : NULL;
}

/* ============================================================================
 * Regex Adapters (Python `re` module)
 * ============================================================================ */

static PyObject *py_mongory_regex_cache(mongory_memory_pool *pool, mongory_value *pattern, PyObject *compiled_re) {
  if (pattern->type == MONGORY_TYPE_REGEX && pattern->data.regex) {
    Py_DECREF(compiled_re);
    return (PyObject *)pattern->data.regex;
  }
  mongory_memory_pool *pattern_pool = pattern->pool;
  py_mongory_memory_pool_t *py_pattern_pool = (py_mongory_memory_pool_t *)pattern_pool;
  py_mongory_matcher_t *owner = py_pattern_pool ? py_pattern_pool->owner : NULL;
  mongory_value *temp_val = mongory_value_wrap_regex(pool, (void *)compiled_re);
  if (temp_val) {
    memcpy(pattern, temp_val, sizeof(mongory_value));
    pattern->pool = pattern_pool;
    pattern->origin = (void *)compiled_re;
    if (owner && owner->keep_alive_list) {
      py_track_owned_ref(owner->keep_alive_list, compiled_re);
    } else if (py_pattern_pool && py_pattern_pool->scratch_refs) {
      py_track_owned_ref(py_pattern_pool->scratch_refs, compiled_re);
    } else {
      Py_DECREF(compiled_re);
    }
  } else {
    Py_DECREF(compiled_re);
  }
  return compiled_re;
}

static bool py_mongory_regex_match_adapter(mongory_memory_pool *pool, mongory_value *pattern, mongory_value *value) {
  if (!pattern || !value || value->type != MONGORY_TYPE_STRING) {
    return false;
  }
  PyObject *py_str = (PyObject *)value->origin;
  PyObject *py_re = NULL;

  if (pattern->type == MONGORY_TYPE_REGEX && pattern->data.regex) {
    py_re = (PyObject *)pattern->data.regex;
  } else if (pattern->type == MONGORY_TYPE_STRING) {
    PyObject *pat_str = (PyObject *)pattern->origin;
    PyObject *compiled = PyObject_CallMethod(py_re_module, "compile", "O", pat_str);
    if (!compiled) {
      PyErr_Clear();
      return false;
    }
    py_re = py_mongory_regex_cache(pool, pattern, compiled);
  } else {
    return false;
  }

  PyObject *match_obj = PyObject_CallMethod(py_re, "search", "O", py_str);
  if (!match_obj) {
    PyErr_Clear();
    return false;
  }
  bool matched = (match_obj != Py_None);
  Py_DECREF(match_obj);
  return matched;
}

static bool py_mongory_regex_options_match_adapter(mongory_memory_pool *pool, mongory_value *pattern,
                                                   const char *options, mongory_value *value) {
  if (!pattern || !value || value->type != MONGORY_TYPE_STRING) {
    return false;
  }
  if (pattern->type == MONGORY_TYPE_STRING) {
    long flags = 0;
    for (const char *flag = options; *flag; flag++) {
      if (*flag == 'i') flags |= 2;  /* re.IGNORECASE */
      else if (*flag == 'm') flags |= 8;  /* re.MULTILINE */
      else if (*flag == 's') flags |= 16; /* re.DOTALL */
    }
    PyObject *pat_str = (PyObject *)pattern->origin;
    PyObject *compiled = PyObject_CallMethod(py_re_module, "compile", "Ol", pat_str, flags);
    if (!compiled) {
      PyErr_Clear();
      return false;
    }
    py_mongory_regex_cache(pool, pattern, compiled);
  }
  return py_mongory_regex_match_adapter(pool, pattern, value);
}

static char *py_mongory_regex_stringify_adapter(mongory_memory_pool *pool, mongory_value *pattern) {
  if (!pattern || pattern->type != MONGORY_TYPE_REGEX || !pattern->data.regex) {
    return NULL;
  }
  py_mongory_memory_pool_t *py_pool = (py_mongory_memory_pool_t *)pool;
  PyObject *py_re = (PyObject *)pattern->data.regex;
  PyObject *pat_attr = PyObject_GetAttrString(py_re, "pattern");
  if (!pat_attr) {
    PyErr_Clear();
    return NULL;
  }
  const char *cstr = PyUnicode_AsUTF8(pat_attr);
  py_track_owned_ref(py_pool ? py_pool->scratch_refs : NULL, pat_attr);
  return (char *)cstr;
}

/* ============================================================================
 * Custom Matcher Adapters
 * ============================================================================ */

static bool py_mongory_custom_matcher_lookup(char *key) {
  if (!current_compile_context || current_compile_context == Py_None) {
    return false;
  }
  PyObject *res = PyObject_CallMethod(current_compile_context, "has_operator", "s", key);
  if (!res) {
    PyErr_Clear();
    return false;
  }
  bool ok = PyObject_IsTrue(res) == 1;
  Py_DECREF(res);
  return ok;
}

static mongory_matcher_custom_context *py_mongory_custom_matcher_build(char *key, mongory_value *condition, void *ctx) {
  if (!condition || !condition->pool || !ctx) {
    return NULL;
  }
  mongory_memory_pool *pool = condition->pool;
  py_mongory_memory_pool_t *py_pool = (py_mongory_memory_pool_t *)pool;
  py_mongory_matcher_t *owner = py_pool ? py_pool->owner : NULL;
  PyObject *py_ctx = (PyObject *)ctx;
  PyObject *cond_obj = condition->origin ? (PyObject *)condition->origin : Py_None;

  PyObject *built = PyObject_CallMethod(py_ctx, "build_operator", "sO", key, cond_obj);
  if (!built || built == Py_None) {
    if (PyErr_Occurred()) {
      PyObject *ptype = NULL, *pvalue = NULL, *ptraceback = NULL;
      PyErr_Fetch(&ptype, &pvalue, &ptraceback);
      const char *msg = "Custom operator failed to compile";
      if (pvalue) {
        PyObject *s = PyObject_Str(pvalue);
        if (s) {
          const char *utf8 = PyUnicode_AsUTF8(s);
          if (utf8) {
            size_t len = strlen(utf8);
            char *buf = (char *)MG_ALLOC(pool, len + 1);
            if (buf) {
              memcpy(buf, utf8, len + 1);
              msg = buf;
            }
          }
          Py_DECREF(s);
        }
      }
      Py_XDECREF(ptype);
      Py_XDECREF(pvalue);
      Py_XDECREF(ptraceback);
      pool->error = MG_ALLOC_PTR(pool, mongory_error);
      if (pool->error) {
        pool->error->type = MONGORY_ERROR_INVALID_ARGUMENT;
        pool->error->message = msg;
      }
    }
    Py_XDECREF(built);
    return NULL;
  }

  /* `built` is a tuple `(name_str, predicate_callable)` */
  if (!PyTuple_Check(built) || PyTuple_GET_SIZE(built) != 2) {
    Py_DECREF(built);
    return NULL;
  }
  PyObject *name_obj = PyTuple_GET_ITEM(built, 0);
  PyObject *pred_obj = PyTuple_GET_ITEM(built, 1);
  const char *name_cstr = PyUnicode_AsUTF8(name_obj);
  if (!name_cstr) {
    PyErr_Clear();
    Py_DECREF(built);
    return NULL;
  }
  size_t name_len = strlen(name_cstr);
  char *copied_name = (char *)MG_ALLOC(pool, name_len + 1);
  if (copied_name) {
    memcpy(copied_name, name_cstr, name_len + 1);
  }

  Py_INCREF(pred_obj);
  if (owner && owner->keep_alive_list) {
    py_track_owned_ref(owner->keep_alive_list, pred_obj);
  }
  Py_DECREF(built);

  mongory_matcher_custom_context *ret = MG_ALLOC_PTR(pool, mongory_matcher_custom_context);
  if (!ret) return NULL;
  ret->name = copied_name ? copied_name : key;
  ret->external_matcher = (void *)pred_obj;
  return ret;
}

static bool py_mongory_custom_matcher_match(void *external_matcher, mongory_value *value) {
  if (!external_matcher) return false;
  PyObject *pred = (PyObject *)external_matcher;
  PyObject *arg = (value && value->origin) ? (PyObject *)value->origin : Py_None;
  PyObject *exists_arg = (value != NULL) ? Py_True : Py_False;
  PyObject *res = PyObject_CallFunctionObjArgs(pred, arg, exists_arg, NULL);
  if (!res) {
    PyErr_Clear();
    return false;
  }
  bool ok = PyObject_IsTrue(res) == 1;
  Py_DECREF(res);
  return ok;
}

/* ============================================================================
 * CMatcher Python Type Implementation
 * ============================================================================ */

static void py_mongory_matcher_release(py_mongory_matcher_t *self) {
  if (self->pool) {
    py_mongory_memory_pool_destroy(self->pool);
    self->pool = NULL;
  }
  while (self->scratch_free_list) {
    py_mongory_memory_pool_t *scratch = self->scratch_free_list;
    self->scratch_free_list = scratch->next_scratch;
    scratch->next_scratch = NULL;
    py_mongory_memory_pool_destroy(scratch);
  }
  self->matcher = NULL;
  self->condition = NULL;
  self->key_cache = NULL;
  Py_CLEAR(self->keep_alive_list);
  Py_CLEAR(self->raw_condition);
  Py_CLEAR(self->context);
}

static void CMatcher_dealloc(py_mongory_matcher_t *self) {
  py_mongory_matcher_release(self);
  Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *CMatcher_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
  (void)args;
  (void)kwds;
  py_mongory_matcher_t *self = (py_mongory_matcher_t *)type->tp_alloc(type, 0);
  if (self) {
    self->matcher = NULL;
    self->condition = NULL;
    self->pool = NULL;
    self->scratch_free_list = NULL;
    self->key_cache = NULL;
    self->keep_alive_list = NULL;
    self->raw_condition = NULL;
    self->context = NULL;
  }
  return (PyObject *)self;
}

static int CMatcher_init(py_mongory_matcher_t *self, PyObject *args, PyObject *kwds) {
  static char *kwlist[] = {"condition", "context", NULL};
  PyObject *cond_obj = NULL;
  PyObject *ctx_obj = Py_None;
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O", kwlist, &cond_obj, &ctx_obj)) {
    return -1;
  }
  if (!PyDict_Check(cond_obj)) {
    PyErr_SetString(py_invalid_condition_error, "Normalized condition must be a dict");
    return -1;
  }

  py_mongory_matcher_release(self);

  self->keep_alive_list = PyList_New(0);
  self->pool = py_mongory_memory_pool_new(self);
  self->scratch_free_list = py_mongory_memory_pool_new(self);
  if (!self->keep_alive_list || !self->pool || !self->scratch_free_list) {
    py_mongory_matcher_release(self);
    PyErr_NoMemory();
    return -1;
  }

  Py_INCREF(cond_obj);
  self->raw_condition = cond_obj;
  Py_INCREF(ctx_obj);
  self->context = ctx_obj;

  mongory_memory_pool *pool_base = &self->pool->base;
  self->key_cache = mongory_table_new(pool_base);
  self->condition = py_to_mongory_value_deep(pool_base, cond_obj);

  PyObject *prev_ctx = current_compile_context;
  current_compile_context = ctx_obj;
  self->matcher = mongory_matcher_new(pool_base, self->condition, (void *)ctx_obj);
  current_compile_context = prev_ctx;

  if (pool_base->error || !self->matcher) {
    char err_msg[512];
    snprintf(err_msg, sizeof(err_msg), "%s",
             (pool_base->error && pool_base->error->message)
                 ? pool_base->error->message
                 : "Failed to compile condition");
    py_mongory_matcher_release(self);
    PyErr_SetString(py_invalid_condition_error, err_msg);
    return -1;
  }

  return 0;
}

static py_mongory_memory_pool_t *py_scratch_acquire(py_mongory_matcher_t *self) {
  py_mongory_memory_pool_t *scratch = self->scratch_free_list;
  if (scratch) {
    self->scratch_free_list = scratch->next_scratch;
    scratch->next_scratch = NULL;
    scratch->base.error = NULL;
    return scratch;
  }
  return py_mongory_memory_pool_new(self);
}

static void py_scratch_release(py_mongory_matcher_t *self, py_mongory_memory_pool_t *scratch) {
  if (!scratch) return;
  py_mongory_memory_pool_reset(scratch);
  scratch->next_scratch = self->scratch_free_list;
  self->scratch_free_list = scratch;
}

static PyObject *CMatcher_match(py_mongory_matcher_t *self, PyObject *record) {
  if (!self->matcher || !self->pool) {
    PyErr_SetString(py_mongory_error, "Matcher is not initialized");
    return NULL;
  }
  py_mongory_memory_pool_t *scratch = py_scratch_acquire(self);
  if (!scratch) {
    return PyErr_NoMemory();
  }
  mongory_memory_pool *scratch_base = &scratch->base;
  mongory_value *data_val = py_to_mongory_value_shallow(scratch_base, record);

  PyObject *prev_ctx = current_compile_context;
  current_compile_context = self->context;
  bool matched = mongory_matcher_match(self->matcher, data_val);
  current_compile_context = prev_ctx;

  if (scratch_base->error) {
    char err_msg[512];
    snprintf(err_msg, sizeof(err_msg), "%s",
             scratch_base->error->message ? scratch_base->error->message : "Match evaluation failed");
    py_scratch_release(self, scratch);
    PyErr_SetString(py_invalid_condition_error, err_msg);
    return NULL;
  }

  py_scratch_release(self, scratch);
  if (matched) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

static PyObject *py_mongory_value_to_py(mongory_value *val);

typedef struct {
  PyObject *dict;
  bool ok;
} table_to_py_ctx;

static bool py_table_entry_to_dict_cb(char *key, mongory_value *val, void *acc) {
  table_to_py_ctx *ctx = (table_to_py_ctx *)acc;
  PyObject *py_val = py_mongory_value_to_py(val);
  if (!py_val) {
    ctx->ok = false;
    return false;
  }
  if (PyDict_SetItemString(ctx->dict, key ? key : "", py_val) < 0) {
    Py_DECREF(py_val);
    ctx->ok = false;
    return false;
  }
  Py_DECREF(py_val);
  return true;
}

static PyObject *py_mongory_value_to_py(mongory_value *val) {
  if (!val) {
    Py_RETURN_NONE;
  }
  if (val->origin != NULL) {
    PyObject *orig = (PyObject *)val->origin;
    Py_INCREF(orig);
    return orig;
  }
  switch (val->type) {
    case MONGORY_TYPE_NULL:
      Py_RETURN_NONE;
    case MONGORY_TYPE_BOOL:
      if (val->data.b) Py_RETURN_TRUE;
      Py_RETURN_FALSE;
    case MONGORY_TYPE_INT:
      return PyLong_FromLongLong((long long)val->data.i);
    case MONGORY_TYPE_DOUBLE:
      return PyFloat_FromDouble(val->data.d);
    case MONGORY_TYPE_STRING:
      return PyUnicode_FromString(val->data.s ? val->data.s : "");
    case MONGORY_TYPE_ARRAY: {
      mongory_array *arr = val->data.a;
      size_t count = arr ? arr->count : 0;
      PyObject *list = PyList_New((Py_ssize_t)count);
      if (!list) return NULL;
      for (size_t i = 0; i < count; i++) {
        PyObject *item = py_mongory_value_to_py(arr->get(arr, i));
        if (!item) {
          Py_DECREF(list);
          return NULL;
        }
        PyList_SET_ITEM(list, (Py_ssize_t)i, item);
      }
      return list;
    }
    case MONGORY_TYPE_TABLE: {
      PyObject *dict = PyDict_New();
      if (!dict) return NULL;
      if (val->data.t && val->data.t->each) {
        table_to_py_ctx ctx = {dict, true};
        val->data.t->each(val->data.t, &ctx, py_table_entry_to_dict_cb);
        if (!ctx.ok) {
          Py_DECREF(dict);
          return NULL;
        }
      }
      return dict;
    }
    default:
      Py_RETURN_NONE;
  }
}

static PyObject *py_tree_node_to_dict(mongory_value *node_val) {
  if (!node_val || node_val->type != MONGORY_TYPE_TABLE || !node_val->data.t) {
    Py_RETURN_NONE;
  }
  mongory_table *node = node_val->data.t;
  PyObject *dict = PyDict_New();
  if (!dict) return NULL;

  mongory_value *name_val = node->get(node, "name");
  if (name_val && name_val->type == MONGORY_TYPE_STRING) {
    PyObject *s = PyUnicode_FromString(name_val->data.s ? name_val->data.s : "");
    PyDict_SetItemString(dict, "name", s);
    Py_XDECREF(s);
  }

  mongory_value *cond_val = node->get(node, "condition");
  PyObject *py_cond = py_mongory_value_to_py(cond_val);
  if (py_cond) {
    PyDict_SetItemString(dict, "condition", py_cond);
    Py_DECREF(py_cond);
  }

  mongory_value *prio_val = node->get(node, "priority");
  if (prio_val && prio_val->type == MONGORY_TYPE_DOUBLE) {
    PyObject *p = PyFloat_FromDouble(prio_val->data.d);
    PyDict_SetItemString(dict, "priority", p);
    Py_XDECREF(p);
  }

  mongory_value *field_val = node->get(node, "field");
  if (field_val && field_val->type == MONGORY_TYPE_STRING && field_val->data.s) {
    PyObject *f = PyUnicode_FromString(field_val->data.s);
    PyDict_SetItemString(dict, "field", f);
    Py_XDECREF(f);
  }

  mongory_value *rec_val = node->get(node, "record");
  if (rec_val) {
    PyObject *r = py_mongory_value_to_py(rec_val);
    if (r) {
      PyDict_SetItemString(dict, "record", r);
      Py_DECREF(r);
    }
  }

  mongory_value *res_val = node->get(node, "result");
  if (res_val && res_val->type == MONGORY_TYPE_STRING && res_val->data.s) {
    PyObject *r = PyUnicode_FromString(res_val->data.s);
    PyDict_SetItemString(dict, "result", r);
    Py_XDECREF(r);
  }

  mongory_value *children_val = node->get(node, "children");
  PyObject *children_list = PyList_New(0);
  if (children_val && children_val->type == MONGORY_TYPE_ARRAY && children_val->data.a) {
    mongory_array *arr = children_val->data.a;
    for (size_t i = 0; i < arr->count; i++) {
      PyObject *child = py_tree_node_to_dict(arr->get(arr, i));
      if (child) {
        PyList_Append(children_list, child);
        Py_DECREF(child);
      }
    }
  }
  PyDict_SetItemString(dict, "children", children_list);
  Py_DECREF(children_list);

  return dict;
}

static PyObject *CMatcher_explain_tree(py_mongory_matcher_t *self, PyObject *Py_UNUSED(ignored)) {
  if (!self->matcher || !self->pool) {
    PyErr_SetString(py_mongory_error, "Matcher is not initialized");
    return NULL;
  }
  py_mongory_memory_pool_t *scratch = py_scratch_acquire(self);
  if (!scratch) return PyErr_NoMemory();
  mongory_memory_pool *scratch_base = &scratch->base;

  mongory_value *tree = mongory_matcher_explain_tree(self->matcher, scratch_base);
  if (scratch_base->error || !tree) {
    char err_msg[512];
    snprintf(err_msg, sizeof(err_msg), "%s",
             (scratch_base->error && scratch_base->error->message)
                 ? scratch_base->error->message
                 : "Explain failed");
    py_scratch_release(self, scratch);
    PyErr_SetString(py_mongory_error, err_msg);
    return NULL;
  }
  PyObject *py_tree = py_tree_node_to_dict(tree);
  py_scratch_release(self, scratch);
  return py_tree;
}

static PyObject *CMatcher_trace_tree(py_mongory_matcher_t *self, PyObject *record) {
  if (!self->matcher || !self->pool) {
    PyErr_SetString(py_mongory_error, "Matcher is not initialized");
    return NULL;
  }
  py_mongory_memory_pool_t *trace_pool = py_mongory_memory_pool_new(self);
  if (!trace_pool) return PyErr_NoMemory();
  mongory_memory_pool *trace_base = &trace_pool->base;

  mongory_value *data_val = py_to_mongory_value_shallow(trace_base, record);
  if (trace_base->error) {
    char err_msg[512];
    snprintf(err_msg, sizeof(err_msg), "%s",
             trace_base->error->message ? trace_base->error->message : "Trace conversion failed");
    py_mongory_memory_pool_destroy(trace_pool);
    PyErr_SetString(py_invalid_condition_error, err_msg);
    return NULL;
  }

  PyObject *prev_ctx = current_compile_context;
  current_compile_context = self->context;
  mongory_value *tree = mongory_matcher_trace_tree(self->matcher, data_val);
  current_compile_context = prev_ctx;

  if (trace_base->error || !tree) {
    char err_msg[512];
    snprintf(err_msg, sizeof(err_msg), "%s",
             (trace_base->error && trace_base->error->message)
                 ? trace_base->error->message
                 : "Trace evaluation failed");
    py_mongory_memory_pool_destroy(trace_pool);
    PyErr_SetString(py_invalid_condition_error, err_msg);
    return NULL;
  }

  PyObject *py_tree = py_tree_node_to_dict(tree);
  py_mongory_memory_pool_destroy(trace_pool);
  return py_tree;
}

static PyObject *CMatcher_condition(py_mongory_matcher_t *self, void *Py_UNUSED(closure)) {
  if (!self->raw_condition) Py_RETURN_NONE;
  Py_INCREF(self->raw_condition);
  return self->raw_condition;
}

static PyMethodDef CMatcher_methods[] = {
  {"match", (PyCFunction)CMatcher_match, METH_O, "Evaluate record against compiled matcher."},
  {"explain_tree", (PyCFunction)CMatcher_explain_tree, METH_NOARGS, "Return structured explain tree dict."},
  {"trace_tree", (PyCFunction)CMatcher_trace_tree, METH_O, "Evaluate record and return structured trace tree dict."},
  {NULL, NULL, 0, NULL}
};

static PyGetSetDef CMatcher_getset[] = {
  {"condition", (getter)CMatcher_condition, NULL, "Normalized condition dict.", NULL},
  {NULL, NULL, NULL, NULL, NULL}
};

static PyTypeObject CMatcherType = {
  PyVarObject_HEAD_INIT(NULL, 0)
  .tp_name = "mongory._mongory_ext.CMatcher",
  .tp_doc = "C99 mongory-core backed compiled matcher.",
  .tp_basicsize = sizeof(py_mongory_matcher_t),
  .tp_itemsize = 0,
  .tp_flags = Py_TPFLAGS_DEFAULT,
  .tp_new = CMatcher_new,
  .tp_init = (initproc)CMatcher_init,
  .tp_dealloc = (destructor)CMatcher_dealloc,
  .tp_methods = CMatcher_methods,
  .tp_getset = CMatcher_getset,
};

static PyModuleDef mongory_ext_module = {
  PyModuleDef_HEAD_INIT,
  .m_name = "mongory._mongory_ext",
  .m_doc = "CPython C-API bridge to mongory-core (C99).",
  .m_size = -1,
};

PyMODINIT_FUNC PyInit__mongory_ext(void) {
  if (PyType_Ready(&CMatcherType) < 0) {
    return NULL;
  }

  py_re_module = PyImport_ImportModule("re");
  if (!py_re_module) {
    return NULL;
  }
  py_re_pattern_type = PyObject_GetAttrString(py_re_module, "Pattern");
  if (!py_re_pattern_type) {
    Py_CLEAR(py_re_module);
    return NULL;
  }

  mongory_init();
  mongory_regex_func_set(py_mongory_regex_match_adapter);
  mongory_regex_options_func_set(py_mongory_regex_options_match_adapter);
  mongory_regex_stringify_func_set(py_mongory_regex_stringify_adapter);
  mongory_value_converter_deep_convert_set(py_value_converter_deep_adapter);
  mongory_value_converter_shallow_convert_set(py_value_converter_shallow_adapter);
  mongory_value_converter_recover_set(py_value_converter_recover_adapter);
  mongory_custom_matcher_lookup_func_set(py_mongory_custom_matcher_lookup);
  mongory_custom_matcher_build_func_set(py_mongory_custom_matcher_build);
  mongory_custom_matcher_match_func_set(py_mongory_custom_matcher_match);

  PyObject *m = PyModule_Create(&mongory_ext_module);
  if (!m) {
    return NULL;
  }

  py_mongory_error = PyErr_NewException("mongory._mongory_ext.MongoryError", PyExc_Exception, NULL);
  PyObject *bases = PyTuple_Pack(2, py_mongory_error, PyExc_ValueError);
  py_invalid_condition_error = PyErr_NewException("mongory._mongory_ext.InvalidConditionError", bases, NULL);
  Py_XDECREF(bases);

  Py_INCREF(&CMatcherType);
  PyModule_AddObject(m, "CMatcher", (PyObject *)&CMatcherType);
  Py_INCREF(py_mongory_error);
  PyModule_AddObject(m, "MongoryError", py_mongory_error);
  Py_INCREF(py_invalid_condition_error);
  PyModule_AddObject(m, "InvalidConditionError", py_invalid_condition_error);

  return m;
}
