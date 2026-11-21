/**
 * @file mongory_ext.c
 * @brief Extension entry point, memory pool wrappers, and thread-local call-frame error helpers.
 */
#include "mongory_ext_internal.h"

PyObject *py_mongory_error = NULL;
PyObject *py_invalid_condition_error = NULL;
PyObject *py_re_module = NULL;
PyObject *py_re_pattern_type = NULL;

#if defined(_MSC_VER)
#  define MG_THREAD_LOCAL __declspec(thread)
#else
#  define MG_THREAD_LOCAL __thread
#endif

static MG_THREAD_LOCAL py_call_frame *current_frame = NULL;

void py_frame_enter(py_call_frame *frame, PyObject *context) {
  frame->context = context;
  frame->exc = NULL;
  frame->build_cause = NULL;
  frame->prev = current_frame;
  current_frame = frame;
}

void py_frame_exit(py_call_frame *frame) {
  current_frame = frame->prev;
}

py_call_frame *py_current_frame(void) {
  return current_frame;
}

bool py_callback_aborted(void) {
  return current_frame && current_frame->exc;
}

/* Fetches and clears the current error, returning the normalized exception (new ref). */
PyObject *py_fetch_exception(void) {
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
void py_raise_exception(PyObject *exc) {
#if PY_VERSION_HEX >= 0x030C0000
  PyErr_SetRaisedException(exc);
#else
  PyObject *type = (PyObject *)Py_TYPE(exc);
  Py_INCREF(type);
  PyErr_Restore(type, exc, PyException_GetTraceback(exc));
#endif
}

/* Parks the current Python error in the active frame; only the first error is kept. */
void py_capture_callback_error(void) {
  py_call_frame *frame = current_frame;
  if (!frame || frame->exc) {
    PyErr_Clear();
    return;
  }
  frame->exc = py_fetch_exception();
}

/* Like py_capture_callback_error, but wraps ordinary exceptions as `err_type(prefix: msg) from exc`. */
void py_capture_callback_error_as(PyObject *err_type, const char *prefix) {
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
void py_set_pool_error(mongory_memory_pool *pool, const char *message) {
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
bool py_frame_finish(py_call_frame *frame, mongory_memory_pool *pool, PyObject *err_type, const char *fallback,
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

py_mongory_memory_pool_t *py_mongory_memory_pool_new(py_mongory_matcher_t *owner) {
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

void py_mongory_memory_pool_reset(py_mongory_memory_pool_t *pool) {
  if (!pool) return;
  if (pool->scratch_refs && PyList_GET_SIZE(pool->scratch_refs) > 0) {
    PyList_SetSlice(pool->scratch_refs, 0, PyList_GET_SIZE(pool->scratch_refs), NULL);
  }
  pool->base.error = NULL;
  pool->base.reset(&pool->base);
}

void py_mongory_memory_pool_destroy(py_mongory_memory_pool_t *pool) {
  if (!pool) return;
  Py_CLEAR(pool->scratch_refs);
  pool->base.free(&pool->base);
}

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
  py_mongory_register_adapters();

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
