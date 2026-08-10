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
        submitBlockingPart(
            (part, partFlag) -> sendPartOnce(part, routingId, flag.getValue(),
                partFlag),
            message, Native.PART_FINAL, "zlink_send_part_rid",
            true);
    }

    SendResult sendMessageFrameNoWaitResult(RoutingId routingId, Message message) {
        Objects.requireNonNull(routingId, "routingId");
        Objects.requireNonNull(message, "message");
        return submitNoWaitPart(
            (part, partFlag) -> sendPartOnce(part, routingId,
                SendFlag.DONTWAIT.getValue(), partFlag),
            message, Native.PART_FINAL, "zlink_send_part_rid");
    }

    boolean send(byte[] routingIdBytes, Message part, SendFlag flags) {
        Objects.requireNonNull(routingIdBytes, "routingIdBytes");
        Objects.requireNonNull(part, "part");
        Objects.requireNonNull(flags, "flags");
        ensureBlockingSendAllowed(flags);
        return submitBooleanPart(
            (message, partFlag) -> sendPartOnce(message, routingIdBytes,
                flags.getValue(), partFlag),
            part, Native.PART_FINAL, flags, "zlink_send_part_rid");
    }

    void send(int rid, Message part, SendFlag flags) {
        Objects.requireNonNull(part, "part");
        Objects.requireNonNull(flags, "flags");
        ensureBlockingSendAllowed(flags);
        int rc = Native.sendMultipartU32(socket.handle(), rid,
            InternalAccess.messageNativeHandle(part), 1, flags.getValue());
        if (rc < 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.SUBMIT);
        InternalAccess.messageMarkTransferred(part);
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
        boolean success = false;
        try {
            rc = Native.sendMultipartU32(socket.handle(), rid, nativeMsg, 1,
                flag.getValue());
            if (rc < 0)
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.SUBMIT);
            success = true;
        } finally {
            if (!success) {
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
            (part, partFlag) -> publishPartOnce(topicId, part,
                flags.getValue(), partFlag),
            message, Native.PART_FINAL, "zlink_publish_part", false);
    }

    SendResult publishMessageFrameNoWaitResult(String topicId, Message message) {
        Objects.requireNonNull(topicId, "topicId");
        Objects.requireNonNull(message, "message");
        return submitNoWaitPart(
            (part, partFlag) -> publishPartOnce(topicId, part,
                SendFlag.DONTWAIT.getValue(), partFlag),
            message, Native.PART_FINAL, "zlink_publish_part");
    }

    void sendMessageFrame(Message message, SendFlag flag) {
        Objects.requireNonNull(message, "message");
        Objects.requireNonNull(flag, "flag");
        ensureBlockingSendAllowed(flag);
        submitBlockingPart(
            (part, partFlag) -> sendPartOnce(part, (RoutingId) null,
                flag.getValue(), partFlag),
            message, Native.PART_FINAL, "zlink_send_part", true);
    }

    boolean sendMessageFrameNoWaitResult(Message message, SendFlag flag) {
        Objects.requireNonNull(message, "message");
        Objects.requireNonNull(flag, "flag");
        return submitBooleanPart(
            (part, partFlag) -> sendPartOnce(part, (RoutingId) null,
                flag.getValue(), partFlag),
            message, Native.PART_FINAL, flag, "zlink_send_part");
    }

    SendResult sendMessageFrameNoWaitResult(Message message) {
        Objects.requireNonNull(message, "message");
        return submitNoWaitPart(
            (part, partFlag) -> sendPartOnce(part, (RoutingId) null,
                SendFlag.DONTWAIT.getValue(), partFlag),
            message, Native.PART_FINAL, "zlink_send_part");
    }

    void sendParts(RoutingId routingId, List<Message> parts,
                   SendFlag flags, boolean nonBlocking) {
        socket.ensureOpen();
        validateParts(parts);
        ensureBlockingSendAllowed(flags);
        boolean explicitNonBlocking =
            (flags.getValue() & SendFlag.DONTWAIT.getValue()) != 0;
        submitBlockingParts(
            (part, partFlag) -> sendPartOnce(part, routingId, flags.getValue(),
                partFlag),
            parts, nonBlocking || explicitNonBlocking,
            routingId == null ? "zlink_send_part" : "zlink_send_part_rid");
    }

    SendResult sendNoWaitPartsResult(RoutingId routingId, List<Message> parts) {
        socket.ensureOpen();
        validateParts(parts);
        return submitNoWaitParts(
            (part, partFlag) -> sendPartOnce(part, routingId,
                SendFlag.DONTWAIT.getValue(), partFlag),
            parts, routingId == null ? "zlink_send_part"
                : "zlink_send_part_rid");
    }

    void publishParts(String topicId, List<Message> parts,
                      SendFlag flags, boolean nonBlocking) {
        socket.ensureOpen();
        validateParts(parts);
        ensureBlockingSendAllowed(flags);
        boolean explicitNonBlocking =
            (flags.getValue() & SendFlag.DONTWAIT.getValue()) != 0;
        MemorySegment nativeTopic = nativeTopic(sendScratch.get(), topicId);
        submitBlockingParts(
            (part, partFlag) -> publishPartOnce(nativeTopic, part,
                flags.getValue(), partFlag),
            parts, nonBlocking || explicitNonBlocking, "zlink_publish_part");
    }

    SendResult publishNoWaitPartsResult(String topicId, List<Message> parts) {
        socket.ensureOpen();
        validateParts(parts);
        MemorySegment nativeTopic = nativeTopic(sendScratch.get(), topicId);
        return submitNoWaitParts(
            (part, partFlag) -> publishPartOnce(nativeTopic, part,
                SendFlag.DONTWAIT.getValue(), partFlag),
            parts, "zlink_publish_part");
    }

    private int sendDirectSegment(int rid, MemorySegment payload, int length,
                                  SendFlag flag) {
        return sendCopied(rid, payload, length, flag.getValue());
    }

    private int sendPartOnce(Message message, RoutingId routingId, int flags,
                             int partFlag) {
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
                    flags, partFlag)
                : Native.sendPart(socket.handle(), messageHandle, flags,
                    partFlag);
        } else {
            rc = useCritical
                ? Native.sendPartRidNoWaitCritical(socket.handle(),
                    nativeRoutingId, messageHandle, flags, partFlag)
                : Native.sendPartRid(socket.handle(), nativeRoutingId,
                    messageHandle, flags, partFlag);
        }
        if (rc == 0) {
            InternalAccess.messageMarkTransferred(message);
        }
        return rc;
    }

    private int sendPartOnce(Message message, byte[] routingIdBytes, int flags,
                             int partFlag) {
        SendScratch scratch = sendScratch.get();
        MemorySegment messageHandle = InternalAccess.messageNativeHandle(message);
        MemorySegment nativeRoutingId = nativeRoutingId(scratch,
            routingIdBytes);
        boolean useCritical =
            (flags & SendFlag.DONTWAIT.getValue()) != 0;
        int rc = useCritical
            ? Native.sendPartRidNoWaitCritical(socket.handle(), nativeRoutingId,
                messageHandle, flags, partFlag)
            : Native.sendPartRid(socket.handle(), nativeRoutingId, messageHandle,
                flags, partFlag);
        if (rc == 0) {
            InternalAccess.messageMarkTransferred(message);
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
        if (rc == 0) {
            InternalAccess.messageMarkTransferred(message);
        }
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
        if (rc == 0) {
            InternalAccess.messageMarkTransferred(message);
        }
        return rc;
    }

    private static MemorySegment nativeTopic(SendScratch scratch,
                                             String topicId) {
        return scratch.arena.allocateFrom(topicId, StandardCharsets.UTF_8);
    }

    private void submitBlockingPart(PartSubmitter submitter, Message part,
                                    int partFlag, String apiName,
                                    boolean retryTransient) {
        while (true) {
            int rc = submitter.submit(part, partFlag);
            if (rc == 0) {
                return;
            }
            int errno = Native.errno();
            if (retryTransient
                ? isTransientBlockingSendErrno(errno)
                : errno == NativeErrno.EINTR) {
                continue;
            }
            throwPartSubmitFailure(apiName);
        }
    }

    private boolean submitBooleanPart(PartSubmitter submitter, Message part,
                                      int partFlag, SendFlag flags,
                                      String apiName) {
        boolean explicitNonBlocking =
            (flags.getValue() & SendFlag.DONTWAIT.getValue()) != 0;
        while (true) {
            int rc = submitter.submit(part, partFlag);
            if (rc == 0) {
                return true;
            }
            int errno = Native.errno();
            if (errno == NativeErrno.EINTR
                || (!explicitNonBlocking
                    && isTransientBlockingSendErrno(errno))) {
                continue;
            }
            if (explicitNonBlocking
                && (errno == NativeErrno.EAGAIN
                    || errno == NativeErrno.EWOULDBLOCK_WIN)) {
                return false;
            }
            throwPartSubmitFailure(apiName);
        }
    }

    private SendResult submitNoWaitPart(PartSubmitter submitter, Message part,
                                        int partFlag, String apiName) {
        while (true) {
            int rc = submitter.submit(part, partFlag);
            if (rc == 0) {
                return SendResult.SENT;
            }
            int errno = Native.errno();
            if (errno == NativeErrno.EINTR) {
                continue;
            }
            return classifyNonBlockingSendErrno(apiName);
        }
    }

    private void submitBlockingParts(PartSubmitter submitter,
                                     List<Message> parts,
                                     boolean backpressureAsSubmitResult,
                                     String apiName) {
        for (int i = 0; i < parts.size(); i++) {
            int partFlag = i + 1 < parts.size()
                ? Native.PART_MORE : Native.PART_FINAL;
            while (true) {
                int rc = submitter.submit(parts.get(i), partFlag);
                if (rc == 0) {
                    break;
                }
                int errno = Native.errno();
                if (errno == NativeErrno.EINTR) {
                    continue;
                }
                if (backpressureAsSubmitResult
                    && NativeSubmitErrors.isBackpressured(errno)) {
                    throw new ZlinkSubmitException(SubmitResult.BACKPRESSURED,
                        errno);
                }
                throwPartSubmitFailure(apiName);
            }
        }
    }

    private SendResult submitNoWaitParts(PartSubmitter submitter,
                                         List<Message> parts,
                                         String apiName) {
        for (int i = 0; i < parts.size(); i++) {
            int partFlag = i + 1 < parts.size()
                ? Native.PART_MORE : Native.PART_FINAL;
            while (true) {
                int rc = submitter.submit(parts.get(i), partFlag);
                if (rc == 0) {
                    break;
                }
                int errno = Native.errno();
                if (errno == NativeErrno.EINTR) {
                    continue;
                }
                return classifyNonBlockingSendErrno(apiName);
            }
        }
        return SendResult.SENT;
    }

    private SendResult classifyNonBlockingSendErrno(String apiName) {
        int errno = Native.errno();
        if (NativeSubmitErrors.isBackpressured(errno))
            return SendResult.BACKPRESSURED;
        if (NativeSubmitErrors.isNotConnected(errno))
            return SendResult.NOT_READY;
        if (NativeSubmitErrors.isNotAdmitted(errno)) {
            throw new ZlinkSubmitException(SubmitResult.NOT_ADMITTED, errno);
        }
        throw ZlinkException.fromLastError(
            systems.zlink.contracts.errors.ErrorCategory.SUBMIT);
    }

    private void throwPartSubmitFailure(String apiName) {
        int errno = Native.errno();
        ZlinkSubmitException submit = NativeSubmitErrors.submitExceptionOrNull(errno);
        if (submit != null)
            throw submit;
        throw ZlinkException.fromLastError(
            systems.zlink.contracts.errors.ErrorCategory.SUBMIT);
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

    private static MemorySegment nativeRoutingId(SendScratch scratch,
                                                 byte[] value) {
        MemorySegment nativeRid = scratch.nativeRoutingId;
        scratch.lastRoutingId = null;
        NativeRoutingIds.writeBytes(nativeRid, value);
        return nativeRid;
    }

    private static boolean isTransientBlockingSendErrno(int errno) {
        return errno == NativeErrno.EINTR
            || errno == NativeErrno.EAGAIN
            || errno == NativeErrno.EWOULDBLOCK_WIN
            || errno == NativeErrno.ENOTCONN
            || errno == NativeErrno.ENOTCONN_WIN
            || errno == NativeErrno.EHOSTUNREACH
            || errno == NativeErrno.EHOSTUNREACH_WIN;
    }

    @FunctionalInterface
    private interface PartSubmitter {
        int submit(Message part, int partFlag);
    }
}
