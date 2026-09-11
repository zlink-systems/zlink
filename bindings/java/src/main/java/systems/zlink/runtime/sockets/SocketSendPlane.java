/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeErrno;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.NativeMessage;
import systems.zlink.runtime.nativeapi.NativeRoutingIds;
import systems.zlink.runtime.nativeapi.NativeSubmitErrors;
import systems.zlink.runtime.nativeapi.SendScratch;

final class SocketSendPlane {
    private final NativeSocketRuntime socket;
    private final ThreadLocal<SendScratch> sendScratch =
        ThreadLocal.withInitial(SendScratch::new);

    SocketSendPlane(NativeSocketRuntime socket) {
        this.socket = socket;
    }

    void sendMessageFrame(RoutingId routingId, Message message, SendFlag flag) {
        Objects.requireNonNull(routingId, "routingId");
        Objects.requireNonNull(message, "message");
        Objects.requireNonNull(flag, "flag");
        ensureBlockingSendAllowed(flag);
        WholeSubmitter submitter = sendSubmitter(routingId, flag.getValue());
        PartAttempt attempt = isDontWait(flag)
            ? submitTrackedNoWait(routingId, submitter, message)
            : socket.completionOwner().withNativeCall(
                () -> submitSingleAttempt(submitter, message));
        requireSuccess(attempt);
    }

    SendResult sendMessageFrameNoWaitResult(RoutingId routingId,
                                             Message message) {
        Objects.requireNonNull(routingId, "routingId");
        Objects.requireNonNull(message, "message");
        return sendResult(submitTrackedNoWait(routingId,
            sendSubmitter(routingId, SendFlag.DONTWAIT.getValue()), message));
    }

    boolean send(byte[] routingIdBytes, Message part, SendFlag flags) {
        Objects.requireNonNull(routingIdBytes, "routingIdBytes");
        Objects.requireNonNull(part, "part");
        Objects.requireNonNull(flags, "flags");
        ensureBlockingSendAllowed(flags);
        RoutingId target = RoutingId.from(routingIdBytes);
        PartAttempt attempt = isDontWait(flags)
            ? submitTrackedNoWait(target,
                sendSubmitter(target, flags.getValue()), part)
            : socket.completionOwner().withNativeCall(() ->
                submitSingleAttempt(sendSubmitter(target, flags.getValue()),
                    part));
        if (attempt.result() == SubmitResult.OK.value()) {
            return true;
        }
        if (isDontWait(flags)
            && attempt.result() == SubmitResult.BACKPRESSURED.value()
            && isWouldBlock(attempt.errno())) {
            return false;
        }
        throwFailure(attempt);
        return false;
    }

    void send(int rid, Message part, SendFlag flags) {
        Objects.requireNonNull(part, "part");
        Objects.requireNonNull(flags, "flags");
        ensureBlockingSendAllowed(flags);
        RoutingId target = RoutingId.from(Integer.toUnsignedLong(rid));
        WholeSubmitter submitter = sendSubmitter(target, flags.getValue());
        PartAttempt attempt = isDontWait(flags)
            ? submitTrackedNoWait(target, submitter, part)
            : socket.completionOwner().withNativeCall(
                () -> submitSingleAttempt(submitter, part));
        requireSuccess(attempt);
    }

    int send(int rid, MemorySegment payload, int length, int sendFlags) {
        Objects.requireNonNull(payload, "payload");
        return sendCopied(rid, payload, length, sendFlags);
    }

    int sendCopied(int rid, MemorySegment payload, int length, int sendFlags) {
        Objects.requireNonNull(payload, "payload");
        SendFlag flag = SendFlag.fromValue(sendFlags);
        ensureBlockingSendAllowed(flag);
        SendScratch scratch = sendScratch.get();
        MemorySegment nativeMsg = scratch.nativeMsg;
        int rc = NativeMessage.messageInitSize(nativeMsg, length);
        if (rc != 0) {
            throw ZlinkException.fromLastError(
                systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
        if (length > 0) {
            MemorySegment.copy(payload, 0,
                NativeMessage.messageData(nativeMsg),
                0, length);
        }
        boolean[] submitted = {false};
        try {
            RoutingId target = RoutingId.from(Integer.toUnsignedLong(rid));
            MemorySegment nativeTarget = nativeRoutingId(scratch, target);
            PartAttempt attempt;
            if (isDontWait(flag)) {
                CompletionOwner.NoWaitAttempt tracked =
                    socket.completionOwner().trackNoWaitSend(target,
                        (context, idOut) -> {
                            submitted[0] = true;
                            return Native.sendRidNoWaitCritical(
                                socket.handle(), nativeTarget, nativeMsg, 1L,
                                flag.getValue(), context, idOut);
                        });
                attempt = new PartAttempt(tracked.result(), tracked.errno());
            } else {
                attempt = socket.completionOwner().withNativeCall(() -> {
                    submitted[0] = true;
                    int result = Native.sendRid(socket.handle(), nativeTarget,
                        nativeMsg, 1L, flag.getValue(), MemorySegment.NULL,
                        MemorySegment.NULL);
                    int errno = result == SubmitResult.OK.value()
                        ? 0 : Native.errno();
                    return new PartAttempt(result, errno);
                });
            }
            requireSuccess(attempt);
        } finally {
            if (!submitted[0]) {
                try {
                    NativeMessage.messageClose(nativeMsg);
                } catch (RuntimeException ignored) {
                }
            }
        }
        return length;
    }

    void publishMessageFrame(String topicId, Message message, SendFlag flags) {
        Objects.requireNonNull(topicId, "topicId");
        Objects.requireNonNull(message, "message");
        Objects.requireNonNull(flags, "flags");
        ensureBlockingSendAllowed(flags);
        requireSuccess(submitSingleAttempt(
            publishSubmitter(topicId, flags.getValue()), message));
    }

    SendResult publishMessageFrameNoWaitResult(String topicId,
                                                Message message) {
        Objects.requireNonNull(topicId, "topicId");
        Objects.requireNonNull(message, "message");
        return sendResult(submitSingleAttempt(
            publishSubmitter(topicId, SendFlag.DONTWAIT.getValue()), message));
    }

    void sendMessageFrame(Message message, SendFlag flag) {
        Objects.requireNonNull(message, "message");
        Objects.requireNonNull(flag, "flag");
        ensureBlockingSendAllowed(flag);
        WholeSubmitter submitter = sendSubmitter(null, flag.getValue());
        PartAttempt attempt = isDontWait(flag)
            ? submitTrackedNoWait(null, submitter, message)
            : socket.completionOwner().withNativeCall(
                () -> submitSingleAttempt(submitter, message));
        requireSuccess(attempt);
    }

    boolean sendMessageFrameNoWaitResult(Message message, SendFlag flag) {
        Objects.requireNonNull(message, "message");
        Objects.requireNonNull(flag, "flag");
        WholeSubmitter submitter = sendSubmitter(null, flag.getValue());
        PartAttempt attempt = isDontWait(flag)
            ? submitTrackedNoWait(null, submitter, message)
            : socket.completionOwner().withNativeCall(
                () -> submitSingleAttempt(submitter, message));
        if (attempt.result() == SubmitResult.OK.value()) {
            return true;
        }
        if (isDontWait(flag)
            && attempt.result() == SubmitResult.BACKPRESSURED.value()
            && isWouldBlock(attempt.errno())) {
            return false;
        }
        throwFailure(attempt);
        return false;
    }

    SendResult sendMessageFrameNoWaitResult(Message message) {
        Objects.requireNonNull(message, "message");
        return sendResult(submitTrackedNoWait(null,
            sendSubmitter(null, SendFlag.DONTWAIT.getValue()), message));
    }

    void sendParts(RoutingId routingId, List<Message> parts,
                   SendFlag flags, boolean nonBlocking) {
        socket.ensureOpen();
        validateParts(parts);
        ensureBlockingSendAllowed(flags);
        WholeSubmitter submitter = sendSubmitter(routingId, flags.getValue());
        PartAttempt attempt = isDontWait(flags)
            ? submitTrackedNoWait(routingId, submitter, parts)
            : socket.completionOwner().withNativeCall(
                () -> submitPartsAttempt(submitter, parts));
        requireSuccess(attempt);
    }

    SendResult sendNoWaitPartsResult(RoutingId routingId,
                                     List<Message> parts) {
        socket.ensureOpen();
        validateParts(parts);
        return sendResult(submitTrackedNoWait(routingId,
            sendSubmitter(routingId, SendFlag.DONTWAIT.getValue()), parts));
    }

    void publishParts(String topicId, List<Message> parts,
                      SendFlag flags, boolean nonBlocking) {
        socket.ensureOpen();
        validateParts(parts);
        ensureBlockingSendAllowed(flags);
        requireSuccess(submitPartsAttempt(
            publishSubmitter(topicId, flags.getValue()), parts));
    }

    SendResult publishNoWaitPartsResult(String topicId,
                                        List<Message> parts) {
        socket.ensureOpen();
        validateParts(parts);
        return sendResult(submitPartsAttempt(
            publishSubmitter(topicId, SendFlag.DONTWAIT.getValue()), parts));
    }

    private WholeSubmitter sendSubmitter(RoutingId routingId, int flags) {
        SendScratch scratch = sendScratch.get();
        MemorySegment nativeRoutingId = routingId == null
            ? MemorySegment.NULL : nativeRoutingId(scratch, routingId);
        boolean critical = (flags & SendFlag.DONTWAIT.getValue()) != 0;
        if (nativeRoutingId.address() == 0L) {
            return (parts, count, context, idOut) -> critical
                ? Native.sendNoWaitCritical(socket.handle(), parts, count,
                    flags, context, idOut)
                : Native.send(socket.handle(), parts, count, flags, context,
                    idOut);
        }
        return (parts, count, context, idOut) -> critical
            ? Native.sendRidNoWaitCritical(socket.handle(), nativeRoutingId,
                parts, count, flags, context, idOut)
            : Native.sendRid(socket.handle(), nativeRoutingId, parts, count,
                flags, context, idOut);
    }

    private WholeSubmitter publishSubmitter(String topicId, int flags) {
        MemorySegment nativeTopic = nativeTopic(sendScratch.get(), topicId);
        boolean critical = (flags & SendFlag.DONTWAIT.getValue()) != 0;
        return (parts, count, context, idOut) -> critical
            ? Native.publishNoWaitCritical(socket.handle(), nativeTopic,
                parts, count, flags)
            : Native.publish(socket.handle(), nativeTopic, parts, count,
                flags);
    }

    private PartAttempt submitSingleAttempt(WholeSubmitter submitter,
                                            Message message) {
        int result = submitter.submit(
            InternalAccess.messageNativeHandle(message), 1L,
            MemorySegment.NULL, MemorySegment.NULL);
        int errno = result == SubmitResult.OK.value() ? 0 : Native.errno();
        InternalAccess.messageMarkTransferred(message);
        return new PartAttempt(result, errno);
    }

    private PartAttempt submitPartsAttempt(WholeSubmitter submitter,
                                           List<Message> parts) {
        SendScratch scratch = sendScratch.get();
        MemorySegment nativeParts = scratch.parts(parts.size());
        long partSize = NativeLayouts.MESSAGE_LAYOUT.byteSize();
        int moved = 0;
        boolean submitted = false;
        try {
            for (; moved < parts.size(); moved++) {
                InternalAccess.messageMoveTo(parts.get(moved),
                    nativeParts.asSlice(partSize * moved, partSize));
            }
            submitted = true;
            int result = submitter.submit(nativeParts, parts.size(),
                MemorySegment.NULL, MemorySegment.NULL);
            int errno = result == SubmitResult.OK.value() ? 0 : Native.errno();
            return new PartAttempt(result, errno);
        } finally {
            if (!submitted && moved > 0) {
                NativeMessage.multipartClose(nativeParts, moved);
            }
        }
    }

    private PartAttempt submitTrackedNoWait(RoutingId target,
                                             WholeSubmitter submitter,
                                             Message message) {
        CompletionOwner.NoWaitAttempt attempt =
            socket.completionOwner().trackNoWaitSend(target,
                (context, idOut) -> {
                    int result = submitter.submit(
                        InternalAccess.messageNativeHandle(message), 1L,
                        context, idOut);
                    InternalAccess.messageMarkTransferred(message);
                    return result;
                });
        return new PartAttempt(attempt.result(), attempt.errno());
    }

    private PartAttempt submitTrackedNoWait(RoutingId target,
                                             WholeSubmitter submitter,
                                             List<Message> parts) {
        SendScratch scratch = sendScratch.get();
        MemorySegment nativeParts = scratch.parts(parts.size());
        long partSize = NativeLayouts.MESSAGE_LAYOUT.byteSize();
        int moved = 0;
        boolean[] nativeCalled = {false};
        try {
            for (; moved < parts.size(); moved++) {
                InternalAccess.messageMoveTo(parts.get(moved),
                    nativeParts.asSlice(partSize * moved, partSize));
            }
            CompletionOwner.NoWaitAttempt attempt =
                socket.completionOwner().trackNoWaitSend(target,
                    (context, idOut) -> {
                        nativeCalled[0] = true;
                        return submitter.submit(nativeParts, parts.size(),
                            context, idOut);
                    });
            return new PartAttempt(attempt.result(), attempt.errno());
        } finally {
            if (!nativeCalled[0] && moved > 0) {
                NativeMessage.multipartClose(nativeParts, moved);
            }
        }
    }

    private static SendResult sendResult(PartAttempt attempt) {
        if (attempt.result() == SubmitResult.OK.value()) {
            return SendResult.SENT;
        }
        ZlinkSubmitException failure = NativeSubmitErrors.submitException(
            attempt.result(), attempt.errno());
        return switch (failure.getResult()) {
            case BACKPRESSURED -> SendResult.BACKPRESSURED;
            case NOT_CONNECTED -> SendResult.NOT_READY;
            default -> throw failure;
        };
    }

    private static boolean isWouldBlock(int errno) {
        return errno == NativeErrno.EAGAIN
            || errno == NativeErrno.EWOULDBLOCK_WIN;
    }

    private static void requireSuccess(PartAttempt attempt) {
        if (attempt.result() != SubmitResult.OK.value()) {
            throwFailure(attempt);
        }
    }

    private static void throwFailure(PartAttempt attempt) {
        throw NativeSubmitErrors.submitException(attempt.result(),
            attempt.errno());
    }

    private static boolean isDontWait(SendFlag flag) {
        return (flag.getValue() & SendFlag.DONTWAIT.getValue()) != 0;
    }

    private static void validateParts(List<Message> parts) {
        Objects.requireNonNull(parts, "parts");
        if (parts.isEmpty()) {
            throw new IllegalArgumentException("parts must not be empty");
        }
        for (int i = 0; i < parts.size(); i++) {
            if (parts.get(i) == null) {
                throw new IllegalArgumentException(
                    "parts[" + i + "] is null");
            }
        }
    }

    private static void ensureBlockingSendAllowed(SendFlag flags) {
        Objects.requireNonNull(flags, "flags");
        if (InternalAccess.inCallback()
            && (flags.getValue() & SendFlag.DONTWAIT.getValue()) == 0) {
            throw new IllegalStateException(
                "blocking send is not supported from callback context; use SendFlag.DONTWAIT");
        }
    }

    private static MemorySegment nativeRoutingId(SendScratch scratch,
                                                  RoutingId routingId) {
        MemorySegment nativeRid = scratch.nativeRoutingId;
        if (scratch.lastRoutingId != routingId) {
            NativeRoutingIds.write(nativeRid, routingId);
            scratch.lastRoutingId = routingId;
        }
        return nativeRid;
    }

    private static MemorySegment nativeTopic(SendScratch scratch,
                                             String topicId) {
        if (topicId.equals(scratch.lastTopicId)) {
            return scratch.lastNativeTopic;
        }
        MemorySegment nativeTopic = scratch.arena.allocateFrom(topicId,
            StandardCharsets.UTF_8);
        scratch.lastTopicId = topicId;
        scratch.lastNativeTopic = nativeTopic;
        return nativeTopic;
    }

    @FunctionalInterface
    private interface WholeSubmitter {
        int submit(MemorySegment parts, long partCount,
                   MemorySegment userContext,
                   MemorySegment completionIdOut);
    }

    private record PartAttempt(int result, int errno) {
    }
}
