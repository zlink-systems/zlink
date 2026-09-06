/* SPDX-License-Identifier: MPL-2.0 */
/* Included by _zlink_native.c. Each ctypes message keeps its original address;
 * multipart submission never relocates or memcpy-copies an initialized msg. */

/* Attribute identifiers are immutable module-lifetime constants. They avoid
 * allocating and hashing the same attribute names on every native entry. */
#define HOTPATH_NAMES(X) \
    X(_data_view_cache) \
    X(_captured_context) \
    X(_captured_completion_id) \
    X(_lock) \
    X(waiting_native) \
    X(settled) \
    X(releasable) \
    X(_shutdown) \
    X(Condition) \
    X(ConfigError) \
    X(RecvError) \
    X(SubmitError) \
    X(ZlinkCompletion) \
    X(_DrainResult) \
    X(_RequestEntry) \
    X(_SendEntry) \
    X(_attempt_send) \
    X(_capture_writable) \
    X(_captured) \
    X(_clone_native_for_send) \
    X(_closed) \
    X(_detached) \
    X(_dispatch_retry) \
    X(_entries) \
    X(_entries_by_id) \
    X(_error) \
    X(_finish_request_submit) \
    X(_handle) \
    X(_index) \
    X(_keepalive) \
    X(_msg) \
    X(_native_wait) \
    X(_owner) \
    X(_part_count) \
    X(_public_owner) \
    X(_published) \
    X(_release_native_wait_locked) \
    X(_release_payload) \
    X(_reply_owner) \
    X(_router_socket) \
    X(_schedule_runtime_owner_locked) \
    X(_settled) \
    X(_socket) \
    X(_state_changed) \
    X(_submit_error) \
    X(_submit_parts) \
    X(_thread_id) \
    X(_topic) \
    X(_topic_raw) \
    X(_track_native_wait_locked) \
    X(_unregister) \
    X(_valid) \
    X(_value) \
    X(acquire) \
    X(await_writable) \
    X(byref) \
    X(call_soon_threadsafe) \
    X(capture) \
    X(clone_payload) \
    X(close) \
    X(completion_id) \
    X(condition) \
    X(context) \
    X(create_future) \
    X(ctypes) \
    X(done) \
    X(fail) \
    X(from_) \
    X(future) \
    X(get) \
    X(kind) \
    X(lib) \
    X(loop) \
    X(notify_all) \
    X(parts) \
    X(payload) \
    X(release) \
    X(reply_token) \
    X(retain_retry) \
    X(retire_native) \
    X(routing_id) \
    X(succeed_send) \
    X(target) \
    X(zlink_completion_close) \
    X(zlink_completion_recv) \
    X(zlink_errno)
#define DECLARE_NAME(name) static PyObject *hp_##name;
HOTPATH_NAMES(DECLARE_NAME)
#undef DECLARE_NAME

static int init_hotpath_names (void)
{
#define INIT_NAME(name) \
    if (!hp_##name) { \
        hp_##name = PyUnicode_InternFromString (#name); \
        if (!hp_##name) return -1; \
    }
    HOTPATH_NAMES(INIT_NAME)
#undef INIT_NAME
    return 0;
}
#undef HOTPATH_NAMES

static PyObject *hot_call (PyObject *object, PyObject *name, const char *format, ...)
{
    PyObject *callable = PyObject_GetAttr (object, name);
    if (!callable)
        return NULL;
    PyObject *args = NULL;
    if (format) {
        va_list ap;
        va_start (ap, format);
        args = Py_VaBuildValue (format, ap);
        va_end (ap);
        if (!args) {
            Py_DECREF (callable);
            return NULL;
        }
        if (!PyTuple_Check (args)) {
            PyObject *tuple = PyTuple_Pack (1, args);
            Py_DECREF (args);
            args = tuple;
            if (!args) {
                Py_DECREF (callable);
                return NULL;
            }
        }
    }
    PyObject *result = PyObject_CallObject (callable, args);
    Py_XDECREF (args);
    Py_DECREF (callable);
    return result;
}

static void close_storage_list (PyObject *parts, Py_ssize_t start)
{
    for (Py_ssize_t i = start; i < PyList_GET_SIZE (parts); ++i) {
        Py_buffer view;
        if (PyObject_GetBuffer (PyList_GET_ITEM (parts, i), &view,
                                PyBUF_WRITABLE) == 0) {
            if (view.len == sizeof (zlink_msg_t))
                zlink_msg_close ((zlink_msg_t *) view.buf);
            PyBuffer_Release (&view);
        }
    }
}

static PyObject *py_materialize_parts (PyObject *self, PyObject *args)
{
    PyObject *payload, *storage_type, *message_type, *received_type, *as_view;
    (void) self;
    if (!PyArg_ParseTuple (args, "OOOOO", &payload, &storage_type,
                          &message_type, &received_type, &as_view))
        return NULL;
    int sequence = PyList_Check (payload) || PyTuple_Check (payload);
    Py_ssize_t count = sequence ? PySequence_Size (payload) : 1;
    if (count == 0) {
        PyErr_SetString (PyExc_ValueError, "payload must not be empty");
        return NULL;
    }
    PyObject *parts = PyList_New (0);
    if (!parts)
        return NULL;
    for (Py_ssize_t i = 0; i < count; ++i) {
        PyObject *part = sequence ? PySequence_Fast_GET_ITEM (payload, i) : payload;
        PyObject *storage = NULL, *source = NULL;
        int clone = PyObject_TypeCheck (part, (PyTypeObject *) message_type);
        if (PyObject_TypeCheck (part, (PyTypeObject *) received_type)) {
            storage = hot_call (part, hp__clone_native_for_send, NULL);
            if (!storage)
                goto fail;
            if (storage == Py_None)
                Py_CLEAR (storage);
        }
        if (!storage) {
            source = clone ? PyObject_GetAttr (part, hp__msg)
              : (PyBytes_CheckExact (part) || PyByteArray_CheckExact (part)
                   ? Py_NewRef (part) : PyObject_CallFunctionObjArgs (as_view, part, NULL));
            if (!source)
                goto fail;
            storage = PyObject_CallObject (storage_type, NULL);
            if (!storage) {
                Py_DECREF (source);
                goto fail;
            }
            Py_buffer dst, src;
            if (PyObject_GetBuffer (storage, &dst, PyBUF_WRITABLE) != 0) {
                Py_DECREF (source);
                Py_DECREF (storage);
                goto fail;
            }
            if (PyObject_GetBuffer (source, &src, PyBUF_SIMPLE) != 0) {
                PyBuffer_Release (&dst);
                Py_DECREF (source);
                Py_DECREF (storage);
                goto fail;
            }
            int err = 0;
            int rc = clone ? init_cloned_message ((zlink_msg_t *) dst.buf,
                                                   (zlink_msg_t *) src.buf, &err)
                           : (int) zlink_msg_init_size ((zlink_msg_t *) dst.buf, src.len);
            if (rc == 0 && !clone && src.len)
                memcpy (zlink_msg_data ((zlink_msg_t *) dst.buf), src.buf, src.len);
            if (rc != 0 && !clone)
                err = zlink_errno ();
            PyBuffer_Release (&src);
            PyBuffer_Release (&dst);
            Py_DECREF (source);
            if (rc != 0) {
                Py_DECREF (storage);
                PyObject *module = PyImport_ImportModule ("zlink.contracts.errors.errors");
                PyObject *type = module ? PyObject_GetAttr (module, hp_ConfigError) : NULL;
                Py_XDECREF (module);
                PyObject *error = type ? PyObject_CallFunction (type, "ii", rc, err) : NULL;
                Py_XDECREF (type);
                if (error) {
                    PyErr_SetObject ((PyObject *) Py_TYPE (error), error);
                    Py_DECREF (error);
                }
                goto fail;
            }
        }
        if (PyList_Append (parts, storage) != 0) {
            Py_buffer view;
            if (PyObject_GetBuffer (storage, &view, PyBUF_WRITABLE) == 0) {
                zlink_msg_close ((zlink_msg_t *) view.buf);
                PyBuffer_Release (&view);
            }
            Py_DECREF (storage);
            goto fail;
        }
        Py_DECREF (storage);
    }
    return parts;
fail:;
    PyObject *type, *value, *tb;
    PyErr_Fetch (&type, &value, &tb);
    close_storage_list (parts, 0);
    Py_DECREF (parts);
    PyErr_Restore (type, value, tb);
    return NULL;
}

static PyObject *py_submit_storage (PyObject *self, PyObject *args)
{
    unsigned long long handle, context;
    PyObject *target, *parts, *timeout;
    int flags;
    (void) self;
    if (!PyArg_ParseTuple (args, "KOOiKO", &handle, &target, &parts, &flags,
                          &context, &timeout))
        return NULL;
    if (!PyList_Check (parts)) {
        PyErr_SetString (PyExc_TypeError, "native parts must be a list");
        return NULL;
    }
    zlink_routing_id_t rid;
    zlink_routing_id_t *rid_ptr = NULL;
    if (target != Py_None) {
        Py_buffer view;
        if (PyObject_GetBuffer (target, &view, PyBUF_SIMPLE) != 0)
            goto fail;
        int rc = copy_routing_id (&view, &rid);
        PyBuffer_Release (&view);
        if (rc != 0)
            goto fail;
        rid_ptr = &rid;
    }
    long timeout_ms = timeout == Py_None ? 0 : PyLong_AsLong (timeout);
    if (timeout_ms == -1 && PyErr_Occurred ())
        goto fail;
    Py_ssize_t count = PyList_GET_SIZE (parts);
    Py_buffer inline_views[8] = {{0}};
    Py_buffer *views = count <= 8 ? inline_views : PyMem_Calloc (count, sizeof (Py_buffer));
    if (!views) {
        PyErr_NoMemory ();
        goto fail;
    }
    Py_ssize_t acquired = 0;
    for (; acquired < count; ++acquired) {
        if (PyObject_GetBuffer (PyList_GET_ITEM (parts, acquired), &views[acquired],
                                PyBUF_WRITABLE) != 0)
            break;
        if (views[acquired].len != sizeof (zlink_msg_t)) {
            ++acquired;
            PyErr_SetString (PyExc_ValueError, "invalid native message storage");
            break;
        }
    }
    int rc = ZLINK_SUBMIT_OK, err = 0;
    uint64_t completion_id = 0;
    if (!PyErr_Occurred ()) {
        /* Preserve the per-part ctypes GIL boundary, including submit_sync.
         * Moving the release outside this loop changes the opportunities for
         * concurrent public multipart senders to enter the Core staging lane. */
        for (Py_ssize_t i = 0; i < count; ++i) {
            int final = i + 1 == count;
            void *ctx = final ? (void *) (uintptr_t) context : NULL;
            uint64_t *out = final && context ? &completion_id : NULL;
            zlink_msg_t *msg = (zlink_msg_t *) views[i].buf;
            Py_BEGIN_ALLOW_THREADS
            if (timeout != Py_None)
                rc = zlink_request_part ((void *) (uintptr_t) handle, rid_ptr, msg,
                       flags, part_flag (i, count), final ? timeout_ms : 0, ctx, out);
            else if (rid_ptr)
                rc = zlink_send_part_rid ((void *) (uintptr_t) handle, rid_ptr,
                       msg, flags, part_flag (i, count), ctx, out);
            else
                rc = zlink_send_part ((void *) (uintptr_t) handle, msg,
                       flags, part_flag (i, count), ctx, out);
            if (rc != ZLINK_SUBMIT_OK)
                err = zlink_errno ();
            Py_END_ALLOW_THREADS
            if (rc != ZLINK_SUBMIT_OK) {
                for (Py_ssize_t j = i; j < count; ++j)
                    zlink_msg_close ((zlink_msg_t *) views[j].buf);
                break;
            }
        }
    }
    for (Py_ssize_t i = 0; i < acquired; ++i)
        PyBuffer_Release (&views[i]);
    if (views != inline_views)
        PyMem_Free (views);
    if (PyErr_Occurred ())
        goto fail;
    return Py_BuildValue ("iiK", rc, err, (unsigned long long) completion_id);
fail:;
    PyObject *type, *value, *tb;
    PyErr_Fetch (&type, &value, &tb);
    close_storage_list (parts, 0);
    PyErr_Restore (type, value, tb);
    return NULL;
}

static PyObject *py_build_received_parts (PyObject *self, PyObject *args)
{
    PyObject *owner;
    PyTypeObject *type;
    (void) self;
    if (!PyArg_ParseTuple (args, "OO!", &owner, &PyType_Type, &type))
        return NULL;
    PyObject *size = PyObject_GetAttr (owner, hp__part_count);
    if (!size)
        return NULL;
    Py_ssize_t count = PyLong_AsSsize_t (size);
    Py_DECREF (size);
    if (count < 0)
        return NULL;
    PyObject *parts = PyTuple_New (count);
    if (!parts)
        return NULL;
    for (Py_ssize_t i = 0; i < count; ++i) {
        PyObject *message = type->tp_alloc (type, 0);
        PyObject *index = PyLong_FromSsize_t (i);
        if (!message || !index
            || PyObject_SetAttr (message, hp__msg, Py_None) < 0
            || PyObject_SetAttr (message, hp__owner, owner) < 0
            || PyObject_SetAttr (message, hp__index, index) < 0
            || PyObject_SetAttr (message, hp__closed, Py_False) < 0
            || PyObject_SetAttr (message, hp_routing_id, Py_None) < 0) {
            Py_XDECREF (message);
            Py_XDECREF (index);
            Py_DECREF (parts);
            return NULL;
        }
        Py_DECREF (index);
        PyTuple_SET_ITEM (parts, i, message);
    }
    return parts;
}

/* Entry synchronization stays on its existing Condition/RLock. Calling its
 * acquire/release attributes uses the same lock without Python context-manager
 * frames. No native registry or parallel copy of entry state is introduced. */
static PyObject *lock_attribute (PyObject *entry, PyObject *name)
{
    PyObject *condition = PyObject_GetAttr (entry, name);
    if (!condition)
        return NULL;
    PyObject *result = hot_call (condition, hp_acquire, NULL);
    if (!result) {
        Py_DECREF (condition);
        return NULL;
    }
    Py_DECREF (result);
    return condition;
}

static PyObject *lock_entry (PyObject *entry)
{
    return lock_attribute (entry, hp_condition);
}

static void unlock_entry (PyObject *condition)
{
    PyObject *type, *value, *tb;
    PyErr_Fetch (&type, &value, &tb);
    PyObject *result = hot_call (condition, hp_release, NULL);
    Py_XDECREF (result);
    Py_DECREF (condition);
    if (type)
        PyErr_Restore (type, value, tb);
}

static int entry_truth (PyObject *entry, PyObject *name)
{
    PyObject *value = PyObject_GetAttr (entry, name);
    if (!value)
        return -1;
    int result = PyObject_IsTrue (value);
    Py_DECREF (value);
    return result;
}

static PyObject *entry_init (PyObject *entry, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"kind", "loop", "condition", NULL};
    int kind;
    PyObject *loop = Py_None, *condition = Py_None;
    if (!PyArg_ParseTupleAndKeywords (args, kwargs, "i|O$O", keywords,
                                     &kind, &loop, &condition))
        return NULL;
    PyObject *owned_condition = NULL, *future = NULL;
    if (condition == Py_None) {
        PyObject *threading = PyImport_ImportModule ("threading");
        owned_condition = threading ? hot_call (threading, hp_Condition, NULL) : NULL;
        Py_XDECREF (threading);
        if (!owned_condition)
            return NULL;
        condition = owned_condition;
    }
    future = loop != Py_None && kind != ZLINK_COMPLETION_SEND
      ? hot_call (loop, hp_create_future, NULL) : Py_NewRef (Py_None);
    PyObject *kind_obj = PyLong_FromLong (kind);
    PyObject *zero = PyLong_FromLong (0);
    if (!future || !kind_obj || !zero)
        goto done;
    if (PyObject_SetAttr (entry, hp_kind, kind_obj) < 0
        || PyObject_SetAttr (entry, hp_loop, loop) < 0
        || PyObject_SetAttr (entry, hp_future, future) < 0
        || PyObject_SetAttr (entry, hp_condition, condition) < 0)
        goto done;
    PyObject *false_fields[] = {hp__published, hp__captured, hp__settled, hp__detached, hp__native_wait, NULL};
    PyObject *none_fields[] = {hp__value, hp__error, NULL};
    PyObject *zero_fields[] = {hp__captured_completion_id, hp__captured_context, hp_completion_id, NULL};
    for (PyObject **field = false_fields; *field; ++field)
        if (PyObject_SetAttr (entry, *field, Py_False) < 0)
            goto done;
    for (PyObject **field = none_fields; *field; ++field)
        if (PyObject_SetAttr (entry, *field, Py_None) < 0)
            goto done;
    for (PyObject **field = zero_fields; *field; ++field)
        if (PyObject_SetAttr (entry, *field, zero) < 0)
            goto done;
done:
    Py_XDECREF (owned_condition);
    Py_XDECREF (future);
    Py_XDECREF (kind_obj);
    Py_XDECREF (zero);
    if (PyErr_Occurred ())
        return NULL;
    Py_RETURN_NONE;
}

static PyObject *entry_clone_payload (PyObject *entry, PyObject *Py_UNUSED (unused))
{
    PyObject *condition = lock_entry (entry);
    if (!condition)
        return NULL;
    PyObject *payload = PyObject_GetAttr (entry, hp_payload);
    PyObject *clones = NULL;
    if (!payload)
        goto done;
    if (payload == Py_None) {
        clones = Py_NewRef (Py_None);
        goto done;
    }
    clones = PyList_New (0);
    if (!clones)
        goto done;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE (payload); ++i) {
        PyObject *source = PyList_GET_ITEM (payload, i);
        PyObject *clone = PyObject_CallObject ((PyObject *) Py_TYPE (source), NULL);
        Py_buffer src, dst;
        if (!clone)
            goto fail;
        if (PyObject_GetBuffer (clone, &dst, PyBUF_WRITABLE) < 0) {
            Py_DECREF (clone);
            goto fail;
        }
        if (PyObject_GetBuffer (source, &src, PyBUF_SIMPLE) < 0) {
            PyBuffer_Release (&dst);
            Py_DECREF (clone);
            goto fail;
        }
        int err;
        int rc = init_cloned_message ((zlink_msg_t *) dst.buf, (zlink_msg_t *) src.buf, &err);
        if (rc == 0 && PyList_Append (clones, clone) < 0) {
            zlink_msg_close ((zlink_msg_t *) dst.buf);
            rc = -1;
        }
        PyBuffer_Release (&src);
        PyBuffer_Release (&dst);
        Py_DECREF (clone);
        if (rc != 0) {
            if (!PyErr_Occurred ()) {
                PyObject *module = PyImport_ImportModule ("zlink.contracts.errors.errors");
                PyObject *error = module ? hot_call (module, hp_ConfigError, "ii", rc, err) : NULL;
                Py_XDECREF (module);
                if (error) {
                    PyErr_SetObject ((PyObject *) Py_TYPE (error), error);
                    Py_DECREF (error);
                }
            }
            goto fail;
        }
    }
    goto done;
fail:;
    PyObject *type, *value, *tb;
    PyErr_Fetch (&type, &value, &tb);
    close_storage_list (clones, 0);
    Py_CLEAR (clones);
    PyErr_Restore (type, value, tb);
done:
    Py_XDECREF (payload);
    unlock_entry (condition);
    return clones;
}

static PyObject *entry_settle (PyObject *, PyObject *);
static PyObject *entry_deliver (PyObject *, PyObject *);

static PyObject *send_entry_succeed (PyObject *entry, PyObject *Py_UNUSED (unused))
{
    PyObject *condition = lock_entry (entry);
    if (!condition)
        return NULL;
    PyObject *payload = PyObject_GetAttr (entry, hp_payload);
    PyObject *deliver = NULL;
    if (!payload)
        goto done;
    if (PyObject_SetAttr (entry, hp_payload, Py_None) < 0
        || PyObject_SetAttr (entry, hp_target, Py_None) < 0)
        goto done;
    if (payload != Py_None)
        close_storage_list (payload, 0);
    int settled = entry_truth (entry, hp__settled);
    if (settled != 0)
        goto done;
    if (PyObject_SetAttr (entry, hp__published, Py_True) < 0
        || PyObject_SetAttr (entry, hp__captured, Py_True) < 0
        || PyObject_SetAttr (entry, hp__native_wait, Py_False) < 0)
        goto done;
    deliver = entry_settle (entry, NULL);
done:
    Py_XDECREF (payload);
    unlock_entry (condition);
    if (PyErr_Occurred ()) {
        Py_XDECREF (deliver);
        return NULL;
    }
    if (deliver) {
        PyObject *result = entry_deliver (entry, deliver);
        Py_DECREF (deliver);
        return result;
    }
    Py_RETURN_NONE;
}

static PyMethodDef entry_init_def = {"__init__", (PyCFunction) entry_init, METH_VARARGS | METH_KEYWORDS, NULL};
static PyMethodDef entry_clone_def = {"clone_payload", entry_clone_payload, METH_NOARGS, NULL};
static PyMethodDef send_succeed_def = {"succeed_send", send_entry_succeed, METH_NOARGS, NULL};

static PyObject *entry_context (PyObject *entry, void *closure)
{
    (void) closure;
    return PyLong_FromVoidPtr (entry);
}

static PyObject *entry_state (PyObject *entry, void *closure)
{
    PyObject *condition = lock_entry (entry);
    if (!condition)
        return NULL;
    PyObject *field = closure ? *(PyObject **) closure : hp__settled;
    PyObject *result = PyObject_GetAttr (entry, field);
    if (!closure && result && PyObject_IsTrue (result)) {
        int waiting = entry_truth (entry, hp__native_wait);
        Py_CLEAR (result);
        if (waiting >= 0)
            result = PyBool_FromLong (!waiting);
    }
    unlock_entry (condition);
    return result;
}

static PyGetSetDef entry_properties[] = {
  {"context", entry_context, NULL, NULL, NULL},
  {"settled", entry_state, NULL, NULL, &hp__settled},
  {"waiting_native", entry_state, NULL, NULL, &hp__native_wait},
  {"releasable", entry_state, NULL, NULL, NULL},
  {NULL}
};

static PyObject *send_entry_init (PyObject *entry, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"loop", "target", "native_parts", "condition", NULL};
    PyObject *loop, *target, *parts, *condition = Py_None;
    if (!PyArg_ParseTupleAndKeywords (args, kwargs, "OOO|$O", keywords,
                                     &loop, &target, &parts, &condition))
        return NULL;
    PyObject *base_args = Py_BuildValue ("iO", ZLINK_COMPLETION_SEND, loop);
    PyObject *base_kwargs = Py_BuildValue ("{s:O}", "condition", condition);
    PyObject *result = base_args && base_kwargs ? entry_init (entry, base_args, base_kwargs) : NULL;
    Py_XDECREF (base_args);
    Py_XDECREF (base_kwargs);
    if (!result)
        return NULL;
    Py_DECREF (result);
    PyObject *retained_target = target == Py_None ? Py_NewRef (Py_None) : PyObject_Bytes (target);
    if (!retained_target)
        return NULL;
    int rc = PyObject_SetAttr (entry, hp_target, retained_target);
    Py_DECREF (retained_target);
    if (rc < 0 || PyObject_SetAttr (entry, hp_payload, parts) < 0)
        return NULL;
    Py_RETURN_NONE;
}

static PyMethodDef send_init_def = {"__init__", (PyCFunction) send_entry_init, METH_VARARGS | METH_KEYWORDS, NULL};

static int close_message_values (PyObject *messages)
{
    if (!messages || !PyList_Check (messages))
        return 0;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE (messages); ++i) {
        PyObject *result = PyObject_CallMethod (PyList_GET_ITEM (messages, i), "close", NULL);
        if (!result) {
            /* Preserve _close_messages' Exception boundary: process-control
             * BaseExceptions must still propagate. */
            if (!PyErr_ExceptionMatches (PyExc_Exception))
                return -1;
            PyErr_Clear ();
        }
        Py_XDECREF (result);
    }
    return 0;
}

static PyObject *entry_settle (PyObject *entry, PyObject *Py_UNUSED (unused))
{
    int published = entry_truth (entry, hp__published);
    int captured = entry_truth (entry, hp__captured);
    int settled = entry_truth (entry, hp__settled);
    if (published < 0 || captured < 0 || settled < 0)
        return NULL;
    if (!published || !captured || settled)
        Py_RETURN_NONE;
    if (PyObject_SetAttr (entry, hp__settled, Py_True) < 0)
        return NULL;
    PyObject *condition = PyObject_GetAttr (entry, hp_condition);
    PyObject *notified = condition ? hot_call (condition, hp_notify_all, NULL) : NULL;
    Py_XDECREF (condition);
    if (!notified)
        return NULL;
    Py_DECREF (notified);
    PyObject *value = PyObject_GetAttr (entry, hp__value);
    PyObject *error = PyObject_GetAttr (entry, hp__error);
    PyObject *detached = PyObject_GetAttr (entry, hp__detached);
    PyObject *result = value && error && detached ? PyTuple_Pack (3, value, error, detached) : NULL;
    Py_XDECREF (value);
    Py_XDECREF (error);
    Py_XDECREF (detached);
    return result;
}

static PyObject *entry_finish_delivery (PyObject *state, PyObject *Py_UNUSED (unused))
{
    PyObject *entry = PyTuple_GET_ITEM (state, 0);
    PyObject *deliver = PyTuple_GET_ITEM (state, 1);
    PyObject *value = PyTuple_GET_ITEM (deliver, 0);
    PyObject *error = PyTuple_GET_ITEM (deliver, 1);
    PyObject *condition = lock_entry (entry);
    if (!condition)
        return NULL;
    int detached = entry_truth (entry, hp__detached);
    unlock_entry (condition);
    if (detached < 0)
        return NULL;
    PyObject *future = PyObject_GetAttr (entry, hp_future);
    if (!future)
        return NULL;
    PyObject *done = hot_call (future, hp_done, NULL);
    if (!done) {
        Py_DECREF (future);
        return NULL;
    }
    int completed = PyObject_IsTrue (done);
    Py_DECREF (done);
    PyObject *result;
    if (detached || completed) {
        result = close_message_values (value) == 0 ? Py_NewRef (Py_None) : NULL;
    } else
        result = PyObject_CallMethod (future, error == Py_None ? "set_result" : "set_exception",
                                      "O", error == Py_None ? value : error);
    Py_DECREF (future);
    return result;
}

static PyMethodDef finish_delivery_def = {"_finish_delivery", entry_finish_delivery, METH_NOARGS, NULL};

static PyObject *entry_deliver (PyObject *entry, PyObject *deliver)
{
    if (deliver == Py_None)
        Py_RETURN_NONE;
    PyObject *future = PyObject_GetAttr (entry, hp_future);
    if (!future)
        return NULL;
    int no_future = future == Py_None;
    Py_DECREF (future);
    if (no_future)
        Py_RETURN_NONE;
    if (PyObject_IsTrue (PyTuple_GET_ITEM (deliver, 2))) {
        if (close_message_values (PyTuple_GET_ITEM (deliver, 0)) < 0)
            return NULL;
        Py_RETURN_NONE;
    }
    PyObject *loop = PyObject_GetAttr (entry, hp_loop);
    PyObject *state = PyTuple_Pack (2, entry, deliver);
    if (!loop || !state) {
        Py_XDECREF (loop);
        Py_XDECREF (state);
        return NULL;
    }
    PyObject *thread_id = PyObject_GetAttr (loop, hp__thread_id);
    if (!thread_id && PyErr_ExceptionMatches (PyExc_AttributeError)) {
        PyErr_Clear ();
        thread_id = Py_NewRef (Py_None);
    }
    if (!thread_id) {
        Py_DECREF (loop);
        Py_DECREF (state);
        return NULL;
    }
    int same_thread = thread_id != Py_None
      && PyLong_AsUnsignedLong (thread_id) == PyThread_get_thread_ident ();
    Py_DECREF (thread_id);
    PyObject *result = NULL;
    if (same_thread)
        result = entry_finish_delivery (state, NULL);
    else {
        PyObject *finish = PyCFunction_New (&finish_delivery_def, state);
        if (finish) {
            result = hot_call (loop, hp_call_soon_threadsafe, "(O)", finish);
            Py_DECREF (finish);
            if (!result && PyErr_ExceptionMatches (PyExc_RuntimeError)) {
                PyErr_Clear ();
                result = close_message_values (PyTuple_GET_ITEM (deliver, 0)) == 0
                  ? Py_NewRef (Py_None) : NULL;
            }
        }
    }
    Py_DECREF (state);
    Py_DECREF (loop);
    if (!result)
        return NULL;
    Py_DECREF (result);
    Py_RETURN_NONE;
}

static PyMethodDef entry_settle_def = {"_settle_if_joined_locked", entry_settle, METH_NOARGS, NULL};
static PyMethodDef entry_deliver_def = {"_deliver", entry_deliver, METH_O, NULL};
static PyObject *entry_capture (PyObject *, PyObject *);
static PyMethodDef entry_capture_def = {"capture", entry_capture, METH_O, NULL};

static int install_entry_method (PyObject *type, PyMethodDef *method)
{
    PyObject *descriptor = PyDescr_NewMethod ((PyTypeObject *) type, method);
    if (!descriptor)
        return -1;
    int rc = PyObject_SetAttrString (type, method->ml_name, descriptor);
    Py_DECREF (descriptor);
    return rc;
}

static PyObject *py_install_entry_methods (PyObject *self, PyObject *args)
{
    PyObject *base, *send, *request;
    (void) self;
    if (!PyArg_ParseTuple (args, "O!O!O!", &PyType_Type, &base,
                          &PyType_Type, &send, &PyType_Type, &request))
        return NULL;
    if (install_entry_method (base, &entry_init_def) < 0
        || install_entry_method (base, &entry_settle_def) < 0
        || install_entry_method (base, &entry_deliver_def) < 0
        || install_entry_method (base, &entry_capture_def) < 0
        || install_entry_method (send, &send_init_def) < 0
        || install_entry_method (send, &entry_clone_def) < 0
        || install_entry_method (request, &entry_clone_def) < 0
        || install_entry_method (send, &send_succeed_def) < 0)
        return NULL;
    for (PyGetSetDef *property = entry_properties; property->name; ++property) {
        PyObject *descriptor = PyDescr_NewGetSet ((PyTypeObject *) base, property);
        if (!descriptor)
            return NULL;
        int rc = PyObject_SetAttrString (base, property->name, descriptor);
        Py_DECREF (descriptor);
        if (rc < 0)
            return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *receive_into (PyObject *args, int mode)
{
    unsigned long long handle;
    int flags;
    PyObject *out, *message_type, *routing_type;
    PyObject *socket = Py_None, *token_factory = Py_None;
    if (!PyArg_ParseTuple (args, "KiOOO|OO", &handle, &flags, &out,
                          &message_type, &routing_type, &socket, &token_factory))
        return NULL;
    PyObject *recv_args = Py_BuildValue ("Ki", handle, flags);
    if (!recv_args)
        return NULL;
    PyObject *received = mode == 1 ? py_router_recv_owner (NULL, recv_args)
      : mode == 2 ? py_subscribe_owner (NULL, recv_args) : py_recv_owner (NULL, recv_args);
    Py_DECREF (recv_args);
    if (!received || received == Py_False)
        return received;
    int rc = (int) PyLong_AsLong (PyTuple_GET_ITEM (received, 0));
    int err = (int) PyLong_AsLong (PyTuple_GET_ITEM (received, 1));
    if (rc != ZLINK_RECV_OK) {
        PyObject *module = PyImport_ImportModule ("zlink.contracts.errors.errors");
        PyObject *error = module ? hot_call (module, hp_RecvError, "ii", rc, err) : NULL;
        Py_XDECREF (module);
        Py_DECREF (received);
        if (error) {
            PyErr_SetObject ((PyObject *) Py_TYPE (error), error);
            Py_DECREF (error);
        }
        return NULL;
    }
    PyObject *owner = PyTuple_GET_ITEM (received, mode == 0 ? 3 : 4);
    PyObject *rid = PyTuple_GET_ITEM (received, 2);
    PyObject *routing = rid == Py_None ? Py_NewRef (Py_None)
      : hot_call (routing_type, hp_from_, "(O)", rid);
    PyObject *parts = NULL, *token = Py_NewRef (Py_None);
    PyObject *build_args = routing ? PyTuple_Pack (2, owner, message_type) : NULL;
    if (build_args) {
        parts = py_build_received_parts (NULL, build_args);
        Py_DECREF (build_args);
    }
    if (!routing || !parts)
        goto fail;
    if (mode == 1 && PyObject_IsTrue (PyTuple_GET_ITEM (received, 3))) {
        PyObject *token_owner = PyObject_GetAttr (socket, hp__reply_owner);
        Py_CLEAR (token);
        if (token_owner) {
            token = PyObject_CallFunctionObjArgs (token_factory, token_owner,
                                                  PyTuple_GET_ITEM (received, 3), NULL);
            Py_DECREF (token_owner);
        }
        if (!token)
            goto fail;
    }
    PyObject *old_owner = PyObject_GetAttr (out, hp__owner);
    if (!old_owner)
        goto fail;
    PyObject *closed = old_owner == Py_None ? Py_NewRef (Py_None)
      : hot_call (old_owner, hp_close, NULL);
    Py_DECREF (old_owner);
    if (!closed)
        goto fail;
    Py_DECREF (closed);
    if (PyObject_SetAttr (out, hp__owner, owner) < 0
        || PyObject_SetAttr (out, hp_parts, parts) < 0
        || PyObject_SetAttr (out, hp_routing_id, routing) < 0)
        goto fail;
    if (mode == 2) {
        PyObject *empty = PyUnicode_FromString ("");
        int set = empty ? PyObject_SetAttr (out, hp__topic, empty) : -1;
        Py_XDECREF (empty);
        if (set < 0 || PyObject_SetAttr (out, hp__topic_raw, PyTuple_GET_ITEM (received, 3)) < 0)
            goto fail;
    } else if (PyObject_SetAttr (out, hp_reply_token, token) < 0
               || PyObject_SetAttr (out, hp__router_socket, socket) < 0)
        goto fail;
    Py_DECREF (routing);
    Py_DECREF (token);
    Py_DECREF (parts);
    Py_DECREF (received);
    Py_RETURN_TRUE;
fail:;
    PyObject *error_type, *value, *tb;
    PyErr_Fetch (&error_type, &value, &tb);
    PyObject *released = hot_call (owner, hp_close, NULL);
    Py_XDECREF (released);
    Py_XDECREF (routing);
    Py_XDECREF (token);
    Py_XDECREF (parts);
    Py_DECREF (received);
    PyErr_Restore (error_type, value, tb);
    return NULL;
}

static PyObject *py_recv_into (PyObject *self, PyObject *args)
{
    (void) self;
    return receive_into (args, 0);
}

static PyObject *py_router_recv_into (PyObject *self, PyObject *args)
{
    (void) self;
    return receive_into (args, 1);
}

static PyObject *py_subscribe_into (PyObject *self, PyObject *args)
{
    (void) self;
    return receive_into (args, 2);
}

static PyObject *owner_drain (PyObject *owner, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"caller", NULL};
    PyObject *caller = Py_None;
    if (!PyArg_ParseTupleAndKeywords (args, kwargs, "|O", keywords, &caller))
        return NULL;
    PyObject *module = PyImport_ImportModule ("zlink._runtime.messaging.routed_async");
    PyObject *native = module ? hot_call (module, hp_lib, NULL) : NULL;
    PyObject *completion_type = module ? PyObject_GetAttr (module, hp_ZlinkCompletion) : NULL;
    PyObject *ctypes = module ? PyObject_GetAttr (module, hp_ctypes) : NULL;
    PyObject *receive = native ? PyObject_GetAttr (native, hp_zlink_completion_recv) : NULL;
    PyObject *close = native ? PyObject_GetAttr (native, hp_zlink_completion_close) : NULL;
    PyObject *byref = ctypes ? PyObject_GetAttr (ctypes, hp_byref) : NULL;
    PyObject *send_type = module ? PyObject_GetAttr (module, hp__SendEntry) : NULL;
    PyObject *request_type = module ? PyObject_GetAttr (module, hp__RequestEntry) : NULL;
    PyObject *retries = PyList_New (0);
    PyObject *flags = PyLong_FromLong (ZLINK_DONTWAIT);
    PyObject *completion = NULL, *pointer = NULL, *entry = NULL, *id = NULL;
    PyObject *guard = NULL, *result = NULL;
    int live = 0;
    Py_ssize_t processed = 0, requests = 0;
    if (!native || !completion_type || !receive || !close || !byref
        || !send_type || !request_type || !retries || !flags)
        goto done;
    while (1) {
        guard = lock_attribute (owner, hp__lock);
        if (!guard)
            goto done;
        PyObject *public_owner = PyObject_GetAttr (owner, hp__public_owner);
        int authorized = public_owner && (public_owner == Py_None || public_owner == caller);
        Py_XDECREF (public_owner);
        unlock_entry (guard);
        guard = NULL;
        if (!authorized)
            break;
        completion = PyObject_CallObject (completion_type, NULL);
        if (!completion)
            goto done;
        Py_buffer view;
        if (PyObject_GetBuffer (completion, &view, PyBUF_WRITABLE) < 0)
            goto done;
        if (view.len != sizeof (zlink_completion_t)) {
            PyBuffer_Release (&view);
            PyErr_SetString (PyExc_ValueError, "invalid completion storage");
            goto done;
        }
        ((zlink_completion_t *) view.buf)->struct_size = sizeof (zlink_completion_t);
        pointer = PyObject_CallFunctionObjArgs (byref, completion, NULL);
        PyObject *socket = PyObject_GetAttr (owner, hp__socket);
        PyObject *handle = socket ? PyObject_GetAttr (socket, hp__handle) : NULL;
        Py_XDECREF (socket);
        /* Use the binding's existing FFI callable: it releases the GIL during
         * native entry and keeps fault injection at the same transport seam. */
        PyObject *rc_obj = pointer && handle
          ? PyObject_CallFunctionObjArgs (receive, handle, pointer, flags, NULL) : NULL;
        Py_XDECREF (handle);
        if (!rc_obj) {
            PyBuffer_Release (&view);
            goto done;
        }
        long rc = PyLong_AsLong (rc_obj);
        Py_DECREF (rc_obj);
        zlink_completion_t record = *(zlink_completion_t *) view.buf;
        PyBuffer_Release (&view);
        if (rc == ZLINK_RECV_NO_DATA)
            break;
        if (rc != ZLINK_RECV_OK) {
            PyObject *err = hot_call (native, hp_zlink_errno, NULL);
            PyObject *error = err ? hot_call (module, hp_RecvError, "lO", rc, err) : NULL;
            Py_XDECREF (err);
            if (error) {
                PyErr_SetObject ((PyObject *) Py_TYPE (error), error);
                Py_DECREF (error);
            }
            goto done;
        }
        live = 1;
        id = PyLong_FromUnsignedLongLong (record.completion_id);
        PyObject *context = PyLong_FromVoidPtr (record.user_context);
        guard = lock_attribute (owner, hp__lock);
        if (!id || !context || !guard) {
            Py_XDECREF (context);
            goto done;
        }
        PyObject *entries = PyObject_GetAttr (owner, hp__entries_by_id);
        entry = entries ? hot_call (entries, hp_get, "(O)", id) : NULL;
        Py_XDECREF (entries);
        if (entry == Py_None) {
            Py_CLEAR (entry);
            entries = PyObject_GetAttr (owner, hp__entries);
            entry = entries ? hot_call (entries, hp_get, "(O)", context) : NULL;
            Py_XDECREF (entries);
        }
        Py_DECREF (context);
        unlock_entry (guard);
        guard = NULL;
        if (!entry)
            goto done;
        if (entry == Py_None) {
            live = 0;
            PyObject *closed = PyObject_CallFunctionObjArgs (close, pointer, NULL);
            if (!closed)
                goto done;
            Py_DECREF (closed);
        } else if (entry_truth (entry, hp_settled) && entry_truth (entry, hp_waiting_native)) {
            live = 0;
            PyObject *closed = PyObject_CallFunctionObjArgs (close, pointer, NULL);
            if (!closed)
                goto done;
            Py_DECREF (closed);
            PyObject *retired = hot_call (entry, hp_retire_native, "(O)", id);
            if (!retired)
                goto done;
            int retire = PyObject_IsTrue (retired);
            Py_DECREF (retired);
            if (retire) {
                guard = lock_attribute (owner, hp__lock);
                if (!guard)
                    goto done;
                PyObject *released = hot_call (owner, hp__release_native_wait_locked, "OO", entry, id);
                unlock_entry (guard);
                guard = NULL;
                if (!released)
                    goto done;
                Py_DECREF (released);
                PyObject *unregistered = hot_call (owner, hp__unregister, "(O)", entry);
                if (!unregistered)
                    goto done;
                Py_DECREF (unregistered);
            }
            if (record.kind == ZLINK_COMPLETION_REQUEST)
                ++requests;
        } else {
            if (record.kind == ZLINK_COMPLETION_WRITABLE
                && (PyObject_TypeCheck (entry, (PyTypeObject *) send_type)
                    || PyObject_TypeCheck (entry, (PyTypeObject *) request_type))) {
                live = 0;
                PyObject *retry = hot_call (owner, hp__capture_writable, "OO", entry, completion);
                if (!retry)
                    goto done;
                int should_retry = PyObject_IsTrue (retry);
                Py_DECREF (retry);
                if (should_retry && PyList_Append (retries, entry) < 0)
                    goto done;
            } else {
                guard = lock_attribute (owner, hp__lock);
                if (!guard)
                    goto done;
                PyObject *entry_id = PyObject_GetAttr (entry, hp_completion_id);
                int matches = entry_id ? PyObject_RichCompareBool (id, entry_id, Py_EQ) : -1;
                Py_XDECREF (entry_id);
                PyObject *released = matches == 1
                  ? hot_call (owner, hp__release_native_wait_locked, "OO", entry, id)
                  : matches == 0 ? Py_NewRef (Py_None) : NULL;
                unlock_entry (guard);
                guard = NULL;
                if (!released)
                    goto done;
                Py_DECREF (released);
                live = 0;
                PyObject *captured = hot_call (entry, hp_capture, "(O)", completion);
                if (!captured)
                    goto done;
                Py_DECREF (captured);
                if (record.kind == ZLINK_COMPLETION_REQUEST)
                    ++requests;
            }
            if (entry_truth (entry, hp_releasable)) {
                PyObject *unregistered = hot_call (owner, hp__unregister, "(O)", entry);
                if (!unregistered)
                    goto done;
                Py_DECREF (unregistered);
            }
        }
        ++processed;
        guard = lock_attribute (owner, hp__lock);
        if (!guard)
            goto done;
        PyObject *condition = PyObject_GetAttr (owner, hp__state_changed);
        PyObject *notified = condition ? hot_call (condition, hp_notify_all, NULL) : NULL;
        Py_XDECREF (condition);
        unlock_entry (guard);
        guard = NULL;
        if (!notified)
            goto done;
        Py_DECREF (notified);
        Py_CLEAR (completion);
        Py_CLEAR (pointer);
        Py_CLEAR (entry);
        Py_CLEAR (id);
    }
    if (PyErr_Occurred ())
        goto done;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE (retries); ++i) {
        PyObject *retried = hot_call (owner, hp__dispatch_retry, "(O)", PyList_GET_ITEM (retries, i));
        if (!retried)
            goto done;
        Py_DECREF (retried);
    }
    result = hot_call (module, hp__DrainResult, "nn", processed, requests);
done:
    if (guard)
        unlock_entry (guard);
    if (live) {
        PyObject *type, *value, *tb;
        PyErr_Fetch (&type, &value, &tb);
        PyObject *closed = PyObject_CallFunctionObjArgs (close, pointer, NULL);
        Py_XDECREF (closed);
        PyErr_Restore (type, value, tb);
    }
    Py_XDECREF (module);
    Py_XDECREF (native);
    Py_XDECREF (completion_type);
    Py_XDECREF (ctypes);
    Py_XDECREF (receive);
    Py_XDECREF (close);
    Py_XDECREF (byref);
    Py_XDECREF (send_type);
    Py_XDECREF (request_type);
    Py_XDECREF (retries);
    Py_XDECREF (flags);
    Py_XDECREF (completion);
    Py_XDECREF (pointer);
    Py_XDECREF (entry);
    Py_XDECREF (id);
    return result;
}

static PyMethodDef owner_drain_def = {"_drain", (PyCFunction) owner_drain, METH_VARARGS | METH_KEYWORDS, NULL};

static int owner_stopped (PyObject *owner, PyObject *entry)
{
    int shutdown = entry_truth (owner, hp__shutdown);
    return shutdown == 0 ? entry_truth (entry, hp_settled) : shutdown;
}

static PyObject *owner_attempt_send (PyObject *owner, PyObject *entry)
{
    PyObject *guard = lock_attribute (owner, hp__lock);
    PyObject *parts = NULL, *submitted = NULL, *result = NULL;
    if (!guard)
        return NULL;
    int stopped = owner_stopped (owner, entry);
    unlock_entry (guard);
    guard = NULL;
    if (stopped < 0)
        return NULL;
    if (stopped)
        Py_RETURN_NONE;
    parts = hot_call (entry, hp_clone_payload, NULL);
    if (!parts) {
        if (!PyErr_ExceptionMatches (PyExc_Exception))
            return NULL;
        PyObject *type, *error, *tb;
        PyErr_Fetch (&type, &error, &tb);
        PyErr_NormalizeException (&type, &error, &tb);
        guard = lock_attribute (owner, hp__lock);
        stopped = guard ? owner_stopped (owner, entry) : -1;
        if (guard)
            unlock_entry (guard);
        guard = NULL;
        if (stopped == 0) {
            PyObject *failed = hot_call (entry, hp_fail, "(O)", error);
            Py_XDECREF (failed);
            if (!PyErr_Occurred ()) {
                result = hot_call (owner, hp__unregister, "(O)", entry);
                Py_XDECREF (result);
            }
        }
        Py_XDECREF (type);
        Py_XDECREF (error);
        Py_XDECREF (tb);
        if (PyErr_Occurred ())
            return NULL;
        Py_RETURN_NONE;
    }
    if (parts == Py_None) {
        Py_DECREF (parts);
        Py_RETURN_NONE;
    }
    guard = lock_attribute (owner, hp__lock);
    if (!guard)
        goto done;
    stopped = owner_stopped (owner, entry);
    if (stopped) {
        close_storage_list (parts, 0);
        if (stopped > 0)
            result = Py_NewRef (Py_None);
        goto done;
    }
    PyObject *target = PyObject_GetAttr (entry, hp_target);
    if (!target) {
        close_storage_list (parts, 0);
        goto done;
    }
    submitted = hot_call (owner, hp__submit_parts, "OOiO", target, parts, ZLINK_DONTWAIT, entry);
    Py_DECREF (target);
    if (!submitted) {
        PyObject *type, *error, *tb;
        PyErr_Fetch (&type, &error, &tb);
        PyErr_NormalizeException (&type, &error, &tb);
        PyObject *failed = hot_call (entry, hp_fail, "(O)", error);
        Py_XDECREF (failed);
        if (!PyErr_Occurred ())
            result = hot_call (owner, hp__unregister, "(O)", entry);
        Py_XDECREF (type);
        Py_XDECREF (error);
        Py_XDECREF (tb);
        goto done;
    }
    int rc, err;
    unsigned long long completion_id;
    if (!PyArg_ParseTuple (submitted, "iiK", &rc, &err, &completion_id))
        goto done;
    if (completion_id) {
        PyObject *entries = PyObject_GetAttr (owner, hp__entries);
        PyObject *context = PyObject_GetAttr (entry, hp_context);
        int registered = entries && context ? PyObject_SetItem (entries, context, entry) : -1;
        Py_XDECREF (entries);
        Py_XDECREF (context);
        if (registered < 0)
            goto done;
        PyObject *waiting = hot_call (entry, hp_await_writable, "K", completion_id);
        if (!waiting)
            goto done;
        Py_DECREF (waiting);
        PyObject *tracked = hot_call (owner, hp__track_native_wait_locked, "(O)", entry);
        if (!tracked)
            goto done;
        Py_DECREF (tracked);
    }
    PyObject *error = NULL;
    if (rc == ZLINK_SUBMIT_OK && !completion_id) {
        PyObject *succeeded = hot_call (entry, hp_succeed_send, NULL);
        if (!succeeded)
            goto done;
        Py_DECREF (succeeded);
    } else if (rc == ZLINK_SUBMIT_OK
               || (rc == ZLINK_SUBMIT_BACKPRESSURED && (err != EAGAIN || !completion_id))) {
        PyObject *module = PyImport_ImportModule ("zlink.contracts.errors.errors");
        error = module ? hot_call (module, hp_SubmitError, "ii", ZLINK_SUBMIT_INTERNAL_ERROR, EPROTO) : NULL;
        Py_XDECREF (module);
        if (!error)
            goto done;
    } else if (rc != ZLINK_SUBMIT_BACKPRESSURED) {
        error = hot_call (owner, hp__submit_error, "ii", rc, err);
        if (!error)
            goto done;
    }
    if (error) {
        PyObject *failed = hot_call (entry, hp_fail, "(O)", error);
        Py_DECREF (error);
        if (!failed)
            goto done;
        Py_DECREF (failed);
    }
    int releasable = entry_truth (entry, hp_releasable);
    if (releasable < 0)
        goto done;
    if (releasable) {
        PyObject *entries = PyObject_GetAttr (owner, hp__entries);
        PyObject *context = PyObject_GetAttr (entry, hp_context);
        PyObject *found = entries && context ? hot_call (entries, hp_get, "(O)", context) : NULL;
        Py_XDECREF (entries);
        Py_XDECREF (context);
        if (found) {
            result = found == entry ? hot_call (owner, hp__unregister, "(O)", entry) : Py_NewRef (Py_None);
            Py_DECREF (found);
        }
    } else {
        PyObject *loop = PyObject_GetAttr (entry, hp_loop);
        result = loop ? hot_call (owner, hp__schedule_runtime_owner_locked, "(O)", loop) : NULL;
        Py_XDECREF (loop);
    }
done:
    if (guard)
        unlock_entry (guard);
    Py_XDECREF (parts);
    Py_XDECREF (submitted);
    return result;
}

static PyMethodDef owner_attempt_send_def = {"_attempt_send", owner_attempt_send, METH_O, NULL};

static PyObject *materialize_payload (PyObject *payload)
{
    PyObject *materializer = PyImport_ImportModule ("zlink._runtime.messaging.native_parts");
    if (!materializer)
        return NULL;
    PyObject *dict = PyModule_GetDict (materializer);
    PyObject *storage_type = PyDict_GetItemString (dict, "ZlinkMsg");
    PyObject *message_type = PyDict_GetItemString (dict, "Message");
    PyObject *received_type = PyDict_GetItemString (dict, "ReceivedMessage");
    PyObject *as_view = PyDict_GetItemString (dict, "_as_bytes_view");
    PyObject *materialize_args = PyTuple_Pack (5, payload, storage_type, message_type, received_type, as_view);
    PyObject *parts = materialize_args ? py_materialize_parts (NULL, materialize_args) : NULL;
    Py_XDECREF (materialize_args);
    Py_DECREF (materializer);
    return parts;
}

static PyObject *py_start_send (PyObject *self, PyObject *args)
{
    PyObject *owner, *target, *payload, *loop;
    (void) self;
    if (!PyArg_ParseTuple (args, "OOOO", &owner, &target, &payload, &loop))
        return NULL;
    PyObject *parts = materialize_payload (payload);
    if (!parts)
        return NULL;
    PyObject *module = PyImport_ImportModule ("zlink._runtime.messaging.routed_async");
    PyObject *type = module ? PyObject_GetAttr (module, hp__SendEntry) : NULL;
    Py_XDECREF (module);
    PyObject *condition = PyObject_GetAttr (owner, hp__state_changed);
    PyObject *entry_args = PyTuple_Pack (3, loop, target, parts);
    PyObject *entry_kwargs = condition ? Py_BuildValue ("{s:O}", "condition", condition) : NULL;
    PyObject *entry = type && entry_args && entry_kwargs ? PyObject_Call (type, entry_args, entry_kwargs) : NULL;
    Py_XDECREF (type);
    Py_XDECREF (condition);
    Py_XDECREF (entry_args);
    Py_XDECREF (entry_kwargs);
    if (!entry) {
        close_storage_list (parts, 0);
        Py_DECREF (parts);
        return NULL;
    }
    Py_DECREF (parts);
    PyObject *guard = lock_attribute (owner, hp__lock);
    if (!guard) {
        PyObject *released = hot_call (entry, hp__release_payload, NULL);
        Py_XDECREF (released);
        Py_DECREF (entry);
        return NULL;
    }
    int shutdown = entry_truth (owner, hp__shutdown);
    PyObject *submitted = NULL;
    if (shutdown > 0) {
        PyObject *released = hot_call (entry, hp__release_payload, NULL);
        Py_XDECREF (released);
#ifdef ESHUTDOWN
        int err = ESHUTDOWN;
#else
        int err = ECANCELED;
#endif
        if (!PyErr_Occurred ())
            raise_submit_error (ZLINK_SUBMIT_INVALID_STATE, err);
    } else if (shutdown == 0)
        submitted = hot_call (owner, hp__attempt_send, "(O)", entry);
    unlock_entry (guard);
    if (!submitted) {
        Py_DECREF (entry);
        return NULL;
    }
    Py_DECREF (submitted);
    return entry;
}

static int call_entry_method (PyObject *object, const char *name, PyObject *argument)
{
    PyObject *result = argument ? PyObject_CallMethod (object, name, "(O)", argument)
                               : PyObject_CallMethod (object, name, NULL);
    if (!result)
        return -1;
    Py_DECREF (result);
    return 0;
}

static PyObject *py_start_request (PyObject *self, PyObject *args)
{
    PyObject *owner, *target, *payload, *loop, *timeout;
    (void) self;
    if (!PyArg_ParseTuple (args, "OOOOO", &owner, &target, &payload, &loop, &timeout))
        return NULL;
    PyObject *parts = materialize_payload (payload);
    if (!parts)
        return NULL;
    PyObject *module = PyImport_ImportModule ("zlink._runtime.messaging.routed_async");
    PyObject *entry = module ? hot_call (module, hp__RequestEntry, "OO", loop, timeout) : NULL;
    Py_XDECREF (module);
    if (!entry) {
        close_storage_list (parts, 0);
        Py_DECREF (parts);
        return NULL;
    }
    PyObject *guard = lock_attribute (owner, hp__lock);
    PyObject *submitted = NULL, *result = NULL;
    if (!guard) {
        close_storage_list (parts, 0);
        goto done;
    }
    int shutdown = entry_truth (owner, hp__shutdown);
    if (shutdown != 0) {
        close_storage_list (parts, 0);
        if (shutdown > 0) {
#ifdef ESHUTDOWN
            int err = ESHUTDOWN;
#else
            int err = ECANCELED;
#endif
            raise_submit_error (ZLINK_SUBMIT_INVALID_STATE, err);
        }
        goto done;
    }
    PyObject *entries = PyObject_GetAttr (owner, hp__entries);
    PyObject *context = PyObject_GetAttr (entry, hp_context);
    int registered = entries && context ? PyObject_SetItem (entries, context, entry) : -1;
    Py_XDECREF (entries);
    Py_XDECREF (context);
    if (registered < 0) {
        close_storage_list (parts, 0);
        goto done;
    }
    submitted = hot_call (owner, hp__submit_parts, "OOiOO", target, parts, ZLINK_DONTWAIT, entry, timeout);
    if (!submitted) {
        PyObject *type, *error, *tb;
        PyErr_Fetch (&type, &error, &tb);
        call_entry_method (entry, "fail_submit", NULL);
        call_entry_method (owner, "_unregister", entry);
        PyErr_Restore (type, error, tb);
        goto done;
    }
    int rc, err;
    unsigned long long completion_id;
    if (!PyArg_ParseTuple (submitted, "iiK", &rc, &err, &completion_id))
        goto done;
    if (rc == ZLINK_SUBMIT_OK) {
        if (!completion_id) {
            if (call_entry_method (entry, "fail_submit", NULL) < 0
                || call_entry_method (owner, "_unregister", entry) < 0)
                goto done;
            raise_submit_error (ZLINK_SUBMIT_INTERNAL_ERROR, EPROTO);
            goto done;
        }
        PyObject *finish = PyObject_GetAttr (owner, hp__finish_request_submit);
        PyObject *finish_args = Py_BuildValue ("OK", entry, completion_id);
        PyObject *finish_kwargs = Py_BuildValue ("{s:O}", "schedule", Py_True);
        PyObject *finished = finish && finish_args && finish_kwargs
          ? PyObject_Call (finish, finish_args, finish_kwargs) : NULL;
        Py_XDECREF (finish);
        Py_XDECREF (finish_args);
        Py_XDECREF (finish_kwargs);
        if (!finished)
            goto done;
        Py_DECREF (finished);
    } else {
        if (completion_id) {
            PyObject *id = PyLong_FromUnsignedLongLong (completion_id);
            int awaited = id ? call_entry_method (entry, "await_writable", id) : -1;
            Py_XDECREF (id);
            if (awaited < 0 || call_entry_method (owner, "_track_native_wait_locked", entry) < 0)
                goto done;
        }
        PyObject *failure = NULL;
        if (rc == ZLINK_SUBMIT_BACKPRESSURED && err == EAGAIN && completion_id) {
            PyObject *retained = hot_call (entry, hp_retain_retry, "OO", target, payload);
            if (!retained) {
                PyObject *type, *tb;
                PyErr_Fetch (&type, &failure, &tb);
                PyErr_NormalizeException (&type, &failure, &tb);
                Py_XDECREF (type);
                Py_XDECREF (tb);
            }
            Py_XDECREF (retained);
        } else if (completion_id)
            failure = hot_call (owner, hp__submit_error, "ii", rc, err);
        else {
            if (call_entry_method (entry, "fail_submit", NULL) < 0
                || call_entry_method (owner, "_unregister", entry) < 0)
                goto done;
            raise_submit_error (rc, err);
            goto done;
        }
        if (PyErr_Occurred ()) {
            Py_XDECREF (failure);
            goto done;
        }
        if (failure) {
            int failed = call_entry_method (entry, "fail", failure);
            Py_DECREF (failure);
            if (failed < 0)
                goto done;
        }
        if (rc == ZLINK_SUBMIT_BACKPRESSURED && entry_truth (entry, hp_releasable)) {
            if (call_entry_method (owner, "_unregister", entry) < 0)
                goto done;
        } else if (call_entry_method (owner, "_schedule_runtime_owner_locked", loop) < 0)
            goto done;
    }
    result = Py_NewRef (entry);
done:
    if (guard)
        unlock_entry (guard);
    Py_XDECREF (submitted);
    Py_DECREF (parts);
    Py_DECREF (entry);
    return result;
}

static PyObject *py_install_owner_methods (PyObject *self, PyObject *type)
{
    (void) self;
    if (!PyType_Check (type)) {
        PyErr_SetString (PyExc_TypeError, "completion owner must be a type");
        return NULL;
    }
    if (install_entry_method (type, &owner_drain_def) < 0
        || install_entry_method (type, &owner_attempt_send_def) < 0)
        return NULL;
    Py_RETURN_NONE;
}

static PyObject *py_completion_messages (PyObject *self, PyObject *args)
{
    Py_buffer view;
    PyTypeObject *message_type;
    PyObject *storage_type;
    (void) self;
    if (!PyArg_ParseTuple (args, "y*O!O", &view, &PyType_Type, &message_type, &storage_type))
        return NULL;
    if (view.len != sizeof (zlink_completion_t)) {
        PyBuffer_Release (&view);
        PyErr_SetString (PyExc_ValueError, "invalid completion storage");
        return NULL;
    }
    zlink_completion_t *completion = (zlink_completion_t *) view.buf;
    PyObject *messages = PyList_New (0);
    if (!messages) {
        PyBuffer_Release (&view);
        return NULL;
    }
    for (size_t i = 0; i < completion->reply_part_count; ++i) {
        PyObject *message = message_type->tp_alloc (message_type, 0);
        PyObject *storage = PyObject_CallObject (storage_type, NULL);
        if (!message || !storage) {
            Py_XDECREF (message);
            Py_XDECREF (storage);
            goto fail;
        }
        Py_buffer dst;
        if (PyObject_GetBuffer (storage, &dst, PyBUF_WRITABLE) < 0) {
            Py_DECREF (message);
            Py_DECREF (storage);
            goto fail;
        }
        int err = 0;
        int rc = dst.len == sizeof (zlink_msg_t)
          ? init_cloned_message ((zlink_msg_t *) dst.buf, &completion->reply_parts[i], &err) : -1;
        if (rc != 0) {
            PyBuffer_Release (&dst);
            Py_DECREF (message);
            Py_DECREF (storage);
            if (!PyErr_Occurred ())
                PyErr_SetString (PyExc_RuntimeError, "cannot clone completion message");
            goto fail;
        }
        if (PyObject_SetAttr (message, hp__msg, storage) < 0
            || PyObject_SetAttr (message, hp__valid, Py_True) < 0
            || PyObject_SetAttr (message, hp__keepalive, Py_None) < 0
            || PyList_Append (messages, message) < 0) {
            zlink_msg_close ((zlink_msg_t *) dst.buf);
            PyBuffer_Release (&dst);
            Py_DECREF (message);
            Py_DECREF (storage);
            goto fail;
        }
        PyBuffer_Release (&dst);
        Py_DECREF (message);
        Py_DECREF (storage);
    }
    PyBuffer_Release (&view);
    return messages;
fail:;
    PyObject *type, *value, *tb;
    PyErr_Fetch (&type, &value, &tb);
    int cleanup = close_message_values (messages);
    Py_DECREF (messages);
    PyBuffer_Release (&view);
    if (cleanup == 0)
        PyErr_Restore (type, value, tb);
    else {
        Py_XDECREF (type);
        Py_XDECREF (value);
        Py_XDECREF (tb);
    }
    return NULL;
}

static PyObject *py_message_data (PyObject *self, PyObject *args)
{
    PyObject *message, *byte_type;
    (void) self;
    if (!PyArg_ParseTuple (args, "OO", &message, &byte_type))
        return NULL;
    PyObject *storage = PyObject_GetAttr (message, hp__msg);
    if (!storage)
        return NULL;
    Py_buffer native;
    int acquired = PyObject_GetBuffer (storage, &native, PyBUF_SIMPLE);
    Py_DECREF (storage);
    if (acquired < 0)
        return NULL;
    if (native.len != sizeof (zlink_msg_t)) {
        PyBuffer_Release (&native);
        PyErr_SetString (PyExc_ValueError, "invalid native message storage");
        return NULL;
    }
    void *data = zlink_msg_data ((zlink_msg_t *) native.buf);
    size_t size = zlink_msg_size ((zlink_msg_t *) native.buf);
    PyBuffer_Release (&native);
    if (!data || !size) {
        PyObject *empty = PyBytes_FromStringAndSize ("", 0);
        if (!empty)
            return NULL;
        PyObject *view = PyMemoryView_FromObject (empty);
        Py_DECREF (empty);
        return view;
    }
    PyObject *cached = PyObject_GetAttr (message, hp__data_view_cache);
    if (!cached && PyErr_ExceptionMatches (PyExc_AttributeError))
        PyErr_Clear ();
    if (PyErr_Occurred ())
        return NULL;
    if (cached && cached != Py_None
        && PyLong_AsVoidPtr (PyTuple_GET_ITEM (cached, 0)) == data
        && PyLong_AsSize_t (PyTuple_GET_ITEM (cached, 1)) == size) {
        PyObject *view = Py_NewRef (PyTuple_GET_ITEM (cached, 2));
        Py_DECREF (cached);
        return view;
    }
    Py_XDECREF (cached);
    PyObject *length = PyLong_FromSize_t (size);
    PyObject *address = PyLong_FromVoidPtr (data);
    PyObject *array_type = length ? PyNumber_Multiply (byte_type, length) : NULL;
    PyObject *array = array_type && address
      ? PyObject_CallMethod (array_type, "from_address", "(O)", address) : NULL;
    PyObject *raw_view = array ? PyMemoryView_FromObject (array) : NULL;
    PyObject *view = raw_view ? PyObject_CallMethod (raw_view, "cast", "s", "B") : NULL;
    Py_XDECREF (array_type);
    Py_XDECREF (array);
    Py_XDECREF (raw_view);
    PyObject *cache = view && address && length ? PyTuple_Pack (3, address, length, view) : NULL;
    Py_XDECREF (address);
    Py_XDECREF (length);
    if (!cache || PyObject_SetAttr (message, hp__data_view_cache, cache) < 0)
        Py_CLEAR (view);
    Py_XDECREF (cache);
    return view;
}

static PyObject *py_msg_close (PyObject *self, PyObject *storage)
{
    Py_buffer native;
    (void) self;
    if (PyObject_GetBuffer (storage, &native, PyBUF_WRITABLE) < 0)
        return NULL;
    if (native.len != sizeof (zlink_msg_t)) {
        PyBuffer_Release (&native);
        PyErr_SetString (PyExc_ValueError, "invalid native message storage");
        return NULL;
    }
    int rc = zlink_msg_close ((zlink_msg_t *) native.buf);
    int err = rc == 0 ? 0 : zlink_errno ();
    PyBuffer_Release (&native);
    return result_tuple (rc, err);
}

static PyObject *request_failure (PyObject *module, int result, int err)
{
    return PyObject_CallMethod (module, "RequestError", "ii", result, err);
}

static PyObject *entry_capture (PyObject *entry, PyObject *completion)
{
    PyObject *module = PyImport_ImportModule ("zlink._runtime.messaging.routed_async");
    if (!module)
        return NULL;
    Py_buffer view;
    if (PyObject_GetBuffer (completion, &view, PyBUF_SIMPLE) < 0) {
        Py_DECREF (module);
        return NULL;
    }
    if (view.len != sizeof (zlink_completion_t)) {
        PyBuffer_Release (&view);
        Py_DECREF (module);
        PyErr_SetString (PyExc_ValueError, "invalid completion storage");
        return NULL;
    }
    zlink_completion_t record = *(zlink_completion_t *) view.buf;
    PyBuffer_Release (&view);
    PyObject *condition = lock_entry (entry);
    PyObject *value = Py_NewRef (Py_None), *failure = Py_NewRef (Py_None);
    PyObject *deliver = NULL, *result = NULL, *discarded = NULL;
    if (!condition)
        goto close_completion;
    int captured = entry_truth (entry, hp__captured);
    int published = entry_truth (entry, hp__published);
    PyObject *entry_id = PyObject_GetAttr (entry, hp_completion_id);
    int matches = entry_id && PyLong_AsUnsignedLongLong (entry_id) == record.completion_id;
    Py_XDECREF (entry_id);
    unlock_entry (condition);
    condition = NULL;
    if (PyErr_Occurred ())
        goto close_completion;
    if (!captured) {
        if ((published && (!matches || record.user_context != (void *) entry))
            || record.kind != ZLINK_COMPLETION_REQUEST) {
            Py_SETREF (failure, request_failure (module, ZLINK_REQUEST_INTERNAL_ERROR, EPROTO));
        } else if (record.request_result != ZLINK_REQUEST_OK) {
            PyObject *err = PyObject_CallMethod (module, "_request_errno", "i", record.request_result);
            if (err) {
                int code = (int) PyLong_AsLong (err);
                Py_DECREF (err);
                Py_SETREF (failure, request_failure (module, record.request_result, code));
            }
        } else {
            PyObject *extension = PyObject_GetAttrString (module, "_native_extension");
            PyObject *message_type = PyObject_GetAttrString (module, "Message");
            PyObject *storage_type = PyObject_GetAttrString (module, "ZlinkMsg");
            PyObject *messages = extension && message_type && storage_type
              ? PyObject_CallMethod (extension, "completion_messages", "OOO", completion, message_type, storage_type) : NULL;
            Py_XDECREF (extension);
            Py_XDECREF (message_type);
            Py_XDECREF (storage_type);
            if (messages)
                Py_SETREF (value, messages);
            else {
                /* Preserve capture's typed INTERNAL_ERROR boundary, including
                 * cleanup failures from cloning a Core-owned reply array. */
                PyErr_Clear ();
                Py_SETREF (failure, request_failure (module, ZLINK_REQUEST_INTERNAL_ERROR, EIO));
            }
        }
    }
close_completion:;
    /* Keep the same close seam as drain and the extension-free owner. */
    PyObject *error_type, *error_value, *error_tb;
    PyErr_Fetch (&error_type, &error_value, &error_tb);
    PyObject *native = hot_call (module, hp_lib, NULL);
    PyObject *ctypes = PyObject_GetAttr (module, hp_ctypes);
    PyObject *pointer = ctypes ? hot_call (ctypes, hp_byref, "(O)", completion) : NULL;
    PyObject *closed = native && pointer ? hot_call (native, hp_zlink_completion_close, "(O)", pointer) : NULL;
    Py_XDECREF (native);
    Py_XDECREF (ctypes);
    Py_XDECREF (pointer);
    if (closed && error_type)
        PyErr_Restore (error_type, error_value, error_tb);
    else {
        Py_XDECREF (error_type);
        Py_XDECREF (error_value);
        Py_XDECREF (error_tb);
    }
    if (!closed || PyErr_Occurred ()) {
        Py_XDECREF (closed);
        goto done;
    }
    Py_DECREF (closed);
    condition = lock_entry (entry);
    if (!condition)
        goto done;
    if (entry_truth (entry, hp__captured)) {
        result = close_message_values (value) == 0 ? Py_NewRef (Py_None) : NULL;
        goto done;
    }
    PyObject *id = PyLong_FromUnsignedLongLong (record.completion_id);
    PyObject *context = PyLong_FromVoidPtr (record.user_context);
    if (!id || !context || !failure
        || PyObject_SetAttr (entry, hp__value, value) < 0
        || PyObject_SetAttr (entry, hp__captured_completion_id, id) < 0
        || PyObject_SetAttr (entry, hp__captured_context, context) < 0) {
        Py_XDECREF (id);
        Py_XDECREF (context);
        goto done;
    }
    Py_DECREF (id);
    Py_DECREF (context);
    published = entry_truth (entry, hp__published);
    if (published) {
        entry_id = PyObject_GetAttr (entry, hp_completion_id);
        matches = entry_id && PyLong_AsUnsignedLongLong (entry_id) == record.completion_id;
        Py_XDECREF (entry_id);
        if (!matches || record.user_context != (void *) entry) {
            Py_XSETREF (failure, request_failure (module, ZLINK_REQUEST_INTERNAL_ERROR, EPROTO));
            if (!failure || PyObject_SetAttr (entry, hp__value, Py_None) < 0)
                goto done;
            discarded = Py_NewRef (value);
        }
        if (matches && PyObject_SetAttr (entry, hp__native_wait, Py_False) < 0)
            goto done;
    }
    if (PyObject_SetAttr (entry, hp__error, failure) < 0
        || PyObject_SetAttr (entry, hp__captured, Py_True) < 0)
        goto done;
    deliver = entry_settle (entry, NULL);
    unlock_entry (condition);
    condition = NULL;
    int cleanup = close_message_values (discarded);
    Py_CLEAR (discarded);
    if (cleanup < 0)
        goto done;
    if (deliver)
        result = entry_deliver (entry, deliver);
done:
    if (condition)
        unlock_entry (condition);
    if (close_message_values (discarded) < 0)
        Py_CLEAR (result);
    Py_XDECREF (discarded);
    Py_XDECREF (deliver);
    Py_XDECREF (value);
    Py_XDECREF (failure);
    Py_DECREF (module);
    return result;
}
