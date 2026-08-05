/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.internal.ContractAccess;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.SubscriptionEntry;
import systems.zlink.contracts.messaging.SubscriptionEvent;
import systems.zlink.contracts.messaging.TopicMessage;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.SendResult;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeErrno;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.NativeRoutingIds;
import systems.zlink.runtime.nativeapi.RecvScratch;

final class TopicPlane {
    private final NativeSocketRuntime socket;

    TopicPlane(NativeSocketRuntime socket) {
        this.socket = socket;
    }

    void publish(String topicId, Message part) {
        publish(topicId, part, SendFlag.NONE);
    }

    void publish(String topicId, Message part, SendFlag flags) {
        Objects.requireNonNull(part, "part");
        validateTopicUtf8(topicId, "topicId");
        socket.publishMessageFrame(topicId, part, flags);
    }

    void publish(String topicId, List<Message> parts) {
        publish(topicId, parts, SendFlag.NONE);
    }

    void publish(String topicId, List<Message> parts, SendFlag flags) {
        Objects.requireNonNull(topicId, "topicId");
        Objects.requireNonNull(parts, "parts");
        Objects.requireNonNull(flags, "flags");
        validateTopicUtf8(topicId, "topicId");
        socket.publishParts(topicId, parts, flags, false);
    }

    SendResult publishNoWaitResult(String topicId, Message part) {
        Objects.requireNonNull(part, "part");
        validateTopicUtf8(topicId, "topicId");
        return socket.publishMessageFrameNoWaitResult(topicId, part);
    }

    SendResult publishNoWaitResult(String topicId, List<Message> parts) {
        Objects.requireNonNull(topicId, "topicId");
        Objects.requireNonNull(parts, "parts");
        validateTopicUtf8(topicId, "topicId");
        return socket.publishNoWaitPartsResult(topicId, parts);
    }

    TopicMessage subscribe() {
        return subscribe(ReceiveFlag.NONE);
    }

    TopicMessage subscribe(ReceiveFlag flags) {
        Objects.requireNonNull(flags, "flags");
        TopicMessage message = subscribeInternal(flags,
            flags == ReceiveFlag.DONTWAIT);
        if (message == null) {
            throw new ZlinkRecvException(RecvResult.NO_DATA, Native.errno());
        }
        return message;
    }

    Optional<TopicMessage> subscribeNoWait() {
        return Optional.ofNullable(subscribeInternal(ReceiveFlag.DONTWAIT,
            true));
    }

    boolean subscribe(TopicMessage result, ReceiveFlag flags) {
        Objects.requireNonNull(result, "result");
        if (flags == ReceiveFlag.DONTWAIT) {
            return subscribeIntoFastNoWait(result);
        }
        TopicMessage fresh = subscribe(flags);
        if (fresh == null)
            return false;
        ContractAccess.topicMessageAdoptFrom(result, fresh);
        return true;
    }

    SubscriptionEvent subscriptionEvent(ReceiveFlag flags) {
        Objects.requireNonNull(flags, "flags");
        socket.ensureOpen();
        socket.prepareRecvLikeOperation();
        RecvScratch scratch = socket.recvScratch();
        int rc = NativeErrno.retryWhileInterrupted(() -> {
            resetSubscriptionScratch(scratch);
            return Native.subscriptionEvent(socket.handle(),
                scratch.routingIdOut, scratch.subscribedOut,
                scratch.topicOut, scratch.topicLenOut, flags.getValue());
        }, result -> result != 0);
        if (rc != 0) {
            int errno = Native.errno();
            if (flags == ReceiveFlag.DONTWAIT
                && (errno == NativeErrno.EAGAIN
                    || errno == NativeErrno.EWOULDBLOCK_WIN)) {
                throw new ZlinkRecvException(RecvResult.NO_DATA, errno);
            }
            throw ZlinkException.fromErrno(
                systems.zlink.contracts.errors.ErrorCategory.RECV, errno);
        }
        return subscriptionEventFromNative(scratch);
    }

    Optional<SubscriptionEvent> trySubscriptionEvent() {
        socket.ensureOpen();
        socket.prepareRecvLikeOperation();
        RecvScratch scratch = socket.recvScratch();
        int rc = NativeErrno.retryWhileInterrupted(() -> {
            resetSubscriptionScratch(scratch);
            return Native.subscriptionEvent(socket.handle(),
                scratch.routingIdOut, scratch.subscribedOut, scratch.topicOut,
                scratch.topicLenOut, ReceiveFlag.DONTWAIT.getValue());
        }, result -> result != 0);
        if (rc == 0) {
            return Optional.of(subscriptionEventFromNative(scratch));
        }
        int errno = Native.errno();
        if (errno == NativeErrno.EAGAIN
            || errno == NativeErrno.EWOULDBLOCK_WIN) {
            return Optional.empty();
        }
        throw ZlinkException.fromLastError(
            systems.zlink.contracts.errors.ErrorCategory.RECV);
    }

    private TopicMessage subscribeInternal(ReceiveFlag flags,
                                           boolean allowNoData) {
        socket.ensureOpen();
        socket.prepareRecvLikeOperation();
        RecvScratch scratch = socket.recvScratch();
        while (true) {
            scratch.topicLenOut.set(ValueLayout.JAVA_LONG, 0,
                RecvScratch.TOPIC_CAPACITY);
            ArrayList<Message> parts = new ArrayList<>();
            RoutingId routingId = null;
            String topicId = "";
            while (true) {
                Message part = new Message();
                boolean success = false;
                try {
                    int rc = Native.subscribePart(socket.handle(),
                        scratch.sourceRidOut, scratch.topicOut,
                        RecvScratch.TOPIC_CAPACITY, scratch.topicLenOut,
                        InternalAccess.messageNativeHandle(part),
                        scratch.hasMoreOut, flags.getValue());
                    if (rc == 0) {
                        success = true;
                        InternalAccess.messageFinishReceive(part,
                            scratch.hasMoreOut.get(ValueLayout.JAVA_INT, 0) != 0);
                        if (parts.isEmpty()) {
                            routingId = NativeRoutingIds.readOut(
                                scratch.sourceRidOut);
                            int topicLength =
                                NativeSocketRuntime.normalizeTopicLength(
                                    scratch.topicOut,
                                    RecvScratch.TOPIC_CAPACITY,
                                    scratch.topicLenOut.get(
                                        ValueLayout.JAVA_LONG, 0));
                            topicId = decodeTopicString(
                                scratch.topicOut, topicLength);
                        }
                        parts.add(part);
                        if (!part.more()) {
                            return ContractAccess.topicMessage(routingId,
                                topicId, parts.toArray(Message[]::new));
                        }
                        SubscribeRemainderResult remainder =
                            subscribeMultipartRemainder(
                            scratch, parts, routingId, topicId,
                            flags.getValue(), true, allowNoData);
                        if (remainder.isRestart()) {
                            break;
                        }
                        return remainder.message();
                    }
                } finally {
                    if (!success) {
                        try {
                            part.close();
                        } catch (RuntimeException ignored) {
                        }
                    }
                }

                int errno = Native.errno();
                Message.closeAll(parts);
                if (errno == NativeErrno.EINTR) {
                    break;
                }
                if (allowNoData
                    && (errno == NativeErrno.EAGAIN
                        || errno == NativeErrno.EWOULDBLOCK_WIN)) {
                    return null;
                }
                throw ZlinkException.fromLastError(
                    systems.zlink.contracts.errors.ErrorCategory.RECV);
            }
        }
    }

    // Non-allocating subscribe hot path for the DONT_WAIT single-part case
    // (the perf/streaming common path). Multipart payloads fall back to the
    // general allocating reader to preserve exact semantics.
    private boolean subscribeIntoFastNoWait(TopicMessage result) {
        socket.ensureOpen();
        socket.prepareRecvLikeOperation();
        RecvScratch scratch = socket.recvScratch();
        while (true) {
            scratch.topicLenOut.set(ValueLayout.JAVA_LONG, 0,
                RecvScratch.TOPIC_CAPACITY);
            Message part = new Message();
            boolean success = false;
            try {
                int rc = Native.subscribePartNoWaitCritical(socket.handle(),
                    scratch.sourceRidOut, scratch.topicOut,
                    RecvScratch.TOPIC_CAPACITY, scratch.topicLenOut,
                    InternalAccess.messageNativeHandle(part),
                    scratch.hasMoreOut, ReceiveFlag.DONTWAIT.getValue());
                if (rc == 0) {
                    boolean hasMore =
                        scratch.hasMoreOut.get(ValueLayout.JAVA_INT, 0) != 0;
                    InternalAccess.messageFinishReceive(part, hasMore);
                    if (hasMore) {
                        success = true;
                        TopicMessage fresh = subscribeAssembleRemainder(
                            scratch, part);
                        ContractAccess.topicMessageAdoptFrom(result, fresh);
                        return true;
                    }
                    RoutingId routingId = NativeRoutingIds.readOut(
                        scratch.sourceRidOut);
                    int topicLength = NativeSocketRuntime.normalizeTopicLength(
                        scratch.topicOut, RecvScratch.TOPIC_CAPACITY,
                        scratch.topicLenOut.get(ValueLayout.JAVA_LONG, 0));
                    String topicId = decodeTopicString(
                        scratch.topicOut, topicLength);
                    success = true;
                    InternalAccess.topicMessageAdoptSingle(result, routingId,
                        topicId, part);
                    return true;
                }
            } finally {
                if (!success) {
                    try {
                        part.close();
                    } catch (RuntimeException ignored) {
                    }
                }
            }

            int errno = Native.errno();
            if (errno == NativeErrno.EINTR) {
                continue;
            }
            if (errno == NativeErrno.EAGAIN
                || errno == NativeErrno.EWOULDBLOCK_WIN) {
                return false;
            }
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.RECV);
        }
    }

    private static String decodeTopicString(MemorySegment topicOut,
                                            int topicLength) {
        if (topicLength == 0) {
            return "";
        }
        byte[] raw = topicOut.asSlice(0, topicLength)
            .toArray(ValueLayout.JAVA_BYTE);
        return new String(raw, StandardCharsets.UTF_8);
    }

    private TopicMessage subscribeAssembleRemainder(RecvScratch scratch,
                                                    Message firstPart) {
        RoutingId routingId = NativeRoutingIds.readOut(scratch.sourceRidOut);
        int topicLength = NativeSocketRuntime.normalizeTopicLength(
            scratch.topicOut, RecvScratch.TOPIC_CAPACITY,
            scratch.topicLenOut.get(ValueLayout.JAVA_LONG, 0));
        String topicId = decodeTopicString(scratch.topicOut,
            topicLength);
        ArrayList<Message> parts = new ArrayList<>();
        parts.add(firstPart);
        return subscribeMultipartRemainder(scratch, parts, routingId, topicId,
            ReceiveFlag.NONE.getValue(), false, false).message();
    }

    private SubscribeRemainderResult subscribeMultipartRemainder(
        RecvScratch scratch,
        ArrayList<Message> parts,
        RoutingId routingId,
        String topicId,
        int flags,
        boolean restartOnEintr,
        boolean allowNoData) {
        while (true) {
            Message next = new Message();
            boolean ok = false;
            try {
                int rc = Native.subscribePart(socket.handle(),
                    scratch.sourceRidOut, scratch.topicOut,
                    RecvScratch.TOPIC_CAPACITY, scratch.topicLenOut,
                    InternalAccess.messageNativeHandle(next),
                    scratch.hasMoreOut, flags);
                if (rc == 0) {
                    ok = true;
                    boolean more =
                        scratch.hasMoreOut.get(ValueLayout.JAVA_INT, 0) != 0;
                    InternalAccess.messageFinishReceive(next, more);
                    parts.add(next);
                    if (!more) {
                        return SubscribeRemainderResult.message(
                            ContractAccess.topicMessage(routingId, topicId,
                                parts.toArray(Message[]::new)));
                    }
                    continue;
                }
            } finally {
                if (!ok) {
                    try {
                        next.close();
                    } catch (RuntimeException ignored) {
                    }
                }
            }
            int errno = Native.errno();
            if (errno == NativeErrno.EINTR) {
                if (!restartOnEintr) {
                    continue;
                }
                Message.closeAll(parts);
                return SubscribeRemainderResult.restart();
            }
            if (allowNoData
                && (errno == NativeErrno.EAGAIN
                    || errno == NativeErrno.EWOULDBLOCK_WIN)) {
                Message.closeAll(parts);
                return SubscribeRemainderResult.message(null);
            }
            Message.closeAll(parts);
            throw ZlinkException.fromLastError(
                systems.zlink.contracts.errors.ErrorCategory.RECV);
        }
    }

    private static final class SubscribeRemainderResult {
        private static final SubscribeRemainderResult RESTART =
            new SubscribeRemainderResult(null, true);

        private final TopicMessage message;
        private final boolean restart;

        private SubscribeRemainderResult(TopicMessage message,
                                         boolean restart) {
            this.message = message;
            this.restart = restart;
        }

        static SubscribeRemainderResult message(TopicMessage message) {
            return new SubscribeRemainderResult(message, false);
        }

        static SubscribeRemainderResult restart() {
            return RESTART;
        }

        TopicMessage message() {
            return message;
        }

        boolean isRestart() {
            return restart;
        }
    }

    void setRoutingId(RoutingId rid) {
        Objects.requireNonNull(rid, "rid");
        byte[] value = InternalAccess.routingIdTrustedBytes(rid);
        socket.setRoutingIdBytes(value, 0, value.length);
    }

    RoutingId getRoutingId() {
        return RoutingId.from(socket.getRoutingIdBytes());
    }

    void setSubscription(String filter) {
        Objects.requireNonNull(filter, "filter");
        byte[] bytes = filter.getBytes(StandardCharsets.UTF_8);
        validateFilterBytes(bytes.length, "filter");
        socket.setSubscriptionBytes(bytes, 0, bytes.length, true);
    }

    void setSubscription(byte[] filter) {
        Objects.requireNonNull(filter, "filter");
        validateFilterBytes(filter.length, "filter");
        socket.setSubscriptionBytes(filter, 0, filter.length, true);
    }

    void unsetSubscription(String filter) {
        Objects.requireNonNull(filter, "filter");
        byte[] bytes = filter.getBytes(StandardCharsets.UTF_8);
        validateFilterBytes(bytes.length, "filter");
        socket.setSubscriptionBytes(bytes, 0, bytes.length, false);
    }

    void unsetSubscription(byte[] filter) {
        Objects.requireNonNull(filter, "filter");
        validateFilterBytes(filter.length, "filter");
        socket.setSubscriptionBytes(filter, 0, filter.length, false);
    }

    List<SubscriptionEntry> subscriptions() {
        socket.ensureOpen();
        ArrayList<SubscriptionEntry> out = new ArrayList<>();
        int capacity = 64;
        try (Arena arena = Arena.ofConfined()) {
            for (long index = 0;; index++) {
                MemorySegment lenInOut = arena.allocate(ValueLayout.JAVA_LONG);
                MemorySegment isPatternOut = arena.allocate(
                    ValueLayout.JAVA_INT);
                byte[] filter = socket.subscriptionAt(index, lenInOut,
                    isPatternOut, capacity);
                if (filter == null) {
                    break;
                }
                capacity = Math.max(capacity, filter.length);
                out.add(SubscriptionEntry.fromBytes(filter,
                    isPatternOut.get(ValueLayout.JAVA_INT, 0) != 0));
            }
        }
        return List.copyOf(out);
    }

    private static SubscriptionEvent subscriptionEventFromNative(
        RecvScratch scratch) {
        int topicLength = NativeSocketRuntime.normalizeTopicLength(
            scratch.topicOut, RecvScratch.TOPIC_CAPACITY,
            scratch.topicLenOut.get(ValueLayout.JAVA_LONG, 0));
        String filter = decodeTopicString(scratch.topicOut,
            topicLength);
        return ContractAccess.subscriptionEvent(Optional.ofNullable(
            NativeRoutingIds.read(scratch.routingIdOut)),
            filter, scratch.subscribedOut.get(ValueLayout.JAVA_INT, 0) != 0);
    }

    private static void resetSubscriptionScratch(RecvScratch scratch) {
        scratch.routingIdOut.set(ValueLayout.JAVA_BYTE,
            NativeLayouts.ROUTING_ID_SIZE_OFFSET,
            (byte) 0);
        scratch.subscribedOut.set(ValueLayout.JAVA_INT, 0, 0);
        scratch.topicLenOut.set(ValueLayout.JAVA_LONG, 0,
            RecvScratch.TOPIC_CAPACITY);
    }

    private static void validateTopicUtf8(String topicId, String name) {
        int chars = topicId.length();
        if ((long) chars * 3L < NativeSocketRuntime.TOPIC_CAPACITY) {
            return;
        }
        int bytes = topicId.getBytes(StandardCharsets.UTF_8).length;
        validateFilterBytes(bytes, name);
    }

    private static void validateFilterBytes(int bytes, String name) {
        if (bytes >= NativeSocketRuntime.TOPIC_CAPACITY) {
            throw new IllegalArgumentException(name + " too long: " + bytes
                + " >= " + NativeSocketRuntime.TOPIC_CAPACITY);
        }
        if (bytes < 0) {
            throw new IllegalArgumentException(
                name + " length must be non-negative");
        }
    }
}
