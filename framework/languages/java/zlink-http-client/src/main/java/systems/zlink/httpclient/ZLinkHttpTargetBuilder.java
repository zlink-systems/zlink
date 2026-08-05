/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import java.util.List;
import java.util.Map;
import systems.zlink.httpclient.internal.HttpClientText;

final class ZLinkHttpTargetBuilder {
    private ZLinkHttpTargetBuilder() {
    }

    static String resolve(String path, List<Map.Entry<String, String>> query) {
        if (query.isEmpty()) {
            return path;
        }
        StringBuilder target = new StringBuilder(path);
        char separator = path.indexOf('?') >= 0 ? '&' : '?';
        for (Map.Entry<String, String> entry : query) {
            target.append(separator)
                .append(HttpClientText.percentEncode(entry.getKey()))
                .append('=')
                .append(HttpClientText.percentEncode(entry.getValue()));
            separator = '&';
        }
        return target.toString();
    }
}
