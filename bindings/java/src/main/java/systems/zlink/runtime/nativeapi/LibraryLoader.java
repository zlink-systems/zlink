package systems.zlink.runtime.nativeapi;

import java.lang.foreign.SymbolLookup;
import java.lang.foreign.Arena;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

final class LibraryLoader {
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
        try (InputStream in = LibraryLoader.class.getResourceAsStream(resourcePath)) {
            if (in == null)
                throw new UnsatisfiedLinkError("zlink native resource not found: " + resourcePath);
            Path tmpDir = Files.createTempDirectory("zlink-native-");
            Path tmp = tmpDir.resolve(libFile);
            Files.copy(in, tmp, java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            tmp.toFile().deleteOnExit();
            tmpDir.toFile().deleteOnExit();
            if ("windows".equals(os))
                preloadWindowsDeps(tmpDir);
            System.load(tmp.toAbsolutePath().toString());
            rememberLoaded(tmp);
        } catch (IOException e) {
            throw new UnsatisfiedLinkError("failed to load zlink native resource: " + e.getMessage());
        }
    }

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
        String[] depNames = new String[] {
                "libcrypto-3-x64.dll",
                "libssl-3-x64.dll"
        };
        for (String dep : depNames) {
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
