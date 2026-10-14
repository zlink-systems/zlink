package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

final class TimerExceptionContractTest {
    private static final Duration PERIOD = Duration.ofMillis(200);
    private static final Duration WAIT = Duration.ofSeconds(5);
    private static final String SPOT_ROLE = "timer-probe";

    @ParameterizedTest
    @EnumSource(ZLinkTimerOverrunPolicy.class)
    void executedTickIsConsumedAfterSynchronousAndAsynchronousFailures(
            ZLinkTimerOverrunPolicy policy) throws Exception {
        var observation = new Observation(policy, false);
        try (var context = start(observation)) {
            observation.finished.get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
            observation
                    .timer
                    .cancel()
                    .toCompletableFuture()
                    .get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
            assertEquals(3, observation.ticks.size());
            long previous = 0;
            for (int index = 0; index < observation.ticks.size(); index++) {
                var tick = observation.ticks.get(index);
                assertEquals(index + 1, tick.deliveryIndex());
                assertTrue(tick.scheduledIndex() > previous, observation.ticks.toString());
                assertTrue(tick.scheduledIndex() >= tick.deliveryIndex());
                assertEquals(tick.scheduledIndex() - previous - 1, tick.skippedTicks());
                if (policy == ZLinkTimerOverrunPolicy.DELAY_NEXT_TICK) {
                    assertEquals(tick.deliveryIndex(), tick.scheduledIndex());
                    assertEquals(0, tick.skippedTicks());
                }
                previous = tick.scheduledIndex();
            }
        }
    }

    @ParameterizedTest
    @EnumSource(ZLinkTimerOverrunPolicy.class)
    void stopOnUnhandledExceptionPreventsTheNextCallback(ZLinkTimerOverrunPolicy policy)
            throws Exception {
        var observation = new Observation(policy, true);
        try (var context = start(observation)) {
            observation.first.get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
            long deadline = System.nanoTime() + WAIT.toNanos();
            while (!observation.timer.isDisposed() && System.nanoTime() < deadline) {
                Thread.sleep(1);
            }
            assertTrue(observation.timer.isDisposed());
            observation
                    .timer
                    .cancel()
                    .toCompletableFuture()
                    .get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
            Thread.sleep(PERIOD.multipliedBy(2).toMillis());
            assertEquals(1, observation.ticks.size());
        }
    }

    private static AnnotationConfigApplicationContext start(Observation observation)
            throws Exception {
        var context = new AnnotationConfigApplicationContext();
        context.registerBean(Observation.class, () -> observation);
        context.register(Config.class, ZLinkFrameworkAutoConfiguration.class);
        try {
            context.refresh();
            context.getBean(ZLinkFrameworkRuntime.class)
                    .spotManager()
                    .getOrCreate(SPOT_ROLE, SPOT_ROLE)
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
            return context;
        } catch (RuntimeException
                | InterruptedException
                | ExecutionException
                | TimeoutException failure) {
            context.close();
            throw failure;
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class Config {
        @Bean
        ZLinkFrameworkConfigurer timers() {
            return options -> {
                options.addLocationStore(new ZLinkInMemoryProviderLocationStore());
                options.addRouteMesh("timers")
                        .listen("inproc://timer-exception-" + UUID.randomUUID())
                        .setRoutingId(RoutingId.from("timer-exception"))
                        .objects()
                        .server()
                        .addSpotFactory(
                                SPOT_ROLE, Probe.class, factory -> factory.disableRelocation());
            };
        }
    }

    static final class Observation {
        final ZLinkTimerOverrunPolicy policy;
        final boolean stop;
        final List<ZLinkTimerTick> ticks = new CopyOnWriteArrayList<>();
        final CompletableFuture<Void> first = new CompletableFuture<>();
        final CompletableFuture<Void> finished = new CompletableFuture<>();
        volatile ZLinkTimer timer;

        Observation(ZLinkTimerOverrunPolicy policy, boolean stop) {
            this.policy = policy;
            this.stop = stop;
        }
    }

    public static final class Probe implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;
        final Observation observation;

        public Probe(ZLinkSpotContext context, Observation observation) {
            this.context = context;
            this.observation = observation;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            return context.addTimer(
                            "probe",
                            PERIOD,
                            Handler.class,
                            new ZLinkTimerOptions(observation.policy, 3, observation.stop))
                    .thenAccept(timer -> observation.timer = timer);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Handler implements ZLinkSpotTimerHandler<Probe> {
        @Override
        public CompletionStage<Void> handle(Probe spot, ZLinkTimerTick tick) {
            var observation = spot.observation;
            observation.ticks.add(tick);
            observation.first.complete(null);
            if (tick.deliveryIndex() == 1)
                throw new IllegalStateException("synchronous timer failure");
            if (tick.deliveryIndex() == 2) {
                return CompletableFuture.failedFuture(
                        new IllegalStateException("asynchronous timer failure"));
            }
            observation.timer.cancel();
            observation.finished.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }
}
