package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

import java.time.Instant;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkDrainStageTest {
    @SuppressWarnings("unchecked")
    static void installShutdownOperation(ZLinkFrameworkRuntime runtime) throws Exception {
        var operationType =
                Arrays.stream(ZLinkFrameworkRuntime.class.getDeclaredClasses())
                        .filter(type -> type.getSimpleName().equals("TerminationOperation"))
                        .findFirst()
                        .orElseThrow();
        var constructor = operationType.getDeclaredConstructors()[0];
        constructor.setAccessible(true);
        Object shutdown =
                Arrays.stream(constructor.getParameterTypes()[0].getEnumConstants())
                        .filter(value -> value.toString().equals("SHUTDOWN"))
                        .findFirst()
                        .orElseThrow();
        var completion = new CompletableFuture<>();
        var active = ZLinkFrameworkRuntime.class.getDeclaredField("activeTermination");
        active.setAccessible(true);
        ((AtomicReference<Object>) active.get(runtime))
                .set(constructor.newInstance(shutdown, Instant.now().plusSeconds(600), completion));
        var observer =
                Arrays.stream(ZLinkFrameworkRuntime.class.getDeclaredMethods())
                        .filter(method -> method.getName().equals("terminationObserver"))
                        .findFirst()
                        .orElseThrow();
        observer.setAccessible(true);
        var drained = ZLinkFrameworkRuntime.class.getDeclaredField("drained");
        drained.setAccessible(true);
        ((CompletableFuture<Object>) drained.get(runtime))
                .whenComplete(
                        (java.util.function.BiConsumer<Object, Throwable>)
                                observer.invoke(runtime, completion, null));
    }

    static AutoCloseable installCleanupFailure(
            ZLinkFrameworkRuntime runtime, RuntimeException failure) throws Exception {
        var registrationField = ZLinkFrameworkRuntime.class.getDeclaredField("registration");
        registrationField.setAccessible(true);
        Object registration = registrationField.get(runtime);
        var executorField = registration.getClass().getDeclaredField("handlerExecutor");
        executorField.setAccessible(true);
        var previous = (AutoCloseable) executorField.get(registration);
        executorField.set(registration, new CleanupFailureExecutor(failure));
        return previous;
    }

    private record CleanupFailureExecutor(RuntimeException failure)
            implements java.util.concurrent.Executor, AutoCloseable {
        @Override
        public void execute(Runnable command) {
            command.run();
        }

        @Override
        public void close() {
            throw failure;
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {true, false})
    void actualCleanupFailurePreservesPrimaryStage(boolean initialActual) throws Exception {
        var runtime = ZLinkFrameworkRuntimeTestAccess.start(new DefaultZLinkFrameworkOptions());
        var cleanupCause = new IllegalStateException("cleanup callback failed");
        try (var previous = installCleanupFailure(runtime, cleanupCause)) {
            Throwable initialCause =
                    initialActual
                            ? new IllegalStateException("initial callback failed")
                            : new systems.zlink.framework.runtime.internal.execution
                                    .ZLinkTeardownDeadlineExceededException();
            var initial =
                    new ZLinkFrameworkShutdown.Failure(
                            initialActual ? "spot_closing" : "target_relocation", initialCause);
            var force =
                    Arrays.stream(ZLinkFrameworkRuntime.class.getDeclaredMethods())
                            .filter(
                                    method ->
                                            method.getName().equals("forceStop")
                                                    && method.getParameterCount() == 2)
                            .findFirst()
                            .orElseThrow();
            force.setAccessible(true);
            Object reason =
                    Arrays.stream(force.getParameterTypes()[0].getEnumConstants())
                            .filter(value -> value.toString().equals("DEADLINE_EXCEEDED"))
                            .findFirst()
                            .orElseThrow();
            force.invoke(runtime, reason, initial);
            var result = runtime.shutdown().toCompletableFuture().get(10, TimeUnit.SECONDS);
            assertEquals(ZLinkFrameworkTerminationOutcome.FORCE_STOPPED, result.outcome());
            assertEquals(ZLinkFrameworkTerminationReason.TEARDOWN_FAILED, result.reason());
            var drained = ZLinkFrameworkRuntime.class.getDeclaredField("drained");
            drained.setAccessible(true);
            Object terminal = ((CompletableFuture<?>) drained.get(runtime)).join();
            var failure = terminal.getClass().getDeclaredMethod("failure");
            failure.setAccessible(true);
            var aggregate =
                    assertInstanceOf(
                            java.util.concurrent.CompletionException.class,
                            failure.invoke(terminal));
            assertEquals(1, aggregate.getSuppressed().length);
            var primary = assertInstanceOf(ZLinkFrameworkShutdown.Failure.class, unwrap(aggregate));
            assertEquals(initialActual ? "spot_closing" : "executor_close", primary.stage());
            var secondary =
                    assertInstanceOf(
                            ZLinkFrameworkShutdown.Failure.class,
                            unwrap(aggregate.getSuppressed()[0]));
            assertEquals(initialActual ? "executor_close" : "target_relocation", secondary.stage());
            if (initialActual) org.junit.jupiter.api.Assertions.assertSame(initial, primary);
            else org.junit.jupiter.api.Assertions.assertSame(initial, secondary);
            Throwable cleanup = initialActual ? secondary : primary;
            while (cleanup.getCause() != null) cleanup = cleanup.getCause();
            org.junit.jupiter.api.Assertions.assertSame(cleanupCause, cleanup);
        } finally {
            runtime.closeAsync().handle((ignored, failure) -> failure).toCompletableFuture().join();
        }
    }

    private static Throwable unwrap(Throwable failure) {
        while (failure instanceof java.util.concurrent.CompletionException
                && failure.getCause() != null) failure = failure.getCause();
        return failure;
    }

    @Test
    void synchronousCallbackErrorIsRecordedBeforeDeadlineObservation() {
        var callbackFailure = new AssertionError("synchronous callback failed");
        var invoked = new ArrayList<String>();
        var plan = new ZLinkFrameworkShutdown(Instant.EPOCH);
        plan.appendStage(
                "spot_closing",
                () -> {
                    invoked.add("spot_closing");
                    throw callbackFailure;
                });
        plan.appendStage(
                "instance_close",
                () -> {
                    invoked.add("instance_close");
                    return CompletableFuture.completedFuture(null);
                });
        plan.runAsync().handle((ignored, failure) -> failure).toCompletableFuture().join();
        var observed =
                assertInstanceOf(ZLinkFrameworkShutdown.Failure.class, plan.deadlineFailure());
        assertEquals("spot_closing", observed.stage());
        org.junit.jupiter.api.Assertions.assertSame(callbackFailure, observed.getCause());
        assertEquals(List.of("spot_closing"), invoked);
        assertEquals(true, ZLinkFrameworkShutdown.hasTeardownFailure(observed));
    }

    @Test
    void cleanupStagesAreConsumedOnce() {
        var calls = new ArrayList<String>();
        var cleanup = new ZLinkFrameworkShutdown(Instant.now().plusSeconds(600));
        cleanup.deferStage(
                "cleanup",
                () -> {
                    calls.add("cleanup");
                    return CompletableFuture.completedFuture(null);
                });
        cleanup.closeAsync().toCompletableFuture().join();
        cleanup.closeAsync().toCompletableFuture().join();
        assertEquals(List.of("cleanup"), calls);
    }

    @Test
    void planReportsFirstStageBeforeInvocationAndAdvancesOnlyOnCompletion() {
        var first = new CompletableFuture<Void>();
        var second = new CompletableFuture<Void>();
        var invoked = new ArrayList<String>();
        var plan = new ZLinkFrameworkShutdown(Instant.EPOCH);
        plan.appendStage(
                "first",
                () -> {
                    assertEquals(
                            "first",
                            ((ZLinkFrameworkShutdown.Failure) plan.deadlineFailure()).stage());
                    invoked.add("first");
                    return first;
                });
        plan.appendStage(
                "second",
                () -> {
                    invoked.add("second");
                    return second;
                });
        assertEquals("first", ((ZLinkFrameworkShutdown.Failure) plan.deadlineFailure()).stage());
        assertEquals(List.of(), invoked);
        var completion = plan.runAsync().toCompletableFuture();
        assertEquals(List.of("first"), invoked);
        first.complete(null);
        assertEquals(List.of("first", "second"), invoked);
        assertEquals("second", ((ZLinkFrameworkShutdown.Failure) plan.deadlineFailure()).stage());
        second.complete(null);
        completion.join();
        assertEquals(null, plan.deadlineFailure());
    }

    @Test
    void planPreservesCallbackFailureInsteadOfReportingLaterPendingStage() {
        var callbackFailure = new java.util.concurrent.TimeoutException("callback failed");
        var plan = new ZLinkFrameworkShutdown(Instant.EPOCH);
        plan.appendStage("spot_closing", () -> CompletableFuture.failedFuture(callbackFailure));
        plan.appendStage(
                "instance_close",
                () -> {
                    throw new AssertionError("later stage invoked");
                });
        plan.runAsync().exceptionally(ignored -> null).toCompletableFuture().join();
        var failure =
                assertInstanceOf(ZLinkFrameworkShutdown.Failure.class, plan.deadlineFailure());
        assertEquals("spot_closing", failure.stage());
        org.junit.jupiter.api.Assertions.assertSame(callbackFailure, failure.getCause());
        assertEquals(true, ZLinkFrameworkShutdown.hasTeardownFailure(failure));
    }

    @Test
    void globalDeadlineRecordsFirstUnfinishedApplicationStage() throws Exception {
        var acceptedRelocation = new CompletableFuture<Void>();
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.start(new DefaultZLinkFrameworkOptions())) {
            installShutdownOperation(runtime);
            var run =
                    ZLinkFrameworkRuntime.class.getDeclaredMethod(
                            "runDrain", CompletionStage.class);
            run.setAccessible(true);
            run.invoke(runtime, acceptedRelocation);
            var force =
                    Arrays.stream(ZLinkFrameworkRuntime.class.getDeclaredMethods())
                            .filter(
                                    method ->
                                            method.getName().equals("forceStop")
                                                    && method.getParameterCount() == 1)
                            .findFirst()
                            .orElseThrow();
            force.setAccessible(true);
            Object reason =
                    Arrays.stream(force.getParameterTypes()[0].getEnumConstants())
                            .filter(value -> value.toString().equals("DEADLINE_EXCEEDED"))
                            .findFirst()
                            .orElseThrow();
            force.invoke(runtime, reason);
            var result = runtime.shutdown().toCompletableFuture().get(10, TimeUnit.SECONDS);
            assertEquals(ZLinkFrameworkTerminationOutcome.FORCE_STOPPED, result.outcome());
            assertEquals(ZLinkFrameworkTerminationReason.DEADLINE_EXCEEDED, result.reason());
            var drained = ZLinkFrameworkRuntime.class.getDeclaredField("drained");
            drained.setAccessible(true);
            Object terminal = ((CompletableFuture<?>) drained.get(runtime)).join();
            var failure = terminal.getClass().getDeclaredMethod("failure");
            failure.setAccessible(true);
            var staged =
                    assertInstanceOf(
                            ZLinkFrameworkShutdown.Failure.class, failure.invoke(terminal));
            assertEquals("target_relocation", staged.stage());
        } finally {
            acceptedRelocation.complete(null);
        }
    }
}
