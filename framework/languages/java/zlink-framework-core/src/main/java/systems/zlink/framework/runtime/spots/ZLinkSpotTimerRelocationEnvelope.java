package systems.zlink.framework.runtime.spots;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Optional;
import java.util.Set;
import java.util.function.Function;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.spots.ZLinkTimerOverrunPolicy;
import systems.zlink.framework.spots.ZLinkTimerTick;

/**
 * Deterministic Framework-owned timer section of a relocation payload.
 */
final class ZLinkSpotTimerRelocationEnvelope {
    private static final int MAGIC = 0x5A4C5452;
    private static final int VERSION = 1;
    private static final int MAX_TIMERS = 100_000;
    private static final int MAX_STRING_BYTES = 1024 * 1024;

    private ZLinkSpotTimerRelocationEnvelope() {
    }

    static List<CanonicalTimer> canonicalize(byte[] payload) {
        if (payload == null || payload.length == 0) {
            return List.of();
        }
        FrozenClassResolver resolver = new FrozenClassResolver();
        return decode(payload, resolver::resolve).timers().stream()
            .map(timer -> {
                Instant next = timer.nextScheduledAt().orElseGet(() ->
                    timer.pendingTick().orElseThrow().tick().scheduledAt());
                CanonicalPending pending = timer.pendingTick()
                    .map(value -> new CanonicalPending(
                        value.tick().deliveryIndex(),
                        value.tick().scheduledIndex(),
                        value.tick().scheduledAt().toEpochMilli(),
                        value.tick().skippedTicks()))
                    .orElse(null);
                return new CanonicalTimer(
                    timer.name(),
                    timer.handlerType().getName(),
                    timer.schedule().period().toMillis(),
                    timer.schedule().options().overrunPolicy().value() + 1,
                    timer.schedule().options().maxCatchUpTicks(),
                    timer.schedule().options().stopOnUnhandledException(),
                    timer.schedule().deliveryIndex(),
                    timer.schedule().lastScheduledIndex(),
                    next.toEpochMilli(),
                    pending);
            }).toList();
    }

    static byte[] encodeCanonical(List<CanonicalTimer> values) {
        if (values.isEmpty()) {
            return new byte[0];
        }
        FrozenClassResolver resolver = new FrozenClassResolver();
        List<ZLinkSpotTimerRegistry.TimerSnapshot> timers = values.stream()
            .map(value -> {
                ZLinkTimerOverrunPolicy policy = switch (value.overrunPolicy()) {
                    case 1 -> ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS;
                    case 2 -> ZLinkTimerOverrunPolicy.CATCH_UP_BOUNDED;
                    case 3 -> ZLinkTimerOverrunPolicy.DELAY_NEXT_TICK;
                    default -> throw invalid(null);
                };
                Duration period = Duration.ofMillis(value.periodMilliseconds());
                Instant next = Instant.ofEpochMilli(
                    value.nextScheduledAtUnixMilliseconds());
                var schedule = new ZLinkSpotTimerSchedule.State(
                    value.name(),
                    period,
                    new ZLinkTimerOptions(
                        policy,
                        Math.toIntExact(value.maxCatchUpTicks()),
                        value.stopOnUnhandledException()),
                    next,
                    value.lastCompletedDeliveryIndex(),
                    value.lastCompletedScheduledIndex());
                Optional<Instant> scheduled = value.pending() == null
                    ? Optional.of(next)
                    : Optional.empty();
                Optional<ZLinkSpotTimerSchedule.PendingTick> pending =
                    value.pending() == null
                        ? Optional.empty()
                        : Optional.of(new ZLinkSpotTimerSchedule.PendingTick(
                            value.pending().scheduledIndex(),
                            new ZLinkTimerTick(
                                value.name(),
                                value.pending().deliveryIndex(),
                                value.pending().scheduledIndex(),
                                period,
                                Instant.ofEpochMilli(
                                    value.pending()
                                        .scheduledAtUnixMilliseconds()),
                                Instant.ofEpochMilli(
                                    value.pending()
                                        .scheduledAtUnixMilliseconds()),
                                Duration.ZERO,
                                Duration.ZERO,
                                Duration.ZERO,
                                value.pending().skippedTicks())));
                return new ZLinkSpotTimerRegistry.TimerSnapshot(
                    value.name(),
                    resolver.resolve(value.handlerType()),
                    schedule,
                    scheduled,
                    pending);
            }).toList();
        return encode(new ZLinkSpotTimerRegistry.FrozenTimers(timers));
    }

    record CanonicalTimer(
        String name,
        String handlerType,
        long periodMilliseconds,
        int overrunPolicy,
        long maxCatchUpTicks,
        boolean stopOnUnhandledException,
        long lastCompletedDeliveryIndex,
        long lastCompletedScheduledIndex,
        long nextScheduledAtUnixMilliseconds,
        CanonicalPending pending) {
    }

    record CanonicalPending(
        long deliveryIndex,
        long scheduledIndex,
        long scheduledAtUnixMilliseconds,
        long skippedTicks) {
    }

    private static final class FrozenClassResolver {
        Class<?> resolve(String name) {
            try {
                return Class.forName(
                    name,
                    false,
                    Thread.currentThread().getContextClassLoader());
            } catch (ClassNotFoundException failure) {
                throw invalid(failure);
            }
        }
    }

    static byte[] encode(ZLinkSpotTimerRegistry.FrozenTimers state) {
        try {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            DataOutputStream output = new DataOutputStream(bytes);
            output.writeInt(MAGIC);
            output.writeInt(VERSION);
            List<ZLinkSpotTimerRegistry.TimerSnapshot> timers =
                state.timers().stream()
                    .sorted(Comparator.comparing(
                        ZLinkSpotTimerRegistry.TimerSnapshot::name))
                    .toList();
            if (timers.size() > MAX_TIMERS) {
                throw invalid(null);
            }
            output.writeInt(timers.size());
            Set<String> names = new HashSet<>();
            for (ZLinkSpotTimerRegistry.TimerSnapshot timer : timers) {
                if (!names.add(timer.name())) {
                    throw invalid(null);
                }
                writeString(output, timer.name());
                writeString(output, timer.handlerType().getName());
                writeSchedule(output, timer.schedule());
                if (timer.pendingTick().isPresent()) {
                    output.writeByte(2);
                    writePendingTick(output, timer.pendingTick().orElseThrow());
                } else {
                    output.writeByte(1);
                    writeInstant(output, timer.nextScheduledAt().orElseThrow());
                }
            }
            output.flush();
            return bytes.toByteArray();
        } catch (IOException error) {
            throw invalid(error);
        }
    }

    static ZLinkSpotTimerRegistry.FrozenTimers decode(
        byte[] payload,
        Function<String, Class<?>> handlerTypes) {
        if (payload == null) {
            throw invalid(null);
        }
        if (payload.length == 0) {
            return new ZLinkSpotTimerRegistry.FrozenTimers(List.of());
        }
        try {
            DataInputStream input = new DataInputStream(
                new ByteArrayInputStream(payload));
            if (input.readInt() != MAGIC || input.readInt() != VERSION) {
                throw invalid(null);
            }
            int count = input.readInt();
            if (count < 0 || count > MAX_TIMERS) {
                throw invalid(null);
            }
            List<ZLinkSpotTimerRegistry.TimerSnapshot> timers =
                new ArrayList<>(count);
            Set<String> names = new HashSet<>();
            for (int index = 0; index < count; index++) {
                String name = readString(input);
                if (!names.add(name)) {
                    throw invalid(null);
                }
                Class<?> handlerType = handlerTypes.apply(readString(input));
                if (handlerType == null) {
                    throw invalid(null);
                }
                ZLinkSpotTimerSchedule.State schedule =
                    readSchedule(input, name);
                int action = input.readUnsignedByte();
                Optional<Instant> next = Optional.empty();
                Optional<ZLinkSpotTimerSchedule.PendingTick> pending =
                    Optional.empty();
                if (action == 1) {
                    next = Optional.of(readInstant(input));
                } else if (action == 2) {
                    pending = Optional.of(readPendingTick(input, name));
                } else {
                    throw invalid(null);
                }
                timers.add(new ZLinkSpotTimerRegistry.TimerSnapshot(
                    name,
                    handlerType,
                    schedule,
                    next,
                    pending));
            }
            if (input.read() != -1) {
                throw invalid(null);
            }
            return new ZLinkSpotTimerRegistry.FrozenTimers(timers);
        } catch (EOFException error) {
            throw invalid(error);
        } catch (IOException | RuntimeException error) {
            if (error instanceof ZLinkConfigurationException configuration) {
                throw configuration;
            }
            throw invalid(error);
        }
    }

    private static void writeSchedule(
        DataOutputStream output,
        ZLinkSpotTimerSchedule.State schedule) throws IOException {
        writeDuration(output, schedule.period());
        output.writeInt(schedule.options().overrunPolicy().value());
        output.writeInt(schedule.options().maxCatchUpTicks());
        output.writeBoolean(schedule.options().stopOnUnhandledException());
        writeInstant(output, schedule.startedAt());
        output.writeLong(schedule.deliveryIndex());
        output.writeLong(schedule.lastScheduledIndex());
    }

    private static ZLinkSpotTimerSchedule.State readSchedule(
        DataInputStream input,
        String name) throws IOException {
        Duration period = readDuration(input);
        ZLinkTimerOverrunPolicy policy = switch (input.readInt()) {
            case 0 -> ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS;
            case 1 -> ZLinkTimerOverrunPolicy.CATCH_UP_BOUNDED;
            case 2 -> ZLinkTimerOverrunPolicy.DELAY_NEXT_TICK;
            default -> throw invalid(null);
        };
        ZLinkTimerOptions options = new ZLinkTimerOptions(
            policy,
            input.readInt(),
            input.readBoolean());
        Instant startedAt = readInstant(input);
        long deliveryIndex = input.readLong();
        long lastScheduledIndex = input.readLong();
        if (period.isZero()
            || period.isNegative()
            || deliveryIndex < 0
            || lastScheduledIndex < 0
            || (policy == ZLinkTimerOverrunPolicy.CATCH_UP_BOUNDED
                && options.maxCatchUpTicks() <= 0)) {
            throw invalid(null);
        }
        return new ZLinkSpotTimerSchedule.State(
            name,
            period,
            options,
            startedAt,
            deliveryIndex,
            lastScheduledIndex);
    }

    private static void writePendingTick(
        DataOutputStream output,
        ZLinkSpotTimerSchedule.PendingTick pending) throws IOException {
        ZLinkTimerTick tick = pending.tick();
        output.writeLong(pending.scheduledIndex());
        output.writeLong(tick.deliveryIndex());
        output.writeLong(tick.scheduledIndex());
        writeDuration(output, tick.period());
        writeInstant(output, tick.scheduledAt());
        writeInstant(output, tick.startedAt());
        writeDuration(output, tick.scheduledElapsed());
        writeDuration(output, tick.startedElapsed());
        writeDuration(output, tick.delay());
        output.writeLong(tick.skippedTicks());
    }

    private static ZLinkSpotTimerSchedule.PendingTick readPendingTick(
        DataInputStream input,
        String name) throws IOException {
        long pendingScheduledIndex = input.readLong();
        long deliveryIndex = input.readLong();
        long scheduledIndex = input.readLong();
        Duration period = readDuration(input);
        Instant scheduledAt = readInstant(input);
        Instant startedAt = readInstant(input);
        Duration scheduledElapsed = readDuration(input);
        Duration startedElapsed = readDuration(input);
        Duration delay = readDuration(input);
        long skippedTicks = input.readLong();
        if (pendingScheduledIndex <= 0
            || pendingScheduledIndex != scheduledIndex
            || deliveryIndex <= 0
            || scheduledIndex <= 0
            || skippedTicks < 0) {
            throw invalid(null);
        }
        return new ZLinkSpotTimerSchedule.PendingTick(
            scheduledIndex,
            new ZLinkTimerTick(
                name,
                deliveryIndex,
                scheduledIndex,
                period,
                scheduledAt,
                startedAt,
                scheduledElapsed,
                startedElapsed,
                delay,
                skippedTicks));
    }

    private static void writeString(
        DataOutputStream output,
        String value) throws IOException {
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        if (bytes.length > MAX_STRING_BYTES) {
            throw invalid(null);
        }
        output.writeInt(bytes.length);
        output.write(bytes);
    }

    private static String readString(DataInputStream input) throws IOException {
        int size = input.readInt();
        if (size < 0 || size > MAX_STRING_BYTES) {
            throw invalid(null);
        }
        byte[] bytes = input.readNBytes(size);
        if (bytes.length != size) {
            throw new EOFException();
        }
        return new String(bytes, StandardCharsets.UTF_8);
    }

    private static void writeDuration(
        DataOutputStream output,
        Duration value) throws IOException {
        output.writeLong(value.getSeconds());
        output.writeInt(value.getNano());
    }

    private static Duration readDuration(DataInputStream input)
        throws IOException {
        return Duration.ofSeconds(input.readLong(), input.readInt());
    }

    private static void writeInstant(
        DataOutputStream output,
        Instant value) throws IOException {
        output.writeLong(value.getEpochSecond());
        output.writeInt(value.getNano());
    }

    private static Instant readInstant(DataInputStream input)
        throws IOException {
        return Instant.ofEpochSecond(input.readLong(), input.readInt());
    }

    private static ZLinkConfigurationException invalid(Throwable cause) {
        return cause == null
            ? new ZLinkConfigurationException(
                "invalid Spot timer relocation envelope")
            : new ZLinkConfigurationException(
                "invalid Spot timer relocation envelope",
                cause);
    }
}
