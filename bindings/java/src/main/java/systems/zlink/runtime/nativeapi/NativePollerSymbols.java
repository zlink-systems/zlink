/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;

import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkConfigException;

final class NativePollerSymbols {
    private static final MethodHandle MH_POLL = downcall("zlink_poll",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.JAVA_LONG,
                    ValueLayout.ADDRESS));
    private static final MethodHandle MH_POLLER_NEW = downcall("zlink_poller_new",
            FunctionDescriptor.of(ValueLayout.ADDRESS));
    private static final MethodHandle MH_POLLER_DESTROY = downcall(
            "zlink_poller_destroy",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    private static final MethodHandle MH_POLLER_SIZE = downcall("zlink_poller_size",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS));
    private static final MethodHandle MH_POLLER_ADD = downcall("zlink_poller_add",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_SHORT));
    private static final MethodHandle MH_POLLER_ADD_FD = downcall(
            "zlink_poller_add_fd",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_SHORT));
    private static final MethodHandle MH_POLLER_ADD_TIMER = downcall(
            "zlink_poller_add_timer",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_POLLER_MODIFY = downcall(
            "zlink_poller_modify",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.JAVA_SHORT));
    private static final MethodHandle MH_POLLER_MODIFY_FD = downcall(
            "zlink_poller_modify_fd",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT, ValueLayout.JAVA_SHORT));
    private static final MethodHandle MH_POLLER_REMOVE = downcall(
            "zlink_poller_remove",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS));
    private static final MethodHandle MH_POLLER_REMOVE_FD = downcall(
            "zlink_poller_remove_fd",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT));
    private static final MethodHandle MH_POLLER_REMOVE_TIMER = downcall(
            "zlink_poller_remove_timer",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS));
    private static final MethodHandle MH_POLLER_WAIT = downcall("zlink_poller_wait",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS, ValueLayout.JAVA_INT,
                    ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));

    private NativePollerSymbols() {
    }

    static int pollRaw(MemorySegment items, int count, int timeoutMs) {
        if (items == null || items.address() == 0 || count <= 0)
            return 0;
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment errorOut = arena.allocate(ValueLayout.JAVA_INT);
            errorOut.set(ValueLayout.JAVA_INT, 0, 0);
            int rc = (int) MH_POLL.invokeExact(items, count, (long) timeoutMs,
                errorOut);
            if (rc < 0) {
                int error = errorOut.get(ValueLayout.JAVA_INT, 0);
                if (error != 0) {
                    throw new ZlinkConfigException(
                        ConfigResult.fromValue(error), Native.errno());
                }
            }
            return rc;
        } catch (RuntimeException ex) {
            throw ex;
        } catch (Throwable t) {
            throw new RuntimeException("poll failed", t);
        }
    }

    static MemorySegment pollerNew() {
        try {
            return (MemorySegment) MH_POLLER_NEW.invokeExact();
        } catch (Throwable t) {
            throw new RuntimeException("zlink_poller_new failed", t);
        }
    }

    static int pollerDestroy(MemorySegment pollerPtr) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment p = arena.allocate(ValueLayout.ADDRESS);
            p.set(ValueLayout.ADDRESS, 0, pollerPtr);
            return (int) MH_POLLER_DESTROY.invokeExact(p);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_poller_destroy failed", t);
        }
    }

    static int pollerSize(MemorySegment poller) {
        try {
            return (int) MH_POLLER_SIZE.invokeExact(poller, MemorySegment.NULL);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_poller_size failed", t);
        }
    }

    static int pollerAdd(MemorySegment poller, MemorySegment socket,
                         MemorySegment userData, int events) {
        try {
            return (int) MH_POLLER_ADD.invokeExact(poller, socket, userData,
                (short) events);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_poller_add failed", t);
        }
    }

    static int pollerAddFd(MemorySegment poller, int fd,
                           MemorySegment userData, int events) {
        try {
            return (int) MH_POLLER_ADD_FD.invokeExact(poller, fd, userData,
                (short) events);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_poller_add_fd failed", t);
        }
    }

    static int pollerAddTimer(MemorySegment poller, MemorySegment timer,
                              MemorySegment userData) {
        try {
            return (int) MH_POLLER_ADD_TIMER.invokeExact(poller, timer,
                userData);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_poller_add_timer failed", t);
        }
    }

    static int pollerModify(MemorySegment poller, MemorySegment socket,
                            int events) {
        try {
            return (int) MH_POLLER_MODIFY.invokeExact(poller, socket,
                (short) events);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_poller_modify failed", t);
        }
    }

    static int pollerModifyFd(MemorySegment poller, int fd, int events) {
        try {
            return (int) MH_POLLER_MODIFY_FD.invokeExact(poller, fd,
                (short) events);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_poller_modify_fd failed", t);
        }
    }

    static int pollerRemove(MemorySegment poller, MemorySegment socket) {
        try {
            return (int) MH_POLLER_REMOVE.invokeExact(poller, socket);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_poller_remove failed", t);
        }
    }

    static int pollerRemoveFd(MemorySegment poller, int fd) {
        try {
            return (int) MH_POLLER_REMOVE_FD.invokeExact(poller, fd);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_poller_remove_fd failed", t);
        }
    }

    static int pollerRemoveTimer(MemorySegment poller, MemorySegment timer) {
        try {
            return (int) MH_POLLER_REMOVE_TIMER.invokeExact(poller, timer);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_poller_remove_timer failed", t);
        }
    }

    static int pollerWait(MemorySegment poller, MemorySegment events,
                          int count, int timeoutMs) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment errorOut = arena.allocate(ValueLayout.JAVA_INT);
            return pollerWait(poller, events, count, timeoutMs, errorOut);
        }
    }

    static int pollerWait(MemorySegment poller, MemorySegment events,
                          int count, int timeoutMs, MemorySegment errorOut) {
        if (events == null || events.address() == 0)
            throw new NullPointerException("events");
        if (errorOut == null || errorOut.address() == 0)
            throw new NullPointerException("errorOut");
        try {
            errorOut.set(ValueLayout.JAVA_INT, 0, 0);
            int rc = (int) MH_POLLER_WAIT.invokeExact(poller, events, count,
                (long) timeoutMs, errorOut);
            if (rc < 0) {
                int error = errorOut.get(ValueLayout.JAVA_INT, 0);
                if (error != 0) {
                    throw new ZlinkConfigException(
                        ConfigResult.fromValue(error), Native.errno());
                }
            }
            return rc;
        } catch (RuntimeException ex) {
            throw ex;
        } catch (Throwable t) {
            throw new RuntimeException("zlink_poller_wait failed", t);
        }
    }

    static int pollerWait(MemorySegment poller, MemorySegment event,
                          int timeoutMs) {
        return pollerWait(poller, event, 1, timeoutMs);
    }

    private static MethodHandle downcall(String name, FunctionDescriptor fd) {
        return NativeSymbols.downcall(name, fd);
    }
}
