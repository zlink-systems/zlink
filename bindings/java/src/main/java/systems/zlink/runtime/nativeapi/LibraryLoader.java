package systems.zlink.runtime.nativeapi;

import java.lang.foreign.SymbolLookup;
import java.lang.foreign.Arena;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.Path;
import java.nio.file.ReadOnlyFileSystemException;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;
import java.util.Optional;

final class LibraryLoader {
    private static final String NATIVE_CACHE_ENV = "ZLINK_JAVA_NATIVE_CACHE";
    private static final String[] WINDOWS_DEP_NAMES = new String[] {
            "libcrypto-3-x64.dll",
            "libssl-3-x64.dll"
    };
    private static final Object LOCK = new Object();
    private static final List<Path> LOADED_LIBRARY_PATHS = new ArrayList<>();
    private static volatile SymbolLookup LOOKUP;

    private LibraryLoader() {}

    public static SymbolLookup lookup() {
        SymbolLookup lookup = LOOKUP;
        if (lookup != null) {
            return lookup;
        }
        synchronized (LOCK) {
            lookup = LOOKUP;
            if (lookup != null) {
                return lookup;
            }
            String path = System.getenv("ZLINK_LIBRARY_PATH");
            if (path != null && !path.isEmpty()) {
                Path p = Path.of(path);
                if (!p.isAbsolute())
                    p = p.toAbsolutePath();
                System.load(p.toString());
                rememberLoaded(p);
                LOOKUP = combinedLookup();
                return LOOKUP;
            }
            try {
                loadFromResources();
                LOOKUP = combinedLookup();
                return LOOKUP;
            } catch (UnsatisfiedLinkError e) {
                System.loadLibrary("zlink");
                LOOKUP = combinedLookup();
                return LOOKUP;
            }
        }
    }

    private static void loadFromResources() {
        String os = normalizeOs(System.getProperty("os.name"));
        String arch = normalizeArch(System.getProperty("os.arch"));
        String libFile = libraryFileName(os);
        String resourcePath = resourcePath(os, arch, libFile);
        try {
            Path extracted = extractNativeBundle(os, arch, resourcePath, libFile);
            if ("windows".equals(os))
                preloadWindowsDeps(extracted.getParent());
            System.load(extracted.toAbsolutePath().toString());
            rememberLoaded(extracted);
        } catch (IOException e) {
            throw new UnsatisfiedLinkError("failed to load zlink native resource: " + e.getMessage());
        }
    }

    static Path extractResource(String resourcePath, String fileName) throws IOException {
        try {
            return extractCachedResource(resourcePath, fileName, nativeCacheRoot());
        } catch (IOException | SecurityException | InvalidPathException
                 | ReadOnlyFileSystemException ignored) {
            return extractTemporaryResource(resourcePath, fileName);
        }
    }

    static Path extractResource(String resourcePath, String fileName, Path cacheRoot)
            throws IOException {
        try {
            return extractCachedResource(resourcePath, fileName, cacheRoot);
        } catch (IOException | SecurityException | ReadOnlyFileSystemException ignored) {
            return extractTemporaryResource(resourcePath, fileName);
        }
    }

    private static Path extractNativeBundle(String os, String arch,
            String resourcePath, String libFile) throws IOException {
        try {
            ResourceDigest digest = digestResource(resourcePath);
            Path cacheDir = nativeCacheRoot().resolve(digest.sha256());
            Path extracted = placeCachedResource(resourcePath, libFile, cacheDir,
                    digest.size());
            if ("windows".equals(os))
                extractWindowsDeps(arch, cacheDir);
            return extracted;
        } catch (IOException | SecurityException | InvalidPathException
                 | ReadOnlyFileSystemException ignored) {
            Path tempDir = Files.createTempDirectory("zlink-native-");
            tempDir.toFile().deleteOnExit();
            Path extracted = copyTemporaryResource(resourcePath, libFile, tempDir);
            if ("windows".equals(os))
                extractWindowsDeps(arch, tempDir);
            return extracted;
        }
    }

    private static Path extractCachedResource(String resourcePath, String fileName,
            Path cacheRoot) throws IOException {
        ResourceDigest digest = digestResource(resourcePath);
        return placeCachedResource(resourcePath, fileName,
                cacheRoot.resolve(digest.sha256()), digest.size());
    }

    private static Path placeCachedResource(String resourcePath, String fileName,
            Path cacheDir, long expectedSize) throws IOException {
        Files.createDirectories(cacheDir);
        Path target = cacheDir.resolve(fileName);
        if (matchesSize(target, expectedSize))
            return target;

        Files.deleteIfExists(target);
        Path temporary = cacheDir.resolve(fileName + "."
                + ProcessHandle.current().pid() + ".tmp");
        try {
            try (InputStream in = openRequiredResource(resourcePath)) {
                Files.copy(in, temporary, StandardCopyOption.REPLACE_EXISTING);
            }
            if (!matchesSize(temporary, expectedSize))
                throw new IOException("native resource size changed while extracting: "
                        + resourcePath);
            try {
                Files.move(temporary, target, StandardCopyOption.ATOMIC_MOVE);
            } catch (FileAlreadyExistsException ignored) {
                if (!matchesSize(target, expectedSize))
                    throw ignored;
            }
        } finally {
            Files.deleteIfExists(temporary);
        }
        if (!matchesSize(target, expectedSize))
            throw new IOException("native cache entry has the wrong size: " + target);
        return target;
    }

    private static Path extractTemporaryResource(String resourcePath, String fileName)
            throws IOException {
        Path tempDir = Files.createTempDirectory("zlink-native-");
        tempDir.toFile().deleteOnExit();
        return copyTemporaryResource(resourcePath, fileName, tempDir);
    }

    private static Path copyTemporaryResource(String resourcePath, String fileName,
            Path tempDir) throws IOException {
        Path target = tempDir.resolve(fileName);
        try (InputStream in = openRequiredResource(resourcePath)) {
            Files.copy(in, target, StandardCopyOption.REPLACE_EXISTING);
        }
        target.toFile().deleteOnExit();
        return target;
    }

    private static ResourceDigest digestResource(String resourcePath) throws IOException {
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new AssertionError("SHA-256 is required by the Java runtime", e);
        }
        long size = 0;
        byte[] buffer = new byte[64 * 1024];
        try (InputStream in = openRequiredResource(resourcePath)) {
            int read;
            while ((read = in.read(buffer)) != -1) {
                digest.update(buffer, 0, read);
                size += read;
            }
        }
        return new ResourceDigest(HexFormat.of().formatHex(digest.digest()), size);
    }

    private static InputStream openRequiredResource(String resourcePath)
            throws IOException {
        InputStream in = LibraryLoader.class.getResourceAsStream(resourcePath);
        if (in == null)
            throw new FileNotFoundException(
                    "zlink native resource not found: " + resourcePath);
        return in;
    }

    private static Path nativeCacheRoot() {
        String configured = System.getenv(NATIVE_CACHE_ENV);
        if (configured != null && !configured.isEmpty())
            return Path.of(configured);
        return Path.of(System.getProperty("user.home", "."), ".cache", "zlink", "native");
    }

    private static boolean matchesSize(Path path, long expectedSize) throws IOException {
        return Files.isRegularFile(path) && Files.size(path) == expectedSize;
    }

    private static void extractWindowsDeps(String arch, Path directory)
            throws IOException {
        for (String dep : WINDOWS_DEP_NAMES) {
            String resourcePath = resourcePath("windows", arch, dep);
            try (InputStream in = LibraryLoader.class.getResourceAsStream(resourcePath)) {
                if (in == null)
                    continue;
                Path target = directory.resolve(dep);
                if (Files.exists(target))
                    continue;
                Path temporary = directory.resolve(dep + "."
                        + ProcessHandle.current().pid() + ".tmp");
                try {
                    Files.copy(in, temporary, StandardCopyOption.REPLACE_EXISTING);
                    try {
                        Files.move(temporary, target, StandardCopyOption.ATOMIC_MOVE);
                    } catch (FileAlreadyExistsException ignored) {
                        // Another JVM completed the identical cache entry first.
                    }
                } finally {
                    Files.deleteIfExists(temporary);
                }
                if (directory.getFileName().toString().startsWith("zlink-native-"))
                    target.toFile().deleteOnExit();
            }
        }
    }

    private record ResourceDigest(String sha256, long size) {}

    private static void rememberLoaded(Path path) {
        LOADED_LIBRARY_PATHS.add(path.toAbsolutePath().normalize());
    }

    private static SymbolLookup combinedLookup() {
        List<SymbolLookup> lookups = new ArrayList<>();
        for (Path path : LOADED_LIBRARY_PATHS) {
            try {
                lookups.add(SymbolLookup.libraryLookup(path, Arena.global()));
            } catch (IllegalArgumentException | IllegalStateException
                     | UnsatisfiedLinkError ignored) {
            }
        }
        lookups.add(SymbolLookup.loaderLookup());
        return name -> {
            for (SymbolLookup lookup : lookups) {
                Optional<java.lang.foreign.MemorySegment> symbol = lookup.find(name);
                if (symbol.isPresent()) {
                    return symbol;
                }
            }
            return Optional.empty();
        };
    }

    private static void preloadWindowsDeps(Path localDir) {
        for (String dep : WINDOWS_DEP_NAMES) {
            Path p = findWindowsDependency(localDir, dep);
            if (p != null) {
                try {
                    System.load(p.toString());
                } catch (UnsatisfiedLinkError ignored) {
                }
            }
        }
    }

    private static Path findWindowsDependency(Path localDir, String fileName) {
        List<Path> dirs = new ArrayList<>();
        if (localDir != null)
            dirs.add(localDir);
        String opensslBin = System.getenv("ZLINK_OPENSSL_BIN");
        if (opensslBin != null && !opensslBin.isEmpty())
            dirs.add(Path.of(opensslBin));
        String runtimeBin = System.getenv("ZLINK_WINDOWS_RUNTIME_BIN");
        if (runtimeBin != null && !runtimeBin.isEmpty())
            dirs.add(Path.of(runtimeBin));
        Path userDir = Path.of(System.getProperty("user.dir", ".")).toAbsolutePath();
        dirs.add(userDir.resolve("../dotnet/native/win-x64").normalize());
        dirs.add(userDir.resolve("../node/prebuilds/win32-x64").normalize());
        dirs.add(Path.of("C:\\Program Files\\OpenSSL-Win64\\bin"));
        dirs.add(Path.of("C:\\Program Files\\Git\\mingw64\\bin"));
        dirs.add(Path.of(
                "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\CommonExtensions\\Microsoft\\TeamFoundation\\Team Explorer\\Git\\mingw64\\bin"));
        String envPath = System.getenv("PATH");
        if (envPath != null && !envPath.isEmpty()) {
            for (String entry : envPath.split(";")) {
                if (!entry.isEmpty())
                    dirs.add(Path.of(entry));
            }
        }

        for (Path dir : dirs) {
            try {
                Path candidate = dir.resolve(fileName);
                if (Files.exists(candidate))
                    return candidate.toAbsolutePath();
            } catch (Exception ignored) {
            }
        }
        return null;
    }

    private static String normalizeOs(String name) {
        String os = name.toLowerCase();
        if (os.contains("win"))
            return "windows";
        if (os.contains("mac") || os.contains("darwin"))
            return "darwin";
        if (os.contains("linux"))
            return "linux";
        return os.replaceAll("\\s+", "");
    }

    private static String normalizeArch(String arch) {
        String a = arch.toLowerCase();
        if (a.equals("amd64") || a.equals("x86_64"))
            return "x86_64";
        if (a.equals("aarch64") || a.equals("arm64"))
            return "aarch64";
        return a.replaceAll("\\s+", "");
    }

    private static String libraryFileName(String os) {
        if ("windows".equals(os))
            return "zlink.dll";
        if ("darwin".equals(os))
            return "libzlink.dylib";
        return "libzlink.so";
    }

    private static String resourcePath(String os, String arch, String fileName) {
        return "/native/" + os + "-" + arch + "/" + fileName;
    }

}
