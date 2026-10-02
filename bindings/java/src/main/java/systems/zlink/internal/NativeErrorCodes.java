/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.internal;

import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;
import java.util.Properties;

/** Native error numbers generated from the Core errno header for the running OS. */
public final class NativeErrorCodes {
    public static final int ENOENT;
    public static final int EINTR;
    public static final int EBADF;
    public static final int EAGAIN;
    public static final int EDEADLK;
    public static final int ENOMEM;
    public static final int EFAULT;
    public static final int EACCES;
    public static final int EBUSY;
    public static final int EEXIST;
    public static final int EINVAL;
    public static final int EPROTO;
    public static final int EADDRINUSE;
    public static final int ENOTSUP;
    public static final int ENOTCONN;
    public static final int ECONNREFUSED;
    public static final int EHOSTUNREACH;
    public static final int ESHUTDOWN;
    public static final int ETIMEDOUT;
    public static final int EIO;
    public static final int ENOBUFS;
    public static final int ESTALE;
    public static final int ECANCELED;
    public static final int EALREADY;
    public static final int EPROTOTYPE;
    public static final int EOPNOTSUPP;
    public static final int ETERM;
    public static final int EFSM;
    public static final int ENOCOMPATPROTO;
    public static final int EMTHREAD;

    static {
        Properties values = load();
        ENOENT = value(values, "ENOENT");
        EINTR = value(values, "EINTR");
        EBADF = value(values, "EBADF");
        EAGAIN = value(values, "EAGAIN");
        EDEADLK = value(values, "EDEADLK");
        ENOMEM = value(values, "ENOMEM");
        EFAULT = value(values, "EFAULT");
        EACCES = value(values, "EACCES");
        EBUSY = value(values, "EBUSY");
        EEXIST = value(values, "EEXIST");
        EINVAL = value(values, "EINVAL");
        EPROTO = value(values, "EPROTO");
        EADDRINUSE = value(values, "EADDRINUSE");
        ENOTSUP = value(values, "ENOTSUP");
        ENOTCONN = value(values, "ENOTCONN");
        ECONNREFUSED = value(values, "ECONNREFUSED");
        EHOSTUNREACH = value(values, "EHOSTUNREACH");
        ESHUTDOWN = value(values, "ESHUTDOWN");
        ETIMEDOUT = value(values, "ETIMEDOUT");
        EIO = value(values, "EIO");
        ENOBUFS = value(values, "ENOBUFS");
        ESTALE = value(values, "ESTALE");
        ECANCELED = value(values, "ECANCELED");
        EALREADY = value(values, "EALREADY");
        EPROTOTYPE = value(values, "EPROTOTYPE");
        EOPNOTSUPP = value(values, "EOPNOTSUPP");
        ETERM = value(values, "ETERM");
        EFSM = value(values, "EFSM");
        ENOCOMPATPROTO = value(values, "ENOCOMPATPROTO");
        EMTHREAD = value(values, "EMTHREAD");
    }

    private static Properties load() {
        String os = System.getProperty("os.name").toLowerCase(Locale.ROOT);
        String platform;
        if (os.contains("mac") || os.contains("darwin")) {
            platform = "darwin";
        } else if (os.contains("win")) {
            platform = "windows";
        } else if (os.contains("linux")) {
            platform = "linux";
        } else {
            throw new ExceptionInInitializerError("Unsupported errno platform: " + os);
        }
        String resource = "/systems/zlink/internal/errno/" + platform + ".properties";
        try (InputStream input = NativeErrorCodes.class.getResourceAsStream(resource)) {
            if (input == null) {
                throw new ExceptionInInitializerError("Missing Core errno resource: " + resource);
            }
            Properties values = new Properties();
            values.load(input);
            return values;
        } catch (IOException failure) {
            throw new ExceptionInInitializerError(failure);
        }
    }

    private static int value(Properties values, String name) {
        String value = values.getProperty(name);
        if (value == null) {
            throw new ExceptionInInitializerError("Missing Core errno value: " + name);
        }
        return Integer.parseInt(value);
    }

    private NativeErrorCodes() {
    }
}
