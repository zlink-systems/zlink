/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.eventing;

import systems.zlink.contracts.sockets.Socket;
import java.time.Duration;

/**
 * Multiplexes sockets, file descriptors, and timers, reporting which become
 * ready. The caller owns this resource and must close it.
 */
public interface Poller extends AutoCloseable {

    void add(Socket socket, long slot, PollEventFlags... events);

    void addFd(int fd, long slot, PollEventFlags... events);

    void add(ZlinkTimer timer, long slot);

    void modify(Socket socket, PollEventFlags... events);

    void modifyFd(int fd, PollEventFlags... events);

    boolean remove(Socket socket);

    boolean remove(int fd);

    boolean remove(ZlinkTimer timer);

    void clear();

    int size();

    int wait(PollEvents events, Duration timeout);

    @Override
    void close();
}
