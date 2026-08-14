package systems.zlink.framework.runtime.handlers;

import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.WildcardType;
import java.util.Arrays;
import java.util.List;
import java.util.Collection;
import java.util.Objects;
import java.util.ServiceLoader;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.errors.ZLinkConfigurationException;

public final class ZLinkHandlerMethodInvoker {
    private static final String CONTINUATION_CLASS_NAME = "kotlin.coroutines.Continuation";
    private static final List<ZLinkSuspendInvocationAdapter> SUSPEND_INVOKERS = ServiceLoader
        .load(ZLinkSuspendInvocationAdapter.class)
        .stream()
        .map(ServiceLoader.Provider::get)
        .toList();

    private ZLinkHandlerMethodInvoker() {
    }

    public static boolean isKotlinSuspendMethod(Method method) {
        Class<?>[] parameterTypes = method.getParameterTypes();
        return parameterTypes.length > 0
            && CONTINUATION_CLASS_NAME.equals(parameterTypes[parameterTypes.length - 1].getName());
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
            return requireContinuationResultType(handlerType, parameterized.getActualTypeArguments()[0]);
        }
        throw new ZLinkConfigurationException(
            "Kotlin suspend handler reply type must resolve from Continuation: "
                + handlerType.getName() + "." + method.getName());
    }

    public static CompletionStage<Object> invoke(Object handler, Method method, Object[] logicalArguments) {
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
        Class<?> handlerType,
        String methodName,
        Object[] logicalArguments) {
        Objects.requireNonNull(handlerType, "handlerType");
        Objects.requireNonNull(methodName, "methodName");
        Object[] args = logicalArguments == null ? new Object[0] : logicalArguments;
        for (Method method : handlerType.getMethods()) {
            if (!methodName.equals(method.getName())) {
                continue;
            }
            Class<?>[] parameterTypes = logicalParameterTypes(method);
            if (parameterTypes.length != args.length) {
                continue;
            }
            if (argumentsMatch(parameterTypes, args)) {
                return method;
            }
        }
        throw new ZLinkConfigurationException(
            "handler method is not found: " + handlerType.getName() + "." + methodName
                + argumentTypesMessage(args));
    }

    public static CompletionStage<Object> invoke(
        Object handler,
        Method method,
        Object[] logicalArguments,
        Collection<ZLinkSuspendInvocationAdapter> suspendInvokers) {
        if (!isKotlinSuspendMethod(method)) {
            try {
                method.setAccessible(true);
                ZLinkApplicationJobContext.beforeFirstApplicationInstruction();
                Object result = method.invoke(handler, logicalArguments);
                if (result instanceof CompletionStage<?> stage) {
                    return stage.thenApply(value -> (Object) value);
                }
                return CompletableFuture.completedFuture(result);
            } catch (IllegalAccessException | InvocationTargetException ex) {
                return CompletableFuture.failedFuture(unwrapReflectionFailure(ex));
            } catch (IllegalArgumentException ex) {
                return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                    "handler method arguments do not match: "
                        + method.getDeclaringClass().getName()
                        + "." + method.getName()
                        + argumentTypesMessage(logicalArguments),
                    ex));
            }
        }
        Collection<ZLinkSuspendInvocationAdapter> effectiveInvokers =
            suspendInvokers == null || suspendInvokers.isEmpty()
                ? SUSPEND_INVOKERS
                : suspendInvokers;
        for (ZLinkSuspendInvocationAdapter invoker : effectiveInvokers) {
            if (invoker.supports(method)) {
                ZLinkApplicationJobContext.beforeFirstApplicationInstruction();
                return invoker.invoke(handler, method, logicalArguments);
            }
        }
        return CompletableFuture.failedFuture(new ZLinkConfigurationException(
            "Kotlin suspend handler requires a registered ZLinkSuspendInvocationAdapter: "
                + method.getDeclaringClass().getName() + "." + method.getName()));
    }

    private static boolean argumentsMatch(Class<?>[] parameterTypes, Object[] arguments) {
        for (int index = 0; index < parameterTypes.length; index++) {
            Object argument = arguments[index];
            if (argument == null) {
                continue;
            }
            Class<?> parameterType = wrapPrimitive(parameterTypes[index]);
            if (!parameterType.isInstance(argument)) {
                return false;
            }
        }
        return true;
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
        while (current instanceof InvocationTargetException invocation && invocation.getCause() != null) {
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
            "Kotlin suspend handler reply type must resolve to a class: " + handlerType.getName());
    }
}
