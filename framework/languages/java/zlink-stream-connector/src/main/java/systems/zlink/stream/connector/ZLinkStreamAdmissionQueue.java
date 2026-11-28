package systems.zlink.stream.connector;

import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import systems.zlink.stream.connector.ZLinkStreamPendingRequests.PendingRequest;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Base64;
import java.util.IdentityHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import java.util.function.LongSupplier;
import java.util.function.Supplier;

/** Owns the enqueue exchange for the authentication requests waiting on a single connection. */
final class ZLinkStreamAdmissionQueue {
    static final String ENQUEUE = "$zlink.admission.enqueue";
    static final String POSITION = "$zlink.admission.position";
    static final String TICKET = "$zlink.admission.ticket";
    static final String REQUIRED = "$zlink.admission.queueRequired";

    // Matches ZLinkAdmissionTickets.ENCODED_LENGTH in Framework core.
    private static final int TICKET_ENCODED_LENGTH = 66;
    private static final long REREGISTRATION_SILENCE_SECONDS = 15;

    private static final ObjectMapper JSON =
            new ObjectMapper()
                    .enable(JsonParser.Feature.STRICT_DUPLICATE_DETECTION)
                    .enable(DeserializationFeature.FAIL_ON_TRAILING_TOKENS);

    private final Map<ZLinkStreamTransportConnection, Waiting> connections =
            new IdentityHashMap<>();
    private final ScheduledExecutorService scheduler;
    private final LongSupplier nextSequence;
    private final EnqueueSender enqueue;
    private final Runnable queued;
    private final Consumer<Position> positionChanged;

    ZLinkStreamAdmissionQueue(
            ScheduledExecutorService scheduler,
            LongSupplier nextSequence,
            EnqueueSender enqueue,
            Runnable queued,
            Consumer<Position> positionChanged) {
        this.scheduler = scheduler;
        this.nextSequence = nextSequence;
        this.enqueue = enqueue;
        this.queued = queued;
        this.positionChanged = positionChanged;
    }

    void add(PendingRequest request) {
        Waiting waiting;
        synchronized (this) {
            waiting = current(request.owner);
            if (!request.isPending()) {
                return;
            }
            if (waiting != null) {
                waiting.requests.add(request);
                return;
            }
            waiting = new Waiting(request.owner);
            connections.put(request.owner, waiting);
            waiting.requests.add(request);
        }
        queued.run();
        register(waiting, null);
    }

    private void register(Waiting waiting, Silence elapsed) {
        Registration registration;
        synchronized (this) {
            if (current(waiting.owner) != waiting
                    || (elapsed != null && waiting.silence != elapsed)) {
                return;
            }
            registration = new Registration(nextSequence.getAsLong(), waiting.requests.getFirst());
            waiting.registration = registration;
            scheduleRegistration(waiting);
        }
        try {
            enqueue.send(
                    waiting.owner, registration.sequence, () -> canWrite(waiting, registration));
        } catch (ZLinkStreamException failure) {
            rejectResponse(waiting.owner, registration.sequence, failure);
        }
    }

    private synchronized boolean canWrite(Waiting waiting, Registration registration) {
        return current(waiting.owner) == waiting && waiting.registration == registration;
    }

    private void scheduleRegistration(Waiting waiting) {
        waiting.cancelTimer();
        var silence = new Silence();
        waiting.silence = silence;
        silence.task =
                scheduler.schedule(
                        () -> register(waiting, silence),
                        REREGISTRATION_SILENCE_SECONDS,
                        TimeUnit.SECONDS);
    }

    boolean response(
            ZLinkStreamTransportConnection owner, long sequence, Supplier<byte[]> payload) {
        Waiting waiting;
        PendingRequest initiator;
        synchronized (this) {
            waiting = current(owner);
            if (!matches(waiting, sequence)) {
                return false;
            }
            initiator = waiting.registration.initiator;
            waiting.registration = null;
        }
        Update update;
        try {
            update = decode(payload.get(), null);
        } catch (ZLinkStreamException failure) {
            initiator.fail(malformed(failure));
            return true;
        }
        apply(waiting, update, false);
        return true;
    }

    boolean rejectResponse(ZLinkStreamTransportConnection owner, long sequence, Throwable failure) {
        PendingRequest initiator;
        synchronized (this) {
            Waiting waiting = current(owner);
            if (!matches(waiting, sequence)) {
                return false;
            }
            initiator = waiting.registration.initiator;
            waiting.registration = null;
        }
        initiator.fail(failure);
        return true;
    }

    private static boolean matches(Waiting waiting, long sequence) {
        return waiting != null
                && waiting.registration != null
                && waiting.registration.sequence == sequence;
    }

    void pushed(ZLinkStreamTransportConnection owner, String name, Supplier<byte[]> payload) {
        Waiting waiting;
        synchronized (this) {
            waiting = current(owner);
        }
        if (waiting == null) {
            return;
        }
        Update update;
        try {
            update = decode(payload.get(), name);
        } catch (ZLinkStreamException failure) {
            throw malformed(new MalformedPush(failure));
        }
        apply(waiting, update, true);
    }

    private void apply(Waiting waiting, Update update, boolean pushed) {
        List<PendingRequest> released = List.of();
        synchronized (this) {
            if (current(waiting.owner) != waiting) {
                return;
            }
            if (update instanceof Ticket) {
                released = new ArrayList<>(waiting.requests);
                remove(waiting);
            } else if (pushed) {
                scheduleRegistration(waiting);
            }
        }
        switch (update) {
            case Position position -> positionChanged.accept(position);
            case Ticket ticket ->
                    released.forEach(request -> request.retryWithTicket(ticket.value));
        }
    }

    synchronized Set<PendingRequest> connectionEnded(
            ZLinkStreamTransportConnection owner, boolean malformedPush) {
        if (owner == null) {
            connections.values().forEach(Waiting::cancelTimer);
            connections.clear();
        } else {
            Waiting waiting = connections.get(owner);
            if (waiting != null) {
                remove(waiting);
                if (malformedPush) {
                    return waiting.requests;
                }
            }
        }
        return Set.of();
    }

    private Waiting current(ZLinkStreamTransportConnection owner) {
        Waiting waiting = connections.get(owner);
        if (waiting != null) {
            waiting.requests.removeIf(request -> !request.isPending());
            if (waiting.requests.isEmpty()) {
                remove(waiting);
                return null;
            }
        }
        return waiting;
    }

    private void remove(Waiting waiting) {
        connections.remove(waiting.owner);
        waiting.cancelTimer();
    }

    private static Update decode(byte[] payload, String name) {
        try {
            JsonNode body = JSON.readTree(payload);
            if (body != null && body.isObject() && body.size() == 2) {
                if (!TICKET.equals(name)
                        && integer(body.get("position"), 1)
                        && integer(body.get("estimatedWaitMs"), 0)) {
                    return new Position(
                            body.get("position").intValue(),
                            body.get("estimatedWaitMs").intValue());
                }
                JsonNode ticket = body.get("ticket");
                if (!POSITION.equals(name)
                        && ticket != null
                        && ticket.isTextual()
                        && validTicket(ticket.textValue())
                        && integer(body.get("expiresInMs"), 1)) {
                    return new Ticket(ticket.textValue());
                }
            }
        } catch (IOException failure) {
            throw malformed(failure);
        }
        throw malformed(null);
    }

    private static boolean integer(JsonNode value, int minimum) {
        return value != null
                && value.isIntegralNumber()
                && value.canConvertToInt()
                && value.intValue() >= minimum;
    }

    private static boolean validTicket(String value) {
        if (value.length() != TICKET_ENCODED_LENGTH) {
            return false;
        }
        try {
            byte[] bytes = Base64.getUrlDecoder().decode(value);
            return bytes[0] == 1
                    && Base64.getUrlEncoder().withoutPadding().encodeToString(bytes).equals(value);
        } catch (IllegalArgumentException failure) {
            return false;
        }
    }

    private static ZLinkStreamException malformed(Throwable cause) {
        return ZLinkStreamException.of(
                ZLinkStreamErrorCode.FRAME_DECODE_FAILED,
                "Invalid authentication admission update.",
                cause);
    }

    @FunctionalInterface
    interface EnqueueSender {
        void send(ZLinkStreamTransportConnection owner, long sequence, BooleanSupplier canWrite);
    }

    private sealed interface Update permits Position, Ticket {}

    record Position(int position, int estimatedWaitMs) implements Update {}

    private record Ticket(String value) implements Update {}

    private record Registration(long sequence, PendingRequest initiator) {}

    static final class MalformedPush extends RuntimeException {
        MalformedPush(Throwable cause) {
            super("Malformed authentication admission Send.", cause);
        }
    }

    private static final class Silence {
        private ScheduledFuture<?> task;
    }

    private static final class Waiting {
        private final ZLinkStreamTransportConnection owner;
        private final LinkedHashSet<PendingRequest> requests = new LinkedHashSet<>();
        private Registration registration;
        private Silence silence;

        private Waiting(ZLinkStreamTransportConnection owner) {
            this.owner = owner;
        }

        private void cancelTimer() {
            if (silence != null) {
                silence.task.cancel(false);
                silence = null;
            }
        }
    }
}
