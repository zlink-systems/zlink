/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.RequestResult;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.atomic.AtomicLong;

public final class RoutedRequestSupport {
    private static final Linker LINKER = Linker.nativeLinker();
    private static final FunctionDescriptor FD_REPLY_CALLBACK =
      FunctionDescriptor.ofVoid(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.JAVA_LONG, ValueLayout.ADDRESS);
    private static final Arena CALLBACK_ARENA = Arena.ofShared();
    private static final MemorySegment REPLY_CALLBACK = LINKER.upcallStub(
      callbackHandle(), FD_REPLY_CALLBACK, CALLBACK_ARENA);
    private static final AtomicLong NEXT_REQUEST_ID = new AtomicLong(1L);
    private static final ConcurrentMap<Long, CompletableFuture<List<Message>>>
      ROUTED_PENDING = new ConcurrentHashMap<>();

    private RoutedRequestSupport() {
    }

    public static MemorySegment replyCallback() {
        return REPLY_CALLBACK;
    }

    public static long nextRequestId() {
        return NEXT_REQUEST_ID.getAndIncrement();
    }

    public static MemorySegment userData(long requestId) {
        return MemorySegment.ofAddress(requestId);
    }

    public static void registerRoutedPending(
            long requestId, CompletableFuture<List<Message>> future) {
        ROUTED_PENDING.put(requestId, future);
    }

    public static void removeRoutedPending(long requestId) {
        ROUTED_PENDING.remove(requestId);
    }

    public static MemorySegment movePayloadToNative(Arena arena,
                                                    List<Message> payload) {
        long messageSize = NativeLayouts.MESSAGE_LAYOUT.byteSize();
        MemorySegment nativeParts = arena.allocate(messageSize * payload.size(),
          NativeLayouts.MESSAGE_LAYOUT.byteAlignment());
        int built = 0;
        try {
            for (int i = 0; i < payload.size(); i++) {
                InternalAccess.messageTransferTo(payload.get(i),
                  nativeParts.asSlice((long) i * messageSize, messageSize));
                built++;
            }
            return nativeParts;
        } catch (RuntimeException ex) {
            for (int i = built; i < payload.size(); i++) {
                try {
                    payload.get(i).close();
                } catch (RuntimeException ignored) {
                }
            }
            throw ex;
        }
    }

    private static MethodHandle callbackHandle() {
        try {
            return MethodHandles.lookup().findStatic(RoutedRequestSupport.class,
              "handleReplyCallback", MethodType.methodType(void.class, int.class,
                MemorySegment.class, long.class, MemorySegment.class));
        } catch (ReflectiveOperationException ex) {
            throw new ExceptionInInitializerError(ex);
        }
    }

    private static void handleReplyCallback(int result,
                                            MemorySegment parts,
                                            long partCount,
                                            MemorySegment userData) {
        long requestId = userData.address();
        CompletableFuture<List<Message>> routedFuture =
            ROUTED_PENDING.remove(requestId);
        if (routedFuture != null) {
            completeRoutedFuture(routedFuture, result, parts, partCount);
        } else {
            NativeMessage.multipartClose(parts, partCount);
        }
    }

    private static void completeRoutedFuture(
            CompletableFuture<List<Message>> future, int result,
            MemorySegment parts, long partCount) {
        try {
            if (result != RequestResult.OK.value()) {
                RequestReplySupport.completeExceptionallyAsync(future,
                    new ZlinkRequestException(RequestResult.fromValue(result),
                        result));
                return;
            }
            if (partCount == 1) {
                Message reply =
                    InternalAccess.messageFromOwnedSingleMessageVectorShared(
                        parts);
                RequestReplySupport.callbackCompletions().execute(() -> {
                    if (!future.complete(List.of(reply))) {
                        reply.close();
                    }
                });
                return;
            }
            Message[] frames =
                InternalAccess.messageFromOwnedMessageVectorShared(parts,
                    partCount);
            List<Message> replyParts = frames.length == 0
                ? List.of()
                : Arrays.asList(frames);
            RequestReplySupport.callbackCompletions().execute(() -> {
                if (!future.complete(replyParts)) {
                    Message.closeAll(frames);
                }
            });
        } catch (Throwable error) {
            RequestReplySupport.completeExceptionallyAsync(future, error);
        } finally {
            NativeMessage.multipartClose(parts, partCount);
        }
    }

}
