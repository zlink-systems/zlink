/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.internal;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.ContextOption;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.SubscriptionEvent;
import systems.zlink.contracts.messaging.TopicMessage;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.CommonSocketOptions;
import systems.zlink.contracts.sockets.DealerSocketOptions;
import systems.zlink.contracts.sockets.PubSocketOptions;
import systems.zlink.contracts.sockets.RouterSocketOptions;
import systems.zlink.contracts.sockets.StreamSocketOptions;
import systems.zlink.contracts.sockets.SubSocketOptions;
import systems.zlink.internal.sockets.SocketOptionKey;
import systems.zlink.contracts.sockets.SendFlags;
import java.nio.ByteBuffer;
import java.util.Objects;
import java.util.List;
import java.util.ServiceLoader;
import java.util.function.BiConsumer;
import java.util.function.BiFunction;

/**
 * Non-exported bridge for runtime code that must fill contract-owned state.
 */
public final class ContractAccess {
    private static volatile PollEventsAccess pollEventsAccess;
    private static volatile RoutingIdAccess routingIdAccess;
    private static volatile ContextOptionsAccess contextOptionsAccess;
    private static volatile SocketOptionsAccess socketOptionsAccess;
    private static volatile SocketOptionFacadesAccess socketOptionFacadesAccess;
    private static volatile NativeErrorAccess nativeErrorAccess;
    private static volatile ReceivedAccess receivedAccess;
    private static volatile SubscriptionEventAccess subscriptionEventAccess;
    private static volatile TopicMessageAccess topicMessageAccess;
    private static volatile MessageAccess messageAccess;
    private static volatile NativeMessageAccess nativeMessageAccess;

    private ContractAccess() {
    }

    public interface PollEventsAccess {
        void markReadyCount(PollEvents events, int readyCount);

        void markEvent(PollEvents events, int index, int sourceKindValue,
                       long slot, int revents, int fd);
    }

    public interface RoutingIdAccess {
        RoutingId fromTrusted(byte[] value);

        RoutingId tryFromInlineCached(int size, long lo, long hi);

        byte[] trustedBytes(RoutingId routingId);
    }

    public interface ContextOptionsAccess {
        void setOption(Context context, ContextOption option, int value);

        void setUInt64Option(Context context, ContextOption option, long value);

        void setOptionData(Context context, ContextOption option, String value);

        int getOption(Context context, ContextOption option);

        long getUInt64Option(Context context, ContextOption option);
    }

    public interface SocketOptionsAccess {
        void setOption(Socket socket, SocketOptionKey<Integer> option,
                       int value);

        void setOption(Socket socket, SocketOptionKey<Long> option,
                       long value);

        void setOption(Socket socket, SocketOptionKey<String> option,
                       String value);

        void setOption(Socket socket, SocketOptionKey<byte[]> option,
                       byte[] value);

        <T> T getOption(Socket socket, SocketOptionKey<T> option);

        void setDealerIntOption(Socket socket, int option, int value);

        int getDealerIntOption(Socket socket, int option);

        int getRouterIntOption(Socket socket, int option);

        void setRouterIntOption(Socket socket, int option, int value);
    }

    public interface SocketOptionFacadesAccess {
        CommonSocketOptions common(Socket socket);

        DealerSocketOptions dealer(Socket socket);

        PubSocketOptions pub(Socket socket);

        RouterSocketOptions router(Socket socket);

        StreamSocketOptions stream(Socket socket);

        SubSocketOptions sub(Socket socket);
    }

    public interface NativeErrorAccess {
        int errno();

        String strerror(int errno);
    }

    public interface ReceivedPartCursor extends AutoCloseable {
        Message nextPartOrNull();

        @Override
        void close();
    }

    public interface ReceivedAccess {
        Received create(RoutingId routingId, Message[] parts);

        Received create(RoutingId routingId, Message[] parts,
                        boolean trustedParts,
                        long requestSeq, boolean hasRequestSeq,
                        BiConsumer<List<Message>, SendFlags> replySender);

        Received create(RoutingId routingId, Message[] parts,
                        boolean trustedParts,
                        long requestSeq, boolean hasRequestSeq,
                        BiConsumer<List<Message>, SendFlags> replySender,
                        Runnable onTerminalState);

        Received create(byte[] routingIdBytes, Message[] parts,
                        boolean trustedParts,
                        long requestSeq, boolean hasRequestSeq,
                        BiConsumer<List<Message>, SendFlags> replySender,
                        Runnable onTerminalState);

        Received create(RoutingId routingId, Message[] parts, long requestSeq,
                        boolean hasRequestSeq,
                        BiConsumer<List<Message>, SendFlags> replySender);

        Received createLazy(byte[] routingIdBytes, Message firstPart,
                            ReceivedPartCursor cursor,
                            long requestSeq, boolean hasRequestSeq,
                            BiConsumer<List<Message>, SendFlags> replySender,
                            Runnable onTerminalState);

        Received createLazy(RoutingId routingId, Message firstPart,
                            ReceivedPartCursor cursor,
                            long requestSeq, boolean hasRequestSeq,
                            BiConsumer<List<Message>, SendFlags> replySender,
                            Runnable onTerminalState);

        void populateRoutedSinglePart(Received received,
                                      byte[] routingIdBytes,
                                      Message singlePart,
                                      long requestSequence,
                                      boolean hasRequestSequence,
                                      BiConsumer<List<Message>, SendFlags>
                                          replySender,
                                      Runnable onTerminalState);

        void forceMaterialize(Received received);

        List<Message> takeParts(Received received);

        void setSendSender(Received received,
                           BiFunction<List<Message>, SendFlags, Boolean> sendSender);

        void adoptFrom(Received target, Received source);
    }

    public interface SubscriptionEventAccess {
        SubscriptionEvent create(java.util.Optional<RoutingId> routingId,
                                 String topic,
                                 boolean subscribed);

        void adoptFrom(SubscriptionEvent target, SubscriptionEvent source);
    }

    public interface TopicMessageAccess {
        TopicMessage create(RoutingId routingId, String topicId,
                            Message[] parts);

        void adoptSingle(TopicMessage target, RoutingId routingId,
                         String topicId, Message part);

        Message prepareReusableSinglePart(TopicMessage target);

        void adoptFrom(TopicMessage target, TopicMessage source);
    }

    public interface MessageAccess {
        Object dataSegment(Message message);

        Object dataSegment(Message message, int knownSize);

        void copyTo(Message message, Object destination);

        void moveTo(Message message, Object destination);

        Object nativeHandle(Message message);

        void setMore(Message message, boolean more);

        boolean more(Message message);

        void finishReceive(Message message, boolean more);

        void transferTo(Message message, Object destination);

        void restoreFromNative(Message message, Object source,
                               boolean moreFlag);

        void markTransferred(Message message);

        int moveInto(Message source, Message target, boolean moreFlag);

        Message sharedCopyOf(Message message);

        Message prepareVectorTarget(Message message);

        void finishVectorMove(Message message, boolean moreFlag);

        void resetReusable(Message message);

        Message adoptOwnedNative(Object nativeMsg);

        Message fromSegment(Object segment, long offset, long length);

        boolean isReusable(Message message);
    }

    public interface NativeMessageAccess {
        long layoutSize();

        Object openSharedMessageScope();

        boolean messageScopeAlive(Object scope);

        void closeMessageScope(Object scope);

        Object allocate(Object scope);

        Object handleFromAddress(long address);

        Object segmentFromAddress(long address, long size);

        Object slice(Object segment, long offset, long size);

        long address(Object segment);

        int init(Object msg);

        int initSize(Object msg, int size);

        int close(Object msg);

        int move(Object destination, Object source);

        int copy(Object destination, Object source);

        long size(Object msg);

        long dataAddress(Object msg);

        int refCount(Object msg);

        void closeVector(Object parts, long count);

        Message[] materializeVector(Object parts, long count,
                                    Message[] reusable,
                                    boolean closeSourceVector);

        Message[] materializeVectorShared(Object parts, long count);

        Message adoptOwnedMessage(Object nativeMsg);

        void copyFromSegment(Object source, long sourceOffset, Object destination,
                             long destinationOffset, long length);

        void copyFromBuffer(ByteBuffer source, Object destination,
                            long destinationOffset, long length);

        void copyToBuffer(Object source, long sourceOffset,
                          ByteBuffer destination, long length);

        ByteBuffer asReadOnlyBuffer(Object source);

        ByteBuffer asMutableBuffer(Object source);
    }

    public static void register(PollEventsAccess access) {
        pollEventsAccess = Objects.requireNonNull(access, "access");
    }

    public static void register(RoutingIdAccess access) {
        routingIdAccess = Objects.requireNonNull(access, "access");
    }

    public static void register(ContextOptionsAccess access) {
        contextOptionsAccess = Objects.requireNonNull(access, "access");
    }

    public static void register(SocketOptionsAccess access) {
        socketOptionsAccess = Objects.requireNonNull(access, "access");
    }

    public static void register(SocketOptionFacadesAccess access) {
        socketOptionFacadesAccess = Objects.requireNonNull(access, "access");
    }

    public static void register(NativeErrorAccess access) {
        nativeErrorAccess = Objects.requireNonNull(access, "access");
    }

    public static void register(ReceivedAccess access) {
        receivedAccess = Objects.requireNonNull(access, "access");
    }

    public static void register(SubscriptionEventAccess access) {
        subscriptionEventAccess = Objects.requireNonNull(access, "access");
    }

    public static void register(TopicMessageAccess access) {
        topicMessageAccess = Objects.requireNonNull(access, "access");
    }

    public static void register(MessageAccess access) {
        messageAccess = Objects.requireNonNull(access, "access");
    }

    public static void register(NativeMessageAccess access) {
        nativeMessageAccess = Objects.requireNonNull(access, "access");
    }

    public static void pollEventsMarkReadyCount(PollEvents events,
                                                int readyCount) {
        pollEventsAccess().markReadyCount(events, readyCount);
    }

    public static void pollEventsMarkEvent(PollEvents events, int index,
                                           int sourceKindValue, long slot,
                                           int revents, int fd) {
        pollEventsAccess().markEvent(events, index, sourceKindValue, slot,
            revents, fd);
    }

    public static RoutingId routingIdFromTrusted(byte[] value) {
        return routingIdAccess().fromTrusted(value);
    }

    public static RoutingId routingIdTryFromInlineCached(int size, long lo,
                                                         long hi) {
        return routingIdAccess().tryFromInlineCached(size, lo, hi);
    }

    public static byte[] routingIdTrustedBytes(RoutingId routingId) {
        return routingIdAccess().trustedBytes(routingId);
    }

    public static void contextSetOption(Context context, ContextOption option,
                                        int value) {
        contextOptionsAccess().setOption(context, option, value);
    }

    public static void contextSetUInt64Option(Context context,
                                              ContextOption option,
                                              long value) {
        contextOptionsAccess().setUInt64Option(context, option, value);
    }

    public static void contextSetOptionData(Context context,
                                            ContextOption option,
                                            String value) {
        contextOptionsAccess().setOptionData(context, option, value);
    }

    public static int contextGetOption(Context context, ContextOption option) {
        return contextOptionsAccess().getOption(context, option);
    }

    public static long contextGetUInt64Option(Context context,
                                              ContextOption option) {
        return contextOptionsAccess().getUInt64Option(context, option);
    }

    public static void socketSetOption(Socket socket,
                                       SocketOptionKey<Integer> option,
                                       int value) {
        socketOptionsAccess().setOption(socket, option, value);
    }

    public static void socketSetOption(Socket socket,
                                       SocketOptionKey<Long> option,
                                       long value) {
        socketOptionsAccess().setOption(socket, option, value);
    }

    public static void socketSetOption(Socket socket,
                                       SocketOptionKey<String> option,
                                       String value) {
        socketOptionsAccess().setOption(socket, option, value);
    }

    public static void socketSetOption(Socket socket,
                                       SocketOptionKey<byte[]> option,
                                       byte[] value) {
        socketOptionsAccess().setOption(socket, option, value);
    }

    public static <T> T socketGetOption(Socket socket,
                                        SocketOptionKey<T> option) {
        return socketOptionsAccess().getOption(socket, option);
    }

    public static void socketSetDealerIntOption(Socket socket, int option,
                                                int value) {
        socketOptionsAccess().setDealerIntOption(socket, option, value);
    }

    public static int socketGetDealerIntOption(Socket socket, int option) {
        return socketOptionsAccess().getDealerIntOption(socket, option);
    }

    public static int socketGetRouterIntOption(Socket socket, int option) {
        return socketOptionsAccess().getRouterIntOption(socket, option);
    }

    public static void socketSetRouterIntOption(Socket socket, int option,
                                                int value) {
        socketOptionsAccess().setRouterIntOption(socket, option, value);
    }

    public static CommonSocketOptions commonSocketOptions(Socket socket) {
        return socketOptionFacadesAccess().common(socket);
    }

    public static DealerSocketOptions dealerSocketOptions(Socket socket) {
        return socketOptionFacadesAccess().dealer(socket);
    }

    public static PubSocketOptions pubSocketOptions(Socket socket) {
        return socketOptionFacadesAccess().pub(socket);
    }

    public static RouterSocketOptions routerSocketOptions(Socket socket) {
        return socketOptionFacadesAccess().router(socket);
    }

    public static StreamSocketOptions streamSocketOptions(Socket socket) {
        return socketOptionFacadesAccess().stream(socket);
    }

    public static SubSocketOptions subSocketOptions(Socket socket) {
        return socketOptionFacadesAccess().sub(socket);
    }

    public static int nativeErrno() {
        return nativeErrorAccess().errno();
    }

    public static String nativeStrerror(int errno) {
        return nativeErrorAccess().strerror(errno);
    }

    public static Received received(RoutingId routingId, Message[] parts) {
        return receivedAccess().create(routingId, parts);
    }

    public static Received received(RoutingId routingId, Message[] parts,
                                    long requestSeq,
                                    boolean hasRequestSeq,
                                    BiConsumer<List<Message>, SendFlags> replySender) {
        return receivedAccess().create(routingId, parts, requestSeq,
            hasRequestSeq, replySender);
    }

    public static Received received(RoutingId routingId, Message[] parts,
                                    boolean trustedParts,
                                    long requestSeq,
                                    boolean hasRequestSeq,
                                    BiConsumer<List<Message>, SendFlags> replySender) {
        return receivedAccess().create(routingId, parts, trustedParts,
            requestSeq, hasRequestSeq, replySender);
    }

    public static Received received(RoutingId routingId, Message[] parts,
                                    boolean trustedParts,
                                    long requestSeq,
                                    boolean hasRequestSeq,
                                    BiConsumer<List<Message>, SendFlags> replySender,
                                    Runnable onTerminalState) {
        return receivedAccess().create(routingId, parts, trustedParts,
            requestSeq, hasRequestSeq, replySender, onTerminalState);
    }

    public static Received received(byte[] routingIdBytes, Message[] parts,
                                    boolean trustedParts,
                                    long requestSeq,
                                    boolean hasRequestSeq,
                                    BiConsumer<List<Message>, SendFlags> replySender,
                                    Runnable onTerminalState) {
        return receivedAccess().create(routingIdBytes, parts,
            trustedParts, requestSeq, hasRequestSeq, replySender,
            onTerminalState);
    }

    public static Received receivedLazy(byte[] routingIdBytes, Message firstPart,
                                        ReceivedPartCursor cursor,
                                        long requestSeq,
                                        boolean hasRequestSeq,
                                        BiConsumer<List<Message>, SendFlags> replySender,
                                        Runnable onTerminalState) {
        return receivedAccess().createLazy(routingIdBytes, firstPart, cursor,
            requestSeq, hasRequestSeq, replySender,
            onTerminalState);
    }

    public static Received receivedLazy(RoutingId routingId, Message firstPart,
                                        ReceivedPartCursor cursor,
                                        long requestSeq,
                                        boolean hasRequestSeq,
                                        BiConsumer<List<Message>, SendFlags> replySender,
                                        Runnable onTerminalState) {
        return receivedAccess().createLazy(routingId, firstPart, cursor,
            requestSeq, hasRequestSeq, replySender, onTerminalState);
    }

    public static TopicMessage topicMessage(RoutingId routingId,
                                            String topicId,
                                            Message[] parts) {
        return topicMessageAccess().create(routingId, topicId, parts);
    }

    public static void topicMessageAdoptSingle(TopicMessage target,
                                               RoutingId routingId,
                                               String topicId,
                                               Message part) {
        topicMessageAccess().adoptSingle(target, routingId, topicId, part);
    }

    public static void topicMessageAdoptFrom(TopicMessage target,
                                             TopicMessage source) {
        topicMessageAccess().adoptFrom(target, source);
    }

    public static Message topicMessagePrepareReusableSinglePart(
      TopicMessage target) {
        return topicMessageAccess().prepareReusableSinglePart(target);
    }

    public static void subscriptionEventAdoptFrom(SubscriptionEvent target,
                                                  SubscriptionEvent source) {
        subscriptionEventAccess().adoptFrom(target, source);
    }

    public static SubscriptionEvent subscriptionEvent(
      java.util.Optional<RoutingId> routingId, String topic, boolean subscribed) {
        return subscriptionEventAccess().create(routingId, topic, subscribed);
    }

    public static void receivedForceMaterialize(Received received) {
        receivedAccess().forceMaterialize(received);
    }

    public static List<Message> receivedTakeParts(Received received) {
        return receivedAccess().takeParts(received);
    }

    public static void receivedSetSendSender(Received received,
                                             BiFunction<List<Message>, SendFlags,
                                                 Boolean> sendSender) {
        receivedAccess().setSendSender(received, sendSender);
    }

    public static void receivedAdoptFrom(Received target, Received source) {
        receivedAccess().adoptFrom(target, source);
    }

    public static void receivedPopulateRoutedSinglePart(
      Received received,
      byte[] routingIdBytes,
      Message singlePart,
      long requestSequence,
      boolean hasRequestSequence,
      BiConsumer<List<Message>, SendFlags> replySender,
      Runnable onTerminalState) {
        receivedAccess().populateRoutedSinglePart(received, routingIdBytes,
            singlePart, requestSequence, hasRequestSequence,
            replySender, onTerminalState);
    }

    public static Object messageDataSegment(Message message) {
        return messageAccess().dataSegment(message);
    }

    public static Object messageDataSegment(Message message, int knownSize) {
        return messageAccess().dataSegment(message, knownSize);
    }

    public static void messageCopyTo(Message message, Object destination) {
        messageAccess().copyTo(message, destination);
    }

    public static Message messageFromSegment(Object segment, long offset,
                                             long length) {
        return messageAccess().fromSegment(segment, offset, length);
    }

    public static void messageMoveTo(Message message, Object destination) {
        messageAccess().moveTo(message, destination);
    }

    public static Object messageNativeHandle(Message message) {
        return messageAccess().nativeHandle(message);
    }

    public static void messageSetMore(Message message, boolean more) {
        messageAccess().setMore(message, more);
    }

    public static boolean messageMore(Message message) {
        return messageAccess().more(message);
    }

    public static void messageFinishReceive(Message message, boolean more) {
        messageAccess().finishReceive(message, more);
    }

    public static void messageTransferTo(Message message, Object destination) {
        messageAccess().transferTo(message, destination);
    }

    public static void messageRestoreFromNative(Message message, Object source,
                                                boolean moreFlag) {
        messageAccess().restoreFromNative(message, source, moreFlag);
    }

    public static void messageMarkTransferred(Message message) {
        messageAccess().markTransferred(message);
    }

    public static int messageMoveInto(Message source, Message target,
                                      boolean moreFlag) {
        return messageAccess().moveInto(source, target, moreFlag);
    }

    public static Message messageSharedCopyOf(Message message) {
        return messageAccess().sharedCopyOf(message);
    }

    public static Message messagePrepareVectorTarget(Message message) {
        return messageAccess().prepareVectorTarget(message);
    }

    public static void messageFinishVectorMove(Message message,
                                               boolean moreFlag) {
        messageAccess().finishVectorMove(message, moreFlag);
    }

    public static void messageResetReusable(Message message) {
        messageAccess().resetReusable(message);
    }

    public static boolean messageIsReusable(Message message) {
        return messageAccess().isReusable(message);
    }

    public static Message messageAdoptOwnedNative(Object nativeMsg) {
        return messageAccess().adoptOwnedNative(nativeMsg);
    }

    public static long nativeMessageLayoutSize() {
        return nativeMessageAccess().layoutSize();
    }

    public static Object nativeMessageOpenSharedScope() {
        return nativeMessageAccess().openSharedMessageScope();
    }

    public static boolean nativeMessageScopeAlive(Object scope) {
        return nativeMessageAccess().messageScopeAlive(scope);
    }

    public static void nativeMessageCloseScope(Object scope) {
        nativeMessageAccess().closeMessageScope(scope);
    }

    public static Object nativeMessageAllocate(Object scope) {
        return nativeMessageAccess().allocate(scope);
    }

    public static Object nativeMessageHandleFromAddress(long address) {
        return nativeMessageAccess().handleFromAddress(address);
    }

    public static Object nativeMessageSegmentFromAddress(long address,
                                                         long size) {
        return nativeMessageAccess().segmentFromAddress(address, size);
    }

    public static Object nativeMessageSlice(Object segment, long offset,
                                            long size) {
        return nativeMessageAccess().slice(segment, offset, size);
    }

    public static long nativeMessageAddress(Object segment) {
        return nativeMessageAccess().address(segment);
    }

    public static int nativeMessageInit(Object msg) {
        return nativeMessageAccess().init(msg);
    }

    public static int nativeMessageInitSize(Object msg, int size) {
        return nativeMessageAccess().initSize(msg, size);
    }

    public static int nativeMessageClose(Object msg) {
        return nativeMessageAccess().close(msg);
    }

    public static int nativeMessageMove(Object destination, Object source) {
        return nativeMessageAccess().move(destination, source);
    }

    public static int nativeMessageCopy(Object destination, Object source) {
        return nativeMessageAccess().copy(destination, source);
    }

    public static long nativeMessageSize(Object msg) {
        return nativeMessageAccess().size(msg);
    }

    public static long nativeMessageDataAddress(Object msg) {
        return nativeMessageAccess().dataAddress(msg);
    }

    public static int nativeMessageRefCount(Object msg) {
        return nativeMessageAccess().refCount(msg);
    }

    public static void nativeMessageCloseVector(Object parts, long count) {
        nativeMessageAccess().closeVector(parts, count);
    }

    public static Message[] nativeMessageMaterializeVector(
        Object parts, long count, Message[] reusable,
        boolean closeSourceVector) {
        return nativeMessageAccess().materializeVector(parts, count, reusable,
            closeSourceVector);
    }

    public static Message[] nativeMessageMaterializeVectorShared(
        Object parts, long count) {
        return nativeMessageAccess().materializeVectorShared(parts, count);
    }

    public static Message nativeMessageAdoptOwned(Object nativeMsg) {
        return nativeMessageAccess().adoptOwnedMessage(nativeMsg);
    }

    public static void nativeMessageCopyFromSegment(Object source,
                                                    long sourceOffset,
                                                    Object destination,
                                                    long destinationOffset,
                                                    long length) {
        nativeMessageAccess().copyFromSegment(source, sourceOffset,
            destination, destinationOffset, length);
    }

    public static void nativeMessageCopyFromBuffer(ByteBuffer source,
                                                   Object destination,
                                                   long destinationOffset,
                                                   long length) {
        nativeMessageAccess().copyFromBuffer(source, destination,
            destinationOffset, length);
    }

    public static void nativeMessageCopyToBuffer(Object source,
                                                 long sourceOffset,
                                                 ByteBuffer destination,
                                                 long length) {
        nativeMessageAccess().copyToBuffer(source, sourceOffset, destination,
            length);
    }

    public static ByteBuffer nativeMessageAsReadOnlyBuffer(Object source) {
        return nativeMessageAccess().asReadOnlyBuffer(source);
    }

    public static ByteBuffer nativeMessageAsMutableBuffer(Object source) {
        return nativeMessageAccess().asMutableBuffer(source);
    }

    private static PollEventsAccess pollEventsAccess() {
        if (pollEventsAccess == null) load(PollEvents.class);
        if (pollEventsAccess == null)
            throw new IllegalStateException(
                "missing contract access for " + PollEvents.class.getName());
        return pollEventsAccess;
    }

    private static RoutingIdAccess routingIdAccess() {
        if (routingIdAccess == null) load(RoutingId.class);
        if (routingIdAccess == null)
            throw new IllegalStateException(
                "missing contract access for " + RoutingId.class.getName());
        return routingIdAccess;
    }

    private static ContextOptionsAccess contextOptionsAccess() {
        if (contextOptionsAccess == null)
            throw new IllegalStateException(
                "missing contract access for context options");
        return contextOptionsAccess;
    }

    private static SocketOptionsAccess socketOptionsAccess() {
        if (socketOptionsAccess == null)
            throw new IllegalStateException(
                "missing contract access for socket options");
        return socketOptionsAccess;
    }

    private static SocketOptionFacadesAccess socketOptionFacadesAccess() {
        if (socketOptionFacadesAccess == null) load(CommonSocketOptions.class);
        if (socketOptionFacadesAccess == null)
            throw new IllegalStateException(
                "missing contract access for socket option facades");
        return socketOptionFacadesAccess;
    }

    private static NativeErrorAccess nativeErrorAccess() {
        if (nativeErrorAccess == null) {
            runtimeBridgeProvider().initializeNativeErrorAccess();
        }
        if (nativeErrorAccess == null)
            throw new IllegalStateException(
                "missing contract access for native errors");
        return nativeErrorAccess;
    }

    private static ReceivedAccess receivedAccess() {
        if (receivedAccess == null) load(Received.class);
        if (receivedAccess == null)
            throw new IllegalStateException(
                "missing contract access for " + Received.class.getName());
        return receivedAccess;
    }

    private static SubscriptionEventAccess subscriptionEventAccess() {
        if (subscriptionEventAccess == null) load(SubscriptionEvent.class);
        if (subscriptionEventAccess == null)
            throw new IllegalStateException(
                "missing contract access for "
                    + SubscriptionEvent.class.getName());
        return subscriptionEventAccess;
    }

    private static TopicMessageAccess topicMessageAccess() {
        if (topicMessageAccess == null) load(TopicMessage.class);
        if (topicMessageAccess == null)
            throw new IllegalStateException(
                "missing contract access for "
                    + TopicMessage.class.getName());
        return topicMessageAccess;
    }

    private static MessageAccess messageAccess() {
        if (messageAccess == null) load(Message.class);
        if (messageAccess == null)
            throw new IllegalStateException(
                "missing contract access for " + Message.class.getName());
        return messageAccess;
    }

    private static NativeMessageAccess nativeMessageAccess() {
        if (nativeMessageAccess == null) {
            runtimeBridgeProvider().initializeNativeMessageAccess();
        }
        if (nativeMessageAccess == null)
            throw new IllegalStateException(
                "missing contract access for native message runtime");
        return nativeMessageAccess;
    }

    private static void load(Class<?> type) {
        try {
            Class.forName(type.getName(), true, type.getClassLoader());
        } catch (ClassNotFoundException ex) {
            throw new IllegalStateException("cannot load " + type.getName(),
                ex);
        }
    }

    private static RuntimeBridgeProvider runtimeBridgeProvider() {
        return RuntimeBridgeProviderHolder.INSTANCE;
    }

    private static final class RuntimeBridgeProviderHolder {
        private static final RuntimeBridgeProvider INSTANCE = ServiceLoader
            .load(RuntimeBridgeProvider.class, ContractAccess.class.getClassLoader())
            .findFirst()
            .orElseThrow(() -> new IllegalStateException(
                "missing runtime bridge provider"));
    }

}
