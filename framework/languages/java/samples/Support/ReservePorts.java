import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Prints {@code count} free TCP ports in {@code [minimum, maximum]}, one bind-check at a time.
 *
 * <p>Single-file source ({@code java ReservePorts.java <count> <minimum> <maximum>}) so the sample
 * runners need no interpreter beyond the JDK they already require. Replaces the Python heredoc this
 * file's logic was ported from 1:1; keep the two in step if either changes.
 *
 * <p>This is a best-effort reservation, exactly like the original: each candidate port is bound and
 * held only long enough to prove it is free, then released when the JVM exits so the caller can
 * bind it for real. A port can still be taken between that release and the caller's own bind; the
 * shuffled scan order only makes that race unlikely, it does not remove it.
 */
public final class ReservePorts {
    public static void main(String[] args) {
        if (args.length != 3) {
            System.err.println("usage: ReservePorts <count> <minimum> <maximum>");
            System.exit(1);
            return;
        }
        int count = Integer.parseInt(args[0]);
        int minimum = Integer.parseInt(args[1]);
        int maximum = Integer.parseInt(args[2]);
        if (count < 1 || minimum < 1 || maximum > 65535 || minimum > maximum) {
            System.err.println("invalid JVM sample port allocation request");
            System.exit(1);
            return;
        }
        if (count > maximum - minimum + 1) {
            System.err.println("JVM sample port pool is smaller than the requested allocation");
            System.exit(1);
            return;
        }

        List<Integer> candidates = new ArrayList<>(maximum - minimum + 1);
        for (int port = minimum; port <= maximum; port++) {
            candidates.add(port);
        }
        Collections.shuffle(candidates, new SecureRandom());

        List<ServerSocket> bound = new ArrayList<>(count);
        try {
            for (int port : candidates) {
                if (bound.size() == count) {
                    break;
                }
                try {
                    ServerSocket socket = new ServerSocket();
                    socket.bind(new InetSocketAddress("127.0.0.1", port));
                    bound.add(socket);
                } catch (java.io.IOException notFree) {
                    // Taken already; try the next shuffled candidate.
                }
            }
            if (bound.size() != count) {
                System.err.println(
                        "unable to bind-check " + count + " ports in " + minimum + "-" + maximum);
                System.exit(1);
                return;
            }
            StringBuilder line = new StringBuilder();
            for (ServerSocket socket : bound) {
                if (line.length() > 0) {
                    line.append(' ');
                }
                line.append(socket.getLocalPort());
            }
            System.out.println(line);
        } finally {
            for (ServerSocket socket : bound) {
                try {
                    socket.close();
                } catch (java.io.IOException ignored) {
                    // Nothing more to release.
                }
            }
        }
    }

    private ReservePorts() {}
}
