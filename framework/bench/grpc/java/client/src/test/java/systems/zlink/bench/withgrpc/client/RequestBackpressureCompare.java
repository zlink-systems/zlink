package systems.zlink.bench.withgrpc.client;

import java.lang.reflect.Method;
import java.net.*;
import java.nio.file.*;
import java.util.*;

/** Before/after use distinct Framework definitions and the same public binding classes. */
public final class RequestBackpressureCompare {
    static final class FrameworkLoader extends URLClassLoader {
        FrameworkLoader(URL[] urls) {
            super(urls, RequestBackpressureCompare.class.getClassLoader());
        }

        protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            if (name.startsWith("systems.zlink.framework.")
                    || name.equals(
                            "systems.zlink.bench.withgrpc.client.RequestBackpressureFramework")
                    || name.startsWith(
                            "systems.zlink.bench.withgrpc.client.RequestBackpressureFramework$")) {
                synchronized (getClassLoadingLock(name)) {
                    Class<?> loaded = findLoadedClass(name);
                    if (loaded == null) loaded = findClass(name);
                    if (resolve) resolveClass(loaded);
                    return loaded;
                }
            }
            return super.loadClass(name, resolve);
        }
    }

    static final class Loaded implements AutoCloseable {
        final FrameworkLoader loader;
        final RequestBackpressureProbe.Arm arm;

        Loaded(String cpFile, String label) throws Exception {
            var urls = new ArrayList<URL>();
            urls.add(
                    RequestBackpressureCompare.class
                            .getProtectionDomain()
                            .getCodeSource()
                            .getLocation());
            for (String path :
                    Files.readString(Path.of(cpFile)).strip().split(java.io.File.pathSeparator))
                urls.add(Path.of(path).toUri().toURL());
            loader = new FrameworkLoader(urls.toArray(URL[]::new));
            Thread t = Thread.currentThread();
            ClassLoader previous = t.getContextClassLoader();
            t.setContextClassLoader(loader);
            try {
                arm =
                        (RequestBackpressureProbe.Arm)
                                loader.loadClass(
                                                "systems.zlink.bench.withgrpc.client.RequestBackpressureFramework")
                                        .getConstructor(String.class)
                                        .newInstance(label);
            } catch (Throwable failure) {
                loader.close();
                throw failure;
            } finally {
                t.setContextClassLoader(previous);
            }
        }

        void window(int number, long nanos, boolean active) throws Exception {
            Thread t = Thread.currentThread();
            ClassLoader previous = t.getContextClassLoader();
            t.setContextClassLoader(loader);
            try {
                RequestBackpressureProbe.window(arm, number, nanos, active);
            } finally {
                t.setContextClassLoader(previous);
            }
        }

        public void close() throws Exception {
            Thread t = Thread.currentThread();
            ClassLoader previous = t.getContextClassLoader();
            t.setContextClassLoader(loader);
            try {
                arm.close();
            } finally {
                t.setContextClassLoader(previous);
                loader.close();
            }
        }
    }

    public static void main(String[] args) throws Exception {
        int rounds = Integer.parseInt(args[2]);
        long nanos = Long.parseLong(args[3]) * 1_000_000L;
        boolean withBinding = Arrays.asList(args).contains("--binding");
        RequestBackpressureProbe.configureCost(
                args.length > 4 && !args[4].startsWith("--") ? args[4] : null);
        RequestBackpressureProbe.THREADS.setThreadCpuTimeEnabled(true);
        RequestBackpressureProbe.THREADS.setThreadAllocatedMemoryEnabled(true);
        System.out.println(
                "META compare=unmodified_artifact_classloaders binding_loader=shared payload="
                        + RequestBackpressureProbe.PAYLOAD_BYTES
                        + ""
                        + " inflight_cap=none allocation_coverage=surviving_platform_threads_only"
                        + " virtual_thread_allocation=excluded available_processors="
                        + Runtime.getRuntime().availableProcessors());
        try (var binding = withBinding ? new RequestBackpressureProbe.BindingArm() : null;
                var before = new Loaded(args[0], "before");
                var after = new Loaded(args[1], "after")) {
            if (binding != null) RequestBackpressureProbe.window(binding, -1, nanos, false);
            before.window(-1, nanos, false);
            after.window(-1, nanos, false);
            for (int i = 0; i < rounds; i++) {
                Loaded[] order =
                        i % 2 == 0 ? new Loaded[] {before, after} : new Loaded[] {after, before};
                for (int position = 0; position < order.length; position++) {
                    order[position].window(i, nanos, true);
                    if (position == 0 && binding != null)
                        RequestBackpressureProbe.window(binding, i, nanos, true);
                }
            }
        }
    }

    public static final class Adapter {
        static final class Arm implements AutoCloseable {
            final RequestBackpressureCompare.FrameworkLoader loader;
            final Method benchmark;
            final String label;

            Arm(String cpFile, String label) throws Exception {
                this.label = label;
                var urls = new ArrayList<java.net.URL>();
                for (String path :
                        Files.readString(Path.of(cpFile)).strip().split(java.io.File.pathSeparator))
                    urls.add(Path.of(path).toUri().toURL());
                loader =
                        new RequestBackpressureCompare.FrameworkLoader(
                                urls.toArray(java.net.URL[]::new));
                try {
                    benchmark =
                            loader.loadClass(
                                            "systems.zlink.framework.runtime.binding.ZLinkJavaRequestCompletionCostTest")
                                    .getMethod("benchmark", int.class, int.class, boolean.class);
                } catch (Throwable failure) {
                    loader.close();
                    throw failure;
                }
            }

            void run(int round, int batches, int width, boolean backpressured, boolean measured)
                    throws Exception {
                Thread t = Thread.currentThread();
                ClassLoader previous = t.getContextClassLoader();
                t.setContextClassLoader(loader);
                try {
                    long cpu = RequestBackpressureProbe.PROCESS.getProcessCpuTime(),
                            allocated =
                                    RequestBackpressureProbe.THREADS.getThreadAllocatedBytes(
                                            t.threadId());
                    long[] result = (long[]) benchmark.invoke(null, batches, width, backpressured);
                    cpu = RequestBackpressureProbe.PROCESS.getProcessCpuTime() - cpu;
                    allocated =
                            RequestBackpressureProbe.THREADS.getThreadAllocatedBytes(t.threadId())
                                    - allocated;
                    if (result.length != 4
                            || result[1] != Math.multiplyExact(batches, width)
                            || result[2] != result[1]
                            || result[3] != result[1])
                        throw new AssertionError("fixture operation conservation");
                    if (measured)
                        System.out.printf(
                                Locale.ROOT,
                                "ADAPTER arm=%s round=%d backpressured=%s ns=%d operations=%d"
                                        + " submits=%d terminals=%d process_cpu_ns=%d"
                                        + " caller_alloc_bytes=%d%n",
                                label,
                                round,
                                backpressured,
                                result[0],
                                result[1],
                                result[2],
                                result[3],
                                cpu,
                                allocated);
                } finally {
                    t.setContextClassLoader(previous);
                }
            }

            public void close() throws Exception {
                loader.close();
            }
        }

        public static void main(String[] args) throws Exception {
            int batches = args.length > 2 ? Integer.parseInt(args[2]) : 512,
                    width = args.length > 3 ? Integer.parseInt(args[3]) : 128,
                    rounds = args.length > 4 ? Integer.parseInt(args[4]) : 3;
            RequestBackpressureProbe.THREADS.setThreadAllocatedMemoryEnabled(true);
            System.out.println(
                    "META mode=targeted_public_adapter_fixture binding_native_transport=false"
                            + " source_transforms=false available_processors="
                            + Runtime.getRuntime().availableProcessors()
                            + " caller_alloc_excludes_other_threads=true");
            try (var before = new Arm(args[0], "before");
                    var after = new Arm(args[1], "after")) {
                for (boolean pressured : new boolean[] {false, true}) {
                    before.run(-1, batches, width, pressured, false);
                    after.run(-1, batches, width, pressured, false);
                    for (int round = 0; round < rounds; round++) {
                        Arm[] order =
                                round % 2 == 0
                                        ? new Arm[] {before, after}
                                        : new Arm[] {after, before};
                        for (Arm arm : order) arm.run(round, batches, width, pressured, true);
                    }
                }
            }
        }
    }
}
