/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import systems.zlink.internal.ContractAccess;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.ContextOption;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ErrorCategory;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.eventing.ZlinkTimer;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.TopicMessage;
import systems.zlink.contracts.sockets.RequestCallback;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.internal.sockets.SocketOptionKey;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.runtime.eventing.NativeTimer;
import systems.zlink.runtime.sockets.SocketMessageHandler;
import systems.zlink.runtime.messaging.ReceivedPartCursor;
import java.lang.foreign.MemorySegment;
import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.function.BiConsumer;
import java.util.function.BiFunction;

/**
 * Non-exported bridge from runtime code to contract-owned internals.
 *
 * <p>Contract classes register their own accessors from inside the defining
 * package. This keeps implementation hooks out of the public API without using
 * reflection or MethodHandle-based private access.
 */
public final class InternalAccess {
    private static volatile ContextAccess contextAccess;    private static volatile SocketAccess socketAccess;
    private static volatile RuntimeSocketAccess runtimeSocketAccess;
    private static volatile TimerAccess timerAccess;
    private static volatile MonitorAccess monitorAccess;

    private InternalAccess() {
    }

    public interface ContextAccess {
        MemorySegment handle(Context context);
        void setOption(Context context, ContextOption option, int value);
        void setOptionData(Context context, ContextOption option, String value);
        int getOption(Context context, ContextOption option);
    }

    public interface SocketAccess {
        MemorySegment handle(Socket socket);
        void setOption(Socket socket, SocketOptionKey<Integer> option, int value);
        void setOption(Socket socket, SocketOptionKey<Long> option, long value);
        void setOption(Socket socket, SocketOptionKey<String> option, String value);
        void setOption(Socket socket, SocketOptionKey<byte[]> option, byte[] value);
        <T> T getOption(Socket socket, SocketOptionKey<T> option);
        void setDealerIntOption(Socket socket, int option, int value);
        int getDealerIntOption(Socket socket, int option);
        int getRouterIntOption(Socket socket, int option);
        void setRouterIntOption(Socket socket, int option, int value);
        boolean inCallback();
        void enterCallback();
        void leaveCallback();
    }

    public interface RuntimeSocketAccess {
        CompletableFuture<List<Message>> dealerRequestAsync(
            DealerSocket socket, List<Message> parts, SendFlags flags,
            Duration timeout);
        boolean dealerRequestCallback(DealerSocket socket, List<Message> parts,
                                      RequestCallback callback,
                                      SendFlags flags, Duration timeout);
        Object routerReceiveSupport(RouterSocket socket,
                                    boolean closeSocketOnClose);
        Received routerRecv(Object support, RecvFlags flags);
        boolean routerRecvInto(Object support, Received target,
                               RecvFlags flags);
        void routerOnReceive(Object support, SocketMessageHandler handler);
        void routerReceiveBeginClose(Object support);
        void routerReceiveFinishClose(Object support);
        CompletableFuture<List<Message>> routerRequestAsync(
            RouterSocket socket, RoutingId routingId, List<Message> parts,
            SendFlags flags, Duration timeout);
        boolean routerRequestCallback(
            RouterSocket socket, RoutingId routingId, List<Message> parts,
            RequestCallback callback, SendFlags flags, Duration timeout);
        void routerReply(RouterSocket socket, RoutingId routingId,
                         long requestSequence, List<Message> parts);
    }

    public interface TimerAccess {
        MemorySegment handle(ZlinkTimer timer);

        ZlinkTimer fromBorrowedHandle(MemorySegment handle);
    }

    public interface MonitorAccess {
        SocketMonitor create(MemorySegment handle, boolean own);
    }

    public static void register(ContextAccess access) {
        contextAccess = Objects.requireNonNull(access, "access");
    }

    public static void register(SocketAccess access) {
        socketAccess = Objects.requireNonNull(access, "access");
    }

    public static void register(RuntimeSocketAccess access) {
        runtimeSocketAccess = Objects.requireNonNull(access, "access");
    }

    public static void register(TimerAccess access) {
        timerAccess = Objects.requireNonNull(access, "access");
    }

    public static void register(MonitorAccess access) {
        monitorAccess = Objects.requireNonNull(access, "access");
    }

    public static MemorySegment contextHandle(Context context) {
        return contextAccess().handle(context);
    }

    public static SocketMonitor monitorSocket(MemorySegment handle,
                                              boolean own) {
        return monitorAccess().create(handle, own);
    }

    public static void contextSetOption(Context context, ContextOption option,
                                        int value) {
        contextAccess().setOption(context, option, value);
    }

    public static void contextSetOptionData(Context context,
                                            ContextOption option,
                                            String value) {
        contextAccess().setOptionData(context, option, value);
    }

    public static int contextGetOption(Context context, ContextOption option) {
        return contextAccess().getOption(context, option);
    }

    public static MemorySegment socketHandle(Socket socket) {
        return socketAccess().handle(socket);
    }

    public static void socketSetOption(Socket socket,
                                       SocketOptionKey<Integer> option,
                                       int value) {
        socketAccess().setOption(socket, option, value);
    }

    public static void socketSetOption(Socket socket,
                                       SocketOptionKey<Long> option,
                                       long value) {
        socketAccess().setOption(socket, option, value);
    }

    public static void socketSetOption(Socket socket,
                                       SocketOptionKey<String> option,
                                       String value) {
        socketAccess().setOption(socket, option, value);
    }

    public static void socketSetOption(Socket socket,
                                       SocketOptionKey<byte[]> option,
                                       byte[] value) {
        socketAccess().setOption(socket, option, value);
    }

    public static <T> T socketGetOption(Socket socket,
                                        SocketOptionKey<T> option) {
        return socketAccess().getOption(socket, option);
    }

    public static void socketSetDealerIntOption(Socket socket, int option,
                                                int value) {
        socketAccess().setDealerIntOption(socket, option, value);
    }

    public static int socketGetDealerIntOption(Socket socket, int option) {
        return socketAccess().getDealerIntOption(socket, option);
    }

    public static int socketGetRouterIntOption(Socket socket, int option) {
        return socketAccess().getRouterIntOption(socket, option);
    }

    public static void socketSetRouterIntOption(Socket socket, int option,
                                                int value) {
        socketAccess().setRouterIntOption(socket, option, value);
    }

    public static ZlinkTimer timerFromBorrowedHandle(MemorySegment handle) {
        return timerAccess().fromBorrowedHandle(handle);
    }

    public static MemorySegment timerHandle(ZlinkTimer timer) {
        return timerAccess().handle(timer);
    }

    public static MemorySegment messageDataSegment(Message message) {
        return (MemorySegment) ContractAccess.messageDataSegment(message);
    }

    public static MemorySegment messageDataSegment(Message message,
                                                   int knownSize) {
        return (MemorySegment) ContractAccess.messageDataSegment(message,
            knownSize);
    }

    public static void messageCopyTo(Message message,
                                     MemorySegment destination) {
        ContractAccess.messageCopyTo(message, destination);
    }

    public static Message messageFromSegment(MemorySegment segment,
                                             long offset,
                                             long length) {
        return ContractAccess.messageFromSegment(segment, offset, length);
    }

    public static void messageMoveTo(Message message,
                                     MemorySegment destination) {
        ContractAccess.messageMoveTo(message, destination);
    }

    public static MemorySegment messageNativeHandle(Message message) {
        return (MemorySegment) ContractAccess.messageNativeHandle(message);
    }

    public static void messageSetMore(Message message, boolean more) {
        ContractAccess.messageSetMore(message, more);
    }

    public static boolean messageMore(Message message) {
        return ContractAccess.messageMore(message);
    }

    public static void messageFinishReceive(Message message, boolean more) {
        ContractAccess.messageFinishReceive(message, more);
    }

    public static Object messageTransferTo(Message message,
                                           MemorySegment destination) {
        ContractAccess.messageTransferTo(message, destination);
        return null;
    }

    public static void messageRestoreFromNative(Message message,
                                                MemorySegment source,
                                                boolean moreFlag,
                                                Object anchor) {
        ContractAccess.messageRestoreFromNative(message, source, moreFlag);
    }

    public static void messageMarkTransferred(Message message) {
        ContractAccess.messageMarkTransferred(message);
    }

    public static int messageMoveInto(Message source, Message target,
                                      boolean moreFlag) {
        return ContractAccess.messageMoveInto(source, target, moreFlag);
    }

    public static Message messageSharedCopyOf(Message message) {
        return ContractAccess.messageSharedCopyOf(message);
    }

    public static Message[] messageFromMessageVector(MemorySegment partsAddr,
                                                 long count) {
        return ContractAccess.nativeMessageMaterializeVector(partsAddr, count,
            null, true);
    }

    public static Message[] messageFromOwnedMessageVector(MemorySegment partsAddr,
                                                      long count) {
        return ContractAccess.nativeMessageMaterializeVector(partsAddr, count,
            null, false);
    }

    public static Message[] messageFromOwnedMessageVectorShared(
      MemorySegment partsAddr,
      long count) {
        return ContractAccess.nativeMessageMaterializeVectorShared(partsAddr,
            count);
    }

    public static Message messageFromOwnedNative(MemorySegment nativeMsg) {
        return ContractAccess.nativeMessageAdoptOwned(nativeMsg);
    }

    public static Received received(RoutingId routingId, Message[] parts) {
        return ContractAccess.received(routingId, parts);
    }

    public static Received received(RoutingId routingId, Message[] parts,
                                    long requestSeq,
                                    boolean hasRequestSeq,
                                    BiConsumer<List<Message>, SendFlags> replySender) {
        return ContractAccess.received(routingId, parts, requestSeq,
            hasRequestSeq, replySender);
    }

    public static Received received(RoutingId routingId, Message[] parts,
                                    boolean trustedParts,
                                    long requestSeq,
                                    boolean hasRequestSeq,
                                    BiConsumer<List<Message>, SendFlags> replySender) {
        return ContractAccess.received(routingId, parts, trustedParts,
            requestSeq, hasRequestSeq, replySender);
    }

    public static Received received(RoutingId routingId, Message[] parts,
                                    boolean trustedParts,
                                    long requestSeq,
                                    boolean hasRequestSeq,
                                    BiConsumer<List<Message>, SendFlags> replySender,
                                    Runnable onTerminalState) {
        return ContractAccess.received(routingId, parts, trustedParts,
            requestSeq, hasRequestSeq, replySender, onTerminalState);
    }

    public static Received received(byte[] routingIdBytes, Message[] parts,
                                    boolean trustedParts,
                                    long requestSeq,
                                    boolean hasRequestSeq,
                                    BiConsumer<List<Message>, SendFlags> replySender,
                                    Runnable onTerminalState) {
        return ContractAccess.received(routingIdBytes, parts,
            trustedParts, requestSeq, hasRequestSeq, replySender,
            onTerminalState);
    }

    public static Received receivedLazy(byte[] routingIdBytes,
                                        Message firstPart,
                                        ReceivedPartCursor cursor,
                                        long requestSeq,
                                        boolean hasRequestSeq,
                                        BiConsumer<List<Message>, SendFlags> replySender,
                                        Runnable onTerminalState) {
        return ContractAccess.receivedLazy(routingIdBytes, firstPart, cursor,
            requestSeq, hasRequestSeq, replySender,
            onTerminalState);
    }

    public static Received receivedLazy(RoutingId routingId, Message firstPart,
                                        ReceivedPartCursor cursor,
                                        long requestSeq,
                                        boolean hasRequestSeq,
                                        BiConsumer<List<Message>, SendFlags> replySender,
                                        Runnable onTerminalState) {
        return ContractAccess.receivedLazy(routingId, firstPart, cursor,
            requestSeq, hasRequestSeq, replySender, onTerminalState);
    }

    public static TopicMessage topicMessage(RoutingId routingId,
                                            String topicId,
                                            Message[] parts) {
        return ContractAccess.topicMessage(routingId, topicId, parts);
    }

    public static void topicMessageAdoptSingle(TopicMessage target,
                                               RoutingId routingId,
                                               String topicId,
                                               Message part) {
        ContractAccess.topicMessageAdoptSingle(target, routingId, topicId, part);
    }

    public static Message topicMessagePrepareReusableSinglePart(
      TopicMessage target) {
        return ContractAccess.topicMessagePrepareReusableSinglePart(target);
    }

    public static void receivedForceMaterialize(Received received) {
        ContractAccess.receivedForceMaterialize(received);
    }

    public static List<Message> receivedTakeParts(Received received) {
        return ContractAccess.receivedTakeParts(received);
    }

    public static void receivedSetSendSender(Received received,
                                             BiFunction<List<Message>, SendFlags,
                                                 Boolean> sendSender) {
        ContractAccess.receivedSetSendSender(received, sendSender);
    }

    public static boolean inCallback() {
        return socketAccess().inCallback();
    }

    public static void enterCallback() {
        socketAccess().enterCallback();
    }

    public static void leaveCallback() {
        socketAccess().leaveCallback();
    }

    public static CompletableFuture<List<Message>> dealerRequestAsync(
            DealerSocket socket, List<Message> parts, SendFlags flags,
            Duration timeout) {
        return runtimeSocketAccess().dealerRequestAsync(socket, parts, flags,
            timeout);
    }

    public static boolean dealerRequestCallback(
            DealerSocket socket, List<Message> parts, RequestCallback callback,
            SendFlags flags, Duration timeout) {
        return runtimeSocketAccess().dealerRequestCallback(socket, parts,
            callback, flags, timeout);
    }

    public static Object routerReceiveSupport(RouterSocket socket,
                                              boolean closeSocketOnClose) {
        return runtimeSocketAccess().routerReceiveSupport(socket,
            closeSocketOnClose);
    }

    public static Received routerRecv(Object support, RecvFlags flags) {
        return runtimeSocketAccess().routerRecv(support, flags);
    }

    public static boolean routerRecvInto(Object support, Received target,
                                         RecvFlags flags) {
        return runtimeSocketAccess().routerRecvInto(support, target, flags);
    }

    public static void routerOnReceive(Object support,
                                       SocketMessageHandler handler) {
        runtimeSocketAccess().routerOnReceive(support, handler);
    }

    public static void routerReceiveBeginClose(Object support) {
        runtimeSocketAccess().routerReceiveBeginClose(support);
    }

    public static void routerReceiveFinishClose(Object support) {
        runtimeSocketAccess().routerReceiveFinishClose(support);
    }

    public static CompletableFuture<List<Message>> routerRequestAsync(
            RouterSocket socket, RoutingId routingId, List<Message> parts,
            SendFlags flags, Duration timeout) {
        return runtimeSocketAccess().routerRequestAsync(socket, routingId,
            parts, flags, timeout);
    }

    public static boolean routerRequestCallback(
            RouterSocket socket, RoutingId routingId, List<Message> parts,
            RequestCallback callback, SendFlags flags, Duration timeout) {
        return runtimeSocketAccess().routerRequestCallback(socket, routingId,
            parts, callback, flags, timeout);
    }

    public static void routerReply(RouterSocket socket, RoutingId routingId,
                                   long requestSequence, List<Message> parts) {
        runtimeSocketAccess().routerReply(socket, routingId, requestSequence,
            parts);
    }

    public static RoutingId routingIdFromTrusted(byte[] value) {
        return ContractAccess.routingIdFromTrusted(value);
    }

    public static byte[] routingIdTrustedBytes(RoutingId routingId) {
        return ContractAccess.routingIdTrustedBytes(routingId);
    }

    public static ZlinkException zlinkExceptionFromLastError(
            ErrorCategory category) {
        return ZlinkException.fromErrno(category, Native.errno());
    }

    public static ZlinkException zlinkExceptionFromErrno(ErrorCategory category,
                                                         int errno) {
        return ZlinkException.fromErrno(category, errno);
    }

    private static ContextAccess contextAccess() {
        if (contextAccess == null)
            load("systems.zlink.runtime.core.NativeContext");
        return require(contextAccess, Context.class);
    }

    private static SocketAccess socketAccess() {
        if (socketAccess == null) load("systems.zlink.runtime.sockets.NativeSocketBase");
        return require(socketAccess, Socket.class);
    }

    private static RuntimeSocketAccess runtimeSocketAccess() {
        if (runtimeSocketAccess == null)
            load("systems.zlink.runtime.sockets.NativeDealerRequestSupport");
        return require(runtimeSocketAccess, Socket.class);
    }

    private static TimerAccess timerAccess() {
        if (timerAccess == null) load(NativeTimer.class);
        return require(timerAccess, NativeTimer.class);
    }

    private static MonitorAccess monitorAccess() {
        if (monitorAccess == null)
            load("systems.zlink.runtime.eventing.NativeMonitorSocket");
        return require(monitorAccess, SocketMonitor.class);
    }

    private static <T> T require(T access, Class<?> ownerType) {
        if (access != null) {
            return access;
        }
        throw new IllegalStateException(
            "internal access not registered: " + ownerType.getName());
    }

    private static void load(Class<?> ownerType) {
        try {
            Class.forName(ownerType.getName(), true, ownerType.getClassLoader());
        } catch (ClassNotFoundException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    private static void load(String className) {
        try {
            Class.forName(className, true, InternalAccess.class.getClassLoader());
        } catch (ClassNotFoundException e) {
            throw new ExceptionInInitializerError(e);
        }
    }
}
