package systems.zlink.framework.runtime.mesh;

import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;

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

    private static boolean matches(
        Type implemented,
        Class<?> adapterContract,
        Class<?> objectType) {
        if (implemented instanceof ParameterizedType parameterized
            && parameterized.getRawType() == adapterContract) {
            return parameterized.getActualTypeArguments().length == 1
                && parameterized.getActualTypeArguments()[0] == objectType;
        }
        if (implemented instanceof Class<?> implementedClass) {
            return matches(implementedClass, adapterContract, objectType);
        }
        return false;
    }
}
