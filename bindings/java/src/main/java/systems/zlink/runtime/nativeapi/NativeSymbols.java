/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.Arena;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.StructLayout;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.invoke.VarHandle;

final class NativeSymbols {
    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LOOKUP = LibraryLoader.lookup();
    // Every Core downcall captures errno as it returns (README §4): runtime
    // code that runs before the binding reads errno cannot change it.
    private static final Linker.Option CAPTURE_ERRNO =
        Linker.Option.captureCallState("errno");
    private static final StructLayout CAPTURE_LAYOUT = Linker.Option.captureStateLayout();
    private static final VarHandle CAPTURED_ERRNO = CAPTURE_LAYOUT.varHandle(
        MemoryLayout.PathElement.groupElement("errno"));
    private static final ThreadLocal<MemorySegment> CAPTURE_STATE =
        ThreadLocal.withInitial(() -> Arena.ofAuto().allocate(CAPTURE_LAYOUT));
    private static final MethodHandle CAPTURE_STATE_OF_THREAD;

    static {
        try {
            CAPTURE_STATE_OF_THREAD = MethodHandles.lookup().findStatic(NativeSymbols.class,
                "captureState", MethodType.methodType(MemorySegment.class));
        } catch (ReflectiveOperationException failure) {
            throw new ExceptionInInitializerError(failure);
        }
    }

    private NativeSymbols() {
    }

    static MemorySegment require(String name) {
        return LOOKUP.find(name).orElseThrow(
          () -> new IllegalStateException(
            "Missing native symbol '" + name
              + "'. Loaded libzlink is incompatible with this Java binding."));
    }

    static MethodHandle downcall(String name, FunctionDescriptor fd) {
        return LOOKUP.find(name)
          .map(symbol -> capturing(LINKER.downcallHandle(symbol, fd, CAPTURE_ERRNO)))
          .orElseGet(() -> missingDowncall(name, fd));
    }

    static MethodHandle downcallCritical(String name,
                                         FunctionDescriptor fd) {
        return LOOKUP.find(name)
          .map(symbol -> capturing(LINKER.downcallHandle(symbol, fd,
              Linker.Option.critical(false), CAPTURE_ERRNO)))
          .orElseGet(() -> missingDowncall(name, fd));
    }

    static MethodHandle downcallOptional(String name, FunctionDescriptor fd) {
        return LOOKUP.find(name)
            .map(symbol -> capturing(LINKER.downcallHandle(symbol, fd, CAPTURE_ERRNO)))
            .orElse(null);
    }


    static MethodHandle downcallAny(String[] names, FunctionDescriptor fd) {
        for (String name : names) {
            if (LOOKUP.find(name).isPresent()) {
                return capturing(LINKER.downcallHandle(require(name), fd, CAPTURE_ERRNO));
            }
        }
        return missingDowncall("one of: " + String.join(", ", names), fd);
    }

    /** The errno that the most recent Core downcall on this thread returned with. */
    static int capturedErrno() {
        return (int) CAPTURED_ERRNO.get(CAPTURE_STATE.get(), 0L);
    }

    private static MemorySegment captureState() {
        return CAPTURE_STATE.get();
    }

    // Supplies this thread's capture segment as the leading argument, so the
    // handle keeps the descriptor's own signature.
    private static MethodHandle capturing(MethodHandle handle) {
        return MethodHandles.foldArguments(handle, 0, CAPTURE_STATE_OF_THREAD);
    }

    static MethodHandle cDowncall(String name, FunctionDescriptor fd) {
        return LINKER.defaultLookup().find(name)
          .map(symbol -> LINKER.downcallHandle(symbol, fd))
          .orElseGet(() -> missingDowncall(name, fd));
    }

    static MethodHandle freeDowncall() {
        return cDowncall("free", FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));
    }

    private static MethodHandle missingDowncall(String name,
                                                FunctionDescriptor fd) {
        MethodType methodType = fd.toMethodType();
        IllegalStateException failure =
          new IllegalStateException(
            "Missing native symbol '" + name
              + "'. Loaded libzlink is incompatible with this Java binding.");
        MethodHandle throwing = MethodHandles.throwException(
          methodType.returnType(), IllegalStateException.class);
        throwing = MethodHandles.insertArguments(throwing, 0, failure);
        return MethodHandles.dropArguments(throwing, 0,
          methodType.parameterArray());
    }
}
