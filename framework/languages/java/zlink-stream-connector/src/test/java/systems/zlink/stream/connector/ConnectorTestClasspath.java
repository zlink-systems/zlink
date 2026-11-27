package systems.zlink.stream.connector;

import java.io.File;
import java.net.URISyntaxException;
import java.net.URLClassLoader;
import java.nio.file.Path;
import java.util.LinkedHashSet;
import java.util.List;

final class ConnectorTestClasspath {
    private ConnectorTestClasspath() {}

    static String resolve() throws URISyntaxException {
        var paths = new LinkedHashSet<String>();
        paths.addAll(List.of(System.getProperty("java.class.path").split(File.pathSeparator)));
        for (ClassLoader loader = ConnectorTestClasspath.class.getClassLoader();
                loader != null;
                loader = loader.getParent()) {
            if (loader instanceof URLClassLoader urls) {
                for (var url : urls.getURLs()) {
                    paths.add(Path.of(url.toURI()).toString());
                }
            }
        }
        return String.join(File.pathSeparator, paths);
    }
}
