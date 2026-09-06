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
import java.util.concurrent.CompletionStage;
import java.util.function.BiConsumer;
import java.util.function.Consumer;
import java.util.function.Function;

/**
 * Aggregates one recv result, including an optional routing id and the owned
 * message parts.
 *
 * <p>The returned {@link #parts()} view is immutable and does not copy the
 * underlying part array. Closing the aggregate closes every owned part.
 */
public final class Received implements AutoCloseable {
    // Callers may pass the same Received instance to multiple recv calls. The
    // binding refills internal state in place, avoiding a per-recv envelope.
    private long replyTokenValue;
    private boolean hasReplyToken;
    private ReplyToken replyToken;
    private BiConsumer<List<Message>, SendFlags> replySender;
    private Function<List<Message>, CompletionStage<Void>> sendSubmitter;
    private Consumer<List<Message>> sendBlockingSubmitter;
    private ContractAccess.RoutedReplyInvoker routedReplySender;
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
                                   long replyTokenValue, boolean hasReplyToken,
                                   BiConsumer<List<Message>, SendFlags> replySender) {
                return new Received(routingId, parts, trustedParts,
                    replyTokenValue, hasReplyToken, replySender);
            }

            @Override
            public Received create(RoutingId routingId, Message[] parts,
                                   boolean trustedParts,
                                   long replyTokenValue, boolean hasReplyToken,
                                   BiConsumer<List<Message>, SendFlags> replySender,
                                   Runnable onTerminalState) {
                return new Received(routingId, parts, trustedParts,
                    replyTokenValue, hasReplyToken, replySender, onTerminalState);
            }

            @Override
            public Received create(byte[] routingIdBytes, Message[] parts,
                                   boolean trustedParts,
                                   long replyTokenValue, boolean hasReplyToken,
                                   BiConsumer<List<Message>, SendFlags> replySender,
                                   Runnable onTerminalState) {
                return new Received(routingIdBytes, parts, trustedParts,
                    replyTokenValue, hasReplyToken, replySender,
                    onTerminalState);
            }

            @Override
            public Received create(RoutingId routingId, Message[] parts,
                                   long replyTokenValue,
                                   boolean hasReplyToken,
                                   BiConsumer<List<Message>, SendFlags> replySender) {
                return new Received(routingId, parts, true,
                    replyTokenValue, hasReplyToken, replySender);
            }

            @Override
            public Received createLazy(byte[] routingIdBytes, Message firstPart,
                                       ContractAccess.ReceivedPartCursor cursor,
                                       long replyTokenValue, boolean hasReplyToken,
                                       BiConsumer<List<Message>, SendFlags> replySender,
                                       Runnable onTerminalState) {
                return new Received(routingIdBytes, firstPart, cursor,
                    replyTokenValue, hasReplyToken, replySender,
                    onTerminalState);
            }

            @Override
            public Received createLazy(RoutingId routingId, Message firstPart,
                                       ContractAccess.ReceivedPartCursor cursor,
                                       long replyTokenValue, boolean hasReplyToken,
                                       BiConsumer<List<Message>, SendFlags> replySender,
                                       Runnable onTerminalState) {
                return new Received(routingId, firstPart, cursor,
                    replyTokenValue, hasReplyToken, replySender, onTerminalState);
            }

            @Override
            public void populateRoutedSinglePart(Received received,
                                                 byte[] routingIdBytes,
                                                 Message singlePart,
                                                 long replyTokenValue,
                                                 boolean hasReplyToken,
                                                 BiConsumer<List<Message>,
                                                     SendFlags> replySender,
                                                 Runnable onTerminalState) {
                received.populateRoutedSinglePart(routingIdBytes, singlePart,
                    replyTokenValue, hasReplyToken,
                    replySender, onTerminalState);
            }

            @Override
            public void populateRoutedTwoParts(Received received,
                                               byte[] routingIdBytes,
                                               Message firstPart,
                                               Message secondPart,
                                               long replyTokenValue,
                                               boolean hasReplyToken,
                                               BiConsumer<List<Message>,
                                                   SendFlags> replySender,
                                               Runnable onTerminalState) {
                received.populateRoutedTwoParts(routingIdBytes, firstPart,
                    secondPart, replyTokenValue, hasReplyToken,
                    replySender, onTerminalState);
            }

            @Override
            public void populateRoutedParts(Received received,
                                            byte[] routingIdBytes,
                                            Message[] parts,
                                            long replyTokenValue,
                                            boolean hasReplyToken,
                                            BiConsumer<List<Message>, SendFlags> replySender,
                                            Runnable onTerminalState) {
                received.populateRoutedParts(routingIdBytes, parts,
                    replyTokenValue, hasReplyToken, replySender, onTerminalState);
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
            public void setSendSubmitters(
              Received received,
              Function<List<Message>, CompletionStage<Void>> submitter,
              Consumer<List<Message>> blockingSubmitter) {
                received.setSendSubmitters(submitter, blockingSubmitter);
            }

            @Override
            public void setRoutedReplySender(
              Received received,
              ContractAccess.RoutedReplyInvoker replySender) {
                received.setRoutedReplySender(replySender);
            }

            @Override
            public boolean hasRoutingIdBytes(Received received) {
                return received.routingIdBytes != null;
            }

            @Override
            public void adoptFrom(Received target, Received source) {
                target.adoptFrom(source);
            }

            @Override
            public void setReplyTokenOwner(Received received, Object owner) {
                received.replyToken = received.hasReplyToken
                    ? ContractAccess.replyToken(owner,
                        received.replyTokenValue) : null;
            }
        });
    }

    /**
     * Create an empty {@code Received} for caller-provided storage. Hand the
     * same instance to {@code recv(Received, ...)} across calls to avoid the
     * per-recv allocation; the binding overwrites the internal state on each
     * successful receive.
     */
    public Received() {
        this.replyTokenValue = 0L;
        this.hasReplyToken = false;
        this.replySender = null;
        this.sendSubmitter = null;
        this.sendBlockingSubmitter = null;
        this.routedReplySender = null;
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
                                  long replyTokenValue,
                                  boolean hasReplyToken,
                                  BiConsumer<List<Message>, SendFlags> replySender,
                                  Runnable onTerminalState) {
        Objects.requireNonNull(singlePart, "singlePart");
        if (routingIdBytes == null
            && replyTokenValue == 0L
            && !hasReplyToken
            && replySender == null
            && onTerminalState == null
            && isReusablePlainSinglePartState()) {
            // HOT PATH: a caller-provided Received repeatedly filled by a
            // plain DEALER/PAIR recv has no routing or request state. Replace
            // only the owned frame; a result previously filled by another
            // socket shape continues through the complete reset below.
            Message previous = realizedParts.set(0, singlePart);
            partsView = null;
            if (previous != singlePart) {
                try { previous.closeFromOwner(); } catch (RuntimeException ignored) {}
            }
            return;
        }
        prepareRoutedStorage(routingIdBytes, 1, replyTokenValue, hasReplyToken,
            replySender, onTerminalState).add(singlePart);
    }

    /** Fill caller-owned receive storage directly for the common 2-part case. */
    void populateRoutedTwoParts(byte[] routingIdBytes,
                                Message firstPart,
                                Message secondPart,
                                long replyTokenValue,
                                boolean hasReplyToken,
                                BiConsumer<List<Message>, SendFlags> replySender,
                                Runnable onTerminalState) {
        Objects.requireNonNull(firstPart, "firstPart");
        Objects.requireNonNull(secondPart, "secondPart");

        ArrayList<Message> storage = prepareRoutedStorage(routingIdBytes, 2,
            replyTokenValue, hasReplyToken, replySender, onTerminalState);
        storage.add(firstPart);
        storage.add(secondPart);
    }

    void populateRoutedParts(byte[] routingIdBytes, Message[] parts,
                             long replyTokenValue, boolean hasReplyToken,
                             BiConsumer<List<Message>, SendFlags> replySender,
                             Runnable onTerminalState) {
        Objects.requireNonNull(parts, "parts");
        ArrayList<Message> storage = prepareRoutedStorage(routingIdBytes,
            parts.length, replyTokenValue, hasReplyToken, replySender,
            onTerminalState);
        for (Message part : parts)
            storage.add(part);
    }

    private ArrayList<Message> prepareRoutedStorage(byte[] routingIdBytes,
            int partCount, long replyTokenValue, boolean hasReplyToken,
            BiConsumer<List<Message>, SendFlags> replySender,
            Runnable onTerminalState) {
        ArrayList<Message> storage = realizedParts;
        realizedParts = null;
        partsView = null;
        if (storage != null) {
            for (int i = 0; i < storage.size(); i++) {
                Message previous = storage.get(i);
                if (previous != null) {
                    try {
                        previous.closeFromOwner();
                    } catch (RuntimeException ignored) {
                    }
                }
            }
            storage.clear();
        }
        ContractAccess.ReceivedPartCursor pendingCursor = cursor;
        cursor = null;
        closeCursorQuietly(pendingCursor);

        closed = false;
        routingId = null;
        this.routingIdBytes = routingIdBytes;
        this.replyTokenValue = replyTokenValue;
        this.hasReplyToken = hasReplyToken;
        this.replySender = replySender;
        sendSubmitter = null;
        sendBlockingSubmitter = null;
        routedReplySender = null;
        this.onTerminalState = onTerminalState;
        if (storage == null) {
            storage = new ArrayList<>(partCount);
        }
        realizedParts = storage;
        return storage;
    }

    private boolean isReusablePlainSinglePartState() {
        return !closed
            && routingId == null
            && routingIdBytes == null
            && replyTokenValue == 0L
            && !hasReplyToken
            && replySender == null
            && sendSubmitter == null
            && sendBlockingSubmitter == null
            && routedReplySender == null
            && onTerminalState == null
            && cursor == null
            && realizedParts != null
            && realizedParts.size() == 1;
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

        this.replyTokenValue = source.replyTokenValue;
        this.hasReplyToken = source.hasReplyToken;
        this.replySender = source.replySender;
        this.sendSubmitter = source.sendSubmitter;
        this.sendBlockingSubmitter = source.sendBlockingSubmitter;
        this.routedReplySender = source.routedReplySender;
        this.routingIdBytes = source.routingIdBytes;
        this.onTerminalState = source.onTerminalState;
        this.realizedParts = source.realizedParts;
        this.cursor = source.cursor;
        this.routingId = source.routingId;
        this.partsView = source.partsView;

        // Detach source so its own close() / finalizer is a no-op.
        source.replyTokenValue = 0L;
        source.hasReplyToken = false;
        source.replySender = null;
        source.sendSubmitter = null;
        source.sendBlockingSubmitter = null;
        source.routedReplySender = null;
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
             long replyTokenValue,
             boolean hasReplyToken,
             BiConsumer<List<Message>, SendFlags> replySender) {
        this(routingId, parts, trustedParts, replyTokenValue,
            hasReplyToken, replySender, null);
    }

    Received(RoutingId routingId, Message[] parts, boolean trustedParts,
             long replyTokenValue,
             boolean hasReplyToken,
             BiConsumer<List<Message>, SendFlags> replySender,
             Runnable onTerminalState) {
        this.routingId = routingId;
        this.routingIdBytes = null;
        this.replyTokenValue = replyTokenValue;
        this.hasReplyToken = hasReplyToken;
        this.replySender = replySender;
        this.sendSubmitter = null;
        this.sendBlockingSubmitter = null;
        this.onTerminalState = onTerminalState;
        Message[] ownedParts = Objects.requireNonNull(parts, "parts");
        Message[] safeParts = trustedParts ? ownedParts
            : Arrays.copyOf(ownedParts, ownedParts.length);
        this.realizedParts = new ArrayList<>(Math.max(1, safeParts.length));
        Collections.addAll(this.realizedParts, safeParts);
        this.cursor = null;
    }

    Received(byte[] routingIdBytes, Message[] parts, boolean trustedParts,
             long replyTokenValue,
             boolean hasReplyToken,
             BiConsumer<List<Message>, SendFlags> replySender) {
        this(routingIdBytes, parts, trustedParts, replyTokenValue,
            hasReplyToken, replySender, null);
    }

    Received(byte[] routingIdBytes, Message[] parts, boolean trustedParts,
             long replyTokenValue,
             boolean hasReplyToken,
             BiConsumer<List<Message>, SendFlags> replySender,
             Runnable onTerminalState) {
        this.routingId = null;
        this.routingIdBytes = routingIdBytes;
        this.replyTokenValue = replyTokenValue;
        this.hasReplyToken = hasReplyToken;
        this.replySender = replySender;
        this.sendSubmitter = null;
        this.sendBlockingSubmitter = null;
        this.onTerminalState = onTerminalState;
        Message[] ownedParts = Objects.requireNonNull(parts, "parts");
        Message[] safeParts = trustedParts ? ownedParts
            : Arrays.copyOf(ownedParts, ownedParts.length);
        this.realizedParts = new ArrayList<>(Math.max(1, safeParts.length));
        Collections.addAll(this.realizedParts, safeParts);
        this.cursor = null;
    }

    Received(RoutingId routingId, Message singlePart,
             long replyTokenValue, boolean hasReplyToken,
             BiConsumer<List<Message>, SendFlags> replySender) {
        this(routingId, singlePart, replyTokenValue, hasReplyToken,
            replySender, null);
    }

    Received(RoutingId routingId, Message singlePart,
             long replyTokenValue, boolean hasReplyToken,
             BiConsumer<List<Message>, SendFlags> replySender,
             Runnable onTerminalState) {
        this.routingId = routingId;
        this.routingIdBytes = null;
        this.replyTokenValue = replyTokenValue;
        this.hasReplyToken = hasReplyToken;
        this.replySender = replySender;
        this.sendSubmitter = null;
        this.sendBlockingSubmitter = null;
        this.onTerminalState = onTerminalState;
        this.realizedParts = new ArrayList<>(1);
        this.realizedParts.add(Objects.requireNonNull(singlePart, "singlePart"));
        this.cursor = null;
    }

    Received(byte[] routingIdBytes, Message singlePart,
             long replyTokenValue, boolean hasReplyToken,
             BiConsumer<List<Message>, SendFlags> replySender) {
        this(routingIdBytes, singlePart, replyTokenValue,
            hasReplyToken, replySender, null);
    }

    Received(byte[] routingIdBytes, Message singlePart,
             long replyTokenValue, boolean hasReplyToken,
             BiConsumer<List<Message>, SendFlags> replySender,
             Runnable onTerminalState) {
        this.routingId = null;
        this.routingIdBytes = routingIdBytes;
        this.replyTokenValue = replyTokenValue;
        this.hasReplyToken = hasReplyToken;
        this.replySender = replySender;
        this.sendSubmitter = null;
        this.sendBlockingSubmitter = null;
        this.onTerminalState = onTerminalState;
        this.realizedParts = new ArrayList<>(1);
        this.realizedParts.add(Objects.requireNonNull(singlePart, "singlePart"));
        this.cursor = null;
    }

    Received(byte[] routingIdBytes, Message firstPart,
             ContractAccess.ReceivedPartCursor cursor, long replyTokenValue,
             boolean hasReplyToken,
             BiConsumer<List<Message>, SendFlags> replySender,
             Runnable onTerminalState) {
        this.routingId = null;
        this.routingIdBytes = routingIdBytes;
        this.replyTokenValue = replyTokenValue;
        this.hasReplyToken = hasReplyToken;
        this.replySender = replySender;
        this.sendSubmitter = null;
        this.sendBlockingSubmitter = null;
        this.onTerminalState = onTerminalState;
        this.realizedParts = new ArrayList<>(4);
        this.realizedParts.add(Objects.requireNonNull(firstPart, "firstPart"));
        this.cursor = cursor;
    }

    Received(RoutingId routingId, Message firstPart,
             ContractAccess.ReceivedPartCursor cursor, long replyTokenValue,
             boolean hasReplyToken,
             BiConsumer<List<Message>, SendFlags> replySender,
             Runnable onTerminalState) {
        this.routingId = routingId;
        this.routingIdBytes = null;
        this.replyTokenValue = replyTokenValue;
        this.hasReplyToken = hasReplyToken;
        this.replySender = replySender;
        this.sendSubmitter = null;
        this.sendBlockingSubmitter = null;
        this.onTerminalState = onTerminalState;
        this.realizedParts = new ArrayList<>(4);
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
        ensureOpen();
        materializeAll();
        if (partsView == null) {
            partsView = Collections.unmodifiableList(realizedParts);
        }
        return partsView;
    }

    public Optional<ReplyToken> replyToken() {
        ensureOpen();
        return Optional.ofNullable(replyToken);
    }

    /** Returns whether exactly one payload part was received. */
    public boolean isSinglePart() {
        ArrayList<Message> parts = realizedParts;
        if (!closed && cursor == null && parts != null) {
            return parts.size() == 1;
        }
        ensureOpen();
        ensureRealizedThrough(1);
        return realizedParts.size() == 1 && cursor == null;
    }

    /** Returns the first payload part. */
    public Message firstPart() {
        ArrayList<Message> parts = realizedParts;
        if (!closed && cursor == null && parts != null && !parts.isEmpty()) {
            return parts.get(0);
        }
        ensureOpen();
        ensureRealizedThrough(0);
        if (realizedParts.isEmpty())
            throw new ZlinkRecvException(RecvResult.NO_DATA);
        return realizedParts.get(0);
    }

    /** Returns the only payload part or throws when the result is multipart. */
    public Message singlePartOrThrow() {
        ArrayList<Message> parts = realizedParts;
        if (!closed && cursor == null && parts != null && parts.size() == 1) {
            return parts.get(0);
        }
        ensureOpen();
        ensureRealizedThrough(1);
        if (realizedParts.size() != 1 || cursor != null)
            throw new ZlinkRecvException(RecvResult.NOT_SUPPORTED);
        return realizedParts.get(0);
    }

    public ReplyOperation reply() {
        return new ReplyBuilder();
    }

    private void submitReply(List<Message> parts, SendFlags flags) {
        Objects.requireNonNull(flags, "flags");
        if (!hasReplyToken
            || (replySender == null && routedReplySender == null)) {
            throw new ZlinkSubmitException(SubmitResult.INVALID_STATE);
        }
        try {
            if (routedReplySender != null && routingIdBytes != null) {
                routedReplySender.submit(routingIdBytes, replyTokenValue,
                    Objects.requireNonNull(parts, "parts"));
                return;
            }
            replySender.accept(Objects.requireNonNull(parts, "parts"),
                flags);
        } catch (IllegalStateException ex) {
            throw new ZlinkSubmitException(SubmitResult.TERMINATED);
        }
    }

    public SendOperation send() {
        return new SendBuilder();
    }

    void setSendSubmitters(
      Function<List<Message>, CompletionStage<Void>> submitter,
      Consumer<List<Message>> blockingSubmitter) {
        this.sendSubmitter = Objects.requireNonNull(submitter, "submitter");
        this.sendBlockingSubmitter = Objects.requireNonNull(
            blockingSubmitter, "blockingSubmitter");
    }

    void setRoutedReplySender(
      ContractAccess.RoutedReplyInvoker replySender) {
        this.routedReplySender = replySender;
    }

    private final class SendBuilder implements SendOperation, SendSubmitOperation {
        private Message singlePart;
        private ArrayList<Message> parts;
        private boolean submitted;

        @Override
        public SendSubmitOperation message(Message part) {
            ensureNotSubmitted();
            Objects.requireNonNull(part, "part");
            if (parts != null) {
                parts.add(part);
            } else if (singlePart == null) {
                singlePart = part;
            } else {
                parts = new ArrayList<>(4);
                parts.add(singlePart);
                parts.add(part);
                singlePart = null;
            }
            return this;
        }

        @Override
        public CompletionStage<Void> submit() {
            List<Message> messages = beginSubmit();
            if (sendSubmitter == null)
                throw new ZlinkSubmitException(SubmitResult.INVALID_STATE);
            try {
                return sendSubmitter.apply(messages);
            } catch (IllegalStateException ex) {
                throw new ZlinkSubmitException(SubmitResult.TERMINATED);
            }
        }

        @Override
        public void submit_sync() {
            List<Message> messages = beginSubmit();
            if (sendBlockingSubmitter == null)
                throw new ZlinkSubmitException(SubmitResult.INVALID_STATE);
            try {
                sendBlockingSubmitter.accept(messages);
            } catch (IllegalStateException ex) {
                throw new ZlinkSubmitException(SubmitResult.TERMINATED);
            }
        }

        private List<Message> beginSubmit() {
            ensureNotSubmitted();
            if (singlePart == null && (parts == null || parts.isEmpty()))
                throw new IllegalArgumentException("at least one message required");
            submitted = true;
            return parts();
        }

        private List<Message> parts() {
            return parts == null ? List.of(singlePart) : parts;
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
            try {
                part.closeFromOwner();
            } catch (RuntimeException ignored) {
            }
            return;
        }

        ContractAccess.ReceivedPartCursor pendingCursor;
        Message singleToClose = null;
        List<Message> toClose = null;
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
                // Detach the owned storage before invoking Message.close().
                // The list is already private to this Received, so copying
                // every multipart receive only adds an array and list
                // allocation without strengthening the ownership boundary.
                toClose = realizedParts;
            }
            realizedParts = null;
        }
        partsView = Collections.emptyList();
        if (singleToClose != null) {
            try {
                singleToClose.closeFromOwner();
            } catch (RuntimeException ignored) {
            }
        } else if (toClose != null) {
            closeOwnedParts(toClose);
        }
        closeCursorQuietly(pendingCursor);
        markTerminal();
    }

    private static void closeOwnedParts(List<Message> parts) {
        for (Message part : parts) {
            if (part != null) {
                try {
                    part.closeFromOwner();
                } catch (RuntimeException ignored) {
                }
            }
        }
    }

    Iterator<Message> iterator() {
        return new Iterator<>() {
            private int index;

            @Override
            public boolean hasNext() {
                if (closed)
                    return false;
                ensureRealizedThrough(index);
                return index < realizedParts.size();
            }

            @Override
            public Message next() {
                ensureOpen();
                ensureRealizedThrough(index);
                if (index >= realizedParts.size())
                    throw new NoSuchElementException();
                Message next = realizedParts.get(index);
                index++;
                return next;
            }
        };
    }

    void forceMaterialize() {
        if (closed)
            return;
        materializeAll();
    }

    List<Message> takeParts() {
        ContractAccess.ReceivedPartCursor pendingCursor;
        ArrayList<Message> detached;
        ensureOpen();
        materializeAll();
        detached = new ArrayList<>(realizedParts);
        realizedParts = null;
        partsView = Collections.emptyList();
        pendingCursor = cursor;
        cursor = null;
        closed = true;
        closeCursorQuietly(pendingCursor);
        markTerminal();
        return Collections.unmodifiableList(detached);
    }

    private void ensureOpen() {
        if (closed)
            throw new IllegalStateException("received is closed");
    }

    private void ensureRealizedThrough(int index) {
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

    private void materializeAll() {
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
