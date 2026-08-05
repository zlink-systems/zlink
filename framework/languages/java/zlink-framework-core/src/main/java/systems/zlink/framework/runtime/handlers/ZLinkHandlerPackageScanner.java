package systems.zlink.framework.runtime.handlers;

import java.io.File;
import java.io.IOException;
import java.net.JarURLConnection;
import java.net.URISyntaxException;
import java.net.URL;
import java.util.Enumeration;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.jar.JarFile;
import systems.zlink.framework.errors.ZLinkConfigurationException;

final class ZLinkHandlerPackageScanner {
    private ZLinkHandlerPackageScanner() {
    }

    static Set<Class<?>> scan(Class<?> markerType) {
        String packageName = markerType.getPackageName();
        String packagePath = packageName.replace('.', '/');
        ClassLoader loader = markerType.getClassLoader();
        Set<Class<?>> classes = new LinkedHashSet<>();
        try {
            Enumeration<URL> resources = loader.getResources(packagePath);
            while (resources.hasMoreElements()) {
                URL resource = resources.nextElement();
                if ("file".equals(resource.getProtocol())) {
                    scanDirectory(loader, packageName, new File(resource.toURI()), classes);
                } else if ("jar".equals(resource.getProtocol())) {
                    scanJar(loader, packagePath, resource, classes);
                }
            }
        } catch (IOException | URISyntaxException ex) {
            throw new ZLinkConfigurationException(
                "failed to scan handler package: " + packageName);
        }
        return classes;
    }

    private static void scanDirectory(
        ClassLoader loader,
        String packageName,
        File directory,
        Set<Class<?>> classes) {
        File[] files = directory.listFiles();
        if (files == null) {
            return;
        }
        for (File file : files) {
            if (file.isDirectory()) {
                scanDirectory(loader, packageName + "." + file.getName(), file, classes);
            } else if (file.getName().endsWith(".class")) {
                String simpleName = file.getName().substring(0, file.getName().length() - 6);
                loadClass(loader, packageName + "." + simpleName, classes);
            }
        }
    }

    private static void scanJar(
        ClassLoader loader,
        String packagePath,
        URL resource,
        Set<Class<?>> classes) throws IOException {
        JarURLConnection connection = (JarURLConnection) resource.openConnection();
        try (JarFile jar = connection.getJarFile()) {
            jar.stream()
                .filter(entry -> !entry.isDirectory())
                .map(entry -> entry.getName())
                .filter(name -> name.startsWith(packagePath) && name.endsWith(".class"))
                .map(name -> name.substring(0, name.length() - 6).replace('/', '.'))
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
