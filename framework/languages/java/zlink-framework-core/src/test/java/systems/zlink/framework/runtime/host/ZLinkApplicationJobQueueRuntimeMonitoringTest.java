package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.RecordComponent;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.monitoring.ZLinkApplicationJobQueueStatus;
import systems.zlink.framework.monitoring.ZLinkCoreHwmStatus;
import systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus;
import systems.zlink.framework.monitoring.ZLinkHostCapacityStatus;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.metrics.ZLinkApplicationJobQueuePressureMetrics;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

final class ZLinkApplicationJobQueueRuntimeMonitoringTest {
    @Test
    void exposesTheExactCapacityRecordsAndResetMethod() throws Exception {
        assertComponents(
            ZLinkApplicationJobQueueStatus.class,
            "configuredProfile",
            "configuredManualMax",
            "configuredPauseThresholdPercent",
            "configuredResumeThresholdPercent",
            "effectiveProcessorCount",
            "effectiveMaxQueuedApplicationJobs",
            "pausePermitCount",
            "resumePermitCount",
            "reservedSupplyPermits",
            "queuedApplicationJobs",
            "permitsInUse",
            "peakPermitsInUse",
            "pressureState",
            "currentPauseDuration",
            "capacityWaiters",
            "capacityWaitCount",
            "capacityWaitDuration");
        assertComponents(
            ZLinkHostCapacityStatus.class,
            "measurementEpoch",
            "coreHwm",
            "applicationJobQueue");
        assertComponents(
            ZLinkFrameworkRuntimeStatus.class,
            "state",
            "isReady",
            "acceptingWork",
            "deadline",
            "relocationResult",
            "terminationResult",
            "safeToShutdown",
            "capacity",
            "sequence",
            "observedAt");
        assertEquals(
            ZLinkHostCapacityStatus.class,
            ZLinkFrameworkRuntimeStatus.class.getMethod("capacity").getReturnType());
        assertEquals(
            void.class,
            ZLinkFrameworkRuntime.class
                .getMethod("resetCapacityMetrics")
                .getReturnType());
        assertThrows(
            NoSuchMethodException.class,
            () -> ZLinkFrameworkRuntime.class
                .getMethod("resetCoreHwmBudgetMetrics"));
    }

    @Test
    void runtimeProjectsOneQueueAggregateAndResetsOneCapacityEpoch()
        throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.configureInboundDispatch().setMaxQueuedApplicationJobs(1);
        ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(
            options,
            new ZLinkJavaBackendAdapterFactory());
        try {
            var queue = options.registration().applicationJobQueue();
            var permit = queue.acquire().toCompletableFuture().join();
            var waiter = queue.acquire().toCompletableFuture();
            var before = runtime.status().capacity();

            assertEquals(1, before.applicationJobQueue()
                .configuredManualMax().orElseThrow());
            assertEquals(1, before.applicationJobQueue()
                .effectiveMaxQueuedApplicationJobs());
            assertEquals(80, before.applicationJobQueue()
                .configuredPauseThresholdPercent());
            assertEquals(60, before.applicationJobQueue()
                .configuredResumeThresholdPercent());
            assertEquals(1, before.applicationJobQueue().pausePermitCount());
            assertEquals(0, before.applicationJobQueue().resumePermitCount());
            assertEquals(1, before.applicationJobQueue().reservedSupplyPermits());
            assertEquals(1, before.applicationJobQueue().capacityWaiters());
            assertEquals(1, before.applicationJobQueue().capacityWaitCount());
            assertTrue(before.coreHwm().effectiveBudgetBytes() > 0);

            assertTrue(waiter.cancel(false));
            runtime.resetCapacityMetrics();
            var after = runtime.status().capacity();
            assertEquals(before.measurementEpoch() + 1, after.measurementEpoch());
            assertEquals(1, after.applicationJobQueue().reservedSupplyPermits());
            assertEquals(1, after.applicationJobQueue().peakPermitsInUse());
            assertEquals(0, after.applicationJobQueue().capacityWaitCount());
            assertEquals(
                before.coreHwm().effectiveBudgetBytes(),
                after.coreHwm().effectiveBudgetBytes());

            permit.close();
            runtime.closeAsync().toCompletableFuture().get(5, TimeUnit.SECONDS);
            assertFalse(runtime.status().capacity().applicationJobQueue()
                .effectiveMaxQueuedApplicationJobs() == 0);
        } finally {
            runtime.close();
        }
    }

    @Test
    void runtimeRegistersTheSameCapacityProjectionUsedByMetrics()
        throws Exception {
        AtomicReference<Supplier<ZLinkHostCapacityStatus>> source =
            new AtomicReference<>();
        AtomicReference<Supplier<ZLinkApplicationJobQueuePressureMetrics>>
            pressureSource = new AtomicReference<>();
        AutoCloseable metrics = ZLinkRuntimeMetrics.install(
            new ZLinkRuntimeMetrics.Sink() {
                @Override
                public void registerHostCapacity(
                    Supplier<ZLinkHostCapacityStatus> value) {
                    source.set(value);
                }

                @Override
                public void registerApplicationJobQueuePressure(
                    Supplier<ZLinkApplicationJobQueuePressureMetrics> value) {
                    pressureSource.set(value);
                }
            });
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.configureInboundDispatch().setMaxQueuedApplicationJobs(1);
        ZLinkFrameworkRuntime runtime = null;
        try {
            runtime = ZLinkFrameworkRuntimeTestAccess.start(
                options,
                new ZLinkJavaBackendAdapterFactory());
            assertEquals(
                runtime.status().capacity(),
                source.get().get());
            assertEquals(
                options.registration().applicationJobQueue().pressureMetrics(),
                pressureSource.get().get());
        } finally {
            if (runtime != null) {
                runtime.close();
                assertEquals(null, source.get().get());
                assertEquals(null, pressureSource.get().get());
            }
            metrics.close();
        }
    }

    private static void assertComponents(Class<?> type, String... names) {
        assertTrue(type.isRecord());
        assertEquals(
            List.of(names),
            Arrays.stream(type.getRecordComponents())
                .map(RecordComponent::getName)
                .toList());
    }
}
