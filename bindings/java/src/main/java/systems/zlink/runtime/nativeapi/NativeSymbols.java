/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

final class NativeSymbols {
    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LOOKUP = LibraryLoader.lookup();

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
          .map(symbol -> LINKER.downcallHandle(symbol, fd))
          .orElseGet(() -> missingDowncall(name, fd));
    }

    static MethodHandle downcallCritical(String name,
                                         FunctionDescriptor fd) {
        return LOOKUP.find(name)
          .map(symbol -> LINKER.downcallHandle(symbol, fd,
              Linker.Option.critical(false)))
          .orElseGet(() -> missingDowncall(name, fd));
    }


    static MethodHandle downcallAny(String[] names, FunctionDescriptor fd) {
        for (String name : names) {
            if (LOOKUP.find(name).isPresent()) {
                return LINKER.downcallHandle(require(name), fd);
            }
        }
        return missingDowncall("one of: " + String.join(", ", names), fd);
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
