/* SPDX-License-Identifier: Apache-2.0 */
module zlink.framework.json.internal {
    requires systems.zlink;
    requires com.fasterxml.jackson.databind;
    requires com.fasterxml.jackson.datatype.jsr310;

    exports systems.zlink.framework.runtime.internal.json to
            systems.zlink.framework,
            zlink.stream.connector;
}
