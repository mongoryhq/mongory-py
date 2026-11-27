/**
 * @file py_mongory_value.c
 * @brief PyObject* <-> mongory_value* conversion, lazy dict/sequence wrappers, and key caches.
 */
#include "mongory_ext_internal.h"

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

mongory_value *py_to_mongory_value_shallow(mongory_memory_pool *pool, PyObject *obj) {
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

mongory_value *py_to_mongory_value_deep(mongory_memory_pool *pool, PyObject *obj) {
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

static bool py_mongory_table_each(mongory_table *self, void *acc, mongory_table_each_pair_callback_func callback) {
  py_mongory_table_t *table = (py_mongory_table_t *)self;
  Py_ssize_t pos = 0;
  PyObject *key = NULL;
  PyObject *val = NULL;
  while (PyDict_Next(table->py_dict, &pos, &key, &val)) {
    const char *key_cstr = NULL;
    PyObject *tmp_str = NULL;
    if (PyUnicode_Check(key)) {
      key_cstr = PyUnicode_AsUTF8(key);
    } else {
      tmp_str = PyObject_Str(key);
      if (tmp_str) {
        key_cstr = PyUnicode_AsUTF8(tmp_str);
      }
    }
    if (!key_cstr) {
      if (PyErr_Occurred()) PyErr_Clear();
      Py_XDECREF(tmp_str);
      return false;
    }
    mongory_value *mg_val = py_to_mongory_value_shallow(self->pool, val);
    bool cont = callback((char *)key_cstr, mg_val, acc);
    Py_XDECREF(tmp_str);
    if (!cont) {
      return false;
    }
  }
  return true;
}

mongory_value *py_mongory_table_wrap(mongory_memory_pool *pool, PyObject *dict_obj) {
  py_mongory_table_t *table = MG_ALLOC_PTR(pool, py_mongory_table_t);
  if (!table) return NULL;
  py_mongory_memory_pool_t *py_pool = (py_mongory_memory_pool_t *)pool;
  table->base.pool = pool;
  table->base.get = py_mongory_table_get;
  table->base.set = NULL;
  table->base.del = NULL;
  table->base.each = py_mongory_table_each;
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

static bool py_mongory_array_each(mongory_array *self, void *acc, mongory_array_callback_func callback) {
  py_mongory_array_t *arr = (py_mongory_array_t *)self;
  Py_ssize_t len = arr->is_tuple ? PyTuple_GET_SIZE(arr->py_seq) : PyList_GET_SIZE(arr->py_seq);
  for (Py_ssize_t i = 0; i < len; i++) {
    PyObject *item = arr->is_tuple ? PyTuple_GET_ITEM(arr->py_seq, i) : PyList_GET_ITEM(arr->py_seq, i);
    mongory_value *mg_val = py_to_mongory_value_shallow(self->pool, item);
    if (!callback(mg_val, acc)) {
      return false;
    }
  }
  return true;
}

mongory_value *py_mongory_array_wrap(mongory_memory_pool *pool, PyObject *seq_obj, bool is_tuple) {
  py_mongory_array_t *arr = MG_ALLOC_PTR(pool, py_mongory_array_t);
  if (!arr) return NULL;
  arr->base.pool = pool;
  arr->base.get = py_mongory_array_get;
  arr->base.set = NULL;
  arr->base.push = NULL;
  arr->base.each = py_mongory_array_each;
  arr->py_seq = seq_obj;
  arr->is_tuple = is_tuple;
  arr->base.count = (size_t)(is_tuple ? PyTuple_GET_SIZE(seq_obj) : PyList_GET_SIZE(seq_obj));
  mongory_value *mg_val = mongory_value_wrap_a(pool, &arr->base);
  if (mg_val) {
    mg_val->origin = (void *)seq_obj;
    mg_val->to_str = py_mongory_value_to_cstr;
  }
  return mg_val;
}

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

PyObject *py_mongory_value_to_py(mongory_value *val) {
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
