package systems.zlink.framework.runtime.handlers;

import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;
import java.lang.reflect.WildcardType;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.function.Predicate;
import systems.zlink.framework.errors.ZLinkConfigurationException;

public final class ZLinkGenericTypeResolver {
    private ZLinkGenericTypeResolver() {
    }

    public static ParameterizedType findInterface(Class<?> type, Class<?> targetRawType) {
        return findInterface(type, targetRawType, Map.of());
    }

    public static ParameterizedType findInterface(Class<?> type, String targetRawTypeName) {
        return findInterface(type, targetRawTypeName, Map.of());
    }

    public static Class<?> requireClassArgument(Class<?> handlerType, Type argument) {
        Type resolved = resolve(argument, Map.of());
        if (resolved instanceof Class<?> klass) {
            return klass;
        }
        if (resolved instanceof ParameterizedType parameterized
            && parameterized.getRawType() instanceof Class<?> raw) {
            return raw;
        }
        throw new ZLinkConfigurationException(
            "handler generic argument must resolve to a class: " + handlerType.getName());
    }

    private static ParameterizedType findInterface(
        Class<?> type,
        Class<?> targetRawType,
        Map<TypeVariable<?>, Type> bindings) {
        return findInterface(type, raw -> raw == targetRawType, bindings);
    }

    private static ParameterizedType findInterface(
        Class<?> type,
        String targetRawTypeName,
        Map<TypeVariable<?>, Type> bindings) {
        return findInterface(type, raw -> raw.getName().equals(targetRawTypeName), bindings);
    }

    private static ParameterizedType findInterface(
        Class<?> type,
        Predicate<Class<?>> rawTypeMatches,
        Map<TypeVariable<?>, Type> bindings) {
        for (Type interfaceType : type.getGenericInterfaces()) {
            ParameterizedType matched = matchType(interfaceType, rawTypeMatches, bindings);
            if (matched != null) {
                return matched;
            }
        }
        Type superclass = type.getGenericSuperclass();
        if (superclass == null || superclass == Object.class) {
            return null;
        }
        return matchType(superclass, rawTypeMatches, bindings);
    }

    private static ParameterizedType matchType(
        Type type,
        Class<?> targetRawType,
        Map<TypeVariable<?>, Type> bindings) {
        return matchType(type, raw -> raw == targetRawType, bindings);
    }

    private static ParameterizedType matchType(
        Type type,
        String targetRawTypeName,
        Map<TypeVariable<?>, Type> bindings) {
        return matchType(type, raw -> raw.getName().equals(targetRawTypeName), bindings);
    }

    private static ParameterizedType matchType(
        Type type,
        Predicate<Class<?>> rawTypeMatches,
        Map<TypeVariable<?>, Type> bindings) {
        Type resolved = resolve(type, bindings);
        if (resolved instanceof ParameterizedType parameterized
            && parameterized.getRawType() instanceof Class<?> raw) {
            ParameterizedType concrete = resolvedParameterized(parameterized, bindings);
            if (rawTypeMatches.test(raw)) {
                return concrete;
            }
            return findInterface(raw, rawTypeMatches, bind(raw, concrete.getActualTypeArguments()));
        }
        if (resolved instanceof Class<?> raw) {
            return findInterface(raw, rawTypeMatches, bindings);
        }
        return null;
    }

    private static Map<TypeVariable<?>, Type> bind(Class<?> raw, Type[] arguments) {
        TypeVariable<?>[] variables = raw.getTypeParameters();
        if (variables.length == 0) {
            return Map.of();
        }
        Map<TypeVariable<?>, Type> resolved = new LinkedHashMap<>();
        for (int index = 0; index < variables.length && index < arguments.length; index++) {
            resolved.put(variables[index], arguments[index]);
        }
        return resolved;
    }

    private static Type resolve(Type type, Map<TypeVariable<?>, Type> bindings) {
        if (type instanceof TypeVariable<?> variable) {
            Type bound = bindings.get(variable);
            return bound == null ? variable : resolve(bound, bindings);
        }
        if (type instanceof ParameterizedType parameterized) {
            return resolvedParameterized(parameterized, bindings);
        }
        if (type instanceof WildcardType wildcard && wildcard.getUpperBounds().length == 1) {
            return resolve(wildcard.getUpperBounds()[0], bindings);
        }
        return type;
    }

    private static ParameterizedType resolvedParameterized(
        ParameterizedType parameterized,
        Map<TypeVariable<?>, Type> bindings) {
        Type[] arguments = parameterized.getActualTypeArguments();
        Type[] resolvedArguments = new Type[arguments.length];
        for (int index = 0; index < arguments.length; index++) {
            resolvedArguments[index] = resolve(arguments[index], bindings);
        }
        return new ResolvedParameterizedType(
            parameterized.getOwnerType(),
            parameterized.getRawType(),
            resolvedArguments);
    }

    private record ResolvedParameterizedType(
        Type ownerType,
        Type rawType,
        Type[] actualTypeArguments) implements ParameterizedType {
        @Override
        public Type[] getActualTypeArguments() {
            return actualTypeArguments.clone();
        }

        @Override
        public Type getRawType() {
            return rawType;
        }

        @Override
        public Type getOwnerType() {
            return ownerType;
        }
    }
}
