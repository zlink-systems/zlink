/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import systems.zlink.internal.ContractAccess;

/** Opaque capability for replying to one ROUTER request. */
public final class ReplyToken {
    private final Object owner;
    private final long value;

    static {
        ContractAccess.register((ContractAccess.ReplyTokenAccess)
            new ContractAccess.ReplyTokenAccess() {
                @Override
                public ReplyToken create(Object owner, long value) {
                    return new ReplyToken(owner, value);
                }

                @Override
                public Object owner(ReplyToken token) {
                    return token.owner;
                }

                @Override
                public long value(ReplyToken token) {
                    return token.value;
                }
            });
    }

    private ReplyToken(Object owner, long value) {
        if (owner == null || value == 0L)
            throw new IllegalArgumentException("invalid reply token");
        this.owner = owner;
        this.value = value;
    }

    @Override
    public boolean equals(Object other) {
        return other instanceof ReplyToken token
            && owner == token.owner && value == token.value;
    }

    @Override
    public int hashCode() {
        return 31 * System.identityHashCode(owner) + Long.hashCode(value);
    }

    @Override
    public String toString() {
        return "ReplyToken";
    }
}
