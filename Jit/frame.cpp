// Copyright (c) Facebook, Inc. and its affiliates. (http://www.facebook.com)
#include "Jit/frame.h"

#include "Python.h"
#include "internal/pycore_pystate.h"
#include "internal/pycore_shadow_frame.h"

#include "Jit/codegen/gen_asm.h"
#include "Jit/log.h"
#include "Jit/runtime.h"
#include "Jit/util.h"

#include <optional>
#include <unordered_set>

static bool is_shadow_frame_for_gen(_PyShadowFrame* shadow_frame) {
  // TODO(bsimmers): This condition will need to change when we support eager
  // coroutine execution in the JIT, since there is no PyGenObject* for the
  // frame while executing eagerly (but isGen() will still return true).
  bool is_jit_gen = _PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_CODE_RT &&
      static_cast<jit::CodeRuntime*>(_PyShadowFrame_GetPtr(shadow_frame))
          ->isGen();

  // Note this may be JIT or interpreted.
  bool is_gen_with_frame =
      _PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_PYFRAME &&
      _PyShadowFrame_GetPyFrame(shadow_frame)->f_gen != nullptr;
  return is_jit_gen || is_gen_with_frame;
}

namespace jit {

namespace {

PyObject* getModuleName(_PyShadowFrame* shadow_frame) {
  PyObject* globals;
  PyObject* result;
  switch (_PyShadowFrame_GetPtrKind(shadow_frame)) {
    case PYSF_PYFRAME: {
      PyFrameObject* pyframe =
          static_cast<PyFrameObject*>(_PyShadowFrame_GetPtr(shadow_frame));
      globals = pyframe->f_globals;
      JIT_DCHECK(
          globals != nullptr, "Python frame (%p) has NULL globals", pyframe);
      result = PyDict_GetItemString(globals, "__name__");
      break;
    }
    case PYSF_CODE_RT: {
      jit::CodeRuntime* code_rt =
          static_cast<CodeRuntime*>(_PyShadowFrame_GetPtr(shadow_frame));
      globals = code_rt->GetGlobals();
      JIT_DCHECK(
          globals != nullptr,
          "JIT Runtime frame (%p) has NULL globals",
          code_rt);
      result = PyDict_GetItemString(globals, "__name__");
      break;
    }
    case PYSF_PYCODE: {
      // TODO(emacs): Implement this once the inliner is out in prod
      result = PyUnicode_FromStringAndSize("<inlined>", 9);
    }
    default: {
      JIT_CHECK(false, "unknown ptr kind");
    }
  }
  Py_XINCREF(result);
  return result;
}

Ref<PyFrameObject> createPyFrame(
    PyThreadState* tstate,
    _PyShadowFrame* shadow_frame) {
  JIT_CHECK(
      _PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_CODE_RT,
      "Unexpected shadow frame type");
  auto code_rt = static_cast<CodeRuntime*>(_PyShadowFrame_GetPtr(shadow_frame));
  Ref<PyFrameObject> py_frame = Ref<PyFrameObject>::steal(
      PyFrame_New(tstate, code_rt->GetCode(), code_rt->GetGlobals(), nullptr));
  JIT_CHECK(py_frame != nullptr, "failed allocating frame");
  // PyFrame_New links the frame into the thread stack.
  Py_CLEAR(py_frame->f_back);
  py_frame->f_executing = 1;
  if (code_rt->isGen()) {
    // Transfer ownership of the new reference to frame to the generator
    // epilogue.  It handles detecting and unlinking the frame if the generator
    // is present in the `data` field of the shadow frame.
    //
    // A generator may be resumed multiple times. If a frame is materialized in
    // one activation, all subsequent activations must link/unlink the
    // materialized frame on function entry/exit. There's no active signal in
    // these cases, so we're forced to check for the presence of the
    // frame. Linking is handled by `_PyJIT_GenSend`, while unlinking is
    // handled by either the epilogue or, in the event that the generator
    // deopts, the interpreter loop. In the future we may refactor things so
    // that `_PyJIT_GenSend` handles both linking and unlinking.
    PyGenObject* gen = _PyShadowFrame_GetGen(shadow_frame);
    // f_gen is borrowed
    py_frame->f_gen = reinterpret_cast<PyObject*>(gen);
    // gi_frame is owned
    gen->gi_frame = py_frame.get();
    Py_INCREF(py_frame);
  }
  shadow_frame->data = _PyShadowFrame_MakeData(py_frame, PYSF_PYFRAME);
  return py_frame;
}

void insertPyFrameBefore(
    PyThreadState* tstate,
    BorrowedRef<PyFrameObject> frame,
    BorrowedRef<PyFrameObject> cursor) {
  if (cursor == nullptr) {
    // Insert frame at the top of the call stack
    Py_XINCREF(tstate->frame);
    frame->f_back = tstate->frame;
    // ThreadState holds a borrowed reference
    tstate->frame = frame;
    return;
  }
  // Insert frame immediately before cursor in the call stack
  // New frame steals reference for cursor->f_back
  frame->f_back = cursor->f_back;
  // Need to create a new reference for cursor to the newly created frame.
  Py_INCREF(frame);
  cursor->f_back = frame;
}

// Get the PyFrameObject for shadow_frame or create and insert one before
// cursor if no PyFrameObject exists.
BorrowedRef<PyFrameObject> materializePyFrame(
    PyThreadState* tstate,
    _PyShadowFrame* shadow_frame,
    PyFrameObject* cursor) {
  if (_PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_PYFRAME) {
    return _PyShadowFrame_GetPyFrame(shadow_frame);
  }
  // Python frame doesn't exist yet, create it and insert it into the
  // call stack.
  Ref<PyFrameObject> frame = createPyFrame(tstate, shadow_frame);
  insertPyFrameBefore(tstate, frame, cursor);
  // Ownership of the new reference is transferred to whomever unlinks the
  // frame (either the JIT epilogue or the interpreter loop).
  return frame.release();
}

// Find a shadow frame in the call stack. If the frame was found, returns the
// last Python frame seen during the search, or nullptr if there was none.
std::optional<PyFrameObject*> findInnermostPyFrameForShadowFrame(
    PyThreadState* tstate,
    _PyShadowFrame* needle) {
  PyFrameObject* prev_py_frame = nullptr;
  _PyShadowFrame* shadow_frame = tstate->shadow_frame;
  while (shadow_frame) {
    if (_PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_PYFRAME) {
      prev_py_frame = _PyShadowFrame_GetPyFrame(shadow_frame);
    } else if (shadow_frame == needle) {
      return prev_py_frame;
    }
    shadow_frame = shadow_frame->prev;
  }
  return {};
}

} // namespace

Ref<PyFrameObject> materializePyFrameForDeopt(PyThreadState* tstate) {
  auto py_frame = Ref<PyFrameObject>::steal(
      materializePyFrame(tstate, tstate->shadow_frame, nullptr));
  return py_frame;
}

void assertShadowCallStackConsistent(PyThreadState* tstate) {
  PyFrameObject* py_frame = tstate->frame;
  _PyShadowFrame* shadow_frame = tstate->shadow_frame;

  while (shadow_frame) {
    if (_PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_PYFRAME) {
      JIT_CHECK(
          py_frame == _PyShadowFrame_GetPyFrame(shadow_frame),
          "Inconsistent shadow and py frame");
      py_frame = py_frame->f_back;
    }
    shadow_frame = shadow_frame->prev;
  }

  if (py_frame != nullptr) {
    std::unordered_set<PyFrameObject*> seen;
    JIT_LOG(
        "Stack walk didn't consume entire python stack! Here's what's left:");
    PyFrameObject* left = py_frame;
    while (left && !seen.count(left)) {
      JIT_LOG("%s", PyUnicode_AsUTF8(left->f_code->co_name));
      seen.insert(left);
      left = left->f_back;
    }
    JIT_CHECK(false, "stack walk didn't consume entire python stack");
  }
}

BorrowedRef<PyFrameObject> materializeShadowCallStack(PyThreadState* tstate) {
  PyFrameObject* prev_py_frame = nullptr;
  _PyShadowFrame* shadow_frame = tstate->shadow_frame;

  while (shadow_frame) {
    prev_py_frame = materializePyFrame(tstate, shadow_frame, prev_py_frame);
    shadow_frame = shadow_frame->prev;
  }

  if (py_debug) {
    assertShadowCallStackConsistent(tstate);
  }

  return tstate->frame;
}

BorrowedRef<PyFrameObject> materializePyFrameForGen(
    PyThreadState* tstate,
    PyGenObject* gen) {
  if (gen->gi_frame) {
    return gen->gi_frame;
  }

  _PyShadowFrame* shadow_frame = &gen->gi_shadow_frame;
  if (!gen->gi_running) {
    auto gen_footer = reinterpret_cast<GenDataFooter*>(gen->gi_jit_data);
    if (gen_footer->state == _PyJitGenState_Completed) {
      return nullptr;
    }
    Ref<PyFrameObject> py_frame = createPyFrame(tstate, shadow_frame);
    py_frame->f_executing = 0;
    // It's safe to destroy our reference to the frame; gen holds a strong
    // reference to the frame which keeps the frame alive.
    return py_frame;
  }

  // Check if the generator's shadow frame is on the call stack. The generator
  // will be marked as running but will not be on the stack when it appears as
  // a predecessor in a chain of generators into which an exception was
  // thrown. For example, given an "await stack" of coroutines like the
  // following, where ` a <- b` indicates a `a` awaits `b`,
  //
  //   coro0 <- coro1 <- coro2
  //
  // if someone does `coro0.throw(...)`, then `coro0` and `coro1` will be
  // marked as running but will not appear on the stack while `coro2` is
  // handling the exception.
  std::optional<PyFrameObject*> cursor =
      findInnermostPyFrameForShadowFrame(tstate, shadow_frame);
  if (cursor.has_value()) {
    return materializePyFrame(tstate, shadow_frame, cursor.value());
  }
  // It's safe to destroy our reference to the frame; gen holds a strong
  // reference to the frame which keeps the frame alive.
  return createPyFrame(tstate, shadow_frame);
}

} // namespace jit

int _PyShadowFrame_HasGen(_PyShadowFrame* shadow_frame) {
  return is_shadow_frame_for_gen(shadow_frame);
}

PyGenObject* _PyShadowFrame_GetGen(_PyShadowFrame* shadow_frame) {
  JIT_DCHECK(
      is_shadow_frame_for_gen(shadow_frame),
      "Not shadow-frame for a generator");

  // For generators, shadow frame is embedded in generator object. Thus we
  // can recover the generator object pointer from the shadow frame pointer.
  return reinterpret_cast<PyGenObject*>(
      reinterpret_cast<uintptr_t>(shadow_frame) -
      offsetof(PyGenObject, gi_shadow_frame));
}

PyCodeObject* _PyShadowFrame_GetCode(_PyShadowFrame* shadow_frame) {
  _PyShadowFrame_PtrKind ptr_kind = _PyShadowFrame_GetPtrKind(shadow_frame);
  void* ptr = _PyShadowFrame_GetPtr(shadow_frame);
  switch (ptr_kind) {
    case PYSF_CODE_RT:
      return static_cast<jit::CodeRuntime*>(ptr)->GetCode();
    case PYSF_PYFRAME:
      return static_cast<PyFrameObject*>(ptr)->f_code;
    case PYSF_PYCODE:
      return static_cast<PyCodeObject*>(ptr);
    default:
      JIT_CHECK(false, "Unsupported ptr kind %d:", ptr_kind);
  }
}

PyObject* _PyShadowFrame_GetFullyQualifiedName(_PyShadowFrame* shadow_frame) {
  PyObject* mod_name = jit::getModuleName(shadow_frame);
  if (!mod_name) {
    return NULL;
  }
  PyCodeObject* code = _PyShadowFrame_GetCode(shadow_frame);
  PyObject* result = PyUnicode_FromFormat("%U:%U", mod_name, code->co_qualname);
  Py_DECREF(mod_name);
  return result;
}
