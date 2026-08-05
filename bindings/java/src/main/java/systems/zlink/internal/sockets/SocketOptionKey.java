/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.internal.sockets;

import java.util.Objects;

/**
 * A typed descriptor for a single socket option, including its id, value type, and read/write access.
 * @param <T> the Java type of the option value
 */
public final class SocketOptionKey<T> {
    private final String name;
    private final int optionId;
    private final Class<T> valueClass;
    private final SocketOptionValueType valueType;
    private final boolean readable;
    private final boolean writable;
    private final int maxReadLength;

    private SocketOptionKey(String name,
                            int optionId,
                            Class<T> valueClass,
                            SocketOptionValueType valueType,
                            boolean readable,
                            boolean writable,
                            int maxReadLength) {
        this.name = Objects.requireNonNull(name, "name");
        this.optionId = optionId;
        this.valueClass = Objects.requireNonNull(valueClass, "valueClass");
        this.valueType = Objects.requireNonNull(valueType, "valueType");
        this.readable = readable;
        this.writable = writable;
        this.maxReadLength = maxReadLength;

        if (!readable && !writable)
            throw new IllegalArgumentException(name + " must be readable or writable");

        if ((valueType == SocketOptionValueType.STRING
            || valueType == SocketOptionValueType.BYTES)
            && readable
            && maxReadLength <= 0) {
            throw new IllegalArgumentException(
                name + " readable string/bytes option requires maxReadLength > 0");
        }
    }

    public static SocketOptionKey<Integer> int32(String name,
                                                  SocketOption option,
                                                  boolean readable,
                                                  boolean writable) {
        return new SocketOptionKey<>(name, option.getValue(), Integer.class,
            SocketOptionValueType.INT32, readable, writable,
            Integer.BYTES);
    }

    public static SocketOptionKey<Long> int64(String name,
                                               SocketOption option,
                                               boolean readable,
                                               boolean writable) {
        return new SocketOptionKey<>(name, option.getValue(), Long.class,
            SocketOptionValueType.INT64, readable, writable,
            Long.BYTES);
    }

    public static SocketOptionKey<Long> uint64(String name,
                                                SocketOption option,
                                                boolean readable,
                                                boolean writable) {
        return new SocketOptionKey<>(name, option.getValue(), Long.class,
            SocketOptionValueType.UINT64, readable, writable,
            Long.BYTES);
    }

    public void requireLongValue(long value) {
        if (valueType != SocketOptionValueType.INT64
            && valueType != SocketOptionValueType.UINT64) {
            throw new IllegalArgumentException(
                name + " does not accept a 64-bit integer");
        }
    }

    public static SocketOptionKey<String> string(String name,
                                                  SocketOption option,
                                                  boolean readable,
                                                  boolean writable,
                                                  int maxReadLength) {
        return new SocketOptionKey<>(name, option.getValue(), String.class,
            SocketOptionValueType.STRING, readable, writable,
            maxReadLength);
    }

    public static SocketOptionKey<byte[]> bytes(String name,
                                                 SocketOption option,
                                                 boolean readable,
                                                 boolean writable,
                                                 int maxReadLength) {
        return new SocketOptionKey<>(name, option.getValue(), byte[].class,
            SocketOptionValueType.BYTES, readable, writable,
            maxReadLength);
    }

    public String name() {
        return name;
    }

    public int optionId() {
        return optionId;
    }

    public Class<T> valueClass() {
        return valueClass;
    }

    public SocketOptionValueType valueType() {
        return valueType;
    }

    public boolean readable() {
        return readable;
    }

    public boolean writable() {
        return writable;
    }

    public int maxReadLength() {
        return maxReadLength;
    }

    public void requireReadable() {
        if (!readable)
            throw new IllegalArgumentException(name + " is not readable");
    }

    public void requireWritable() {
        if (!writable)
            throw new IllegalArgumentException(name + " is not writable");
    }

    public T cast(Object value) {
        Objects.requireNonNull(value, "value");
        if (!valueClass.isInstance(value)) {
            throw new IllegalArgumentException(
                name + " expects " + valueClass.getSimpleName()
                    + ", got " + value.getClass().getSimpleName());
        }
        return valueClass.cast(value);
    }

    @Override
    public String toString() {
        return name;
    }
}
