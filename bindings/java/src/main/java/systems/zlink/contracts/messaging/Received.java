/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.messaging.ReplyOperation;
import systems.zlink.contracts.messaging.ReplySubmitOperation;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.messaging.SendSubmitOperation;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.internal.ContractAccess;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Iterator;
import java.util.List;
import java.util.NoSuchElementException;
import java.util.Objects;
import java.util.Optional;
import java.util.function.BiConsumer;
import java.util.function.BiFunction;

/**
 * Aggregates one recv result, including an optional routing id and the owned
 * message parts.
 *
 * <p>The returned {@link #parts()} view is immutable and does not copy the
 * underlying part array. Closing the aggregate closes every owned part.
 */
public final class Received implements AutoCloseable {
    private static final ThreadLocal<ArrayList<ArrayList<Message>>>
        PARTS_POOL = ThreadLocal.withInitial(ArrayList::new);
    private static final int PARTS_POOL_CAPACITY = 16;

    private static ArrayList<Message> acquirePartsList(int initialCapacity) {
        ArrayList<ArrayList<Message>> pool = PARTS_POOL.get();
        int n = pool.size();
        if (n > 0) {
            ArrayList<Message> list = pool.remove(n - 1);
            return list;
        }
        return new ArrayList<>(Math.max(1, initialCapacity));
    }

    private static void releasePartsList(ArrayList<Message> list) {
        if (list == null) return;
        list.clear();
        ArrayList<ArrayList<Message>> pool = PARTS_POOL.get();
        if (pool.size() < PARTS_POOL_CAPACITY) {
            pool.add(list);
        }
    }

    // Callers may pass the same Received instance to multiple recv calls. The
    // binding refills internal state via adoptFrom() in place, avoiding the
    // per-recv Received allocation.
    private long requestSequence;
    private boolean hasRequestSequence;
    private BiConsumer<List<Message>, SendFlags> replySender;
    private BiFunction<List<Message>, SendFlags, Boolean> sendSender;
    private byte[] routingIdBytes;
    private Runnable onTerminalState;
    private ArrayList<Message> realizedParts;
    private ContractAccess.ReceivedPartCursor cursor;
    private RoutingId routingId;
    private List<Message> partsView;
    private boolean closed;

    static {
        ContractAccess.register(new ContractAccess.ReceivedAccess() {
            @Override
            public Received create(RoutingId routingId, Message[] parts) {
                return new Received(routingId, parts);
            }

            @Override
            public Received create(RoutingId routingId, Message[] parts,
                                   boolean trustedParts,
                                   long requestSeq, boolean hasRequestSeq,
                                   BiConsumer<List<Message>, SendFlags> replySender) {
                return new Received(routingId, parts, trustedParts,
                    requestSeq, hasRequestSeq, replySender);
            }

            @Override
            public Received create(RoutingId routingId, Message[] parts,
                                   boolean trustedParts,
                                   long requestSeq, boolean hasRequestSeq,
                                   BiConsumer<List<Message>, SendFlags> replySender,
                                   Runnable onTerminalState) {
                return new Received(routingId, parts, trustedParts,
                    requestSeq, hasRequestSeq, replySender, onTerminalState);
            }

            @Override
            public Received create(byte[] routingIdBytes, Message[] parts,
                                   boolean trustedParts,
                                   long requestSeq, boolean hasRequestSeq,
                                   BiConsumer<List<Message>, SendFlags> replySender,
                                   Runnable onTerminalState) {
                return new Received(routingIdBytes, parts, trustedParts,
                    requestSeq, hasRequestSeq, replySender,
                    onTerminalState);
            }

            @Override
            public Received create(RoutingId routingId, Message[] parts,
                                   long requestSeq,
                                   boolean hasRequestSeq,
                                   BiConsumer<List<Message>, SendFlags> replySender) {
                return new Received(routingId, parts, true,
                    requestSeq, hasRequestSeq, replySender);
            }

            @Override
            public Received createLazy(byte[] routingIdBytes, Message firstPart,
                                       ContractAccess.ReceivedPartCursor cursor,
                                       long requestSeq, boolean hasRequestSeq,
                                       BiConsumer<List<Message>, SendFlags> replySender,
                                       Runnable onTerminalState) {
                return new Received(routingIdBytes, firstPart, cursor,
                    requestSeq, hasRequestSeq, replySender,
                    onTerminalState);
            }

            @Override
            public Received createLazy(RoutingId routingId, Message firstPart,
                                       ContractAccess.ReceivedPartCursor cursor,
                                       long requestSeq, boolean hasRequestSeq,
                                       BiConsumer<List<Message>, SendFlags> replySender,
                                       Runnable onTerminalState) {
                return new Received(routingId, firstPart, cursor,
                    requestSeq, hasRequestSeq, replySender, onTerminalState);
            }

            @Override
            public void populateRoutedSinglePart(Received received,
                                                 byte[] routingIdBytes,
                                                 Message singlePart,
                                                 long requestSequence,
                                                 boolean hasRequestSequence,
                                                 BiConsumer<List<Message>,
                                                     SendFlags> replySender,
                                                 Runnable onTerminalState) {
                received.populateRoutedSinglePart(routingIdBytes, singlePart,
                    requestSequence, hasRequestSequence,
                    replySender, onTerminalState);
            }

            @Override
            public void forceMaterialize(Received received) {
                received.forceMaterialize();
            }

            @Override
            public List<Message> takeParts(Received received) {
                return received.takeParts();
            }

            @Override
            public void setSendSender(Received received,
                                      BiFunction<List<Message>, SendFlags,
                                          Boolean> sendSender) {
                received.setSendSender(sendSender);
            }

            @Override
            public void adoptFrom(Received target, Received source) {
                target.adoptFrom(source);
            }
        });
    }

    /**
     * Create an empty {@code Received} for caller-provided storage. Hand the
     * same instance to {@code recv(Received, ...)} across calls to avoid the
     * per-recv allocation; the binding overwrites the internal state on each
     * successful receive via {@code adoptFrom}.
     */
    public Received() {
        this.requestSequence = 0L;
        this.hasRequestSequence = false;
        this.replySender = null;
        this.sendSender = null;
        this.routingIdBytes = null;
        this.onTerminalState = null;
        this.realizedParts = null;
        this.cursor = null;
        this.routingId = null;
        this.partsView = null;
        this.closed = false;
    }

    /**
     * Populate this Received in place with a single-part routed recv
     * result.
     *
     * <p>HOT PATH: caller-provided recv storage avoids allocating a fresh
     * {@link Received} and then closing/adopting it for every routed message.
     */
    void populateRoutedSinglePart(byte[] routingIdBytes,
                                  Message singlePart,
                                  long requestSequence,
                                  boolean hasRequestSequence,
                                  BiConsumer<List<Message>, SendFlags> replySender,
                                  Runnable onTerminalState) {
        Objects.requireNonNull(singlePart, "singlePart");
        // Discard any prior owned state without recycling the parts list,
        // so we can reuse it without reallocation.
        if (realizedParts != null && !realizedParts.isEmpty()) {
            for (int i = 0; i < realizedParts.size(); i++) {
                Message part = realizedParts.get(i);
                if (part != null) {
                    try { part.close(); } catch (RuntimeException ignored) {}
                }
            }
            realizedParts.clear();
        }
        ContractAccess.ReceivedPartCursor pendingCursor = cursor;
        cursor = null;
        this.closed = false;
        this.routingId = null;
        this.routingIdBytes = routingIdBytes;
        this.requestSequence = requestSequence;
        this.hasRequestSequence = hasRequestSequence;
        this.replySender = replySender;
        this.sendSender = null;
        this.onTerminalState = onTerminalState;
        if (this.realizedParts == null) {
            this.realizedParts = acquirePartsList(1);
        }
        this.realizedParts.add(singlePart);
        this.partsView = null;
        closeCursorQuietly(pendingCursor);
    }

    /**
     * Replace this Received's internal state with the contents of
     * {@code source}, transferring ownership of the parts and routing-id
     * storage. Closes any state currently held by {@code this} first. After
     * this call, {@code source} is left in an empty (already-detached) state
     * and should not be reused.
     */
    void adoptFrom(Received source) {
        Objects.requireNonNull(source, "source");
        if (source == this) return;

        // Close any state currently held by this Received before adopting.
        close();
        this.closed = false;

        this.requestSequence = source.requestSequence;
        this.hasRequestSequence = source.hasRequestSequence;
        this.replySender = source.replySender;
        this.sendSender = source.sendSender;
        this.routingIdBytes = source.routingIdBytes;
        this.onTerminalState = source.onTerminalState;
        this.realizedParts = source.realizedParts;
        this.cursor = source.cursor;
        this.routingId = source.routingId;
        this.partsView = source.partsView;

        // Detach source so its own close() / finalizer is a no-op.
        source.requestSequence = 0L;
        source.hasRequestSequence = false;
        source.replySender = null;
        source.sendSender = null;
        source.routingIdBytes = null;
        source.onTerminalState = null;
        source.realizedParts = null;
        source.cursor = null;
        source.routingId = null;
        source.partsView = null;
        source.closed = true;
    }

    public Received(RoutingId routingId, Message[] parts) {
        this(routingId, parts, false, 0L, false, null);
    }

    public Received(byte[] routingIdBytes, Message[] parts) {
        this(routingIdBytes, parts, false, 0L, false, null);
    }

    Received(RoutingId routingId, Message[] parts, boolean trustedParts) {
        this(routingId, parts, trustedParts, 0L, false, null);
    }

    Received(byte[] routingIdBytes, Message[] parts, boolean trustedParts) {
        this(routingIdBytes, parts, trustedParts, 0L, false, null);
    }

    Received(RoutingId routingId, Message[] parts, boolean trustedParts,
             long requestSequence,
             boolean hasRequestSequence,
             BiConsumer<List<Message>, SendFlags> replySender) {
        this(routingId, parts, trustedParts, requestSequence,
            hasRequestSequence, replySender, null);
    }

    Received(RoutingId routingId, Message[] parts, boolean trustedParts,
             long requestSequence,
             boolean hasRequestSequence,
             BiConsumer<List<Message>, SendFlags> replySender,
             Runnable onTerminalState) {
        this.routingId = routingId;
        this.routingIdBytes = null;
        this.requestSequence = requestSequence;
        this.hasRequestSequence = hasRequestSequence;
        this.replySender = replySender;
        this.sendSender = null;
        this.onTerminalState = onTerminalState;
        Message[] ownedParts = Objects.requireNonNull(parts, "parts");
        Message[] safeParts = trustedParts ? ownedParts
            : Arrays.copyOf(ownedParts, ownedParts.length);
        this.realizedParts = acquirePartsList(safeParts.length);
        Collections.addAll(this.realizedParts, safeParts);
        this.cursor = null;
    }

    Received(byte[] routingIdBytes, Message[] parts, boolean trustedParts,
             long requestSequence,
             boolean hasRequestSequence,
             BiConsumer<List<Message>, SendFlags> replySender) {
        this(routingIdBytes, parts, trustedParts, requestSequence,
            hasRequestSequence, replySender, null);
    }

    Received(byte[] routingIdBytes, Message[] parts, boolean trustedParts,
             long requestSequence,
             boolean hasRequestSequence,
             BiConsumer<List<Message>, SendFlags> replySender,
             Runnable onTerminalState) {
        this.routingId = null;
        this.routingIdBytes = routingIdBytes;
        this.requestSequence = requestSequence;
        this.hasRequestSequence = hasRequestSequence;
        this.replySender = replySender;
        this.sendSender = null;
        this.onTerminalState = onTerminalState;
        Message[] ownedParts = Objects.requireNonNull(parts, "parts");
        Message[] safeParts = trustedParts ? ownedParts
            : Arrays.copyOf(ownedParts, ownedParts.length);
        this.realizedParts = acquirePartsList(safeParts.length);
        Collections.addAll(this.realizedParts, safeParts);
        this.cursor = null;
    }

    Received(RoutingId routingId, Message singlePart,
             long requestSequence, boolean hasRequestSequence,
             BiConsumer<List<Message>, SendFlags> replySender) {
        this(routingId, singlePart, requestSequence, hasRequestSequence,
            replySender, null);
    }

    Received(RoutingId routingId, Message singlePart,
             long requestSequence, boolean hasRequestSequence,
             BiConsumer<List<Message>, SendFlags> replySender,
             Runnable onTerminalState) {
        this.routingId = routingId;
        this.routingIdBytes = null;
        this.requestSequence = requestSequence;
        this.hasRequestSequence = hasRequestSequence;
        this.replySender = replySender;
        this.sendSender = null;
        this.onTerminalState = onTerminalState;
        this.realizedParts = acquirePartsList(1);
        this.realizedParts.add(Objects.requireNonNull(singlePart, "singlePart"));
        this.cursor = null;
    }

    Received(byte[] routingIdBytes, Message singlePart,
             long requestSequence, boolean hasRequestSequence,
             BiConsumer<List<Message>, SendFlags> replySender) {
        this(routingIdBytes, singlePart, requestSequence,
            hasRequestSequence, replySender, null);
    }

    Received(byte[] routingIdBytes, Message singlePart,
             long requestSequence, boolean hasRequestSequence,
             BiConsumer<List<Message>, SendFlags> replySender,
             Runnable onTerminalState) {
        this.routingId = null;
        this.routingIdBytes = routingIdBytes;
        this.requestSequence = requestSequence;
        this.hasRequestSequence = hasRequestSequence;
        this.replySender = replySender;
        this.sendSender = null;
        this.onTerminalState = onTerminalState;
        this.realizedParts = acquirePartsList(1);
        this.realizedParts.add(Objects.requireNonNull(singlePart, "singlePart"));
        this.cursor = null;
    }

    Received(byte[] routingIdBytes, Message firstPart,
             ContractAccess.ReceivedPartCursor cursor, long requestSequence,
             boolean hasRequestSequence,
             BiConsumer<List<Message>, SendFlags> replySender,
             Runnable onTerminalState) {
        this.routingId = null;
        this.routingIdBytes = routingIdBytes;
        this.requestSequence = requestSequence;
        this.hasRequestSequence = hasRequestSequence;
        this.replySender = replySender;
        this.sendSender = null;
        this.onTerminalState = onTerminalState;
        this.realizedParts = acquirePartsList(4);
        this.realizedParts.add(Objects.requireNonNull(firstPart, "firstPart"));
        this.cursor = cursor;
    }

    Received(RoutingId routingId, Message firstPart,
             ContractAccess.ReceivedPartCursor cursor, long requestSequence,
             boolean hasRequestSequence,
             BiConsumer<List<Message>, SendFlags> replySender,
             Runnable onTerminalState) {
        this.routingId = routingId;
        this.routingIdBytes = null;
        this.requestSequence = requestSequence;
        this.hasRequestSequence = hasRequestSequence;
        this.replySender = replySender;
        this.sendSender = null;
        this.onTerminalState = onTerminalState;
        this.realizedParts = acquirePartsList(4);
        this.realizedParts.add(Objects.requireNonNull(firstPart, "firstPart"));
        this.cursor = cursor;
    }

    /** Returns the routing id when the transport delivered one. */
    public Optional<RoutingId> getRoutingId() {
        return Optional.ofNullable(routingIdOrNull());
    }

    RoutingId routingIdOrNull() {
        if (routingId == null && routingIdBytes != null) {
            routingId = ContractAccess.routingIdFromTrusted(routingIdBytes);
        }
        return routingId;
    }

    RoutingId routingIdOrThrow() {
        RoutingId resolved = routingIdOrNull();
        if (resolved == null)
            throw new ZlinkRecvException(RecvResult.NO_DATA);
        return resolved;
    }

    /** Returns the owned parts as an immutable view without copying. */
    public List<Message> parts() {
        synchronized (this) {
            ensureOpen();
            materializeAllLocked();
            if (partsView == null) {
                partsView = Collections.unmodifiableList(realizedParts);
            }
            return partsView;
        }
    }

    public Optional<Long> requestSeq() {
        return hasRequestSequence ? Optional.of(requestSequence)
            : Optional.empty();
    }

    /** Returns whether exactly one payload part was received. */
    public boolean isSinglePart() {
        ArrayList<Message> parts = realizedParts;
        if (!closed && cursor == null && parts != null) {
            return parts.size() == 1;
        }
        synchronized (this) {
            ensureOpen();
            ensureRealizedThroughLocked(1);
            return realizedParts.size() == 1 && cursor == null;
        }
    }

    /** Returns the first payload part. */
    public Message firstPart() {
        ArrayList<Message> parts = realizedParts;
        if (!closed && cursor == null && parts != null && !parts.isEmpty()) {
            return parts.get(0);
        }
        synchronized (this) {
            ensureOpen();
            ensureRealizedThroughLocked(0);
            if (realizedParts.isEmpty())
                throw new ZlinkRecvException(RecvResult.NO_DATA);
            return realizedParts.get(0);
        }
    }

    /** Returns the only payload part or throws when the result is multipart. */
    public Message singlePartOrThrow() {
        ArrayList<Message> parts = realizedParts;
        if (!closed && cursor == null && parts != null && parts.size() == 1) {
            return parts.get(0);
        }
        synchronized (this) {
            ensureOpen();
            ensureRealizedThroughLocked(1);
            if (realizedParts.size() != 1 || cursor != null)
                throw new ZlinkRecvException(RecvResult.NOT_SUPPORTED);
            return realizedParts.get(0);
        }
    }

    public ReplyOperation reply() {
        return new ReplyBuilder();
    }

    private void submitReply(List<Message> parts, SendFlags flags) {
        if (!hasRequestSequence || replySender == null) {
            throw new ZlinkSubmitException(SubmitResult.INVALID_STATE);
        }
        try {
            replySender.accept(Objects.requireNonNull(parts, "parts"),
                Objects.requireNonNull(flags, "flags"));
        } catch (IllegalStateException ex) {
            throw new ZlinkSubmitException(SubmitResult.TERMINATED);
        }
    }

    public SendOperation send() {
        return new SendBuilder();
    }

    private boolean submitSend(List<Message> parts, SendFlags flags) {
        Objects.requireNonNull(parts, "parts");
        Objects.requireNonNull(flags, "flags");
        if (sendSender == null) {
            throw new ZlinkSubmitException(SubmitResult.INVALID_STATE);
        }
        try {
            return sendSender.apply(parts, flags);
        } catch (IllegalStateException ex) {
            throw new ZlinkSubmitException(SubmitResult.TERMINATED);
        }
    }

    void setSendSender(BiFunction<List<Message>, SendFlags, Boolean> sendSender) {
        this.sendSender = sendSender;
    }

    private final class SendBuilder implements SendOperation, SendSubmitOperation {
        private final BuilderParts parts = new BuilderParts();
        private SendFlags flags = SendFlags.NONE;
        private boolean submitted;

        @Override
        public SendSubmitOperation message(Message part) {
            ensureNotSubmitted();
            parts.add(part);
            return this;
        }

        @Override
        public SendSubmitOperation flags(SendFlags value) {
            ensureNotSubmitted();
            flags = Objects.requireNonNull(value, "flags");
            return this;
        }

        @Override
        public boolean submit() {
            ensureNotSubmitted();
            if (parts.isEmpty())
                throw new IllegalArgumentException("at least one message required");
            submitted = true;
            return submitSend(parts.asList(), flags);
        }

        private void ensureNotSubmitted() {
            if (submitted)
                throw new IllegalStateException("operation already submitted");
        }
    }

    private final class ReplyBuilder implements ReplyOperation, ReplySubmitOperation {
        private final BuilderParts parts = new BuilderParts();
        private boolean submitted;

        @Override
        public ReplySubmitOperation message(Message part) {
            ensureNotSubmitted();
            parts.add(part);
            return this;
        }

        @Override
        public void submit() {
            ensureNotSubmitted();
            if (parts.isEmpty())
                throw new IllegalArgumentException("at least one message required");
            submitted = true;
            submitReply(parts.asList(), SendFlags.NONE);
        }

        private void ensureNotSubmitted() {
            if (submitted)
                throw new IllegalStateException("operation already submitted");
        }
    }

    private static final class BuilderParts {
        private Message singlePart;
        private ArrayList<Message> parts;
        private List<Message> view;

        void add(Message part) {
            Objects.requireNonNull(part, "part");
            view = null;
            if (parts != null) {
                parts.add(part);
                return;
            }
            if (singlePart == null) {
                singlePart = part;
                return;
            }
            parts = new ArrayList<>(4);
            parts.add(singlePart);
            parts.add(part);
            singlePart = null;
        }

        boolean isEmpty() {
            return singlePart == null && (parts == null || parts.isEmpty());
        }

        List<Message> asList() {
            if (parts != null)
                return parts;
            if (view == null)
                view = singlePart == null ? List.of() : List.of(singlePart);
            return view;
        }
    }

    @Override
    public void close() {
        ArrayList<Message> fastParts = realizedParts;
        if (!closed && cursor == null && fastParts != null
            && fastParts.size() == 1) {
            Message part = fastParts.get(0);
            closed = true;
            realizedParts = null;
            partsView = Collections.emptyList();
            releasePartsList(fastParts);
            try {
                part.close();
            } catch (RuntimeException ignored) {
            }
            return;
        }

        ContractAccess.ReceivedPartCursor pendingCursor;
        Message singleToClose = null;
        List<Message> toClose = null;
        ArrayList<Message> partsToRelease = null;
        synchronized (this) {
            if (closed)
                return;
            closed = true;
            pendingCursor = cursor;
            cursor = null;
            // realizedParts can be null when the caller closed a freshly
            // constructed (no-arg) Received that was never populated by a
            // recv. The no-arg ctor + canonical recv pattern allows this:
            // an empty Received passed to socket.recv(received, DONT_WAIT)
            // stays null on EAGAIN, and the caller may close() it without
            // ever having observed a successful recv.
            if (realizedParts != null) {
                int n = realizedParts.size();
                if (n == 1) {
                    singleToClose = realizedParts.get(0);
                } else if (n > 1) {
                    toClose = new ArrayList<>(realizedParts);
                }
                partsToRelease = realizedParts;
                realizedParts = null;
            }
            partsView = Collections.emptyList();
        }
        if (singleToClose != null) {
            try {
                singleToClose.close();
            } catch (RuntimeException ignored) {
            }
        } else if (toClose != null) {
            Message.closeAll(toClose);
        }
        releasePartsList(partsToRelease);
        closeCursorQuietly(pendingCursor);
        markTerminal();
    }

    Iterator<Message> iterator() {
        return new Iterator<>() {
            private int index;

            @Override
            public boolean hasNext() {
                synchronized (Received.this) {
                    if (closed)
                        return false;
                    ensureRealizedThroughLocked(index);
                    return index < realizedParts.size();
                }
            }

            @Override
            public Message next() {
                synchronized (Received.this) {
                    ensureOpen();
                    ensureRealizedThroughLocked(index);
                    if (index >= realizedParts.size())
                        throw new NoSuchElementException();
                    Message next = realizedParts.get(index);
                    index++;
                    return next;
                }
            }
        };
    }

    void forceMaterialize() {
        synchronized (this) {
            if (closed)
                return;
            materializeAllLocked();
        }
    }

    List<Message> takeParts() {
        ContractAccess.ReceivedPartCursor pendingCursor;
        ArrayList<Message> detached;
        ArrayList<Message> partsToRelease;
        synchronized (this) {
            ensureOpen();
            materializeAllLocked();
            detached = new ArrayList<>(realizedParts);
            partsToRelease = realizedParts;
            realizedParts = null;
            partsView = Collections.emptyList();
            pendingCursor = cursor;
            cursor = null;
            closed = true;
        }
        releasePartsList(partsToRelease);
        closeCursorQuietly(pendingCursor);
        markTerminal();
        return Collections.unmodifiableList(detached);
    }

    private void ensureOpen() {
        if (closed)
            throw new IllegalStateException("received is closed");
    }

    private void ensureRealizedThroughLocked(int index) {
        while (!closed && realizedParts.size() <= index && cursor != null) {
            Message next = cursor.nextPartOrNull();
            if (next == null) {
                cursor = null;
                markTerminal();
                break;
            }
            realizedParts.add(next);
        }
    }

    private void materializeAllLocked() {
        while (!closed && cursor != null) {
            Message next = cursor.nextPartOrNull();
            if (next == null) {
                cursor = null;
                markTerminal();
                break;
            }
            realizedParts.add(next);
        }
    }

    private void markTerminal() {
        if (onTerminalState != null) {
            onTerminalState.run();
        }
    }

    private static void closeCursorQuietly(ContractAccess.ReceivedPartCursor cursor) {
        if (cursor == null)
            return;
        try {
            cursor.close();
        } catch (RuntimeException ignored) {
        }
    }
}
