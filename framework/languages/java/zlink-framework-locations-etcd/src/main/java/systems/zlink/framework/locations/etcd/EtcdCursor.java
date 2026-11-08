package systems.zlink.framework.locations.etcd;

import com.google.protobuf.ByteString;

import java.io.*;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Base64;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

final class EtcdCursor {
    record Snapshot(long cluster, long revision, long now, long started, ByteString last) {
        boolean expired() {
            return System.nanoTime() - started >= java.util.concurrent.TimeUnit.MINUTES.toNanos(5);
        }

        Snapshot advance(ByteString value) {
            return new Snapshot(cluster, revision, now, started, value);
        }
    }

    private final byte[] secret = new byte[32];

    EtcdCursor() {
        new SecureRandom().nextBytes(secret);
    }

    private byte[] sign(byte[] value) {
        try {
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(secret, "HmacSHA256"));
            return mac.doFinal(value);
        } catch (java.security.GeneralSecurityException error) {
            throw new IllegalStateException(error);
        }
    }

    String encode(Snapshot snapshot, String namespace, ByteString prefix) {
        try {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            DataOutputStream output = new DataOutputStream(bytes);
            output.writeUTF(namespace);
            write(output, prefix);
            output.writeLong(snapshot.cluster());
            output.writeLong(snapshot.revision());
            output.writeLong(snapshot.now());
            output.writeLong(snapshot.started());
            write(output, snapshot.last());
            byte[] body = bytes.toByteArray();
            bytes.write(sign(body));
            String result =
                    Base64.getUrlEncoder().withoutPadding().encodeToString(bytes.toByteArray());
            if (result.length() > 4096) throw EtcdEncoding.failure();
            return result;
        } catch (IOException error) {
            throw new IllegalStateException(error);
        }
    }

    Snapshot decode(String value, String namespace, ByteString prefix) {
        try {
            byte[] bytes = Base64.getUrlDecoder().decode(value);
            if (bytes.length < 32) return null;
            int length = bytes.length - 32;
            if (!MessageDigest.isEqual(
                    sign(java.util.Arrays.copyOf(bytes, length)),
                    java.util.Arrays.copyOfRange(bytes, length, bytes.length))) return null;
            DataInputStream input = new DataInputStream(new ByteArrayInputStream(bytes, 0, length));
            if (!input.readUTF().equals(namespace) || !read(input).equals(prefix)) return null;
            Snapshot result =
                    new Snapshot(
                            input.readLong(),
                            input.readLong(),
                            input.readLong(),
                            input.readLong(),
                            read(input));
            if (input.available() != 0
                    || result.revision() <= 0
                    || result.now() < 0
                    || result.now() > EtcdEncoding.MAX_TIME
                    || result.expired()) return null;
            return result;
        } catch (IOException | IllegalArgumentException error) {
            return null;
        }
    }

    private static void write(DataOutputStream output, ByteString bytes) throws IOException {
        output.writeInt(bytes.size());
        bytes.writeTo(output);
    }

    private static ByteString read(DataInputStream input) throws IOException {
        int length = input.readInt();
        if (length < 0 || length > 1024) throw new IOException();
        byte[] bytes = input.readNBytes(length);
        if (bytes.length != length) throw new EOFException();
        return ByteString.copyFrom(bytes);
    }
}
