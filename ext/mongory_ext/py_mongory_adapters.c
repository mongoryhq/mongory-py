/**
 * @file py_mongory_adapters.c
 * @brief Bridges mongory-core regex, value converter, and custom matcher hooks to Python.
 */
#include "mongory_ext_internal.h"

static mongory_value *py_value_converter_deep_adapter(mongory_memory_pool *pool, void *value) {
  return py_to_mongory_value_deep(pool, (PyObject *)value);
}

static mongory_value *py_value_converter_shallow_adapter(mongory_memory_pool *pool, void *value) {
  return py_to_mongory_value_shallow(pool, (PyObject *)value);
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
      if (*flag == 'i') flags |= 2;       /* re.IGNORECASE */
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
  py_call_frame *frame = py_current_frame();
  PyObject *context = frame ? frame->context : NULL;
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
    py_call_frame *frame = py_current_frame();
    if (frame && !frame->build_cause) {
      frame->build_cause = cause;
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

void py_mongory_register_adapters(void) {
  mongory_regex_func_set(py_mongory_regex_match_adapter);
  mongory_regex_options_func_set(py_mongory_regex_options_match_adapter);
  mongory_regex_stringify_func_set(py_mongory_regex_stringify_adapter);

  mongory_value_converter_deep_convert_set(py_value_converter_deep_adapter);
  mongory_value_converter_shallow_convert_set(py_value_converter_shallow_adapter);
  mongory_value_converter_recover_set(py_value_converter_recover_adapter);

  mongory_custom_matcher_lookup_func_set(py_mongory_custom_matcher_lookup);
  mongory_custom_matcher_build_func_set(py_mongory_custom_matcher_build);
  mongory_custom_matcher_match_func_set(py_mongory_custom_matcher_match);
}
