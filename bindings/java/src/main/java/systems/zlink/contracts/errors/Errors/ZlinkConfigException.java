/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.errors;


/** Thrown when reading or applying a configuration option fails. */
public final class ZlinkConfigException
  extends TypedZlinkException {
    public ZlinkConfigException(ConfigResult result) {
        this(result, 0);
    }

    public ZlinkConfigException(ConfigResult result, int nativeErrno) {
        super(result, result.name(), result.value(), nativeErrno);
    }

    public ConfigResult getResult() {
        return (ConfigResult) result();
    }
}
