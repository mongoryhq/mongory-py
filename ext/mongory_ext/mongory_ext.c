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
  /* Recursive per-matcher lock: core mutates shared matcher state (lazy builds, tracing) and may hold
   * its own mutex while calling back into Python, so concurrent callers must wait without the GIL. */
  PyThread_type_lock lock;
  unsigned long lock_owner;
  int lock_depth;
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

#if defined(_MSC_VER)
#  define MG_THREAD_LOCAL __declspec(thread)
#else
#  define MG_THREAD_LOCAL __thread
#endif

/*
 * Per-call state for a compile/match/trace entering mongory-core. Core callbacks cannot report
 * Python exceptions, so the first one is parked here and re-raised once core returns.
 */
typedef struct py_call_frame {
  PyObject *context;     /* borrowed CompileContext (or None) */
  PyObject *exc;         /* pending exception to re-raise as-is */
  PyObject *build_cause; /* exception chained as __cause__ of a compile error */
  struct py_call_frame *prev;
} py_call_frame;

static MG_THREAD_LOCAL py_call_frame *current_frame = NULL;

static void py_frame_enter(py_call_frame *frame, PyObject *context) {
  frame->context = context;
  frame->exc = NULL;
  frame->build_cause = NULL;
  frame->prev = current_frame;
  current_frame = frame;
}

static void py_frame_exit(py_call_frame *frame) {
  current_frame = frame->prev;
}

static inline bool py_callback_aborted(void) {
  return current_frame && current_frame->exc;
}

/* Fetches and clears the current error, returning the normalized exception (new ref). */
static PyObject *py_fetch_exception(void) {
#if PY_VERSION_HEX >= 0x030C0000
  return PyErr_GetRaisedException();
#else
  PyObject *type = NULL, *value = NULL, *tb = NULL;
  PyErr_Fetch(&type, &value, &tb);
  PyErr_NormalizeException(&type, &value, &tb);
  if (value && tb) {
    PyException_SetTraceback(value, tb);
  }
  Py_XDECREF(type);
  Py_XDECREF(tb);
  return value;
#endif
}

/* Raises `exc` (steals the reference). */
static void py_raise_exception(PyObject *exc) {
#if PY_VERSION_HEX >= 0x030C0000
  PyErr_SetRaisedException(exc);
#else
  PyObject *type = (PyObject *)Py_TYPE(exc);
  Py_INCREF(type);
  PyErr_Restore(type, exc, PyException_GetTraceback(exc));
#endif
}

/* Parks the current Python error in the active frame; only the first error is kept. */
static void py_capture_callback_error(void) {
  py_call_frame *frame = current_frame;
  if (!frame || frame->exc) {
    PyErr_Clear();
    return;
  }
  frame->exc = py_fetch_exception();
}

/* Like py_capture_callback_error, but wraps ordinary exceptions as `err_type(prefix: msg) from exc`. */
static void py_capture_callback_error_as(PyObject *err_type, const char *prefix) {
  if (!PyErr_ExceptionMatches(PyExc_Exception)) {
    py_capture_callback_error();
    return;
  }
  PyObject *cause = py_fetch_exception();
  PyErr_Format(err_type, "%s: %S", prefix, cause);
  PyObject *wrapped = py_fetch_exception();
  if (wrapped && cause) {
    PyException_SetCause(wrapped, cause);
  } else {
    Py_XDECREF(cause);
  }
  if (!wrapped) return;
  if (!current_frame || current_frame->exc) {
    Py_DECREF(wrapped);
    return;
  }
  current_frame->exc = wrapped;
}

/* `message` must outlive the pool (literal or pool-allocated). */
static void py_set_pool_error(mongory_memory_pool *pool, const char *message) {
  if (pool->error) return;
  mongory_error *error = MG_ALLOC_PTR(pool, mongory_error);
  if (!error) {
    pool->error = &MONGORY_ALLOC_ERROR;
    return;
  }
  error->type = MONGORY_ERROR_INVALID_ARGUMENT;
  error->message = message;
  pool->error = error;
}

/*
 * Converts the outcome of a core call into a Python error. Returns true (with an error set) on failure.
 * Must run before `pool` is released because the error message may live in it.
 */
static bool py_frame_finish(py_call_frame *frame, mongory_memory_pool *pool, PyObject *err_type, const char *fallback,
                            bool failed) {
  if (frame->exc) {
    Py_CLEAR(frame->build_cause);
    py_raise_exception(frame->exc);
    frame->exc = NULL;
    return true;
  }
  if (!failed && !pool->error) {
    Py_CLEAR(frame->build_cause);
    return false;
  }
  const char *msg = (pool->error && pool->error->message) ? pool->error->message : fallback;
  PyErr_SetString(err_type, msg);
  if (frame->build_cause) {
    PyObject *exc = py_fetch_exception();
    PyException_SetCause(exc, frame->build_cause);
    frame->build_cause = NULL;
    py_raise_exception(exc);
  }
  return true;
}

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
    /* Beyond int64: use a double only when it is exact, otherwise distinct ints would compare equal. */
    double d = PyLong_AsDouble(obj);
    if (!PyErr_Occurred()) {
      PyObject *as_float = PyFloat_FromDouble(d);
      int exact = as_float ? PyObject_RichCompareBool(obj, as_float, Py_EQ) : -1;
      Py_XDECREF(as_float);
      if (exact == 1) {
        return mongory_value_wrap_d(pool, d);
      }
    }
    PyErr_Clear();
    return mongory_value_wrap_u(pool, (void *)obj);
  }
  if (PyFloat_Check(obj)) {
    return mongory_value_wrap_d(pool, PyFloat_AsDouble(obj));
  }
  if (PyUnicode_Check(obj)) {
    Py_ssize_t size = 0;
    const char *cstr = PyUnicode_AsUTF8AndSize(obj, &size);
    /* Core strings are NUL-terminated; embedded NULs would silently truncate comparisons. */
    if (!cstr || strlen(cstr) != (size_t)size) {
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

  if (PyList_Check(obj) || PyTuple_Check(obj)) {
    return py_mongory_array_wrap(pool, obj, PyTuple_Check(obj));
  }
  if (PyDict_Check(obj)) {
    return py_mongory_table_wrap(pool, obj);
  }

  py_mongory_memory_pool_t *py_pool = (py_mongory_memory_pool_t *)pool;
  py_mongory_matcher_t *owner = py_pool ? py_pool->owner : NULL;
  if (!converted && owner && owner->context && owner->context != Py_None && !py_callback_aborted()) {
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
      py_capture_callback_error();
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
    if (!arr) return NULL;
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
    if (!table) return NULL;
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

  if (!converted && owner && owner->context && owner->context != Py_None && !py_callback_aborted()) {
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
      py_capture_callback_error();
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
  if (!table) return NULL;
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
  if (!arr) return NULL;
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
  if (!pattern || !value || value->type != MONGORY_TYPE_STRING || !value->origin || py_callback_aborted()) {
    return false;
  }
  PyObject *py_str = (PyObject *)value->origin;
  PyObject *py_re = NULL;

  if (pattern->type == MONGORY_TYPE_REGEX && pattern->data.regex) {
    py_re = (PyObject *)pattern->data.regex;
  } else if (pattern->type == MONGORY_TYPE_STRING && pattern->origin) {
    PyObject *pat_str = (PyObject *)pattern->origin;
    PyObject *compiled = PyObject_CallMethod(py_re_module, "compile", "O", pat_str);
    if (!compiled) {
      py_capture_callback_error_as(py_invalid_condition_error, "Invalid $regex pattern");
      return false;
    }
    py_re = py_mongory_regex_cache(pool, pattern, compiled);
  } else {
    return false;
  }

  PyObject *match_obj = PyObject_CallMethod(py_re, "search", "O", py_str);
  if (!match_obj) {
    py_capture_callback_error();
    return false;
  }
  bool matched = (match_obj != Py_None);
  Py_DECREF(match_obj);
  return matched;
}

static bool py_mongory_regex_options_match_adapter(mongory_memory_pool *pool, mongory_value *pattern,
                                                   const char *options, mongory_value *value) {
  if (!pattern || !value || value->type != MONGORY_TYPE_STRING || py_callback_aborted()) {
    return false;
  }
  if (pattern->type == MONGORY_TYPE_STRING && pattern->origin) {
    long flags = 0;
    for (const char *flag = options; *flag; flag++) {
      if (*flag == 'i') flags |= 2;  /* re.IGNORECASE */
      else if (*flag == 'm') flags |= 8;  /* re.MULTILINE */
      else if (*flag == 's') flags |= 16; /* re.DOTALL */
    }
    PyObject *pat_str = (PyObject *)pattern->origin;
    PyObject *compiled = PyObject_CallMethod(py_re_module, "compile", "Ol", pat_str, flags);
    if (!compiled) {
      py_capture_callback_error_as(py_invalid_condition_error, "Invalid $regex pattern");
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
  PyObject *context = current_frame ? current_frame->context : NULL;
  if (!context || context == Py_None || py_callback_aborted()) {
    return false;
  }
  PyObject *res = PyObject_CallMethod(context, "has_operator", "s", key);
  if (!res) {
    py_capture_callback_error();
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

  if (py_callback_aborted()) {
    py_set_pool_error(pool, "Custom operator failed to compile");
    return NULL;
  }
  PyObject *built = PyObject_CallMethod(py_ctx, "build_operator", "sO", key, cond_obj);
  if (!built) {
    if (!PyErr_ExceptionMatches(PyExc_Exception)) {
      /* KeyboardInterrupt / SystemExit etc. propagate unchanged. */
      py_capture_callback_error();
      py_set_pool_error(pool, "Custom operator failed to compile");
      return NULL;
    }
    PyObject *cause = py_fetch_exception();
    const char *msg = "Custom operator failed to compile";
    PyObject *s = cause ? PyObject_Str(cause) : NULL;
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
    PyErr_Clear();
    if (current_frame && !current_frame->build_cause) {
      current_frame->build_cause = cause;
    } else {
      Py_XDECREF(cause);
    }
    py_set_pool_error(pool, msg);
    return NULL;
  }

  /* `built` is a tuple `(name_str, predicate_callable)` */
  const char *name_cstr = NULL;
  if (PyTuple_Check(built) && PyTuple_GET_SIZE(built) == 2 && PyCallable_Check(PyTuple_GET_ITEM(built, 1))) {
    name_cstr = PyUnicode_AsUTF8(PyTuple_GET_ITEM(built, 0));
  }
  if (!name_cstr) {
    PyErr_Clear();
    Py_DECREF(built);
    py_set_pool_error(pool, "Custom operator build_operator() must return (name: str, predicate: callable)");
    return NULL;
  }
  PyObject *pred_obj = PyTuple_GET_ITEM(built, 1);
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
  if (!external_matcher || py_callback_aborted()) return false;
  PyObject *pred = (PyObject *)external_matcher;
  PyObject *arg = (value && value->origin) ? (PyObject *)value->origin : Py_None;
  PyObject *exists_arg = (value != NULL) ? Py_True : Py_False;
  PyObject *res = PyObject_CallFunctionObjArgs(pred, arg, exists_arg, NULL);
  if (!res) {
    py_capture_callback_error();
    return false;
  }
  int truth = PyObject_IsTrue(res);
  Py_DECREF(res);
  if (truth < 0) {
    py_capture_callback_error();
    return false;
  }
  return truth == 1;
}

/* ============================================================================
 * CMatcher Python Type Implementation
 * ============================================================================ */

static int py_mongory_matcher_release(py_mongory_matcher_t *self) {
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
  return 0;
}

static int CMatcher_traverse(py_mongory_matcher_t *self, visitproc visit, void *arg) {
  Py_VISIT(self->keep_alive_list);
  Py_VISIT(self->raw_condition);
  Py_VISIT(self->context);
  return 0;
}

static void CMatcher_dealloc(py_mongory_matcher_t *self) {
  PyObject_GC_UnTrack((PyObject *)self);
  py_mongory_matcher_release(self);
  if (self->lock) {
    PyThread_free_lock(self->lock);
    self->lock = NULL;
  }
  Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *CMatcher_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
  (void)args;
  (void)kwds;
  py_mongory_matcher_t *self = (py_mongory_matcher_t *)type->tp_alloc(type, 0);
  if (!self) return NULL;
  /* tp_alloc zero-fills the remaining fields. */
  self->lock = PyThread_allocate_lock();
  if (!self->lock) {
    Py_DECREF(self);
    return PyErr_NoMemory();
  }
  return (PyObject *)self;
}

/* Recursive acquire; blocks with the GIL released so the holder can finish its Python callbacks. */
static void py_matcher_lock(py_mongory_matcher_t *self) {
  unsigned long tid = PyThread_get_thread_ident();
  if (self->lock_depth > 0 && self->lock_owner == tid) {
    self->lock_depth++;
    return;
  }
  if (!PyThread_acquire_lock(self->lock, NOWAIT_LOCK)) {
    Py_BEGIN_ALLOW_THREADS
    PyThread_acquire_lock(self->lock, WAIT_LOCK);
    Py_END_ALLOW_THREADS
  }
  self->lock_owner = tid;
  self->lock_depth = 1;
}

static void py_matcher_unlock(py_mongory_matcher_t *self) {
  if (--self->lock_depth == 0) {
    self->lock_owner = 0;
    PyThread_release_lock(self->lock);
  }
}

static int CMatcher_init(py_mongory_matcher_t *self, PyObject *args, PyObject *kwds) {
  static char *kwlist[] = {"condition", "context", NULL};
  PyObject *cond_obj = NULL;
  PyObject *ctx_obj = Py_None;
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O", kwlist, &cond_obj, &ctx_obj)) {
    return -1;
  }
  /* Re-initializing would free pools that an in-flight match may still use. */
  if (self->pool) {
    PyErr_SetString(PyExc_TypeError, "CMatcher is already initialized");
    return -1;
  }
  if (!PyDict_Check(cond_obj)) {
    PyErr_SetString(py_invalid_condition_error, "Normalized condition must be a dict");
    return -1;
  }

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
  py_call_frame frame;
  py_frame_enter(&frame, ctx_obj);
  self->key_cache = mongory_table_new(pool_base);
  self->condition = py_to_mongory_value_deep(pool_base, cond_obj);
  self->matcher = mongory_matcher_new(pool_base, self->condition, (void *)ctx_obj);
  py_frame_exit(&frame);

  if (py_frame_finish(&frame, pool_base, py_invalid_condition_error, "Failed to compile condition",
                      !self->matcher || !self->key_cache || !self->condition)) {
    py_mongory_matcher_release(self);
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

static bool py_matcher_check_ready(py_mongory_matcher_t *self) {
  if (!self->matcher || !self->pool) {
    PyErr_SetString(py_mongory_error, "Matcher is not initialized");
    return false;
  }
  return true;
}

static PyObject *CMatcher_match(py_mongory_matcher_t *self, PyObject *record) {
  if (!py_matcher_check_ready(self)) return NULL;
  py_matcher_lock(self);
  py_mongory_memory_pool_t *scratch = py_scratch_acquire(self);
  if (!scratch) {
    py_matcher_unlock(self);
    return PyErr_NoMemory();
  }
  mongory_memory_pool *scratch_base = &scratch->base;

  py_call_frame frame;
  py_frame_enter(&frame, self->context);
  mongory_value *data_val = py_to_mongory_value_shallow(scratch_base, record);
  bool matched = mongory_matcher_match(self->matcher, data_val);
  py_frame_exit(&frame);

  bool failed = py_frame_finish(&frame, scratch_base, py_invalid_condition_error, "Match evaluation failed", false);
  py_scratch_release(self, scratch);
  py_matcher_unlock(self);
  if (failed) return NULL;
  return PyBool_FromLong(matched);
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
  if (!py_matcher_check_ready(self)) return NULL;
  py_matcher_lock(self);
  py_mongory_memory_pool_t *scratch = py_scratch_acquire(self);
  if (!scratch) {
    py_matcher_unlock(self);
    return PyErr_NoMemory();
  }
  mongory_memory_pool *scratch_base = &scratch->base;

  py_call_frame frame;
  py_frame_enter(&frame, self->context);
  mongory_value *tree = mongory_matcher_explain_tree(self->matcher, scratch_base);
  py_frame_exit(&frame);

  PyObject *py_tree = NULL;
  if (!py_frame_finish(&frame, scratch_base, py_mongory_error, "Explain failed", !tree)) {
    py_tree = py_tree_node_to_dict(tree);
  }
  py_scratch_release(self, scratch);
  py_matcher_unlock(self);
  return py_tree;
}

static PyObject *CMatcher_trace_tree(py_mongory_matcher_t *self, PyObject *record) {
  if (!py_matcher_check_ready(self)) return NULL;
  py_matcher_lock(self);
  py_mongory_memory_pool_t *trace_pool = py_mongory_memory_pool_new(self);
  if (!trace_pool) {
    py_matcher_unlock(self);
    return PyErr_NoMemory();
  }
  mongory_memory_pool *trace_base = &trace_pool->base;

  py_call_frame frame;
  py_frame_enter(&frame, self->context);
  mongory_value *data_val = py_to_mongory_value_shallow(trace_base, record);
  mongory_value *tree = NULL;
  if (data_val && !trace_base->error && !frame.exc) {
    tree = mongory_matcher_trace_tree(self->matcher, data_val);
  }
  py_frame_exit(&frame);

  PyObject *py_tree = NULL;
  if (!py_frame_finish(&frame, trace_base, py_invalid_condition_error, "Trace evaluation failed", !tree)) {
    py_tree = py_tree_node_to_dict(tree);
  }
  py_mongory_memory_pool_destroy(trace_pool);
  py_matcher_unlock(self);
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
  .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
  .tp_new = CMatcher_new,
  .tp_init = (initproc)CMatcher_init,
  .tp_dealloc = (destructor)CMatcher_dealloc,
  .tp_traverse = (traverseproc)CMatcher_traverse,
  .tp_clear = (inquiry)py_mongory_matcher_release,
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
  if (!py_mongory_error) goto fail;
  PyObject *bases = PyTuple_Pack(2, py_mongory_error, PyExc_ValueError);
  if (!bases) goto fail;
  py_invalid_condition_error = PyErr_NewException("mongory._mongory_ext.InvalidConditionError", bases, NULL);
  Py_DECREF(bases);
  if (!py_invalid_condition_error) goto fail;

  if (PyModule_AddObjectRef(m, "CMatcher", (PyObject *)&CMatcherType) < 0 ||
      PyModule_AddObjectRef(m, "MongoryError", py_mongory_error) < 0 ||
      PyModule_AddObjectRef(m, "InvalidConditionError", py_invalid_condition_error) < 0) {
    goto fail;
  }
  return m;

fail:
  Py_CLEAR(py_mongory_error);
  Py_CLEAR(py_invalid_condition_error);
  Py_DECREF(m);
  return NULL;
}
