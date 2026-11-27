/**
 * @file py_mongory_cmatcher.c
 * @brief CMatcher Python type lifecycle, GC hooks, matching, and explain_tree/trace_tree methods.
 */
#include "mongory_ext_internal.h"

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
  py_frame_enter(&frame);
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
  py_frame_enter(&frame);
  mongory_value *data_val = py_to_mongory_value_shallow(scratch_base, record);
  bool matched = mongory_matcher_match(self->matcher, data_val);
  py_frame_exit(&frame);

  bool failed = py_frame_finish(&frame, scratch_base, py_invalid_condition_error, "Match evaluation failed", false);
  py_scratch_release(self, scratch);
  py_matcher_unlock(self);
  if (failed) return NULL;
  return PyBool_FromLong(matched);
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
  py_frame_enter(&frame);
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
  py_frame_enter(&frame);
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

PyTypeObject CMatcherType = {
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
