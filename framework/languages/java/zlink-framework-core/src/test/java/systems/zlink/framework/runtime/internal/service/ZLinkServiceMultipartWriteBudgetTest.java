package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.io.IOException;
import java.lang.classfile.ClassFile;
import java.lang.classfile.ClassTransform;
import java.lang.classfile.instruction.InvokeInstruction;
import java.lang.constant.ClassDesc;
import java.lang.constant.MethodTypeDesc;
import java.nio.ByteBuffer;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;

/** Counts executed source-view conversions without adding a runtime test hook. */
public final class ZLinkServiceMultipartWriteBudgetTest {
    private static final AtomicInteger sourceViews = new AtomicInteger();
    private static final AtomicInteger directWrites = new AtomicInteger();

    @Test
    void serialRequestAndReplyWritePartsWithoutSourceBufferViews() throws Exception {
        sourceViews.set(0);
        directWrites.set(0);
        Class<?> codec = isolatedCodec();
        var encode = codec.getMethod("encodeFrameworkMultipartMessage", List.class);
        byte[] header = new byte[] {1, 2, 3};
        byte[] body = new byte[1024];
        for (int i = 0; i < body.length; i++) body[i] = (byte) i;
        byte[] expected = expectedFrame(header, body);
        int frames = 0;
        for (int roundtrip = 0; roundtrip < 1000; roundtrip++) {
            for (int direction = 0; direction < 2; direction++) {
                try (Message first = Message.from(header);
                        Message second = Message.from(body);
                        Message frame = (Message) encode.invoke(null, List.of(first, second))) {
                    assertArrayEquals(expected, frame.toByteArray());
                    assertArrayEquals(header, first.toByteArray());
                    assertArrayEquals(body, second.toByteArray());
                    frames++;
                }
            }
        }
        assertEquals(2000, frames, "both request and reply were written");
        assertEquals(0, sourceViews.get(), "source ByteBuffer views per 1000 roundtrips");
        assertEquals(4000, directWrites.get(), "one direct write per part");
    }

    private static byte[] expectedFrame(byte[] header, byte[] body) {
        // Build multipart bytes independently of the part writer.
        // The existing golden test pins the complete application frame.
        return new ZLinkServiceM6AWireCodec()
                .encodeApplicationPayload(
                        new ZLinkServiceM6AWireCodec.ApplicationPayload(
                                ServiceWireConstants.FRAMEWORK_MULTIPART_PACKET_NAME,
                                ServiceWireConstants.FRAMEWORK_MULTIPART_CONTENT_TYPE,
                                ByteBuffer.allocate(3 * Integer.BYTES + header.length + body.length)
                                        .putInt(2)
                                        .putInt(header.length)
                                        .put(header)
                                        .putInt(body.length)
                                        .put(body)
                                        .array()));
    }

    public static ByteBuffer sourceView(Message source) {
        sourceViews.incrementAndGet();
        return source.dataBuffer();
    }

    public static int directWrite(Message source, ByteBuffer target) {
        directWrites.incrementAndGet();
        return source.copyTo(target);
    }

    private Class<?> isolatedCodec() throws ClassNotFoundException {
        String codecName = ZLinkServiceM6AWireCodec.class.getName();
        return new ClassLoader(getClass().getClassLoader()) {
            @Override
            protected Class<?> loadClass(String name, boolean resolve)
                    throws ClassNotFoundException {
                if (!name.startsWith("systems.zlink.framework.runtime.internal.service.")
                        || name.equals(ZLinkServiceMultipartWriteBudgetTest.class.getName()))
                    return super.loadClass(name, resolve);
                synchronized (getClassLoadingLock(name)) {
                    Class<?> type = findLoadedClass(name);
                    if (type == null) {
                        try (var input = getResourceAsStream(name.replace('.', '/') + ".class")) {
                            if (input == null) throw new ClassNotFoundException(name);
                            byte[] bytes = input.readAllBytes();
                            if (name.equals(codecName)) {
                                var file = ClassFile.of();
                                ClassDesc helper =
                                        ClassDesc.of(
                                                ZLinkServiceMultipartWriteBudgetTest.class
                                                        .getName());
                                bytes =
                                        file.transformClass(
                                                file.parse(bytes),
                                                ClassTransform.transformingMethodBodies(
                                                        method ->
                                                                method.methodName()
                                                                        .equalsString(
                                                                                "writeMultipart"),
                                                        (builder, element) -> {
                                                            if (element
                                                                            instanceof
                                                                            InvokeInstruction call
                                                                    && call.owner()
                                                                            .asSymbol()
                                                                            .equals(
                                                                                    ClassDesc.of(
                                                                                            Message
                                                                                                    .class
                                                                                                    .getName()))
                                                                    && call.name()
                                                                            .equalsString(
                                                                                    "dataBuffer")) {
                                                                builder.invokestatic(
                                                                        helper,
                                                                        "sourceView",
                                                                        MethodTypeDesc.ofDescriptor(
                                                                                "(Lsystems/zlink/contracts/messaging/Message;)Ljava/nio/ByteBuffer;"));
                                                            } else if (element
                                                                            instanceof
                                                                            InvokeInstruction call
                                                                    && call.owner()
                                                                            .asSymbol()
                                                                            .equals(
                                                                                    ClassDesc.of(
                                                                                            Message
                                                                                                    .class
                                                                                                    .getName()))
                                                                    && call.name()
                                                                            .equalsString(
                                                                                    "copyTo")) {
                                                                builder.invokestatic(
                                                                        helper,
                                                                        "directWrite",
                                                                        MethodTypeDesc.ofDescriptor(
                                                                                "(Lsystems/zlink/contracts/messaging/Message;Ljava/nio/ByteBuffer;)I"));
                                                            } else builder.with(element);
                                                        }));
                            }
                            type = defineClass(name, bytes, 0, bytes.length);
                        } catch (IOException failure) {
                            throw new ClassNotFoundException(name, failure);
                        }
                    }
                    if (resolve) resolveClass(type);
                    return type;
                }
            }
        }.loadClass(codecName);
    }
}
