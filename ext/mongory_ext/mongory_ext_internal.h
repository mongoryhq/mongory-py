#ifndef MONGORY_PY_EXT_INTERNAL_H
#define MONGORY_PY_EXT_INTERNAL_H

/**
 * @file mongory_ext_internal.h
 * @brief Shared types, globals, and internal prototypes across mongory-py C translation units.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <pythread.h>
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
} py_mongory_array_t;

/*
 * Per-call state for a compile/match/trace entering mongory-core. Core callbacks cannot report
 * Python exceptions, so the first one is parked here and re-raised once core returns.
 */
typedef struct py_call_frame {
  PyObject *exc;         /* pending exception to re-raise as-is */
  PyObject *build_cause; /* exception chained as __cause__ of a compile error */
  struct py_call_frame *prev;
} py_call_frame;

/* Shared Python module / exception / regex references initialized in PyInit__mongory_ext */
extern PyObject *py_mongory_error;
extern PyObject *py_invalid_condition_error;
extern PyObject *py_re_module;
extern PyObject *py_re_pattern_type;
extern PyTypeObject CMatcherType;

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

/* Pool & call-frame error helpers (mongory_ext.c) */
py_mongory_memory_pool_t *py_mongory_memory_pool_new(py_mongory_matcher_t *owner);
void py_mongory_memory_pool_reset(py_mongory_memory_pool_t *pool);
void py_mongory_memory_pool_destroy(py_mongory_memory_pool_t *pool);

void py_frame_enter(py_call_frame *frame);
void py_frame_exit(py_call_frame *frame);
py_call_frame *py_current_frame(void);
bool py_callback_aborted(void);
PyObject *py_fetch_exception(void);
void py_raise_exception(PyObject *exc);
void py_capture_callback_error(void);
void py_capture_callback_error_as(PyObject *err_type, const char *prefix);
void py_set_pool_error(mongory_memory_pool *pool, const char *message);
bool py_frame_finish(py_call_frame *frame, mongory_memory_pool *pool, PyObject *err_type, const char *fallback,
                     bool failed);

/* Value conversion, lazy wrappers, and key caches (py_mongory_value.c) */
mongory_value *py_to_mongory_value_deep(mongory_memory_pool *pool, PyObject *obj);
mongory_value *py_to_mongory_value_shallow(mongory_memory_pool *pool, PyObject *obj);
mongory_value *py_mongory_table_wrap(mongory_memory_pool *pool, PyObject *dict_obj);
mongory_value *py_mongory_array_wrap(mongory_memory_pool *pool, PyObject *seq_obj, bool is_tuple);
PyObject *py_mongory_value_to_py(mongory_value *val);

/* Core regex, value converter & custom matcher adapters (py_mongory_adapters.c) */
void py_mongory_register_adapters(void);

#endif /* MONGORY_PY_EXT_INTERNAL_H */
