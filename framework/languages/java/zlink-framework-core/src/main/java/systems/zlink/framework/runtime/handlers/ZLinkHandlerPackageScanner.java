package systems.zlink.framework.runtime.handlers;

import systems.zlink.framework.errors.ZLinkConfigurationException;

import java.io.File;
import java.io.IOException;
import java.net.JarURLConnection;
import java.net.URISyntaxException;
import java.net.URL;
import java.util.Enumeration;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.jar.JarFile;

final class ZLinkHandlerPackageScanner {
    private static final String FILE_PROTOCOL = "file";
    private static final String JAR_PROTOCOL = "jar";
    private static final String CLASS_SUFFIX = ".class";

    private ZLinkHandlerPackageScanner() {}

    static Set<Class<?>> scan(Class<?> markerType) {
        String packageName = markerType.getPackageName();
        String packagePath = packageName.replace('.', '/');
        ClassLoader loader = markerType.getClassLoader();
        Set<Class<?>> classes = new LinkedHashSet<>();
        try {
            Enumeration<URL> resources = loader.getResources(packagePath);
            while (resources.hasMoreElements()) {
                URL resource = resources.nextElement();
                if (FILE_PROTOCOL.equals(resource.getProtocol())) {
                    scanDirectory(loader, packageName, new File(resource.toURI()), classes);
                } else if (JAR_PROTOCOL.equals(resource.getProtocol())) {
                    scanJar(loader, packagePath, resource, classes);
                }
            }
        } catch (IOException | URISyntaxException ex) {
            throw new ZLinkConfigurationException("failed to scan handler package: " + packageName);
        }
        return classes;
    }

    private static void scanDirectory(
            ClassLoader loader, String packageName, File directory, Set<Class<?>> classes) {
        File[] files = directory.listFiles();
        if (files == null) {
            return;
        }
        for (File file : files) {
            if (file.isDirectory()) {
                scanDirectory(loader, packageName + "." + file.getName(), file, classes);
            } else if (file.getName().endsWith(CLASS_SUFFIX)) {
                String simpleName =
                        file.getName()
                                .substring(0, file.getName().length() - CLASS_SUFFIX.length());
                loadClass(loader, packageName + "." + simpleName, classes);
            }
        }
    }

    private static void scanJar(
            ClassLoader loader, String packagePath, URL resource, Set<Class<?>> classes)
            throws IOException {
        JarURLConnection connection = (JarURLConnection) resource.openConnection();
        try (JarFile jar = connection.getJarFile()) {
            jar.stream()
                    .filter(entry -> !entry.isDirectory())
                    .map(entry -> entry.getName())
                    .filter(name -> name.startsWith(packagePath) && name.endsWith(CLASS_SUFFIX))
                    .map(
                            name ->
                                    name.substring(0, name.length() - CLASS_SUFFIX.length())
                                            .replace('/', '.'))
                    .forEach(className -> loadClass(loader, className, classes));
        }
    }

    private static void loadClass(ClassLoader loader, String className, Set<Class<?>> classes) {
        try {
            classes.add(Class.forName(className, false, loader));
        } catch (ClassNotFoundException | NoClassDefFoundError ignored) {
            // Optional dependencies in the same package should not make registration unusable.
        }
    }
}
