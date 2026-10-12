package systems.zlink.framework.runtime.handlers;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.WildcardType;
import java.util.Arrays;
import java.util.Collection;
import java.util.List;
import java.util.Objects;
import java.util.ServiceLoader;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class ZLinkHandlerMethodInvoker {
    public static final String HANDLER_METHOD_NAME = "handle";
    private static final String CONTINUATION_CLASS_NAME = "kotlin.coroutines.Continuation";
    private static final List<ZLinkSuspendInvocationAdapter> SUSPEND_INVOKERS =
            ServiceLoader.load(ZLinkSuspendInvocationAdapter.class).stream()
                    .map(ServiceLoader.Provider::get)
                    .toList();

    private ZLinkHandlerMethodInvoker() {}

    public static PreparedMethod prepareChannel(
            Class<?> handlerType, Method method, Class<?> payloadType, Class<?> contextType) {
        Method selected =
                method == null
                        ? requireHandlerMethodForTypes(
                                handlerType, HANDLER_METHOD_NAME, payloadType, contextType)
                        : method;
        Class<?>[] parameters = logicalParameterTypes(selected);
        int[] arguments = new int[parameters.length];
        for (int index = 1; index < arguments.length; index++) {
            arguments[index] = parameters[index].isAssignableFrom(contextType) ? 1 : -1;
        }
        return prepare(selected, arguments);
    }

    public static Method requireHandlerMethodForTypes(
            Class<?> handlerType, String methodName, Class<?>... argumentTypes) {
        Method bridge = null;
        for (Method method : handlerType.getMethods()) {
            if (!methodName.equals(method.getName())) continue;
            Class<?>[] parameters = logicalParameterTypes(method);
            if (parameters.length != argumentTypes.length) continue;
            boolean matches = true;
            for (int index = 0; index < parameters.length; index++) {
                if (argumentTypes[index] != null
                        && !wrapPrimitive(parameters[index])
                                .isAssignableFrom(wrapPrimitive(argumentTypes[index]))) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                if (!method.isBridge()) return method;
                bridge = method;
            }
        }
        if (bridge != null) return bridge;
        throw new ZLinkConfigurationException(
                "handler method is not found: " + handlerType.getName() + "." + methodName);
    }

    public static PreparedMethod prepareSpot(
            Class<?> handlerType,
            Method method,
            Class<?> spotType,
            Class<?> payloadType,
            Class<?> contextType) {
        Method selected =
                method == null
                        ? requireHandlerMethodForTypes(
                                handlerType,
                                HANDLER_METHOD_NAME,
                                spotType,
                                payloadType,
                                contextType)
                        : method;
        if (method == null) return prepare(selected, 0, 1, 2);
        Class<?>[] parameters = logicalParameterTypes(selected);
        if (parameters.length == 2) return prepare(selected, 0, 1);
        int[] arguments = new int[parameters.length];
        for (int index = 0; index < parameters.length; index++) {
            arguments[index] =
                    parameters[index].isAssignableFrom(contextType)
                            ? 2
                            : parameters[index].isAssignableFrom(spotType) ? 0 : 1;
        }
        return prepare(selected, arguments);
    }

    public static PreparedMethod prepareActor(
            Class<?> handlerType,
            Method method,
            Class<?> spotType,
            Class<?> actorType,
            Class<?> payloadType,
            Class<?> contextType) {
        Method selected =
                method == null
                        ? requireHandlerMethodForTypes(
                                handlerType,
                                HANDLER_METHOD_NAME,
                                spotType,
                                actorType,
                                contextType,
                                payloadType)
                        : method;
        return logicalParameterTypes(selected).length == 2
                ? prepare(selected, 1, 3)
                : prepare(selected, 0, 1, 2, 3);
    }

    public static PreparedMethod prepare(Method method, int... arguments) {
        method.setAccessible(true);
        ZLinkSuspendInvocationAdapter suspendInvoker = null;
        if (isKotlinSuspendMethod(method)) {
            suspendInvoker =
                    SUSPEND_INVOKERS.stream()
                            .filter(invoker -> invoker.supports(method))
                            .findFirst()
                            .orElseThrow(
                                    () ->
                                            new ZLinkConfigurationException(
                                                    "Kotlin suspend handler requires a registered ZLinkSuspendInvocationAdapter: "
                                                            + method.getDeclaringClass().getName()
                                                            + "."
                                                            + method.getName()));
        }
        boolean identity = true;
        for (int index = 0; index < arguments.length; index++) {
            if (arguments[index] != index) identity = false;
        }
        return new PreparedMethod(
                method, identity ? null : arguments.clone(), arguments.length, suspendInvoker);
    }

    public static final class PreparedMethod {
        private final Method method;
        private final int[] argumentIndices;
        private final int argumentCount;
        private final ZLinkSuspendInvocationAdapter suspendInvoker;

        private PreparedMethod(
                Method method,
                int[] argumentIndices,
                int argumentCount,
                ZLinkSuspendInvocationAdapter suspendInvoker) {
            this.method = method;
            this.argumentIndices = argumentIndices;
            this.argumentCount = argumentCount;
            this.suspendInvoker = suspendInvoker;
        }

        public Method method() {
            return method;
        }

        public PreparedMethod withSuspendInvokers(
                Collection<ZLinkSuspendInvocationAdapter> invokers) {
            if (suspendInvoker == null || invokers == null || invokers.isEmpty()) return this;
            ZLinkSuspendInvocationAdapter selected =
                    invokers.stream()
                            .filter(invoker -> invoker.supports(method))
                            .findFirst()
                            .orElseThrow(
                                    () ->
                                            new ZLinkConfigurationException(
                                                    "Kotlin suspend handler requires a registered ZLinkSuspendInvocationAdapter: "
                                                            + method.getDeclaringClass().getName()
                                                            + "."
                                                            + method.getName()));
            return new PreparedMethod(method, argumentIndices, argumentCount, selected);
        }

        public Object[] arguments(Object... values) {
            if (argumentIndices == null) {
                return values.length == argumentCount
                        ? values
                        : Arrays.copyOf(values, argumentCount);
            }
            Object[] arguments = new Object[argumentIndices.length];
            for (int index = 0; index < arguments.length; index++) {
                int source = argumentIndices[index];
                arguments[index] = source < 0 ? null : values[source];
            }
            return arguments;
        }

        public CompletionStage<Object> invoke(Object handler, Object... values) {
            Object[] arguments = arguments(values);
            ZLinkApplicationJobContext.beforeFirstApplicationInstruction();
            if (suspendInvoker != null) {
                return suspendInvoker.invoke(handler, method, arguments);
            }
            try {
                Object result = method.invoke(handler, arguments);
                if (result instanceof CompletionStage<?> stage) {
                    return stage.thenApply(value -> (Object) value);
                }
                return CompletableFuture.completedFuture(result);
            } catch (IllegalAccessException | InvocationTargetException ex) {
                return CompletableFuture.failedFuture(unwrapReflectionFailure(ex));
            } catch (IllegalArgumentException ex) {
                return CompletableFuture.failedFuture(
                        new ZLinkConfigurationException(
                                "handler method arguments do not match: "
                                        + method.getDeclaringClass().getName()
                                        + "."
                                        + method.getName()
                                        + argumentTypesMessage(arguments),
                                ex));
            }
        }
    }

    public static boolean isKotlinSuspendMethod(Method method) {
        Class<?>[] parameterTypes = method.getParameterTypes();
        return parameterTypes.length > 0
                && CONTINUATION_CLASS_NAME.equals(
                        parameterTypes[parameterTypes.length - 1].getName());
    }

    public static Class<?>[] logicalParameterTypes(Method method) {
        Class<?>[] parameterTypes = method.getParameterTypes();
        return isKotlinSuspendMethod(method)
                ? Arrays.copyOf(parameterTypes, parameterTypes.length - 1)
                : parameterTypes;
    }

    public static Class<?> kotlinSuspendReplyType(Class<?> handlerType, Method method) {
        if (!isKotlinSuspendMethod(method)) {
            throw new IllegalArgumentException("method is not a Kotlin suspend method");
        }
        Type[] parameterTypes = method.getGenericParameterTypes();
        Type continuationType = parameterTypes[parameterTypes.length - 1];
        if (continuationType instanceof ParameterizedType parameterized
                && parameterized.getRawType() instanceof Class<?> raw
                && CONTINUATION_CLASS_NAME.equals(raw.getName())) {
            return requireContinuationResultType(
                    handlerType, parameterized.getActualTypeArguments()[0]);
        }
        throw new ZLinkConfigurationException(
                "Kotlin suspend handler reply type must resolve from Continuation: "
                        + handlerType.getName()
                        + "."
                        + method.getName());
    }

    public static CompletionStage<Object> invoke(
            Object handler, Method method, Object[] logicalArguments) {
        return invoke(handler, method, logicalArguments, SUSPEND_INVOKERS);
    }

    public static CompletionStage<Object> invokeHandler(
            Object handler,
            String methodName,
            Object[] logicalArguments,
            Collection<ZLinkSuspendInvocationAdapter> suspendInvokers) {
        Method method = requireHandlerMethod(handler.getClass(), methodName, logicalArguments);
        return invoke(handler, method, logicalArguments, suspendInvokers);
    }

    public static Method requireHandlerMethod(
            Class<?> handlerType, String methodName, Object[] logicalArguments) {
        Objects.requireNonNull(handlerType, "handlerType");
        Objects.requireNonNull(methodName, "methodName");
        Object[] args = logicalArguments == null ? new Object[0] : logicalArguments;
        Class<?>[] types = new Class<?>[args.length];
        for (int index = 0; index < args.length; index++) {
            types[index] = args[index] == null ? null : args[index].getClass();
        }
        return requireHandlerMethodForTypes(handlerType, methodName, types);
    }

    public static CompletionStage<Object> invoke(
            Object handler,
            Method method,
            Object[] logicalArguments,
            Collection<ZLinkSuspendInvocationAdapter> suspendInvokers) {
        int[] indices = new int[logicalArguments.length];
        for (int index = 0; index < indices.length; index++) indices[index] = index;
        try {
            return prepare(method, indices)
                    .withSuspendInvokers(suspendInvokers)
                    .invoke(handler, logicalArguments);
        } catch (ZLinkConfigurationException ex) {
            return CompletableFuture.failedFuture(ex);
        }
    }

    private static String argumentTypesMessage(Object[] arguments) {
        Object[] args = arguments == null ? new Object[0] : arguments;
        StringBuilder message = new StringBuilder("(");
        for (int index = 0; index < args.length; index++) {
            if (index > 0) {
                message.append(", ");
            }
            Object argument = args[index];
            message.append(argument == null ? "null" : argument.getClass().getName());
        }
        return message.append(")").toString();
    }

    private static Class<?> wrapPrimitive(Class<?> type) {
        if (!type.isPrimitive()) {
            return type;
        }
        if (type == int.class) {
            return Integer.class;
        }
        if (type == long.class) {
            return Long.class;
        }
        if (type == boolean.class) {
            return Boolean.class;
        }
        if (type == byte.class) {
            return Byte.class;
        }
        if (type == short.class) {
            return Short.class;
        }
        if (type == float.class) {
            return Float.class;
        }
        if (type == double.class) {
            return Double.class;
        }
        if (type == char.class) {
            return Character.class;
        }
        return Void.class;
    }

    private static Throwable unwrapReflectionFailure(Throwable throwable) {
        Throwable current = throwable;
        while (current instanceof InvocationTargetException invocation
                && invocation.getCause() != null) {
            current = invocation.getCause();
        }
        return current;
    }

    private static Class<?> requireContinuationResultType(Class<?> handlerType, Type argument) {
        if (argument instanceof WildcardType wildcard && wildcard.getLowerBounds().length == 1) {
            return requireContinuationResultType(handlerType, wildcard.getLowerBounds()[0]);
        }
        if (argument instanceof Class<?> klass) {
            return klass;
        }
        if (argument instanceof ParameterizedType parameterized
                && parameterized.getRawType() instanceof Class<?> raw) {
            return raw;
        }
        throw new ZLinkConfigurationException(
                "Kotlin suspend handler reply type must resolve to a class: "
                        + handlerType.getName());
    }
}
