/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.runtime.nativeapi.MessagePartsBuffer;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeErrno;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.RoutedRequestSupport;
import systems.zlink.runtime.nativeapi.RequestReplySupport;

/** Core reply completion handed to the socket's existing completion dispatcher. */
final class CoreRequestSupport implements AutoCloseable {
    private static final int DONT_WAIT = 1;

    private final NativeSocketRuntime socket;
    private final boolean dealer;
    private final RoutedRequestSupport requests;

    CoreRequestSupport(NativeSocketRuntime socket, boolean dealer) {
        this.socket = Objects.requireNonNull(socket, "socket");
        this.dealer = dealer;
        this.requests = new RoutedRequestSupport(
            new RoutedRequestSupport.CallbackLifecycle() {
                @Override
                public void enter() {
                    SocketCore.enterCallback();
                }

                @Override
                public void exit() {
                    SocketCore.leaveCallback();
                }

                @Override
                public void dispatch(Runnable completion) {
                    socket.dispatchCompletion(completion);
                }
            });
    }

    CompletionStage<List<Message>> submit(List<Message> sourceParts,
                                          Duration timeout,
                                          MemorySegment target) {
        Objects.requireNonNull(sourceParts, "sourceParts");
        if (sourceParts.isEmpty()) {
            throw new IllegalArgumentException("parts must not be empty");
        }
        for (int i = 0; i < sourceParts.size(); i++) {
            Objects.requireNonNull(sourceParts.get(i), "parts[" + i + "]");
        }
        if (timeout != null && timeout.isNegative()) {
            throw new IllegalArgumentException("timeout must not be negative");
        }
        long requestId = requests.nextRequestId();
        CompletableFuture<List<Message>> future = new CompletableFuture<>();
        requests.registerRoutedPending(requestId, future);

        MessagePartsBuffer materializer = new MessagePartsBuffer();
        for (Message part : sourceParts) {
            materializer.add(part);
        }
        int timeoutMs = RequestReplySupport.toTimeoutInt(
            RequestReplySupport.timeoutMillis(timeout));
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment nativeParts = materializer.copyToNativeArray(arena);
            MemorySegment nativeRid = target.asSlice(0,
                NativeLayouts.ROUTING_ID_LAYOUT.byteSize());
            long stride = NativeLayouts.MESSAGE_LAYOUT.byteSize();
            int result;
            try {
                try {
                    result = SubmitResult.OK.value();
                    for (int index = 0; index < sourceParts.size(); index++) {
                        int partFlag = index + 1 < sourceParts.size()
                            ? Native.PART_MORE : Native.PART_FINAL;
                        MemorySegment nativePart = nativeParts.asSlice(
                            index * stride, stride);
                        int rc = dealer
                            ? Native.dealerRequestTransportPairPart(
                                socket.handle(), target, nativePart, DONT_WAIT,
                                partFlag,
                                partFlag == Native.PART_FINAL ? timeoutMs : 0,
                                partFlag == Native.PART_FINAL
                                    ? requests.replyCallback()
                                    : MemorySegment.NULL,
                                partFlag == Native.PART_FINAL
                                    ? RoutedRequestSupport.userData(requestId)
                                    : MemorySegment.NULL)
                            : Native.routerRequestTransportPairPart(
                                socket.handle(), nativeRid,
                                target.get(ValueLayout.JAVA_LONG,
                                    NativeLayouts.ROUTED_SUBMIT_TARGET_PAIR_ID_OFFSET),
                                target.get(ValueLayout.JAVA_LONG,
                                    NativeLayouts.ROUTED_SUBMIT_TARGET_GENERATION_OFFSET),
                                nativePart, DONT_WAIT, partFlag,
                                partFlag == Native.PART_FINAL ? timeoutMs : 0,
                                partFlag == Native.PART_FINAL
                                    ? requests.replyCallback()
                                    : MemorySegment.NULL,
                                partFlag == Native.PART_FINAL
                                    ? RoutedRequestSupport.userData(requestId)
                                    : MemorySegment.NULL);
                        if (rc != SubmitResult.OK.value()) {
                            result = rc;
                            break;
                        }
                    }
                } finally {
                    // The request-part Core entrypoint consumes each native
                    // message on both success and rejection. Close any
                    // remaining initialized slots exactly as the legacy
                    // record submitter did; successful slots are already
                    // empty and therefore harmless here.
                    MessagePartsBuffer.closeNativeArray(nativeParts,
                        sourceParts.size());
                }
            } catch (RuntimeException failure) {
                requests.removeRoutedPending(requestId);
                throw failure;
            }
            if (result != SubmitResult.OK.value()) {
                int errno = Native.errno();
                requests.removeRoutedPending(requestId);
                future.completeExceptionally(new ZlinkSubmitException(
                    submitResult(result), errno));
                return future;
            }
            // Core has accepted ownership of the request copies. The public
            // source messages are consumed only after the complete record was
            // accepted, matching the existing raw request ownership contract.
            Message.closeAll(sourceParts);
            return future;
        }
    }

    @Override
    public void close() {
        requests.close(new ZlinkRequestException(RequestResult.TERMINATED,
            NativeErrno.ECANCELED));
    }

    private static SubmitResult submitResult(int result) {
        try {
            return SubmitResult.fromValue(result);
        } catch (IllegalArgumentException ignored) {
            return SubmitResult.INTERNAL_ERROR;
        }
    }
}
