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
        PartSubmitter submitter = (part, partFlag, context, idOut) ->
            sendPartOnce(part, routingId, flag.getValue(), partFlag,
                context, idOut);
        if (isDontWait(flag)) {
            requirePartSuccess(submitTrackedNoWaitPart(routingId, submitter,
                message, Native.PART_FINAL));
        } else {
            socket.completionOwner().withNativeCall(() -> {
                submitBlockingPart(submitter, message, Native.PART_FINAL);
                return null;
            });
        }
    }

    SendResult sendMessageFrameNoWaitResult(RoutingId routingId, Message message) {
        Objects.requireNonNull(routingId, "routingId");
        Objects.requireNonNull(message, "message");
        return submitNoWaitPart(routingId,
            (part, partFlag, context, idOut) -> sendPartOnce(part, routingId,
                SendFlag.DONTWAIT.getValue(), partFlag, context, idOut),
            message, Native.PART_FINAL);
    }

    boolean send(byte[] routingIdBytes, Message part, SendFlag flags) {
        Objects.requireNonNull(routingIdBytes, "routingIdBytes");
        Objects.requireNonNull(part, "part");
        Objects.requireNonNull(flags, "flags");
        ensureBlockingSendAllowed(flags);
        RoutingId target = RoutingId.from(routingIdBytes);
        return submitBooleanPart(target,
            (message, partFlag, context, idOut) -> sendPartOnce(message,
                target, flags.getValue(), partFlag, context, idOut),
            part, Native.PART_FINAL, flags);
    }

    void send(int rid, Message part, SendFlag flags) {
        Objects.requireNonNull(part, "part");
        Objects.requireNonNull(flags, "flags");
        ensureBlockingSendAllowed(flags);
        RoutingId target = RoutingId.from(Integer.toUnsignedLong(rid));
        PartSubmitter submitter = (message, partFlag, context, idOut) ->
            sendPartOnce(message, target, flags.getValue(), partFlag,
                context, idOut);
        PartAttempt attempt = isDontWait(flags)
            ? submitTrackedNoWaitPart(target, submitter, part,
                Native.PART_FINAL)
            : socket.completionOwner().withNativeCall(() ->
                submitPartAttempt(submitter, part, Native.PART_FINAL));
        requirePartSuccess(attempt);
    }

    int send(int rid, MemorySegment payload, int length, int sendFlags) {
        Objects.requireNonNull(payload, "payload");
        SendFlag flag = SendFlag.fromValue(sendFlags);
        return sendDirectSegment(rid, payload, length, flag);
    }

    int sendCopied(int rid, MemorySegment payload, int length, int sendFlags) {
        Objects.requireNonNull(payload, "payload");
        SendFlag flag = SendFlag.fromValue(sendFlags);
        ensureBlockingSendAllowed(flag);
        SendScratch scratch = sendScratch.get();
        MemorySegment nativeMsg = scratch.nativeMsg;
        int rc = NativeMessage.messageInitSize(nativeMsg, length);
        if (rc != 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        if (length > 0) {
            MemorySegment dst = NativeMessage.messageData(nativeMsg)
                .reinterpret(length);
            MemorySegment.copy(payload, 0, dst, 0, length);
        }
        boolean[] consumed = {false};
        try {
            RoutingId target = RoutingId.from(Integer.toUnsignedLong(rid));
            MemorySegment nativeTarget = nativeRoutingId(scratch, target);
            CompletionOwner.NoWaitAttempt tracked;
            if (isDontWait(flag)) {
                tracked = socket.completionOwner().trackNoWaitSend(target,
                    (context, idOut) -> {
                        int result = Native.sendPartRidNoWaitCritical(
                            socket.handle(), nativeTarget, nativeMsg,
                            flag.getValue(), Native.PART_FINAL, context,
                            idOut);
                        consumed[0] = true;
                        return result;
                    });
                requirePartSuccess(new PartAttempt(tracked.result(),
                    tracked.errno()));
            } else {
                PartAttempt attempt = socket.completionOwner()
                    .withNativeCall(() -> {
                        int result = Native.sendPartRid(socket.handle(),
                            nativeTarget, nativeMsg, flag.getValue(),
                            Native.PART_FINAL);
                        int errno = result == SubmitResult.OK.value()
                            ? 0 : Native.errno();
                        consumed[0] = true;
                        return new PartAttempt(result, errno);
                    });
                requirePartSuccess(attempt);
            }
        } finally {
            if (!consumed[0]) {
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
        submitBlockingPart(
            (part, partFlag, context, idOut) -> publishPartOnce(topicId, part,
                flags.getValue(), partFlag),
            message, Native.PART_FINAL);
    }

    SendResult publishMessageFrameNoWaitResult(String topicId, Message message) {
        Objects.requireNonNull(topicId, "topicId");
        Objects.requireNonNull(message, "message");
        return submitPublishNoWaitPart(
            (part, partFlag, context, idOut) -> publishPartOnce(topicId, part,
                SendFlag.DONTWAIT.getValue(), partFlag),
            message, Native.PART_FINAL);
    }

    void sendMessageFrame(Message message, SendFlag flag) {
        Objects.requireNonNull(message, "message");
        Objects.requireNonNull(flag, "flag");
        ensureBlockingSendAllowed(flag);
        PartSubmitter submitter = (part, partFlag, context, idOut) ->
            sendPartOnce(part, (RoutingId) null, flag.getValue(), partFlag,
                context, idOut);
        if (isDontWait(flag)) {
            requirePartSuccess(submitTrackedNoWaitPart(null, submitter,
                message, Native.PART_FINAL));
        } else {
            socket.completionOwner().withNativeCall(() -> {
                submitBlockingPart(submitter, message, Native.PART_FINAL);
                return null;
            });
        }
    }

    boolean sendMessageFrameNoWaitResult(Message message, SendFlag flag) {
        Objects.requireNonNull(message, "message");
        Objects.requireNonNull(flag, "flag");
        return submitBooleanPart(null,
            (part, partFlag, context, idOut) -> sendPartOnce(part,
                (RoutingId) null, flag.getValue(), partFlag, context, idOut),
            message, Native.PART_FINAL, flag);
    }

    SendResult sendMessageFrameNoWaitResult(Message message) {
        Objects.requireNonNull(message, "message");
        return submitNoWaitPart(null,
            (part, partFlag, context, idOut) -> sendPartOnce(part,
                (RoutingId) null, SendFlag.DONTWAIT.getValue(), partFlag,
                context, idOut),
            message, Native.PART_FINAL);
    }

    void sendParts(RoutingId routingId, List<Message> parts,
                   SendFlag flags, boolean nonBlocking) {
        socket.ensureOpen();
        validateParts(parts);
        ensureBlockingSendAllowed(flags);
        PartSubmitter submitter = (part, partFlag, context, idOut) ->
            sendPartOnce(part, routingId, flags.getValue(), partFlag,
                context, idOut);
        if (isDontWait(flags)) {
            requirePartSuccess(submitNoWaitPartsAttempt(routingId, submitter,
                parts));
        } else {
            socket.completionOwner().withNativeCall(() -> {
                submitBlockingParts(submitter, parts);
                return null;
            });
        }
    }

    SendResult sendNoWaitPartsResult(RoutingId routingId, List<Message> parts) {
        socket.ensureOpen();
        validateParts(parts);
        return submitNoWaitParts(routingId,
            (part, partFlag, context, idOut) -> sendPartOnce(part, routingId,
                SendFlag.DONTWAIT.getValue(), partFlag, context, idOut),
            parts);
    }

    void publishParts(String topicId, List<Message> parts,
                      SendFlag flags, boolean nonBlocking) {
        socket.ensureOpen();
        validateParts(parts);
        ensureBlockingSendAllowed(flags);
        MemorySegment nativeTopic = nativeTopic(sendScratch.get(), topicId);
        submitBlockingParts(
            (part, partFlag, context, idOut) -> publishPartOnce(nativeTopic,
                part,
                flags.getValue(), partFlag),
            parts);
    }

    SendResult publishNoWaitPartsResult(String topicId, List<Message> parts) {
        socket.ensureOpen();
        validateParts(parts);
        MemorySegment nativeTopic = nativeTopic(sendScratch.get(), topicId);
        return submitPublishNoWaitParts(
            (part, partFlag, context, idOut) -> publishPartOnce(nativeTopic,
                part,
                SendFlag.DONTWAIT.getValue(), partFlag),
            parts);
    }

    private int sendDirectSegment(int rid, MemorySegment payload, int length,
                                  SendFlag flag) {
        return sendCopied(rid, payload, length, flag.getValue());
    }

    private int sendPartOnce(Message message, RoutingId routingId, int flags,
                             int partFlag, MemorySegment userContext,
                             MemorySegment completionIdOut) {
        SendScratch scratch = sendScratch.get();
        MemorySegment messageHandle = InternalAccess.messageNativeHandle(message);
        MemorySegment nativeRoutingId = routingId == null
            ? MemorySegment.NULL
            : nativeRoutingId(scratch, routingId);
        boolean useCritical =
            (flags & SendFlag.DONTWAIT.getValue()) != 0;
        int rc;
        if (nativeRoutingId.address() == 0) {
            rc = useCritical
                ? Native.sendPartNoWaitCritical(socket.handle(), messageHandle,
                    flags, partFlag, userContext, completionIdOut)
                : Native.sendPart(socket.handle(), messageHandle, flags,
                    partFlag, userContext, completionIdOut);
        } else {
            rc = useCritical
                ? Native.sendPartRidNoWaitCritical(socket.handle(),
                    nativeRoutingId, messageHandle, flags, partFlag,
                    userContext, completionIdOut)
                : Native.sendPartRid(socket.handle(), nativeRoutingId,
                    messageHandle, flags, partFlag, userContext,
                    completionIdOut);
        }
        return rc;
    }

    private int publishPartOnce(String topicId, Message message, int flags,
                                int partFlag) {
        SendScratch scratch = sendScratch.get();
        MemorySegment nativeTopic = nativeTopic(scratch, topicId);
        MemorySegment messageHandle = InternalAccess.messageNativeHandle(message);
        boolean useCritical =
            (flags & SendFlag.DONTWAIT.getValue()) != 0;
        int rc = useCritical
            ? Native.publishPartNoWaitCritical(socket.handle(), nativeTopic,
                messageHandle, flags, partFlag)
            : Native.publishPart(socket.handle(), nativeTopic, messageHandle,
                flags, partFlag);
        return rc;
    }

    private int publishPartOnce(MemorySegment nativeTopic, Message message,
                                int flags, int partFlag) {
        MemorySegment messageHandle = InternalAccess.messageNativeHandle(message);
        boolean useCritical =
            (flags & SendFlag.DONTWAIT.getValue()) != 0;
        int rc = useCritical
            ? Native.publishPartNoWaitCritical(socket.handle(), nativeTopic,
                messageHandle, flags, partFlag)
            : Native.publishPart(socket.handle(), nativeTopic, messageHandle,
                flags, partFlag);
        return rc;
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

    private void submitBlockingPart(PartSubmitter submitter, Message part,
                                    int partFlag) {
        PartAttempt attempt = submitPartAttempt(submitter, part, partFlag);
        if (attempt.result() != SubmitResult.OK.value()) {
            throwPartSubmitFailure(attempt.result(), attempt.errno());
        }
    }

    private boolean submitBooleanPart(RoutingId target,
                                      PartSubmitter submitter, Message part,
                                      int partFlag, SendFlag flags) {
        boolean explicitNonBlocking =
            (flags.getValue() & SendFlag.DONTWAIT.getValue()) != 0;
        PartAttempt attempt = explicitNonBlocking
            ? submitTrackedNoWaitPart(target, submitter, part, partFlag)
            : socket.completionOwner().withNativeCall(() ->
                submitPartAttempt(submitter, part, partFlag));
        if (attempt.result() == SubmitResult.OK.value()) {
            return true;
        }
        if (explicitNonBlocking
            && attempt.result() == SubmitResult.BACKPRESSURED.value()
            && isWouldBlock(attempt.errno())) {
            return false;
        }
        throwPartSubmitFailure(attempt.result(), attempt.errno());
        return false;
    }

    private SendResult submitNoWaitPart(RoutingId target,
                                        PartSubmitter submitter, Message part,
                                        int partFlag) {
        PartAttempt attempt = submitTrackedNoWaitPart(target, submitter, part,
            partFlag);
        if (attempt.result() == SubmitResult.OK.value()) {
            return SendResult.SENT;
        }
        return classifyNonBlockingSendResult(attempt.result(), attempt.errno());
    }

    private void submitBlockingParts(PartSubmitter submitter,
                                     List<Message> parts) {
        for (int i = 0; i < parts.size(); i++) {
            int partFlag = i + 1 < parts.size()
                ? Native.PART_MORE : Native.PART_FINAL;
            PartAttempt attempt = submitPartAttempt(submitter, parts.get(i),
                partFlag);
            if (attempt.result() != SubmitResult.OK.value()) {
                throwPartSubmitFailure(attempt.result(), attempt.errno());
            }
        }
    }

    private SendResult submitNoWaitParts(RoutingId target,
                                         PartSubmitter submitter,
                                         List<Message> parts) {
        PartAttempt attempt = submitNoWaitPartsAttempt(target, submitter,
            parts);
        if (attempt.result() == SubmitResult.OK.value()) {
            return SendResult.SENT;
        }
        return classifyNonBlockingSendResult(attempt.result(),
            attempt.errno());
    }

    private PartAttempt submitNoWaitPartsAttempt(RoutingId target,
                                                 PartSubmitter submitter,
                                                 List<Message> parts) {
        CompletionOwner.NoWaitAttempt attempt =
            socket.completionOwner().trackNoWaitSend(target,
                (context, idOut) -> {
                    for (int i = 0; i < parts.size(); i++) {
                        int partFlag = i + 1 < parts.size()
                            ? Native.PART_MORE : Native.PART_FINAL;
                        boolean last = partFlag == Native.PART_FINAL;
                        int result = submitter.submit(parts.get(i), partFlag,
                            last ? context : MemorySegment.NULL,
                            last ? idOut : MemorySegment.NULL);
                        // Every part call consumes the native message even
                        // when the packet's FINAL admission is backpressured.
                        InternalAccess.messageMarkTransferred(parts.get(i));
                        if (result != SubmitResult.OK.value()) {
                            return result;
                        }
                    }
                    return SubmitResult.OK.value();
                });
        return new PartAttempt(attempt.result(), attempt.errno());
    }

    private SendResult submitPublishNoWaitPart(PartSubmitter submitter,
                                               Message part, int partFlag) {
        PartAttempt attempt = submitPartAttempt(submitter, part, partFlag);
        if (attempt.result() == SubmitResult.OK.value()) {
            return SendResult.SENT;
        }
        return classifyNonBlockingSendResult(attempt.result(),
            attempt.errno());
    }

    private SendResult submitPublishNoWaitParts(PartSubmitter submitter,
                                                List<Message> parts) {
        for (int i = 0; i < parts.size(); i++) {
            int partFlag = i + 1 < parts.size()
                ? Native.PART_MORE : Native.PART_FINAL;
            PartAttempt attempt = submitPartAttempt(submitter, parts.get(i),
                partFlag);
            if (attempt.result() != SubmitResult.OK.value()) {
                return classifyNonBlockingSendResult(attempt.result(),
                    attempt.errno());
            }
        }
        return SendResult.SENT;
    }

    private PartAttempt submitPartAttempt(PartSubmitter submitter,
                                          Message part, int partFlag) {
        return completePartAttempt(part, submitter.submit(part, partFlag,
            MemorySegment.NULL, MemorySegment.NULL));
    }

    private PartAttempt submitTrackedNoWaitPart(RoutingId target,
                                                PartSubmitter submitter,
                                                Message part, int partFlag) {
        CompletionOwner.NoWaitAttempt attempt =
            socket.completionOwner().trackNoWaitSend(target,
                (context, idOut) -> {
                    int result = submitter.submit(part, partFlag, context,
                        idOut);
                    InternalAccess.messageMarkTransferred(part);
                    return result;
                });
        return new PartAttempt(attempt.result(), attempt.errno());
    }

    private PartAttempt completePartAttempt(Message part, int result) {
        int errno = result == SubmitResult.OK.value() ? 0 : Native.errno();
        // Every Core part call consumes the native part on success and
        // failure. BACKPRESSURED means Core retained no payload, not that the
        // submitted part itself remains caller-owned.
        InternalAccess.messageMarkTransferred(part);
        return new PartAttempt(result, errno);
    }

    private static boolean isWouldBlock(int errno) {
        return errno == NativeErrno.EAGAIN
            || errno == NativeErrno.EWOULDBLOCK_WIN;
    }

    private static SendResult classifyNonBlockingSendResult(int result,
                                                            int errno) {
        ZlinkSubmitException failure =
            NativeSubmitErrors.submitException(result, errno);
        return switch (failure.getResult()) {
            case BACKPRESSURED -> SendResult.BACKPRESSURED;
            case NOT_CONNECTED -> SendResult.NOT_READY;
            default -> throw failure;
        };
    }

    private static void throwPartSubmitFailure(int result, int errno) {
        throw NativeSubmitErrors.submitException(result, errno);
    }

    private static void requirePartSuccess(PartAttempt attempt) {
        if (attempt.result() != SubmitResult.OK.value()) {
            throwPartSubmitFailure(attempt.result(), attempt.errno());
        }
    }

    private static boolean isDontWait(SendFlag flag) {
        return (flag.getValue() & SendFlag.DONTWAIT.getValue()) != 0;
    }

    private static void validateParts(List<Message> parts) {
        if (parts.isEmpty())
            throw new IllegalArgumentException("parts must not be empty");
        for (int i = 0; i < parts.size(); i++) {
            if (parts.get(i) == null)
                throw new IllegalArgumentException("parts[" + i + "] is null");
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

    @FunctionalInterface
    private interface PartSubmitter {
        int submit(Message part, int partFlag, MemorySegment userContext,
                   MemorySegment completionIdOut);
    }

    private record PartAttempt(int result, int errno) {
    }
}
