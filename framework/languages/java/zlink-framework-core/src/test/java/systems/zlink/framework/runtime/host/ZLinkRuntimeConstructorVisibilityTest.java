package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntime;
import systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireRuntime;

import java.lang.reflect.Constructor;
import java.lang.reflect.Modifier;
import java.util.Arrays;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ThreadPoolExecutor;

final class ZLinkRuntimeConstructorVisibilityTest {
    @RepeatedTest(100)
    void runtimeInjectionConstructorsRemainPackagePrivate() {
        assertPackagePrivate(
                constructorWith(
                        ZLinkUserSpotRetireRuntime.class,
                        parameters ->
                                parameters.length == 16 && parameters[13] == java.util.List.class));
        assertPackagePrivate(
                constructorWith(
                        ZLinkSpotRuntime.class,
                        parameters ->
                                Arrays.asList(parameters).contains(ScheduledExecutorService.class)
                                        && Arrays.asList(parameters)
                                                .contains(ThreadPoolExecutor.class)));
        assertPackagePrivate(
                constructorWith(
                        ZLinkChannelRuntime.class,
                        parameters -> Arrays.asList(parameters).contains(ExecutorService.class)));
    }

    private static Constructor<?> constructorWith(
            Class<?> type, java.util.function.Predicate<Class<?>[]> signature) {
        Constructor<?> constructor =
                Arrays.stream(type.getDeclaredConstructors())
                        .filter(candidate -> signature.test(candidate.getParameterTypes()))
                        .findFirst()
                        .orElse(null);
        assertNotNull(constructor, "injection constructor signature is missing on " + type);
        return constructor;
    }

    private static void assertPackagePrivate(Constructor<?> constructor) {
        int modifiers = constructor.getModifiers();
        assertFalse(Modifier.isPublic(modifiers), "injection constructor must not be public");
        assertFalse(Modifier.isProtected(modifiers), "injection constructor must not be protected");
        assertFalse(
                Modifier.isPrivate(modifiers), "injection constructor must remain package-private");
    }
}
