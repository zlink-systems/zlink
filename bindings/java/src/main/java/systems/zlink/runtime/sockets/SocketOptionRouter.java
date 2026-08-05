/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;
import systems.zlink.internal.sockets.SocketOptions;

import systems.zlink.contracts.sockets.SocketType;

final class SocketOptionRouter {
    enum Family {
        COMMON, ROUTER, PUB, SUB, STREAM
    }

    record Route(Family family, int optionId) {
        int nativeCommonOptionId() {
            return translateCommonOptionId(optionId);
        }
    }

    private SocketOptionRouter() {
    }

    static Route route(int optionId, SocketType type) {
        if (optionId == SocketOptions.ROUTER_MANDATORY.optionId()) {
            return new Route(Family.ROUTER, 0x3101);
        }
        if (optionId == SocketOptions.PROBE_ROUTER.optionId()) {
            return new Route(Family.ROUTER, 0x3103);
        }
        if (optionId == SocketOptions.CONNECT_ROUTING_ID.optionId()) {
            return new Route(Family.ROUTER, 0x3104);
        }
        if (optionId == SocketOptions.XPUB_VERBOSE.optionId()) {
            return new Route(Family.PUB, 0x3301);
        }
        if (optionId == SocketOptions.XPUB_VERBOSER.optionId()) {
            return new Route(Family.PUB, 0x3302);
        }
        if (optionId == SocketOptions.XPUB_MANUAL.optionId()) {
            return new Route(Family.PUB, 0x3303);
        }
        if (optionId == SocketOptions.XPUB_MANUAL_LAST_VALUE.optionId()
            && type == SocketType.XPUB) {
            return new Route(Family.PUB, 0x3304);
        }
        if (optionId == SocketOptions.XPUB_NODROP.optionId()) {
            return new Route(Family.PUB, 0x3305);
        }
        if (optionId == SocketOptions.XPUB_WELCOME_MSG.optionId()) {
            return new Route(Family.PUB, 0x3306);
        }
        if (optionId == SocketOptions.TOPICS_COUNT.optionId()) {
            if (type == SocketType.SUB || type == SocketType.XSUB) {
                return new Route(Family.SUB, 0x3400);
            }
            if (type == SocketType.PUB || type == SocketType.XPUB) {
                return new Route(Family.PUB, 0x3307);
            }
        }
        if (optionId == SocketOptions.PUB_APPROVE_SUBSCRIBE_BYTES.optionId()) {
            return new Route(Family.PUB, 0x3308);
        }
        if (optionId == SocketOptions.PUB_REJECT_SUBSCRIBE_BYTES.optionId()) {
            return new Route(Family.PUB, 0x3309);
        }
        if (optionId == SocketOptions.STREAM_NOTIFY.optionId()) {
            return new Route(Family.STREAM, 0x3501);
        }
        return new Route(Family.COMMON, optionId);
    }

    private static int translateCommonOptionId(int optionId) {
        return switch (optionId) {
            case 4 -> 0x3001;
            case 8 -> 0x3003;
            case 9 -> 0x3004;
            case 11 -> 0x3005;
            case 12 -> 0x3006;
            case 14 -> 0x3007;
            case 15 -> 0x3008;
            case 16 -> 0x3009;
            case 17 -> 0x300A;
            case 18 -> 0x300B;
            case 19 -> 0x300C;
            case 21 -> 0x300D;
            case 22 -> 0x300E;
            case 23 -> 0x300F;
            case 24 -> 0x3010;
            case 25 -> 0x3011;
            case 27 -> 0x3012;
            case 28 -> 0x3013;
            case 32 -> 0x3014;
            case 34 -> 0x3015;
            case 35 -> 0x3016;
            case 36 -> 0x3017;
            case 37 -> 0x3018;
            case 39 -> 0x3019;
            case 42 -> 0x301A;
            case 54 -> 0x301B;
            case 57 -> 0x301C;
            case 66 -> 0x301D;
            case 70 -> 0x301E;
            case 74 -> 0x3020;
            case 75 -> 0x3021;
            case 76 -> 0x3022;
            case 77 -> 0x3023;
            case 79 -> 0x3024;
            case 80 -> 0x3025;
            case 84 -> 0x3026;
            case 92 -> 0x3027;
            case 95 -> 0x3028;
            case 96 -> 0x3029;
            case 97 -> 0x302A;
            case 98 -> 0x302B;
            case 99 -> 0x302C;
            case 100 -> 0x302D;
            case 101 -> 0x302E;
            case 102 -> 0x302F;
            case 117 -> 0x3030;
            case 118 -> 0x3031;
            default -> optionId;
        };
    }
}
