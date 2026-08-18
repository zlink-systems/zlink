package systems.zlink.framework.runtime.mesh;

import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;

final class ZLinkRelocationAdapterTypeMatcher {
    private ZLinkRelocationAdapterTypeMatcher() {
    }

    static boolean matches(
        Class<?> adapterType,
        Class<?> adapterContract,
        Class<?> objectType) {
        for (Type implemented : adapterType.getGenericInterfaces()) {
            if (matches(implemented, adapterContract, objectType)) {
                return true;
            }
        }
        Class<?> parent = adapterType.getSuperclass();
        return parent != null
            && parent != Object.class
            && matches(parent, adapterContract, objectType);
    }

    /**
     * Walks past an intermediate interface between the adapter and the
     * contract (e.g. a base/delta capability interface that itself extends
     * the plain capture/restore contract), substituting the intermediate
     * interface's own type variable for the contract's type parameter so an
     * adapter can implement the contract indirectly (spec 15 §5).
     */
    private static boolean matches(
        Type implemented,
        Class<?> adapterContract,
        Class<?> objectType) {
        if (implemented instanceof ParameterizedType parameterized) {
            Object rawType = parameterized.getRawType();
            if (rawType == adapterContract) {
                return parameterized.getActualTypeArguments().length == 1
                    && parameterized.getActualTypeArguments()[0] == objectType;
            }
            if (!(rawType instanceof Class<?> rawClass)) {
                return false;
            }
            for (Type superImplemented : rawClass.getGenericInterfaces()) {
                if (matches(
                        substitute(superImplemented, rawClass, parameterized),
                        adapterContract,
                        objectType)) {
                    return true;
                }
            }
            return false;
        }
        if (implemented instanceof Class<?> implementedClass) {
            return matches(implementedClass, adapterContract, objectType);
        }
        return false;
    }

    /**
     * Replaces {@code declaringClass}'s own type variables in {@code type}
     * with the concrete arguments the subtype supplied for them in
     * {@code actual} (e.g. resolving {@code TActor} in
     * {@code ZLinkActorRelocationAdapter<TActor>} to {@code TestActor} using
     * {@code ZLinkActorBaseDeltaRelocationAdapter<TestActor>}'s argument).
     */
    private static Type substitute(
        Type type,
        Class<?> declaringClass,
        ParameterizedType actual) {
        if (!(type instanceof ParameterizedType parameterized)) {
            return type;
        }
        TypeVariable<?>[] declaringParameters = declaringClass.getTypeParameters();
        Type[] arguments = parameterized.getActualTypeArguments();
        Type[] resolved = new Type[arguments.length];
        boolean changed = false;
        for (int index = 0; index < arguments.length; index++) {
            resolved[index] = arguments[index];
            if (arguments[index] instanceof TypeVariable<?> variable) {
                for (int slot = 0; slot < declaringParameters.length; slot++) {
                    if (declaringParameters[slot].equals(variable)
                        && slot < actual.getActualTypeArguments().length) {
                        resolved[index] = actual.getActualTypeArguments()[slot];
                        changed = true;
                    }
                }
            }
        }
        return changed
            ? new ResolvedParameterizedType(parameterized.getRawType(), resolved)
            : parameterized;
    }

    private record ResolvedParameterizedType(
        Type rawType, Type[] actualTypeArguments) implements ParameterizedType {
        @Override public Type[] getActualTypeArguments() {
            return actualTypeArguments;
        }

        @Override public Type getRawType() {
            return rawType;
        }

        @Override public Type getOwnerType() {
            return null;
        }
    }
}
