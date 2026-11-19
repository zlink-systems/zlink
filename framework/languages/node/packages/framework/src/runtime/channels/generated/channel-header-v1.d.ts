import * as $protobuf from "protobufjs";
import Long = require("long");

/** Namespace zlink. */
export namespace zlink {

    /** Namespace framework. */
    namespace framework {

        /** Namespace channel. */
        namespace channel {

            /** Namespace v1. */
            namespace v1 {

                /** ChannelKind enum. */
                enum ChannelKind {

                    /** CHANNEL_KIND_UNSPECIFIED value */
                    CHANNEL_KIND_UNSPECIFIED = 0,

                    /** REQUEST value */
                    REQUEST = 1,

                    /** RESPONSE value */
                    RESPONSE = 2,

                    /** COMMAND value */
                    COMMAND = 3,

                    /** PUBLISH value */
                    PUBLISH = 4,

                    /** ERROR value */
                    ERROR = 5
                }

                /** FlowOrigin enum. */
                enum FlowOrigin {

                    /** FLOW_ORIGIN_UNSPECIFIED value */
                    FLOW_ORIGIN_UNSPECIFIED = 0,

                    /** INBOUND value */
                    INBOUND = 1,

                    /** TIMER value */
                    TIMER = 2,

                    /** APPLICATION value */
                    APPLICATION = 3,

                    /** LIFECYCLE value */
                    LIFECYCLE = 4
                }

                /**
                 * Properties of a MetadataEntry.
                 * @deprecated Use zlink.framework.channel.v1.MetadataEntry.$Properties instead.
                 */
                interface IMetadataEntry extends zlink.framework.channel.v1.MetadataEntry.$Properties {
                }

                /** Represents a MetadataEntry. */
                class MetadataEntry {

                    /**
                     * Constructs a new MetadataEntry.
                     * @param [properties] Properties to set
                     */
                    constructor(properties?: zlink.framework.channel.v1.MetadataEntry.$Properties);

                    /** Unknown fields preserved while decoding when enabled */
                    $unknowns?: Uint8Array[];

                    /** MetadataEntry key. */
                    key: string;

                    /** MetadataEntry value. */
                    value: string;

                    /**
                     * Creates a new MetadataEntry instance using the specified properties.
                     * @param [properties] Properties to set
                     * @returns MetadataEntry instance
                     */
                    static create(properties: zlink.framework.channel.v1.MetadataEntry.$Shape): zlink.framework.channel.v1.MetadataEntry & zlink.framework.channel.v1.MetadataEntry.$Shape;
                    static create(properties?: zlink.framework.channel.v1.MetadataEntry.$Properties): zlink.framework.channel.v1.MetadataEntry;

                    /**
                     * Encodes the specified MetadataEntry message. Does not implicitly {@link zlink.framework.channel.v1.MetadataEntry.verify|verify} messages.
                     * @param message MetadataEntry message or plain object to encode
                     * @param [writer] Writer to encode to
                     * @returns Writer
                     */
                    static encode(message: zlink.framework.channel.v1.MetadataEntry.$Properties, writer?: $protobuf.Writer): $protobuf.Writer;

                    /**
                     * Encodes the specified MetadataEntry message, length delimited. Does not implicitly {@link zlink.framework.channel.v1.MetadataEntry.verify|verify} messages.
                     * @param message MetadataEntry message or plain object to encode
                     * @param [writer] Writer to encode to
                     * @returns Writer
                     */
                    static encodeDelimited(message: zlink.framework.channel.v1.MetadataEntry.$Properties, writer?: $protobuf.Writer): $protobuf.Writer;

                    /**
                     * Decodes a MetadataEntry message from the specified reader or buffer.
                     * @param reader Reader or buffer to decode from
                     * @param [length] Message length if known beforehand
                     * @returns {zlink.framework.channel.v1.MetadataEntry & zlink.framework.channel.v1.MetadataEntry.$Shape} MetadataEntry
                     * @throws {Error} If the payload is not a reader or valid buffer
                     * @throws {$protobuf.util.ProtocolError} If required fields are missing
                     */
                    static decode(reader: ($protobuf.Reader|Uint8Array), length?: number): zlink.framework.channel.v1.MetadataEntry & zlink.framework.channel.v1.MetadataEntry.$Shape;

                    /**
                     * Decodes a MetadataEntry message from the specified reader or buffer, length delimited.
                     * @param reader Reader or buffer to decode from
                     * @returns {zlink.framework.channel.v1.MetadataEntry & zlink.framework.channel.v1.MetadataEntry.$Shape} MetadataEntry
                     * @throws {Error} If the payload is not a reader or valid buffer
                     * @throws {$protobuf.util.ProtocolError} If required fields are missing
                     */
                    static decodeDelimited(reader: ($protobuf.Reader|Uint8Array)): zlink.framework.channel.v1.MetadataEntry & zlink.framework.channel.v1.MetadataEntry.$Shape;

                    /**
                     * Verifies a MetadataEntry message.
                     * @param message Plain object to verify
                     * @returns `null` if valid, otherwise the reason why it is not
                     */
                    static verify(message: { [k: string]: any }): (string|null);

                    /**
                     * Creates a MetadataEntry message from a plain object. Also converts values to their respective internal types.
                     * @param object Plain object
                     * @returns MetadataEntry
                     */
                    static fromObject(object: { [k: string]: any }): zlink.framework.channel.v1.MetadataEntry;

                    /**
                     * Creates a plain object from a MetadataEntry message. Also converts values to other types if specified.
                     * @param message MetadataEntry
                     * @param [options] Conversion options
                     * @returns Plain object
                     */
                    static toObject(message: zlink.framework.channel.v1.MetadataEntry, options?: $protobuf.IConversionOptions): { [k: string]: any };

                    /**
                     * Converts this MetadataEntry to JSON.
                     * @returns JSON object
                     */
                    toJSON(): { [k: string]: any };

                    /**
                     * Gets the type url for MetadataEntry
                     * @param [prefix] Custom type url prefix, defaults to `"type.googleapis.com"`
                     * @returns The type url
                     */
                    static getTypeUrl(prefix?: string): string;
                }

                namespace MetadataEntry {

                    /** Properties of a MetadataEntry. */
                    interface $Properties {

                        /** MetadataEntry key */
                        key?: (string|null);

                        /** MetadataEntry value */
                        value?: (string|null);

                        /** Unknown fields preserved while decoding when enabled */
                        $unknowns?: Uint8Array[];
                    }

                    /** Shape of a MetadataEntry. */
                    type $Shape = zlink.framework.channel.v1.MetadataEntry.$Properties;
                }

                /**
                 * Properties of a ChannelHeader.
                 * @deprecated Use zlink.framework.channel.v1.ChannelHeader.$Properties instead.
                 */
                interface IChannelHeader extends zlink.framework.channel.v1.ChannelHeader.$Properties {
                }

                /** Represents a ChannelHeader. */
                class ChannelHeader {

                    /**
                     * Constructs a new ChannelHeader.
                     * @param [properties] Properties to set
                     */
                    constructor(properties?: zlink.framework.channel.v1.ChannelHeader.$Properties);

                    /** Unknown fields preserved while decoding when enabled */
                    $unknowns?: Uint8Array[];

                    /** ChannelHeader kind. */
                    kind: zlink.framework.channel.v1.ChannelKind;

                    /** ChannelHeader channelName. */
                    channelName: string;

                    /** ChannelHeader messageName. */
                    messageName: string;

                    /** ChannelHeader contentType. */
                    contentType: string;

                    /** ChannelHeader correlationId. */
                    correlationId?: (string|null);

                    /** ChannelHeader deadline. */
                    deadline?: (string|null);

                    /** ChannelHeader errorCode. */
                    errorCode?: (string|null);

                    /** ChannelHeader errorMessage. */
                    errorMessage?: (string|null);

                    /** ChannelHeader flowId. */
                    flowId?: (string|null);

                    /** ChannelHeader flowOrigin. */
                    flowOrigin?: (zlink.framework.channel.v1.FlowOrigin|null);

                    /** ChannelHeader source. */
                    source?: (string|null);

                    /** ChannelHeader topic. */
                    topic?: (string|null);

                    /** ChannelHeader metadata. */
                    metadata: zlink.framework.channel.v1.MetadataEntry.$Properties[];

                    /**
                     * Creates a new ChannelHeader instance using the specified properties.
                     * @param [properties] Properties to set
                     * @returns ChannelHeader instance
                     */
                    static create(properties: zlink.framework.channel.v1.ChannelHeader.$Shape): zlink.framework.channel.v1.ChannelHeader & zlink.framework.channel.v1.ChannelHeader.$Shape;
                    static create(properties?: zlink.framework.channel.v1.ChannelHeader.$Properties): zlink.framework.channel.v1.ChannelHeader;

                    /**
                     * Encodes the specified ChannelHeader message. Does not implicitly {@link zlink.framework.channel.v1.ChannelHeader.verify|verify} messages.
                     * @param message ChannelHeader message or plain object to encode
                     * @param [writer] Writer to encode to
                     * @returns Writer
                     */
                    static encode(message: zlink.framework.channel.v1.ChannelHeader.$Properties, writer?: $protobuf.Writer): $protobuf.Writer;

                    /**
                     * Encodes the specified ChannelHeader message, length delimited. Does not implicitly {@link zlink.framework.channel.v1.ChannelHeader.verify|verify} messages.
                     * @param message ChannelHeader message or plain object to encode
                     * @param [writer] Writer to encode to
                     * @returns Writer
                     */
                    static encodeDelimited(message: zlink.framework.channel.v1.ChannelHeader.$Properties, writer?: $protobuf.Writer): $protobuf.Writer;

                    /**
                     * Decodes a ChannelHeader message from the specified reader or buffer.
                     * @param reader Reader or buffer to decode from
                     * @param [length] Message length if known beforehand
                     * @returns {zlink.framework.channel.v1.ChannelHeader & zlink.framework.channel.v1.ChannelHeader.$Shape} ChannelHeader
                     * @throws {Error} If the payload is not a reader or valid buffer
                     * @throws {$protobuf.util.ProtocolError} If required fields are missing
                     */
                    static decode(reader: ($protobuf.Reader|Uint8Array), length?: number): zlink.framework.channel.v1.ChannelHeader & zlink.framework.channel.v1.ChannelHeader.$Shape;

                    /**
                     * Decodes a ChannelHeader message from the specified reader or buffer, length delimited.
                     * @param reader Reader or buffer to decode from
                     * @returns {zlink.framework.channel.v1.ChannelHeader & zlink.framework.channel.v1.ChannelHeader.$Shape} ChannelHeader
                     * @throws {Error} If the payload is not a reader or valid buffer
                     * @throws {$protobuf.util.ProtocolError} If required fields are missing
                     */
                    static decodeDelimited(reader: ($protobuf.Reader|Uint8Array)): zlink.framework.channel.v1.ChannelHeader & zlink.framework.channel.v1.ChannelHeader.$Shape;

                    /**
                     * Verifies a ChannelHeader message.
                     * @param message Plain object to verify
                     * @returns `null` if valid, otherwise the reason why it is not
                     */
                    static verify(message: { [k: string]: any }): (string|null);

                    /**
                     * Creates a ChannelHeader message from a plain object. Also converts values to their respective internal types.
                     * @param object Plain object
                     * @returns ChannelHeader
                     */
                    static fromObject(object: { [k: string]: any }): zlink.framework.channel.v1.ChannelHeader;

                    /**
                     * Creates a plain object from a ChannelHeader message. Also converts values to other types if specified.
                     * @param message ChannelHeader
                     * @param [options] Conversion options
                     * @returns Plain object
                     */
                    static toObject(message: zlink.framework.channel.v1.ChannelHeader, options?: $protobuf.IConversionOptions): { [k: string]: any };

                    /**
                     * Converts this ChannelHeader to JSON.
                     * @returns JSON object
                     */
                    toJSON(): { [k: string]: any };

                    /**
                     * Gets the type url for ChannelHeader
                     * @param [prefix] Custom type url prefix, defaults to `"type.googleapis.com"`
                     * @returns The type url
                     */
                    static getTypeUrl(prefix?: string): string;
                }

                namespace ChannelHeader {

                    /** Properties of a ChannelHeader. */
                    interface $Properties {

                        /** ChannelHeader kind */
                        kind?: (zlink.framework.channel.v1.ChannelKind|null);

                        /** ChannelHeader channelName */
                        channelName?: (string|null);

                        /** ChannelHeader messageName */
                        messageName?: (string|null);

                        /** ChannelHeader contentType */
                        contentType?: (string|null);

                        /** ChannelHeader correlationId */
                        correlationId?: (string|null);

                        /** ChannelHeader deadline */
                        deadline?: (string|null);

                        /** ChannelHeader errorCode */
                        errorCode?: (string|null);

                        /** ChannelHeader errorMessage */
                        errorMessage?: (string|null);

                        /** ChannelHeader flowId */
                        flowId?: (string|null);

                        /** ChannelHeader flowOrigin */
                        flowOrigin?: (zlink.framework.channel.v1.FlowOrigin|null);

                        /** ChannelHeader source */
                        source?: (string|null);

                        /** ChannelHeader topic */
                        topic?: (string|null);

                        /** ChannelHeader metadata */
                        metadata?: (zlink.framework.channel.v1.MetadataEntry.$Properties[]|null);

                        /** Unknown fields preserved while decoding when enabled */
                        $unknowns?: Uint8Array[];
                    }

                    /** Shape of a ChannelHeader. */
                    type $Shape = zlink.framework.channel.v1.ChannelHeader.$Properties;
                }
            }
        }
    }
}
