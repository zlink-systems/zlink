/* SPDX-License-Identifier: MPL-2.0 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zlink.h>

typedef int (*submit_part_fn) (void *, zlink_msg_t *, zlink_send_flags_t, zlink_part_flag_t);
typedef int (*submit_rid_part_fn) (
  void *, const zlink_routing_id_t *, zlink_msg_t *, zlink_send_flags_t, zlink_part_flag_t);
typedef int (*publish_part_fn) (
  void *, const char *, zlink_msg_t *, zlink_send_flags_t, zlink_part_flag_t);

static int copy_routing_id (Py_buffer *view, zlink_routing_id_t *rid);

typedef struct prepared_parts_t
{
    zlink_msg_t *parts;
    Py_buffer *views;
    Py_ssize_t count;
} prepared_parts_t;

typedef struct received_parts_t
{
    zlink_msg_t *parts;
    Py_ssize_t count;
    Py_ssize_t capacity;
} received_parts_t;

typedef struct socket_send_op_t
{
    PyObject_HEAD void *handle;
    PyObject *payload;
    PyObject *parts;
    int flags;
    int submitted;
} socket_send_op_t;

typedef struct routed_send_op_t
{
    PyObject_HEAD void *handle;
    zlink_routing_id_t routing_id;
    PyObject *payload;
    PyObject *parts;
    int flags;
    int submitted;
} routed_send_op_t;

typedef struct publisher_send_op_t
{
    PyObject_HEAD void *handle;
    PyObject *topic;
    PyObject *payload;
    PyObject *parts;
    int flags;
    int submitted;
} publisher_send_op_t;

typedef struct bytes_parts_owner_t
{
    PyObject_HEAD PyObject *parts;
    char *open_parts;
    char inline_open_part;
    Py_ssize_t part_count;
    int closed;
} bytes_parts_owner_t;

typedef struct native_parts_owner_t
{
    PyObject_HEAD zlink_msg_t *parts;
    char *open_parts;
    char inline_open_part;
    Py_ssize_t part_count;
    int closed;
} native_parts_owner_t;


static int is_recv_no_data_result (int rc)
{
    return rc == ZLINK_RECV_NO_DATA;
}


static int copy_routing_id (Py_buffer *view, zlink_routing_id_t *rid);

static void release_prepared_parts (prepared_parts_t *prepared, Py_ssize_t close_from)
{
    if (!prepared)
        return;
    if (prepared->parts) {
        for (Py_ssize_t i = close_from; i < prepared->count; ++i)
            zlink_msg_close (&prepared->parts[i]);
        PyMem_Free (prepared->parts);
        prepared->parts = NULL;
    }
    if (prepared->views) {
        for (Py_ssize_t i = 0; i < prepared->count; ++i) {
            if (prepared->views[i].obj)
                PyBuffer_Release (&prepared->views[i]);
        }
        PyMem_Free (prepared->views);
        prepared->views = NULL;
    }
    prepared->count = 0;
}

static int payload_items (PyObject *payload, PyObject ***items_out, Py_ssize_t *count_out)
{
    PyObject **items = NULL;
    Py_ssize_t count = 1;

    if (PyList_Check (payload) || PyTuple_Check (payload)) {
        count = PySequence_Size (payload);
        if (count <= 0) {
            PyErr_SetString (PyExc_ValueError, "parts must not be empty");
            return -1;
        }
        items = PyMem_Calloc ((size_t) count, sizeof (PyObject *));
        if (!items) {
            PyErr_NoMemory ();
            return -1;
        }
        for (Py_ssize_t i = 0; i < count; ++i) {
            items[i] = PySequence_Fast_GET_ITEM (payload, i);
        }
    } else {
        items = PyMem_Calloc (1, sizeof (PyObject *));
        if (!items) {
            PyErr_NoMemory ();
            return -1;
        }
        items[0] = payload;
    }

    *items_out = items;
    *count_out = count;
    return 0;
}

static int prepare_parts (PyObject *payload, prepared_parts_t *prepared)
{
    PyObject **items = NULL;
    Py_ssize_t count = 0;

    memset (prepared, 0, sizeof (*prepared));
    if (payload_items (payload, &items, &count) != 0)
        return -1;

    prepared->parts = PyMem_Calloc ((size_t) count, sizeof (zlink_msg_t));
    prepared->views = PyMem_Calloc ((size_t) count, sizeof (Py_buffer));
    prepared->count = count;
    if (!prepared->parts || !prepared->views) {
        PyMem_Free (items);
        release_prepared_parts (prepared, 0);
        PyErr_NoMemory ();
        return -1;
    }

    for (Py_ssize_t i = 0; i < count; ++i) {
        if (PyObject_GetBuffer (items[i], &prepared->views[i], PyBUF_CONTIG_RO) != 0) {
            PyMem_Free (items);
            release_prepared_parts (prepared, 0);
            return -1;
        }

        if (zlink_msg_init_size (&prepared->parts[i], (size_t) prepared->views[i].len)
            != ZLINK_CONFIG_OK) {
            int err = zlink_errno ();
            PyMem_Free (items);
            release_prepared_parts (prepared, 0);
            if (err != 0)
                errno = err;
            PyErr_SetFromErrnoWithFilename (PyExc_OSError, NULL);
            return -1;
        }
        if (prepared->views[i].len > 0) {
            memcpy (zlink_msg_data (&prepared->parts[i]), prepared->views[i].buf,
                    (size_t) prepared->views[i].len);
        }
    }

    PyMem_Free (items);
    return 0;
}

static zlink_part_flag_t part_flag (Py_ssize_t index, Py_ssize_t count)
{
    return (index + 1 < count) ? ZLINK_PART_MORE : ZLINK_PART_FINAL;
}

static PyObject *result_tuple (int rc, int err)
{
    return Py_BuildValue ("ii", rc, err);
}

static void bytes_parts_owner_dealloc (bytes_parts_owner_t *owner)
{
    Py_XDECREF (owner->parts);
    if (owner->open_parts != &owner->inline_open_part)
        PyMem_Free (owner->open_parts);
    Py_TYPE (owner)->tp_free ((PyObject *) owner);
}

static int bytes_parts_owner_check_index (bytes_parts_owner_t *owner, Py_ssize_t index)
{
    if (index < 0 || index >= owner->part_count) {
        PyErr_SetString (PyExc_IndexError, "received part index out of range");
        return -1;
    }
    if (owner->closed || !owner->open_parts[index]) {
        PyErr_SetString (PyExc_RuntimeError, "received message is closed");
        return -1;
    }
    return 0;
}

static int
bytes_parts_owner_index (bytes_parts_owner_t *owner, PyObject *index_obj, Py_ssize_t *index_out)
{
    Py_ssize_t index = PyLong_AsSsize_t (index_obj);
    if (index == -1 && PyErr_Occurred ())
        return -1;
    if (bytes_parts_owner_check_index (owner, index) != 0)
        return -1;
    *index_out = index;
    return 0;
}

static PyObject *bytes_parts_owner_part_count (bytes_parts_owner_t *owner, void *closure)
{
    (void) closure;
    return PyLong_FromSsize_t (owner->part_count);
}

static PyObject *bytes_parts_owner_size (bytes_parts_owner_t *owner, PyObject *index_obj)
{
    Py_ssize_t index = 0;
    PyObject *part = NULL;
    if (bytes_parts_owner_index (owner, index_obj, &index) != 0)
        return NULL;
    part = PyTuple_GET_ITEM (owner->parts, index);
    return PyLong_FromSsize_t (PyBytes_GET_SIZE (part));
}

static PyObject *bytes_parts_owner_data (bytes_parts_owner_t *owner, PyObject *index_obj)
{
    Py_ssize_t index = 0;
    PyObject *part = NULL;
    if (bytes_parts_owner_index (owner, index_obj, &index) != 0)
        return NULL;
    part = PyTuple_GET_ITEM (owner->parts, index);
    return PyMemoryView_FromObject (part);
}

static PyObject *bytes_parts_owner_to_bytes (bytes_parts_owner_t *owner, PyObject *index_obj)
{
    Py_ssize_t index = 0;
    PyObject *part = NULL;
    if (bytes_parts_owner_index (owner, index_obj, &index) != 0)
        return NULL;
    part = PyTuple_GET_ITEM (owner->parts, index);
    Py_INCREF (part);
    return part;
}

static PyObject *bytes_parts_owner_close_part (bytes_parts_owner_t *owner, PyObject *index_obj)
{
    Py_ssize_t index = PyLong_AsSsize_t (index_obj);
    if (index == -1 && PyErr_Occurred ())
        return NULL;
    if (owner->closed || index < 0 || index >= owner->part_count)
        Py_RETURN_NONE;
    owner->open_parts[index] = 0;
    for (Py_ssize_t i = 0; i < owner->part_count; ++i) {
        if (owner->open_parts[i])
            Py_RETURN_NONE;
    }
    owner->closed = 1;
    Py_RETURN_NONE;
}

static PyObject *bytes_parts_owner_close (bytes_parts_owner_t *owner, PyObject *Py_UNUSED (ignored))
{
    if (!owner->closed) {
        owner->closed = 1;
        if (owner->open_parts)
            memset (owner->open_parts, 0, (size_t) owner->part_count);
    }
    Py_RETURN_NONE;
}

static PyMethodDef bytes_parts_owner_methods[] = {
  {"size", (PyCFunction) bytes_parts_owner_size, METH_O, "Return part size."},
  {"data", (PyCFunction) bytes_parts_owner_data, METH_O, "Return part memoryview."},
  {"to_bytes", (PyCFunction) bytes_parts_owner_to_bytes, METH_O, "Return part bytes."},
  {"close_part", (PyCFunction) bytes_parts_owner_close_part, METH_O, "Close one part."},
  {"close", (PyCFunction) bytes_parts_owner_close, METH_NOARGS, "Close all parts."},
  {NULL, NULL, 0, NULL},
};

static PyGetSetDef bytes_parts_owner_getset[] = {
  {"_part_count", (getter) bytes_parts_owner_part_count, NULL, "part count", NULL},
  {NULL, NULL, NULL, NULL, NULL},
};

static PyTypeObject bytes_parts_owner_type = {
  PyVarObject_HEAD_INIT (NULL, 0).tp_name = "zlink._native._zlink_native.BytesReceivedPartsOwner",
  .tp_basicsize = sizeof (bytes_parts_owner_t),
  .tp_dealloc = (destructor) bytes_parts_owner_dealloc,
  .tp_flags = Py_TPFLAGS_DEFAULT,
  .tp_methods = bytes_parts_owner_methods,
  .tp_getset = bytes_parts_owner_getset,
};

static void native_parts_owner_close_all (native_parts_owner_t *owner)
{
    if (owner->closed)
        return;
    owner->closed = 1;
    if (owner->parts) {
        for (Py_ssize_t i = 0; i < owner->part_count; ++i) {
            if (owner->open_parts && owner->open_parts[i]) {
                zlink_msg_close (&owner->parts[i]);
                owner->open_parts[i] = 0;
            }
        }
        free (owner->parts);
        owner->parts = NULL;
    }
}

static void native_parts_owner_dealloc (native_parts_owner_t *owner)
{
    native_parts_owner_close_all (owner);
    if (owner->open_parts != &owner->inline_open_part)
        PyMem_Free (owner->open_parts);
    Py_TYPE (owner)->tp_free ((PyObject *) owner);
}

static int native_parts_owner_check_index (native_parts_owner_t *owner, Py_ssize_t index)
{
    if (index < 0 || index >= owner->part_count) {
        PyErr_SetString (PyExc_IndexError, "received part index out of range");
        return -1;
    }
    if (owner->closed || !owner->parts || !owner->open_parts[index]) {
        PyErr_SetString (PyExc_RuntimeError, "received message is closed");
        return -1;
    }
    return 0;
}

static int
native_parts_owner_index (native_parts_owner_t *owner, PyObject *index_obj, Py_ssize_t *index_out)
{
    Py_ssize_t index = PyLong_AsSsize_t (index_obj);
    if (index == -1 && PyErr_Occurred ())
        return -1;
    if (native_parts_owner_check_index (owner, index) != 0)
        return -1;
    *index_out = index;
    return 0;
}

static PyObject *native_parts_owner_part_count (native_parts_owner_t *owner, void *closure)
{
    (void) closure;
    return PyLong_FromSsize_t (owner->part_count);
}

static PyObject *native_parts_owner_size (native_parts_owner_t *owner, PyObject *index_obj)
{
    Py_ssize_t index = 0;
    if (native_parts_owner_index (owner, index_obj, &index) != 0)
        return NULL;
    return PyLong_FromSize_t (zlink_msg_size (&owner->parts[index]));
}

static PyObject *native_parts_owner_data (native_parts_owner_t *owner, PyObject *index_obj)
{
    Py_ssize_t index = 0;
    void *data = NULL;
    Py_ssize_t size = 0;
    PyObject *snapshot = NULL;
    PyObject *view_obj = NULL;

    if (native_parts_owner_index (owner, index_obj, &index) != 0)
        return NULL;
    data = zlink_msg_data (&owner->parts[index]);
    size = (Py_ssize_t) zlink_msg_size (&owner->parts[index]);
    /* HOT PATH: return a Python-owned snapshot so a memoryview remains valid
     * after the received part is closed; do not replace this with a native
     * zero-copy view unless the public lifetime contract changes. */
    if (!data || size <= 0)
        snapshot = PyBytes_FromStringAndSize ("", 0);
    else
        snapshot = PyBytes_FromStringAndSize ((const char *) data, size);
    if (!snapshot)
        return NULL;
    view_obj = PyMemoryView_FromObject (snapshot);
    Py_DECREF (snapshot);
    return view_obj;
}

static PyObject *native_parts_owner_to_bytes (native_parts_owner_t *owner, PyObject *index_obj)
{
    Py_ssize_t index = 0;
    void *data = NULL;
    size_t size = 0;

    if (native_parts_owner_index (owner, index_obj, &index) != 0)
        return NULL;
    data = zlink_msg_data (&owner->parts[index]);
    size = zlink_msg_size (&owner->parts[index]);
    return PyBytes_FromStringAndSize ((const char *) data, (Py_ssize_t) size);
}

static PyObject *native_parts_owner_close_part (native_parts_owner_t *owner, PyObject *index_obj)
{
    Py_ssize_t index = PyLong_AsSsize_t (index_obj);
    int any_open = 0;
    if (index == -1 && PyErr_Occurred ())
        return NULL;
    if (owner->closed || !owner->parts || index < 0 || index >= owner->part_count)
        Py_RETURN_NONE;
    if (owner->open_parts[index]) {
        zlink_msg_close (&owner->parts[index]);
        owner->open_parts[index] = 0;
    }
    for (Py_ssize_t i = 0; i < owner->part_count; ++i) {
        if (owner->open_parts[i]) {
            any_open = 1;
            break;
        }
    }
    if (!any_open) {
        free (owner->parts);
        owner->parts = NULL;
        owner->closed = 1;
    }
    Py_RETURN_NONE;
}

static PyObject *native_parts_owner_close (native_parts_owner_t *owner,
                                           PyObject *Py_UNUSED (ignored))
{
    native_parts_owner_close_all (owner);
    Py_RETURN_NONE;
}

static PyMethodDef native_parts_owner_methods[] = {
  {"size", (PyCFunction) native_parts_owner_size, METH_O, "Return part size."},
  {"data", (PyCFunction) native_parts_owner_data, METH_O, "Return part memoryview."},
  {"to_bytes", (PyCFunction) native_parts_owner_to_bytes, METH_O, "Return part bytes."},
  {"close_part", (PyCFunction) native_parts_owner_close_part, METH_O, "Close one part."},
  {"close", (PyCFunction) native_parts_owner_close, METH_NOARGS, "Close all parts."},
  {NULL, NULL, 0, NULL},
};

static PyGetSetDef native_parts_owner_getset[] = {
  {"_part_count", (getter) native_parts_owner_part_count, NULL, "part count", NULL},
  {NULL, NULL, NULL, NULL, NULL},
};

static PyTypeObject native_parts_owner_type = {
  PyVarObject_HEAD_INIT (NULL, 0).tp_name = "zlink._native._zlink_native.NativeReceivedPartsOwner",
  .tp_basicsize = sizeof (native_parts_owner_t),
  .tp_dealloc = (destructor) native_parts_owner_dealloc,
  .tp_flags = Py_TPFLAGS_DEFAULT,
  .tp_methods = native_parts_owner_methods,
  .tp_getset = native_parts_owner_getset,
};

static PyObject *build_native_parts_owner (received_parts_t *received)
{
    native_parts_owner_t *owner = NULL;

    owner = PyObject_New (native_parts_owner_t, &native_parts_owner_type);
    if (!owner)
        return NULL;
    owner->parts = received->parts;
    owner->part_count = received->count;
    owner->closed = 0;
    owner->open_parts = NULL;
    owner->inline_open_part = 0;
    if (received->count == 1) {
        owner->inline_open_part = 1;
        owner->open_parts = &owner->inline_open_part;
    } else {
        owner->open_parts = PyMem_Calloc ((size_t) received->count, sizeof (char));
        if (!owner->open_parts) {
            owner->parts = NULL;
            Py_DECREF (owner);
            PyErr_NoMemory ();
            return NULL;
        }
        memset (owner->open_parts, 1, (size_t) received->count);
    }
    received->parts = NULL;
    received->count = 0;
    received->capacity = 0;
    return (PyObject *) owner;
}

static int raise_submit_error (int result, int err)
{
    PyObject *module = PyImport_ImportModule ("zlink.contracts.errors.errors");
    PyObject *error_type = NULL;
    PyObject *error = NULL;

    if (!module)
        return -1;
    error_type = PyObject_GetAttrString (module, "SubmitError");
    Py_DECREF (module);
    if (!error_type)
        return -1;
    error = PyObject_CallFunction (error_type, "ii", result, err);
    Py_DECREF (error_type);
    if (!error)
        return -1;
    PyErr_SetObject ((PyObject *) Py_TYPE (error), error);
    Py_DECREF (error);
    return -1;
}

static int socket_send_op_add_message (socket_send_op_t *op, PyObject *payload)
{
    if (op->parts) {
        return PyList_Append (op->parts, payload);
    }
    if (!op->payload) {
        Py_INCREF (payload);
        op->payload = payload;
        return 0;
    }

    PyObject *parts = PyList_New (2);
    if (!parts)
        return -1;
    PyList_SET_ITEM (parts, 0, op->payload);
    Py_INCREF (payload);
    PyList_SET_ITEM (parts, 1, payload);
    op->payload = NULL;
    op->parts = parts;
    return 0;
}

static void socket_send_op_dealloc (socket_send_op_t *op)
{
    Py_XDECREF (op->payload);
    Py_XDECREF (op->parts);
    Py_TYPE (op)->tp_free ((PyObject *) op);
}

static PyObject *socket_send_op_message (socket_send_op_t *op, PyObject *payload)
{
    if (op->submitted)
        return raise_submit_error (ZLINK_SUBMIT_INVALID_STATE, 0), NULL;
    if (socket_send_op_add_message (op, payload) != 0)
        return NULL;
    Py_INCREF (op);
    return (PyObject *) op;
}

static PyObject *socket_send_op_messages (socket_send_op_t *op, PyObject *args)
{
    Py_ssize_t count = PyTuple_GET_SIZE (args);
    if (op->submitted)
        return raise_submit_error (ZLINK_SUBMIT_INVALID_STATE, 0), NULL;
    for (Py_ssize_t i = 0; i < count; ++i) {
        if (socket_send_op_add_message (op, PyTuple_GET_ITEM (args, i)) != 0)
            return NULL;
    }
    Py_INCREF (op);
    return (PyObject *) op;
}

static PyObject *socket_send_op_flags (socket_send_op_t *op, PyObject *flags)
{
    long value = 0;
    if (op->submitted)
        return raise_submit_error (ZLINK_SUBMIT_INVALID_STATE, 0), NULL;
    value = PyLong_AsLong (flags);
    if (value == -1 && PyErr_Occurred ())
        return NULL;
    op->flags = (int) value;
    Py_INCREF (op);
    return (PyObject *) op;
}

static PyObject *socket_send_op_submit (socket_send_op_t *op, PyObject *Py_UNUSED (ignored))
{
    PyObject *payload = NULL;
    prepared_parts_t prepared;
    Py_ssize_t close_from = 0;
    int rc = ZLINK_SUBMIT_OK;
    int err = 0;

    if (op->submitted)
        return raise_submit_error (ZLINK_SUBMIT_INVALID_STATE, 0), NULL;
    payload = op->parts ? op->parts : op->payload;
    if (!payload)
        return raise_submit_error (ZLINK_SUBMIT_INVALID_ARGUMENT, 0), NULL;

    op->submitted = 1;
    if (!op->parts) {
        Py_buffer view = {0};
        const char *payload_data = NULL;
        Py_ssize_t payload_size = 0;
        int has_view = 0;
        zlink_msg_t part;

        if (PyByteArray_Check (payload)) {
            payload_data = PyByteArray_AS_STRING (payload);
            payload_size = PyByteArray_GET_SIZE (payload);
        } else if (PyBytes_Check (payload)) {
            payload_data = PyBytes_AS_STRING (payload);
            payload_size = PyBytes_GET_SIZE (payload);
        } else {
            if (PyObject_GetBuffer (payload, &view, PyBUF_CONTIG_RO) != 0)
                return NULL;
            payload_data = (const char *) view.buf;
            payload_size = view.len;
            has_view = 1;
        }
        /* HOT PATH: copy the caller buffer into a native message so a failed
         * send leaves the Python payload unchanged. Do not replace this with
         * a move/borrow shortcut for perf-only loops. */
        if (zlink_msg_init_size (&part, (size_t) payload_size) != ZLINK_CONFIG_OK) {
            err = zlink_errno ();
            if (has_view)
                PyBuffer_Release (&view);
            if (err != 0)
                errno = err;
            PyErr_SetFromErrnoWithFilename (PyExc_OSError, NULL);
            return NULL;
        }
        if (payload_size > 0)
            memcpy (zlink_msg_data (&part), payload_data, (size_t) payload_size);

        Py_BEGIN_ALLOW_THREADS rc =
          zlink_send_part (op->handle, &part, (zlink_send_flags_t) op->flags, ZLINK_PART_FINAL);
        if (rc != ZLINK_SUBMIT_OK) {
            err = zlink_errno ();
            zlink_msg_close (&part);
        }
        Py_END_ALLOW_THREADS

          if (has_view) PyBuffer_Release (&view);
        if (rc == ZLINK_SUBMIT_OK)
            Py_RETURN_TRUE;
        if ((op->flags & ZLINK_DONTWAIT) && rc == ZLINK_SUBMIT_BACKPRESSURED)
            Py_RETURN_FALSE;
        raise_submit_error (rc, err);
        return NULL;
    }

    if (prepare_parts (payload, &prepared) != 0)
        return NULL;
    close_from = prepared.count;

    Py_BEGIN_ALLOW_THREADS for (Py_ssize_t i = 0; i < prepared.count; ++i)
    {
        rc = zlink_send_part (op->handle, &prepared.parts[i], (zlink_send_flags_t) op->flags,
                              part_flag (i, prepared.count));
        if (rc != ZLINK_SUBMIT_OK) {
            err = zlink_errno ();
            for (Py_ssize_t j = i; j < prepared.count; ++j)
                zlink_msg_close (&prepared.parts[j]);
            break;
        }
    }
    Py_END_ALLOW_THREADS

      release_prepared_parts (&prepared, close_from);
    if (rc == ZLINK_SUBMIT_OK)
        Py_RETURN_TRUE;
    if ((op->flags & ZLINK_DONTWAIT) && rc == ZLINK_SUBMIT_BACKPRESSURED)
        Py_RETURN_FALSE;
    raise_submit_error (rc, err);
    return NULL;
}

static PyMethodDef socket_send_op_methods[] = {
  {"message", (PyCFunction) socket_send_op_message, METH_O, "Add one payload part."},
  {"messages", (PyCFunction) socket_send_op_messages, METH_VARARGS, "Add payload parts."},
  {"flags", (PyCFunction) socket_send_op_flags, METH_O, "Set send flags."},
  {"submit", (PyCFunction) socket_send_op_submit, METH_NOARGS, "Submit payload parts."},
  {NULL, NULL, 0, NULL},
};

static PyTypeObject socket_send_op_type = {
  PyVarObject_HEAD_INIT (NULL, 0).tp_name = "zlink._native._zlink_native.SocketSendOp",
  .tp_basicsize = sizeof (socket_send_op_t),
  .tp_dealloc = (destructor) socket_send_op_dealloc,
  .tp_flags = Py_TPFLAGS_DEFAULT,
  .tp_methods = socket_send_op_methods,
};

static PyObject *py_socket_send_op (PyObject *self, PyObject *handle_obj)
{
    unsigned long long handle_value = 0;
    socket_send_op_t *op = NULL;

    (void) self;
    handle_value = PyLong_AsUnsignedLongLong (handle_obj);
    if (handle_value == (unsigned long long) -1 && PyErr_Occurred ())
        return NULL;
    op = PyObject_New (socket_send_op_t, &socket_send_op_type);
    if (!op)
        return NULL;
    op->handle = (void *) (uintptr_t) handle_value;
    op->payload = NULL;
    op->parts = NULL;
    op->flags = 0;
    op->submitted = 0;
    return (PyObject *) op;
}

static int routed_send_op_add_message (routed_send_op_t *op, PyObject *payload)
{
    if (op->parts)
        return PyList_Append (op->parts, payload);
    if (!op->payload) {
        Py_INCREF (payload);
        op->payload = payload;
        return 0;
    }

    PyObject *parts = PyList_New (2);
    if (!parts)
        return -1;
    PyList_SET_ITEM (parts, 0, op->payload);
    Py_INCREF (payload);
    PyList_SET_ITEM (parts, 1, payload);
    op->payload = NULL;
    op->parts = parts;
    return 0;
}

static void routed_send_op_dealloc (routed_send_op_t *op)
{
    Py_XDECREF (op->payload);
    Py_XDECREF (op->parts);
    Py_TYPE (op)->tp_free ((PyObject *) op);
}

static PyObject *routed_send_op_message (routed_send_op_t *op, PyObject *payload)
{
    if (op->submitted)
        return raise_submit_error (ZLINK_SUBMIT_INVALID_STATE, 0), NULL;
    if (routed_send_op_add_message (op, payload) != 0)
        return NULL;
    Py_INCREF (op);
    return (PyObject *) op;
}

static PyObject *routed_send_op_messages (routed_send_op_t *op, PyObject *args)
{
    Py_ssize_t count = PyTuple_GET_SIZE (args);
    if (op->submitted)
        return raise_submit_error (ZLINK_SUBMIT_INVALID_STATE, 0), NULL;
    for (Py_ssize_t i = 0; i < count; ++i) {
        if (routed_send_op_add_message (op, PyTuple_GET_ITEM (args, i)) != 0)
            return NULL;
    }
    Py_INCREF (op);
    return (PyObject *) op;
}

static PyObject *routed_send_op_flags (routed_send_op_t *op, PyObject *flags)
{
    long value = 0;
    if (op->submitted)
        return raise_submit_error (ZLINK_SUBMIT_INVALID_STATE, 0), NULL;
    value = PyLong_AsLong (flags);
    if (value == -1 && PyErr_Occurred ())
        return NULL;
    op->flags = (int) value;
    Py_INCREF (op);
    return (PyObject *) op;
}

static PyObject *routed_send_op_submit (routed_send_op_t *op, PyObject *Py_UNUSED (ignored))
{
    PyObject *payload = NULL;
    prepared_parts_t prepared;
    Py_ssize_t close_from = 0;
    int rc = ZLINK_SUBMIT_OK;
    int err = 0;

    if (op->submitted)
        return raise_submit_error (ZLINK_SUBMIT_INVALID_STATE, 0), NULL;
    payload = op->parts ? op->parts : op->payload;
    if (!payload)
        return raise_submit_error (ZLINK_SUBMIT_INVALID_ARGUMENT, 0), NULL;

    op->submitted = 1;
    if (!op->parts) {
        Py_buffer view = {0};
        const char *payload_data = NULL;
        Py_ssize_t payload_size = 0;
        int has_view = 0;
        zlink_msg_t part;

        if (PyByteArray_Check (payload)) {
            payload_data = PyByteArray_AS_STRING (payload);
            payload_size = PyByteArray_GET_SIZE (payload);
        } else if (PyBytes_Check (payload)) {
            payload_data = PyBytes_AS_STRING (payload);
            payload_size = PyBytes_GET_SIZE (payload);
        } else {
            if (PyObject_GetBuffer (payload, &view, PyBUF_CONTIG_RO) != 0)
                return NULL;
            payload_data = (const char *) view.buf;
            payload_size = view.len;
            has_view = 1;
        }
        /* HOT PATH: copy the caller buffer into a native message so a failed
         * routed send leaves the Python payload unchanged. Do not replace
         * this with a move/borrow shortcut for perf-only loops. */
        if (zlink_msg_init_size (&part, (size_t) payload_size) != ZLINK_CONFIG_OK) {
            err = zlink_errno ();
            if (has_view)
                PyBuffer_Release (&view);
            if (err != 0)
                errno = err;
            PyErr_SetFromErrnoWithFilename (PyExc_OSError, NULL);
            return NULL;
        }
        if (payload_size > 0)
            memcpy (zlink_msg_data (&part), payload_data, (size_t) payload_size);

        Py_BEGIN_ALLOW_THREADS rc = zlink_send_part_rid (
          op->handle, &op->routing_id, &part, (zlink_send_flags_t) op->flags, ZLINK_PART_FINAL);
        if (rc != ZLINK_SUBMIT_OK) {
            err = zlink_errno ();
            zlink_msg_close (&part);
        }
        Py_END_ALLOW_THREADS

          if (has_view) PyBuffer_Release (&view);
        if (rc == ZLINK_SUBMIT_OK)
            Py_RETURN_TRUE;
        if ((op->flags & ZLINK_DONTWAIT) && rc == ZLINK_SUBMIT_BACKPRESSURED)
            Py_RETURN_FALSE;
        raise_submit_error (rc, err);
        return NULL;
    }

    if (prepare_parts (payload, &prepared) != 0)
        return NULL;
    close_from = prepared.count;

    Py_BEGIN_ALLOW_THREADS for (Py_ssize_t i = 0; i < prepared.count; ++i)
    {
        rc = zlink_send_part_rid (op->handle, &op->routing_id, &prepared.parts[i],
                                  (zlink_send_flags_t) op->flags, part_flag (i, prepared.count));
        if (rc != ZLINK_SUBMIT_OK) {
            err = zlink_errno ();
            for (Py_ssize_t j = i; j < prepared.count; ++j)
                zlink_msg_close (&prepared.parts[j]);
            break;
        }
    }
    Py_END_ALLOW_THREADS

      release_prepared_parts (&prepared, close_from);
    if (rc == ZLINK_SUBMIT_OK)
        Py_RETURN_TRUE;
    if ((op->flags & ZLINK_DONTWAIT) && rc == ZLINK_SUBMIT_BACKPRESSURED)
        Py_RETURN_FALSE;
    raise_submit_error (rc, err);
    return NULL;
}

static PyMethodDef routed_send_op_methods[] = {
  {"message", (PyCFunction) routed_send_op_message, METH_O, "Add one payload part."},
  {"messages", (PyCFunction) routed_send_op_messages, METH_VARARGS, "Add payload parts."},
  {"flags", (PyCFunction) routed_send_op_flags, METH_O, "Set send flags."},
  {"submit", (PyCFunction) routed_send_op_submit, METH_NOARGS, "Submit routed payload parts."},
  {NULL, NULL, 0, NULL},
};

static PyTypeObject routed_send_op_type = {
  PyVarObject_HEAD_INIT (NULL, 0).tp_name = "zlink._native._zlink_native.RoutedSendOp",
  .tp_basicsize = sizeof (routed_send_op_t),
  .tp_dealloc = (destructor) routed_send_op_dealloc,
  .tp_flags = Py_TPFLAGS_DEFAULT,
  .tp_methods = routed_send_op_methods,
};

static PyObject *py_routed_send_op (PyObject *self, PyObject *args)
{
    unsigned long long handle_value = 0;
    PyObject *routing_id_obj = NULL;
    Py_buffer rid_view = {0};
    routed_send_op_t *op = NULL;

    (void) self;
    if (!PyArg_ParseTuple (args, "KO", &handle_value, &routing_id_obj))
        return NULL;
    op = PyObject_New (routed_send_op_t, &routed_send_op_type);
    if (!op)
        return NULL;
    op->handle = (void *) (uintptr_t) handle_value;
    op->payload = NULL;
    op->parts = NULL;
    op->flags = 0;
    op->submitted = 0;
    if (PyObject_GetBuffer (routing_id_obj, &rid_view, PyBUF_CONTIG_RO) != 0) {
        Py_DECREF (op);
        return NULL;
    }
    if (copy_routing_id (&rid_view, &op->routing_id) != 0) {
        PyBuffer_Release (&rid_view);
        Py_DECREF (op);
        return NULL;
    }
    PyBuffer_Release (&rid_view);
    return (PyObject *) op;
}

static PyObject *validated_topic_bytes (PyObject *topic)
{
    PyObject *topic_bytes = NULL;
    Py_buffer view = {0};

    if (PyUnicode_Check (topic)) {
        topic_bytes = PyUnicode_AsUTF8String (topic);
        if (!topic_bytes)
            return NULL;
    } else if (PyBytes_Check (topic)) {
        Py_INCREF (topic);
        topic_bytes = topic;
    } else {
        if (PyObject_GetBuffer (topic, &view, PyBUF_CONTIG_RO) != 0)
            return NULL;
        topic_bytes = PyBytes_FromStringAndSize ((const char *) view.buf, view.len);
        PyBuffer_Release (&view);
        if (!topic_bytes)
            return NULL;
    }

    if (memchr (PyBytes_AS_STRING (topic_bytes), '\0', (size_t) PyBytes_GET_SIZE (topic_bytes))) {
        Py_DECREF (topic_bytes);
        PyErr_SetString (PyExc_ValueError, "topic must not contain NUL bytes");
        return NULL;
    }
    return topic_bytes;
}

static int publisher_send_op_add_message (publisher_send_op_t *op, PyObject *payload)
{
    if (op->parts)
        return PyList_Append (op->parts, payload);
    if (!op->payload) {
        Py_INCREF (payload);
        op->payload = payload;
        return 0;
    }

    PyObject *parts = PyList_New (2);
    if (!parts)
        return -1;
    PyList_SET_ITEM (parts, 0, op->payload);
    Py_INCREF (payload);
    PyList_SET_ITEM (parts, 1, payload);
    op->payload = NULL;
    op->parts = parts;
    return 0;
}

static void publisher_send_op_dealloc (publisher_send_op_t *op)
{
    Py_XDECREF (op->topic);
    Py_XDECREF (op->payload);
    Py_XDECREF (op->parts);
    Py_TYPE (op)->tp_free ((PyObject *) op);
}

static PyObject *publisher_send_op_message (publisher_send_op_t *op, PyObject *payload)
{
    if (op->submitted)
        return raise_submit_error (ZLINK_SUBMIT_INVALID_STATE, 0), NULL;
    if (publisher_send_op_add_message (op, payload) != 0)
        return NULL;
    Py_INCREF (op);
    return (PyObject *) op;
}

static PyObject *publisher_send_op_messages (publisher_send_op_t *op, PyObject *args)
{
    Py_ssize_t count = PyTuple_GET_SIZE (args);
    if (op->submitted)
        return raise_submit_error (ZLINK_SUBMIT_INVALID_STATE, 0), NULL;
    for (Py_ssize_t i = 0; i < count; ++i) {
        if (publisher_send_op_add_message (op, PyTuple_GET_ITEM (args, i)) != 0)
            return NULL;
    }
    Py_INCREF (op);
    return (PyObject *) op;
}

static PyObject *publisher_send_op_flags (publisher_send_op_t *op, PyObject *flags)
{
    long value = 0;
    if (op->submitted)
        return raise_submit_error (ZLINK_SUBMIT_INVALID_STATE, 0), NULL;
    value = PyLong_AsLong (flags);
    if (value == -1 && PyErr_Occurred ())
        return NULL;
    op->flags = (int) value;
    Py_INCREF (op);
    return (PyObject *) op;
}

static PyObject *publisher_send_op_submit (publisher_send_op_t *op, PyObject *Py_UNUSED (ignored))
{
    PyObject *payload = NULL;
    prepared_parts_t prepared;
    Py_ssize_t close_from = 0;
    const char *topic = NULL;
    int rc = ZLINK_SUBMIT_OK;
    int err = 0;

    if (op->submitted)
        return raise_submit_error (ZLINK_SUBMIT_INVALID_STATE, 0), NULL;
    payload = op->parts ? op->parts : op->payload;
    if (!payload)
        return raise_submit_error (ZLINK_SUBMIT_INVALID_ARGUMENT, 0), NULL;

    if (!op->parts) {
        Py_buffer view = {0};
        const char *payload_data = NULL;
        Py_ssize_t payload_size = 0;
        int has_view = 0;
        zlink_msg_t part;

        op->submitted = 1;
        if (PyByteArray_Check (payload)) {
            payload_data = PyByteArray_AS_STRING (payload);
            payload_size = PyByteArray_GET_SIZE (payload);
        } else if (PyBytes_Check (payload)) {
            payload_data = PyBytes_AS_STRING (payload);
            payload_size = PyBytes_GET_SIZE (payload);
        } else {
            if (PyObject_GetBuffer (payload, &view, PyBUF_CONTIG_RO) != 0)
                return NULL;
            payload_data = (const char *) view.buf;
            payload_size = view.len;
            has_view = 1;
        }
        /* HOT PATH: copy the caller buffer into a native message so a failed
         * publish leaves the Python payload unchanged. Do not replace this
         * with a move/borrow shortcut for perf-only loops. */
        if (zlink_msg_init_size (&part, (size_t) payload_size) != ZLINK_CONFIG_OK) {
            err = zlink_errno ();
            if (has_view)
                PyBuffer_Release (&view);
            if (err != 0)
                errno = err;
            PyErr_SetFromErrnoWithFilename (PyExc_OSError, NULL);
            return NULL;
        }
        if (payload_size > 0)
            memcpy (zlink_msg_data (&part), payload_data, (size_t) payload_size);

        topic = PyBytes_AS_STRING (op->topic);
        Py_BEGIN_ALLOW_THREADS rc = zlink_publish_part (
          op->handle, topic, &part, (zlink_send_flags_t) op->flags, ZLINK_PART_FINAL);
        if (rc != ZLINK_SUBMIT_OK) {
            err = zlink_errno ();
            zlink_msg_close (&part);
        }
        Py_END_ALLOW_THREADS

          if (has_view) PyBuffer_Release (&view);
        if (rc == ZLINK_SUBMIT_OK)
            Py_RETURN_TRUE;
        if ((op->flags & ZLINK_DONTWAIT) && rc == ZLINK_SUBMIT_BACKPRESSURED)
            Py_RETURN_FALSE;
        raise_submit_error (rc, err);
        return NULL;
    }

    if (prepare_parts (payload, &prepared) != 0)
        return NULL;
    close_from = prepared.count;
    topic = PyBytes_AS_STRING (op->topic);
    op->submitted = 1;

    Py_BEGIN_ALLOW_THREADS for (Py_ssize_t i = 0; i < prepared.count; ++i)
    {
        rc = zlink_publish_part (op->handle, topic, &prepared.parts[i],
                                 (zlink_send_flags_t) op->flags, part_flag (i, prepared.count));
        if (rc != ZLINK_SUBMIT_OK) {
            err = zlink_errno ();
            for (Py_ssize_t j = i; j < prepared.count; ++j)
                zlink_msg_close (&prepared.parts[j]);
            break;
        }
    }
    Py_END_ALLOW_THREADS

      release_prepared_parts (&prepared, close_from);
    if (rc == ZLINK_SUBMIT_OK)
        Py_RETURN_TRUE;
    if ((op->flags & ZLINK_DONTWAIT) && rc == ZLINK_SUBMIT_BACKPRESSURED)
        Py_RETURN_FALSE;
    raise_submit_error (rc, err);
    return NULL;
}

static PyMethodDef publisher_send_op_methods[] = {
  {"message", (PyCFunction) publisher_send_op_message, METH_O, "Add one payload part."},
  {"messages", (PyCFunction) publisher_send_op_messages, METH_VARARGS, "Add payload parts."},
  {"flags", (PyCFunction) publisher_send_op_flags, METH_O, "Set send flags."},
  {"submit", (PyCFunction) publisher_send_op_submit, METH_NOARGS, "Submit topic payload parts."},
  {NULL, NULL, 0, NULL},
};

static PyTypeObject publisher_send_op_type = {
  PyVarObject_HEAD_INIT (NULL, 0).tp_name = "zlink._native._zlink_native.PublisherSendOp",
  .tp_basicsize = sizeof (publisher_send_op_t),
  .tp_dealloc = (destructor) publisher_send_op_dealloc,
  .tp_flags = Py_TPFLAGS_DEFAULT,
  .tp_methods = publisher_send_op_methods,
};

static PyObject *py_publisher_send_op (PyObject *self, PyObject *args)
{
    unsigned long long handle_value = 0;
    PyObject *topic = NULL;
    publisher_send_op_t *op = NULL;

    (void) self;
    if (!PyArg_ParseTuple (args, "KO", &handle_value, &topic))
        return NULL;
    op = PyObject_New (publisher_send_op_t, &publisher_send_op_type);
    if (!op)
        return NULL;
    op->handle = (void *) (uintptr_t) handle_value;
    op->topic = NULL;
    op->payload = NULL;
    op->parts = NULL;
    op->flags = 0;
    op->submitted = 0;
    op->topic = validated_topic_bytes (topic);
    if (!op->topic) {
        Py_DECREF (op);
        return NULL;
    }
    return (PyObject *) op;
}

static void close_received_parts (received_parts_t *received)
{
    if (!received || !received->parts)
        return;
    for (Py_ssize_t i = 0; i < received->count; ++i)
        zlink_msg_close (&received->parts[i]);
    free (received->parts);
    received->parts = NULL;
    received->count = 0;
    received->capacity = 0;
}

static int append_received_part (received_parts_t *received, zlink_msg_t *part)
{
    if (received->count == received->capacity) {
        Py_ssize_t next_capacity = received->capacity == 0 ? 4 : received->capacity * 2;
        zlink_msg_t *next =
          realloc (received->parts, (size_t) next_capacity * sizeof (zlink_msg_t));
        if (!next)
            return -1;
        received->parts = next;
        received->capacity = next_capacity;
    }
    received->parts[received->count++] = *part;
    return 0;
}

static PyObject *py_send_parts (PyObject *self, PyObject *args)
{
    unsigned long long handle_value = 0;
    PyObject *payload = NULL;
    int flags = 0;
    prepared_parts_t prepared;
    int rc = ZLINK_SUBMIT_OK;
    int err = 0;
    Py_ssize_t close_from = 0;

    (void) self;
    if (!PyArg_ParseTuple (args, "KOi", &handle_value, &payload, &flags))
        return NULL;
    if (prepare_parts (payload, &prepared) != 0)
        return NULL;
    close_from = prepared.count;

    void *handle = (void *) (uintptr_t) handle_value;
    Py_BEGIN_ALLOW_THREADS for (Py_ssize_t i = 0; i < prepared.count; ++i)
    {
        rc = zlink_send_part (handle, &prepared.parts[i], (zlink_send_flags_t) flags,
                              part_flag (i, prepared.count));
        if (rc != ZLINK_SUBMIT_OK) {
            err = zlink_errno ();
            for (Py_ssize_t j = i; j < prepared.count; ++j)
                zlink_msg_close (&prepared.parts[j]);
            break;
        }
    }
    Py_END_ALLOW_THREADS

      release_prepared_parts (&prepared, close_from);
    return result_tuple (rc, err);
}

static int copy_routing_id (Py_buffer *view, zlink_routing_id_t *rid)
{
    if (view->len <= 0 || view->len > 255) {
        PyErr_SetString (PyExc_ValueError, "routing_id length must be between 1 and 255");
        return -1;
    }
    rid->size = (uint8_t) view->len;
    memcpy (rid->data, view->buf, (size_t) view->len);
    return 0;
}

static PyObject *py_send_parts_rid (PyObject *self, PyObject *args)
{
    unsigned long long handle_value = 0;
    PyObject *routing_id_obj = NULL;
    PyObject *payload = NULL;
    int flags = 0;
    Py_buffer rid_view = {0};
    zlink_routing_id_t rid;
    prepared_parts_t prepared;
    int rc = ZLINK_SUBMIT_OK;
    int err = 0;
    Py_ssize_t close_from = 0;

    (void) self;
    if (!PyArg_ParseTuple (args, "KOOi", &handle_value, &routing_id_obj, &payload, &flags))
        return NULL;
    if (PyObject_GetBuffer (routing_id_obj, &rid_view, PyBUF_CONTIG_RO) != 0)
        return NULL;
    if (copy_routing_id (&rid_view, &rid) != 0) {
        PyBuffer_Release (&rid_view);
        return NULL;
    }
    PyBuffer_Release (&rid_view);
    if (prepare_parts (payload, &prepared) != 0)
        return NULL;
    close_from = prepared.count;

    void *handle = (void *) (uintptr_t) handle_value;
    Py_BEGIN_ALLOW_THREADS for (Py_ssize_t i = 0; i < prepared.count; ++i)
    {
        rc = zlink_send_part_rid (handle, &rid, &prepared.parts[i], (zlink_send_flags_t) flags,
                                  part_flag (i, prepared.count));
        if (rc != ZLINK_SUBMIT_OK) {
            err = zlink_errno ();
            for (Py_ssize_t j = i; j < prepared.count; ++j)
                zlink_msg_close (&prepared.parts[j]);
            break;
        }
    }
    Py_END_ALLOW_THREADS

      release_prepared_parts (&prepared, close_from);
    return result_tuple (rc, err);
}

static PyObject *py_publish_parts (PyObject *self, PyObject *args)
{
    unsigned long long handle_value = 0;
    const char *topic = NULL;
    Py_ssize_t topic_len = 0;
    PyObject *payload = NULL;
    int flags = 0;
    prepared_parts_t prepared;
    int rc = ZLINK_SUBMIT_OK;
    int err = 0;
    Py_ssize_t close_from = 0;

    (void) self;
    if (!PyArg_ParseTuple (args, "Ky#Oi", &handle_value, &topic, &topic_len, &payload, &flags))
        return NULL;
    if (prepare_parts (payload, &prepared) != 0)
        return NULL;
    close_from = prepared.count;

    void *handle = (void *) (uintptr_t) handle_value;
    const int release_gil = (flags & ZLINK_DONTWAIT) == 0;
    PyThreadState *_save = NULL;
    if (release_gil)
        _save = PyEval_SaveThread ();
    for (Py_ssize_t i = 0; i < prepared.count; ++i) {
        rc = zlink_publish_part (handle, topic, &prepared.parts[i], (zlink_send_flags_t) flags,
                                 part_flag (i, prepared.count));
        if (rc != ZLINK_SUBMIT_OK) {
            err = zlink_errno ();
            for (Py_ssize_t j = i; j < prepared.count; ++j)
                zlink_msg_close (&prepared.parts[j]);
            break;
        }
    }
    if (release_gil)
        PyEval_RestoreThread (_save);

    release_prepared_parts (&prepared, close_from);
    return result_tuple (rc, err);
}


static PyObject *build_recv_result (int rc,
                                    int err,
                                    const zlink_routing_id_t *routing_id,
                                    received_parts_t *received)
{
    PyObject *routing_obj = Py_None;
    PyObject *parts_obj = Py_None;
    PyObject *result = NULL;

    if (rc != ZLINK_RECV_OK) {
        Py_INCREF (Py_None);
        Py_INCREF (Py_None);
        return Py_BuildValue ("iiNN", rc, err, Py_None, Py_None);
    }

    if (routing_id && routing_id->size > 0) {
        routing_obj = PyBytes_FromStringAndSize ((const char *) routing_id->data,
                                                 (Py_ssize_t) routing_id->size);
        if (!routing_obj)
            return NULL;
    } else {
        Py_INCREF (Py_None);
    }

    parts_obj = PyTuple_New (received->count);
    if (!parts_obj) {
        Py_DECREF (routing_obj);
        return NULL;
    }

    for (Py_ssize_t i = 0; i < received->count; ++i) {
        void *data = zlink_msg_data (&received->parts[i]);
        size_t size = zlink_msg_size (&received->parts[i]);
        PyObject *part = PyBytes_FromStringAndSize ((const char *) data, (Py_ssize_t) size);
        if (!part) {
            Py_DECREF (routing_obj);
            Py_DECREF (parts_obj);
            return NULL;
        }
        PyTuple_SET_ITEM (parts_obj, i, part);
    }

    result = Py_BuildValue ("iiNN", rc, err, routing_obj, parts_obj);
    return result;
}

static PyObject *py_recv_parts (PyObject *self, PyObject *args)
{
    unsigned long long handle_value = 0;
    int flags = 0;
    int rc = ZLINK_RECV_OK;
    int err = 0;
    zlink_routing_id_t routing_copy;
    int has_routing = 0;
    received_parts_t received = {0};

    (void) self;
    memset (&routing_copy, 0, sizeof (routing_copy));
    if (!PyArg_ParseTuple (args, "Ki", &handle_value, &flags))
        return NULL;

    void *handle = (void *) (uintptr_t) handle_value;
    Py_BEGIN_ALLOW_THREADS while (1)
    {
        const zlink_routing_id_t *source_rid = NULL;
        zlink_msg_t part;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        zlink_recv_flags_t recv_flags =
          received.count == 0 ? (zlink_recv_flags_t) flags : ZLINK_DONTWAIT;

        if (zlink_msg_init (&part) != ZLINK_CONFIG_OK) {
            err = zlink_errno ();
            if (err == 0)
                err = ENOMEM;
            rc = ZLINK_RECV_INTERNAL_ERROR;
            break;
        }
        rc = zlink_recv_part (handle, &source_rid, &part, &has_more, recv_flags);
        if (rc != ZLINK_RECV_OK) {
            err = zlink_errno ();
            zlink_msg_close (&part);
            break;
        }
        if (received.count == 0 && source_rid && source_rid->size > 0) {
            routing_copy = *source_rid;
            has_routing = 1;
        }
        if (append_received_part (&received, &part) != 0) {
            zlink_msg_close (&part);
            rc = ZLINK_RECV_INTERNAL_ERROR;
            err = ENOMEM;
            break;
        }
        if (has_more == ZLINK_PART_FINAL)
            break;
    }
    Py_END_ALLOW_THREADS

      PyObject *result = build_recv_result (rc, err, has_routing ? &routing_copy : NULL, &received);
    close_received_parts (&received);
    return result;
}

static PyObject *py_recv_owner (PyObject *self, PyObject *args)
{
    unsigned long long handle_value = 0;
    int flags = 0;
    int rc = ZLINK_RECV_OK;
    int err = 0;
    zlink_routing_id_t routing_copy;
    int has_routing = 0;
    received_parts_t received = {0};

    (void) self;
    memset (&routing_copy, 0, sizeof (routing_copy));
    if (!PyArg_ParseTuple (args, "Ki", &handle_value, &flags))
        return NULL;

    void *handle = (void *) (uintptr_t) handle_value;
    const int release_gil = (flags & ZLINK_DONTWAIT) == 0;
    PyThreadState *_save = NULL;
    if (release_gil)
        _save = PyEval_SaveThread ();
    while (1) {
        const zlink_routing_id_t *source_rid = NULL;
        zlink_msg_t part;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        zlink_recv_flags_t recv_flags =
          received.count == 0 ? (zlink_recv_flags_t) flags : ZLINK_DONTWAIT;

        if (zlink_msg_init (&part) != ZLINK_CONFIG_OK) {
            err = zlink_errno ();
            if (err == 0)
                err = ENOMEM;
            rc = ZLINK_RECV_INTERNAL_ERROR;
            break;
        }
        rc = zlink_recv_part (handle, &source_rid, &part, &has_more, recv_flags);
        if (rc != ZLINK_RECV_OK) {
            err = zlink_errno ();
            zlink_msg_close (&part);
            break;
        }
        if (received.count == 0 && source_rid && source_rid->size > 0) {
            routing_copy = *source_rid;
            has_routing = 1;
        }
        if (append_received_part (&received, &part) != 0) {
            zlink_msg_close (&part);
            rc = ZLINK_RECV_INTERNAL_ERROR;
            err = ENOMEM;
            break;
        }
        if (has_more == ZLINK_PART_FINAL)
            break;
    }
    if (release_gil)
        PyEval_RestoreThread (_save);

    if (rc != ZLINK_RECV_OK) {
        close_received_parts (&received);
        if ((flags & ZLINK_DONTWAIT) && is_recv_no_data_result (rc))
            Py_RETURN_FALSE;
        Py_INCREF (Py_None);
        Py_INCREF (Py_None);
        return Py_BuildValue ("iiNN", rc, err, Py_None, Py_None);
    }

    PyObject *routing_obj = Py_None;
    PyObject *owner_obj = build_native_parts_owner (&received);
    if (!owner_obj) {
        close_received_parts (&received);
        return NULL;
    }
    if (has_routing) {
        routing_obj = PyBytes_FromStringAndSize ((const char *) routing_copy.data,
                                                 (Py_ssize_t) routing_copy.size);
        if (!routing_obj) {
            Py_DECREF (owner_obj);
            return NULL;
        }
    } else {
        Py_INCREF (Py_None);
    }
    return Py_BuildValue ("iiNN", rc, err, routing_obj, owner_obj);
}

static PyObject *py_subscribe_parts (PyObject *self, PyObject *args)
{
    unsigned long long handle_value = 0;
    int flags = 0;
    int rc = ZLINK_RECV_OK;
    int err = 0;
    zlink_routing_id_t routing_copy;
    int has_routing = 0;
    char topic[256];
    size_t topic_len = 0;
    received_parts_t received = {0};

    (void) self;
    memset (&routing_copy, 0, sizeof (routing_copy));
    memset (topic, 0, sizeof (topic));
    if (!PyArg_ParseTuple (args, "Ki", &handle_value, &flags))
        return NULL;

    void *handle = (void *) (uintptr_t) handle_value;
    Py_BEGIN_ALLOW_THREADS while (1)
    {
        const zlink_routing_id_t *source_rid = NULL;
        zlink_msg_t part;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        char next_topic[256];
        size_t next_topic_len = 0;
        zlink_recv_flags_t recv_flags =
          received.count == 0 ? (zlink_recv_flags_t) flags : ZLINK_DONTWAIT;

        if (zlink_msg_init (&part) != ZLINK_CONFIG_OK) {
            err = zlink_errno ();
            if (err == 0)
                err = ENOMEM;
            rc = ZLINK_RECV_INTERNAL_ERROR;
            break;
        }
        rc = zlink_subscribe_part (handle, &source_rid, next_topic, sizeof (next_topic),
                                   &next_topic_len, &part, &has_more, recv_flags);
        if (rc != ZLINK_RECV_OK) {
            err = zlink_errno ();
            zlink_msg_close (&part);
            break;
        }
        if (received.count == 0) {
            if (source_rid && source_rid->size > 0) {
                routing_copy = *source_rid;
                has_routing = 1;
            }
            topic_len = next_topic_len < sizeof (topic) ? next_topic_len : sizeof (topic);
            memcpy (topic, next_topic, topic_len);
        }
        if (append_received_part (&received, &part) != 0) {
            zlink_msg_close (&part);
            rc = ZLINK_RECV_INTERNAL_ERROR;
            err = ENOMEM;
            break;
        }
        if (has_more == ZLINK_PART_FINAL)
            break;
    }
    Py_END_ALLOW_THREADS

      if (rc != ZLINK_RECV_OK)
    {
        close_received_parts (&received);
        if ((flags & ZLINK_DONTWAIT) && is_recv_no_data_result (rc))
            Py_RETURN_FALSE;
        Py_INCREF (Py_None);
        Py_INCREF (Py_None);
        Py_INCREF (Py_None);
        return Py_BuildValue ("iiNNN", rc, err, Py_None, Py_None, Py_None);
    }

    PyObject *routing_obj = Py_None;
    PyObject *topic_obj = PyBytes_FromStringAndSize (topic, (Py_ssize_t) topic_len);
    PyObject *parts_obj = PyTuple_New (received.count);
    if (!topic_obj || !parts_obj) {
        Py_XDECREF (topic_obj);
        Py_XDECREF (parts_obj);
        close_received_parts (&received);
        return NULL;
    }
    if (has_routing) {
        routing_obj = PyBytes_FromStringAndSize ((const char *) routing_copy.data,
                                                 (Py_ssize_t) routing_copy.size);
        if (!routing_obj) {
            Py_DECREF (topic_obj);
            Py_DECREF (parts_obj);
            close_received_parts (&received);
            return NULL;
        }
    } else {
        Py_INCREF (Py_None);
    }

    for (Py_ssize_t i = 0; i < received.count; ++i) {
        void *data = zlink_msg_data (&received.parts[i]);
        size_t size = zlink_msg_size (&received.parts[i]);
        PyObject *part = PyBytes_FromStringAndSize ((const char *) data, (Py_ssize_t) size);
        if (!part) {
            Py_DECREF (routing_obj);
            Py_DECREF (topic_obj);
            Py_DECREF (parts_obj);
            close_received_parts (&received);
            return NULL;
        }
        PyTuple_SET_ITEM (parts_obj, i, part);
    }

    PyObject *result = Py_BuildValue ("iiNNN", rc, err, routing_obj, topic_obj, parts_obj);
    close_received_parts (&received);
    return result;
}

static PyObject *py_subscribe_owner (PyObject *self, PyObject *args)
{
    unsigned long long handle_value = 0;
    int flags = 0;
    int rc = ZLINK_RECV_OK;
    int err = 0;
    zlink_routing_id_t routing_copy;
    int has_routing = 0;
    char topic[256];
    size_t topic_len = 0;
    received_parts_t received = {0};

    (void) self;
    memset (&routing_copy, 0, sizeof (routing_copy));
    memset (topic, 0, sizeof (topic));
    if (!PyArg_ParseTuple (args, "Ki", &handle_value, &flags))
        return NULL;

    void *handle = (void *) (uintptr_t) handle_value;
    const int release_gil = (flags & ZLINK_DONTWAIT) == 0;
    PyThreadState *_save = NULL;
    if (release_gil)
        _save = PyEval_SaveThread ();
    while (1) {
        const zlink_routing_id_t *source_rid = NULL;
        zlink_msg_t part;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        char next_topic[256];
        size_t next_topic_len = 0;
        zlink_recv_flags_t recv_flags =
          received.count == 0 ? (zlink_recv_flags_t) flags : ZLINK_DONTWAIT;

        if (zlink_msg_init (&part) != ZLINK_CONFIG_OK) {
            err = zlink_errno ();
            if (err == 0)
                err = ENOMEM;
            rc = ZLINK_RECV_INTERNAL_ERROR;
            break;
        }
        rc = zlink_subscribe_part (handle, &source_rid, next_topic, sizeof (next_topic),
                                   &next_topic_len, &part, &has_more, recv_flags);
        if (rc != ZLINK_RECV_OK) {
            err = zlink_errno ();
            zlink_msg_close (&part);
            break;
        }
        if (received.count == 0) {
            if (source_rid && source_rid->size > 0) {
                routing_copy = *source_rid;
                has_routing = 1;
            }
            topic_len = next_topic_len < sizeof (topic) ? next_topic_len : sizeof (topic);
            memcpy (topic, next_topic, topic_len);
        }
        if (append_received_part (&received, &part) != 0) {
            zlink_msg_close (&part);
            rc = ZLINK_RECV_INTERNAL_ERROR;
            err = ENOMEM;
            break;
        }
        if (has_more == ZLINK_PART_FINAL)
            break;
    }
    if (release_gil)
        PyEval_RestoreThread (_save);

    if (rc != ZLINK_RECV_OK) {
        close_received_parts (&received);
        if ((flags & ZLINK_DONTWAIT) && is_recv_no_data_result (rc))
            Py_RETURN_FALSE;
        Py_INCREF (Py_None);
        Py_INCREF (Py_None);
        Py_INCREF (Py_None);
        return Py_BuildValue ("iiNNN", rc, err, Py_None, Py_None, Py_None);
    }

    PyObject *routing_obj = Py_None;
    PyObject *topic_obj = PyBytes_FromStringAndSize (topic, (Py_ssize_t) topic_len);
    PyObject *owner_obj = build_native_parts_owner (&received);
    if (!topic_obj || !owner_obj) {
        Py_XDECREF (topic_obj);
        Py_XDECREF (owner_obj);
        close_received_parts (&received);
        return NULL;
    }
    if (has_routing) {
        routing_obj = PyBytes_FromStringAndSize ((const char *) routing_copy.data,
                                                 (Py_ssize_t) routing_copy.size);
        if (!routing_obj) {
            Py_DECREF (topic_obj);
            Py_DECREF (owner_obj);
            return NULL;
        }
    } else {
        Py_INCREF (Py_None);
    }

    return Py_BuildValue ("iiNNN", rc, err, routing_obj, topic_obj, owner_obj);
}



static PyMethodDef zlink_native_methods[] = {
  {"socket_send_op", py_socket_send_op, METH_O, "Create a native simple socket send builder."},
  {"routed_send_op", py_routed_send_op, METH_VARARGS,
   "Create a native routed socket send builder."},
  {"publisher_send_op", py_publisher_send_op, METH_VARARGS,
   "Create a native publisher send builder."},
  {"send_parts", py_send_parts, METH_VARARGS,
   "Submit multipart payload parts through zlink_send_part."},
  {"send_parts_rid", py_send_parts_rid, METH_VARARGS,
   "Submit routed multipart payload parts through zlink_send_part_rid."},
  {"publish_parts", py_publish_parts, METH_VARARGS,
   "Submit topic multipart payload parts through zlink_publish_part."},
  {"recv_parts", py_recv_parts, METH_VARARGS,
   "Receive multipart payload parts through zlink_recv_part."},
  {"recv_owner", py_recv_owner, METH_VARARGS,
   "Receive multipart payload parts as a native bytes owner."},
  {"subscribe_parts", py_subscribe_parts, METH_VARARGS,
   "Receive topic multipart payload parts through zlink_subscribe_part."},
  {"subscribe_owner", py_subscribe_owner, METH_VARARGS,
   "Receive topic multipart payload parts as a native owner."},
  {NULL, NULL, 0, NULL},
};

static struct PyModuleDef zlink_native_module = {
  PyModuleDef_HEAD_INIT, "_zlink_native", "Private native bridge for zlink Python hot paths.", -1,
  zlink_native_methods,
};

PyMODINIT_FUNC PyInit__zlink_native (void)
{
    PyObject *module = NULL;
    if (PyType_Ready (&socket_send_op_type) < 0)
        return NULL;
    if (PyType_Ready (&routed_send_op_type) < 0)
        return NULL;
    if (PyType_Ready (&publisher_send_op_type) < 0)
        return NULL;
    if (PyType_Ready (&bytes_parts_owner_type) < 0)
        return NULL;
    if (PyType_Ready (&native_parts_owner_type) < 0)
        return NULL;
    module = PyModule_Create (&zlink_native_module);
    if (!module)
        return NULL;
    Py_INCREF (&socket_send_op_type);
    if (PyModule_AddObject (module, "SocketSendOp", (PyObject *) &socket_send_op_type) != 0) {
        Py_DECREF (&socket_send_op_type);
        Py_DECREF (module);
        return NULL;
    }
    Py_INCREF (&routed_send_op_type);
    if (PyModule_AddObject (module, "RoutedSendOp", (PyObject *) &routed_send_op_type) != 0) {
        Py_DECREF (&routed_send_op_type);
        Py_DECREF (module);
        return NULL;
    }
    Py_INCREF (&publisher_send_op_type);
    if (PyModule_AddObject (module, "PublisherSendOp", (PyObject *) &publisher_send_op_type) != 0) {
        Py_DECREF (&publisher_send_op_type);
        Py_DECREF (module);
        return NULL;
    }
    Py_INCREF (&bytes_parts_owner_type);
    if (PyModule_AddObject (module, "BytesReceivedPartsOwner", (PyObject *) &bytes_parts_owner_type)
        != 0) {
        Py_DECREF (&bytes_parts_owner_type);
        Py_DECREF (module);
        return NULL;
    }
    Py_INCREF (&native_parts_owner_type);
    if (PyModule_AddObject (module, "NativeReceivedPartsOwner",
                            (PyObject *) &native_parts_owner_type)
        != 0) {
        Py_DECREF (&native_parts_owner_type);
        Py_DECREF (module);
        return NULL;
    }
    return module;
}
