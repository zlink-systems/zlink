/*eslint-disable block-scoped-var, id-length, no-control-regex, no-magic-numbers, no-mixed-operators, no-prototype-builtins, no-redeclare, no-shadow, no-var, sort-vars, default-case, jsdoc/require-param*/
"use strict";

var $protobuf = require("protobufjs/minimal");

// Common aliases
var $Reader = $protobuf.Reader, $Writer = $protobuf.Writer, $util = $protobuf.util;
var $Object = $util.global.Object, $undefined = $util.global.undefined, $Error = $util.global.Error, $RangeError = $util.global.RangeError, $TypeError = $util.global.TypeError, $String = $util.global.String, $Array = $util.global.Array;

// Exported root namespace
var $root = $protobuf.roots["default"] || ($protobuf.roots["default"] = {});

$root.zlink = (function() {

    /**
     * Namespace zlink.
     * @exports zlink
     * @namespace
     */
    var zlink = {};

    zlink.framework = (function() {

        /**
         * Namespace framework.
         * @memberof zlink
         * @namespace
         */
        var framework = {};

        framework.channel = (function() {

            /**
             * Namespace channel.
             * @memberof zlink.framework
             * @namespace
             */
            var channel = {};

            channel.v1 = (function() {

                /**
                 * Namespace v1.
                 * @memberof zlink.framework.channel
                 * @namespace
                 */
                var v1 = {};

                /**
                 * ChannelKind enum.
                 * @name zlink.framework.channel.v1.ChannelKind
                 * @enum {number}
                 * @property {number} CHANNEL_KIND_UNSPECIFIED=0 CHANNEL_KIND_UNSPECIFIED value
                 * @property {number} REQUEST=1 REQUEST value
                 * @property {number} RESPONSE=2 RESPONSE value
                 * @property {number} COMMAND=3 COMMAND value
                 * @property {number} PUBLISH=4 PUBLISH value
                 * @property {number} ERROR=5 ERROR value
                 */
                v1.ChannelKind = (function() {
                    var valuesById = $Object.create(null), values = $Object.create(valuesById);
                    values[valuesById[0] = "CHANNEL_KIND_UNSPECIFIED"] = 0;
                    values[valuesById[1] = "REQUEST"] = 1;
                    values[valuesById[2] = "RESPONSE"] = 2;
                    values[valuesById[3] = "COMMAND"] = 3;
                    values[valuesById[4] = "PUBLISH"] = 4;
                    values[valuesById[5] = "ERROR"] = 5;
                    return values;
                })();

                /**
                 * FlowOrigin enum.
                 * @name zlink.framework.channel.v1.FlowOrigin
                 * @enum {number}
                 * @property {number} FLOW_ORIGIN_UNSPECIFIED=0 FLOW_ORIGIN_UNSPECIFIED value
                 * @property {number} INBOUND=1 INBOUND value
                 * @property {number} TIMER=2 TIMER value
                 * @property {number} APPLICATION=3 APPLICATION value
                 * @property {number} LIFECYCLE=4 LIFECYCLE value
                 */
                v1.FlowOrigin = (function() {
                    var valuesById = $Object.create(null), values = $Object.create(valuesById);
                    values[valuesById[0] = "FLOW_ORIGIN_UNSPECIFIED"] = 0;
                    values[valuesById[1] = "INBOUND"] = 1;
                    values[valuesById[2] = "TIMER"] = 2;
                    values[valuesById[3] = "APPLICATION"] = 3;
                    values[valuesById[4] = "LIFECYCLE"] = 4;
                    return values;
                })();

                v1.MetadataEntry = (function() {

                    /**
                     * Properties of a MetadataEntry.
                     * @typedef {Object} zlink.framework.channel.v1.MetadataEntry.$Properties
                     * @property {string|null} [key] MetadataEntry key
                     * @property {string|null} [value] MetadataEntry value
                     * @property {Array.<Uint8Array>} [$unknowns] Unknown fields preserved while decoding when enabled
                     */

                    /**
                     * Properties of a MetadataEntry.
                     * @memberof zlink.framework.channel.v1
                     * @interface IMetadataEntry
                     * @augments zlink.framework.channel.v1.MetadataEntry.$Properties
                     * @deprecated Use zlink.framework.channel.v1.MetadataEntry.$Properties instead.
                     */

                    /**
                     * Shape of a MetadataEntry.
                     * @typedef {zlink.framework.channel.v1.MetadataEntry.$Properties} zlink.framework.channel.v1.MetadataEntry.$Shape
                     */

                    /**
                     * Constructs a new MetadataEntry.
                     * @memberof zlink.framework.channel.v1
                     * @classdesc Represents a MetadataEntry.
                     * @constructor
                     * @param {zlink.framework.channel.v1.MetadataEntry.$Properties=} [properties] Properties to set
                     * @property {Array.<Uint8Array>} [$unknowns] Unknown fields preserved while decoding when enabled
                     */
                    var MetadataEntry = function (properties) {
                        if (properties)
                            for (var keys = $Object.keys(properties), i = 0; i < keys.length; ++i)
                                if (properties[keys[i]] != null && keys[i] !== "__proto__")
                                    this[keys[i]] = properties[keys[i]];
                    };

                    /**
                     * MetadataEntry key.
                     * @member {string} key
                     * @memberof zlink.framework.channel.v1.MetadataEntry
                     * @instance
                     */
                    MetadataEntry.prototype.key = "";

                    /**
                     * MetadataEntry value.
                     * @member {string} value
                     * @memberof zlink.framework.channel.v1.MetadataEntry
                     * @instance
                     */
                    MetadataEntry.prototype.value = "";

                    /**
                     * Creates a new MetadataEntry instance using the specified properties.
                     * @function create
                     * @memberof zlink.framework.channel.v1.MetadataEntry
                     * @static
                     * @param {zlink.framework.channel.v1.MetadataEntry.$Properties=} [properties] Properties to set
                     * @returns {zlink.framework.channel.v1.MetadataEntry} MetadataEntry instance
                     * @type {{
                     *   (properties: zlink.framework.channel.v1.MetadataEntry.$Shape): zlink.framework.channel.v1.MetadataEntry & zlink.framework.channel.v1.MetadataEntry.$Shape;
                     *   (properties?: zlink.framework.channel.v1.MetadataEntry.$Properties): zlink.framework.channel.v1.MetadataEntry;
                     * }}
                     */
                    MetadataEntry.create = function(properties) {
                        return new MetadataEntry(properties);
                    };

                    /**
                     * Encodes the specified MetadataEntry message. Does not implicitly {@link zlink.framework.channel.v1.MetadataEntry.verify|verify} messages.
                     * @function encode
                     * @memberof zlink.framework.channel.v1.MetadataEntry
                     * @static
                     * @param {zlink.framework.channel.v1.MetadataEntry.$Properties} message MetadataEntry message or plain object to encode
                     * @param {$protobuf.Writer} [writer] Writer to encode to
                     * @returns {$protobuf.Writer} Writer
                     */
                    MetadataEntry.encode = function (message, writer, _depth) {
                        if (!writer)
                            writer = $Writer.create();
                        if (_depth === $undefined)
                            _depth = 0;
                        if (_depth > $util.recursionLimit)
                            throw $Error("max depth exceeded");
                        if (message.key != null && $Object.hasOwnProperty.call(message, "key") && message.key !== "")
                            writer.uint32(/* id 1, wireType 2 =*/10).string(message.key);
                        if (message.value != null && $Object.hasOwnProperty.call(message, "value") && message.value !== "")
                            writer.uint32(/* id 2, wireType 2 =*/18).string(message.value);
                        if (message.$unknowns != null && $Object.hasOwnProperty.call(message, "$unknowns"))
                            for (var i = 0; i < message.$unknowns.length; ++i)
                                writer.raw(message.$unknowns[i]);
                        return writer;
                    };

                    /**
                     * Encodes the specified MetadataEntry message, length delimited. Does not implicitly {@link zlink.framework.channel.v1.MetadataEntry.verify|verify} messages.
                     * @function encodeDelimited
                     * @memberof zlink.framework.channel.v1.MetadataEntry
                     * @static
                     * @param {zlink.framework.channel.v1.MetadataEntry.$Properties} message MetadataEntry message or plain object to encode
                     * @param {$protobuf.Writer} [writer] Writer to encode to
                     * @returns {$protobuf.Writer} Writer
                     */
                    MetadataEntry.encodeDelimited = function(message, writer) {
                        return this.encode(message, (writer || $Writer.create()).fork()).ldelim();
                    };

                    /**
                     * Decodes a MetadataEntry message from the specified reader or buffer.
                     * @function decode
                     * @memberof zlink.framework.channel.v1.MetadataEntry
                     * @static
                     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
                     * @param {number} [length] Message length if known beforehand
                     * @returns {zlink.framework.channel.v1.MetadataEntry & zlink.framework.channel.v1.MetadataEntry.$Shape} MetadataEntry
                     * @throws {Error} If the payload is not a reader or valid buffer
                     * @throws {$protobuf.util.ProtocolError} If required fields are missing
                     */
                    MetadataEntry.decode = function (reader, length, _end, _depth, _target) {
                        if (!(reader instanceof $Reader))
                            reader = $Reader.create(reader);
                        if (_depth === $undefined)
                            _depth = 0;
                        if (_depth > $Reader.recursionLimit)
                            throw $Error("max depth exceeded");
                        var end, message, value;
                        if (length === $undefined)
                            end = reader.len;
                        else {
                            end = reader.pos + length;
                            if (end > reader.len)
                                throw $RangeError("index out of range");
                            length = reader.len;
                            reader.len = end;
                        }
                        message = _target || new $root.zlink.framework.channel.v1.MetadataEntry();
                        while (reader.pos < end) {
                            var start = reader.pos;
                            var tag = reader.tag();
                            if (tag === _end) {
                                _end = $undefined;
                                break;
                            }
                            var wireType = tag & 7;
                            switch (tag >>>= 3) {
                            case 1: {
                                    if (wireType !== 2)
                                        break;
                                    if ((value = reader.stringVerify()).length)
                                        message.key = value;
                                    else
                                        delete message.key;
                                    continue;
                                }
                            case 2: {
                                    if (wireType !== 2)
                                        break;
                                    if ((value = reader.stringVerify()).length)
                                        message.value = value;
                                    else
                                        delete message.value;
                                    continue;
                                }
                            }
                            reader.skipType(wireType, _depth, tag);
                            if (!reader.discardUnknown) {
                                $util.makeProp(message, "$unknowns", false);
                                (message.$unknowns || (message.$unknowns = [])).push(reader.raw(start, reader.pos));
                            }
                        }
                        if (length !== $undefined) {
                            if (reader.pos !== end)
                                throw $RangeError("index out of range");
                            reader.len = length;
                        }
                        if (_end !== $undefined)
                            throw $Error("missing end group");
                        return message;
                    };

                    /**
                     * Decodes a MetadataEntry message from the specified reader or buffer, length delimited.
                     * @function decodeDelimited
                     * @memberof zlink.framework.channel.v1.MetadataEntry
                     * @static
                     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
                     * @returns {zlink.framework.channel.v1.MetadataEntry & zlink.framework.channel.v1.MetadataEntry.$Shape} MetadataEntry
                     * @throws {Error} If the payload is not a reader or valid buffer
                     * @throws {$protobuf.util.ProtocolError} If required fields are missing
                     */
                    MetadataEntry.decodeDelimited = function(reader) {
                        if (!(reader instanceof $Reader))
                            reader = new $Reader(reader);
                        return this.decode(reader, reader.size());
                    };

                    /**
                     * Verifies a MetadataEntry message.
                     * @function verify
                     * @memberof zlink.framework.channel.v1.MetadataEntry
                     * @static
                     * @param {Object.<string,*>} message Plain object to verify
                     * @returns {string|null} `null` if valid, otherwise the reason why it is not
                     */
                    MetadataEntry.verify = function (message, _depth) {
                        if (typeof message !== "object" || message === null)
                            return "object expected";
                        if (_depth === $undefined)
                            _depth = 0;
                        if (_depth > $util.recursionLimit)
                            return "max depth exceeded";
                        if (message.key != null && $Object.hasOwnProperty.call(message, "key"))
                            if (!$util.isString(message.key))
                                return "key: string expected";
                        if (message.value != null && $Object.hasOwnProperty.call(message, "value"))
                            if (!$util.isString(message.value))
                                return "value: string expected";
                        return null;
                    };

                    /**
                     * Creates a MetadataEntry message from a plain object. Also converts values to their respective internal types.
                     * @function fromObject
                     * @memberof zlink.framework.channel.v1.MetadataEntry
                     * @static
                     * @param {Object.<string,*>} object Plain object
                     * @returns {zlink.framework.channel.v1.MetadataEntry} MetadataEntry
                     */
                    MetadataEntry.fromObject = function (object, _depth) {
                        if (object instanceof $root.zlink.framework.channel.v1.MetadataEntry)
                            return object;
                        if (!$util.isObject(object))
                            throw $TypeError(".zlink.framework.channel.v1.MetadataEntry: object expected");
                        if (_depth === $undefined)
                            _depth = 0;
                        if (_depth > $util.recursionLimit)
                            throw $Error("max depth exceeded");
                        var message = new $root.zlink.framework.channel.v1.MetadataEntry();
                        if (object.key != null)
                            if (typeof object.key !== "string" || object.key.length)
                                message.key = $String(object.key);
                        if (object.value != null)
                            if (typeof object.value !== "string" || object.value.length)
                                message.value = $String(object.value);
                        return message;
                    };

                    /**
                     * Creates a plain object from a MetadataEntry message. Also converts values to other types if specified.
                     * @function toObject
                     * @memberof zlink.framework.channel.v1.MetadataEntry
                     * @static
                     * @param {zlink.framework.channel.v1.MetadataEntry} message MetadataEntry
                     * @param {$protobuf.IConversionOptions} [options] Conversion options
                     * @returns {Object.<string,*>} Plain object
                     */
                    MetadataEntry.toObject = function (message, options, _depth) {
                        if (!options)
                            options = {};
                        if (_depth === $undefined)
                            _depth = 0;
                        if (_depth > $util.recursionLimit)
                            throw $Error("max depth exceeded");
                        var object = {};
                        if (options.defaults) {
                            object.key = "";
                            object.value = "";
                        }
                        if (message.key != null && $Object.hasOwnProperty.call(message, "key"))
                            object.key = message.key;
                        if (message.value != null && $Object.hasOwnProperty.call(message, "value"))
                            object.value = message.value;
                        return object;
                    };

                    /**
                     * Converts this MetadataEntry to JSON.
                     * @function toJSON
                     * @memberof zlink.framework.channel.v1.MetadataEntry
                     * @instance
                     * @returns {Object.<string,*>} JSON object
                     */
                    MetadataEntry.prototype.toJSON = function() {
                        return MetadataEntry.toObject(this, $protobuf.util.toJSONOptions);
                    };

                    /**
                     * Gets the type url for MetadataEntry
                     * @function getTypeUrl
                     * @memberof zlink.framework.channel.v1.MetadataEntry
                     * @static
                     * @param {string} [prefix] Custom type url prefix, defaults to `"type.googleapis.com"`
                     * @returns {string} The type url
                     */
                    MetadataEntry.getTypeUrl = function(prefix) {
                        if (prefix === $undefined)
                            prefix = "type.googleapis.com";
                        return prefix + "/zlink.framework.channel.v1.MetadataEntry";
                    };

                    return MetadataEntry;
                })();

                v1.ChannelHeader = (function() {

                    /**
                     * Properties of a ChannelHeader.
                     * @typedef {Object} zlink.framework.channel.v1.ChannelHeader.$Properties
                     * @property {zlink.framework.channel.v1.ChannelKind|null} [kind] ChannelHeader kind
                     * @property {string|null} [channelName] ChannelHeader channelName
                     * @property {string|null} [messageName] ChannelHeader messageName
                     * @property {string|null} [contentType] ChannelHeader contentType
                     * @property {string|null} [correlationId] ChannelHeader correlationId
                     * @property {string|null} [deadline] ChannelHeader deadline
                     * @property {string|null} [errorCode] ChannelHeader errorCode
                     * @property {string|null} [errorMessage] ChannelHeader errorMessage
                     * @property {string|null} [flowId] ChannelHeader flowId
                     * @property {zlink.framework.channel.v1.FlowOrigin|null} [flowOrigin] ChannelHeader flowOrigin
                     * @property {string|null} [source] ChannelHeader source
                     * @property {string|null} [topic] ChannelHeader topic
                     * @property {Array.<zlink.framework.channel.v1.MetadataEntry.$Properties>|null} [metadata] ChannelHeader metadata
                     * @property {Array.<Uint8Array>} [$unknowns] Unknown fields preserved while decoding when enabled
                     */

                    /**
                     * Properties of a ChannelHeader.
                     * @memberof zlink.framework.channel.v1
                     * @interface IChannelHeader
                     * @augments zlink.framework.channel.v1.ChannelHeader.$Properties
                     * @deprecated Use zlink.framework.channel.v1.ChannelHeader.$Properties instead.
                     */

                    /**
                     * Shape of a ChannelHeader.
                     * @typedef {zlink.framework.channel.v1.ChannelHeader.$Properties} zlink.framework.channel.v1.ChannelHeader.$Shape
                     */

                    /**
                     * Constructs a new ChannelHeader.
                     * @memberof zlink.framework.channel.v1
                     * @classdesc Represents a ChannelHeader.
                     * @constructor
                     * @param {zlink.framework.channel.v1.ChannelHeader.$Properties=} [properties] Properties to set
                     * @property {Array.<Uint8Array>} [$unknowns] Unknown fields preserved while decoding when enabled
                     */
                    var ChannelHeader = function (properties) {
                        this.metadata = [];
                        if (properties)
                            for (var keys = $Object.keys(properties), i = 0; i < keys.length; ++i)
                                if (properties[keys[i]] != null && keys[i] !== "__proto__")
                                    this[keys[i]] = properties[keys[i]];
                    };

                    /**
                     * ChannelHeader kind.
                     * @member {zlink.framework.channel.v1.ChannelKind} kind
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @instance
                     */
                    ChannelHeader.prototype.kind = 0;

                    /**
                     * ChannelHeader channelName.
                     * @member {string} channelName
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @instance
                     */
                    ChannelHeader.prototype.channelName = "";

                    /**
                     * ChannelHeader messageName.
                     * @member {string} messageName
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @instance
                     */
                    ChannelHeader.prototype.messageName = "";

                    /**
                     * ChannelHeader contentType.
                     * @member {string} contentType
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @instance
                     */
                    ChannelHeader.prototype.contentType = "";

                    /**
                     * ChannelHeader correlationId.
                     * @member {string|null|undefined} correlationId
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @instance
                     */
                    ChannelHeader.prototype.correlationId = null;

                    /**
                     * ChannelHeader deadline.
                     * @member {string|null|undefined} deadline
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @instance
                     */
                    ChannelHeader.prototype.deadline = null;

                    /**
                     * ChannelHeader errorCode.
                     * @member {string|null|undefined} errorCode
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @instance
                     */
                    ChannelHeader.prototype.errorCode = null;

                    /**
                     * ChannelHeader errorMessage.
                     * @member {string|null|undefined} errorMessage
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @instance
                     */
                    ChannelHeader.prototype.errorMessage = null;

                    /**
                     * ChannelHeader flowId.
                     * @member {string|null|undefined} flowId
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @instance
                     */
                    ChannelHeader.prototype.flowId = null;

                    /**
                     * ChannelHeader flowOrigin.
                     * @member {zlink.framework.channel.v1.FlowOrigin|null|undefined} flowOrigin
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @instance
                     */
                    ChannelHeader.prototype.flowOrigin = null;

                    /**
                     * ChannelHeader source.
                     * @member {string|null|undefined} source
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @instance
                     */
                    ChannelHeader.prototype.source = null;

                    /**
                     * ChannelHeader topic.
                     * @member {string|null|undefined} topic
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @instance
                     */
                    ChannelHeader.prototype.topic = null;

                    /**
                     * ChannelHeader metadata.
                     * @member {Array.<zlink.framework.channel.v1.MetadataEntry.$Properties>} metadata
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @instance
                     */
                    ChannelHeader.prototype.metadata = $util.emptyArray;

                    // OneOf field names bound to virtual getters and setters
                    var $oneOfFields;

                    // Virtual OneOf for proto3 optional field
                    $Object.defineProperty(ChannelHeader.prototype, "_correlationId", {
                        get: $util.oneOfGetter($oneOfFields = ["correlationId"]),
                        set: $util.oneOfSetter($oneOfFields)
                    });

                    // Virtual OneOf for proto3 optional field
                    $Object.defineProperty(ChannelHeader.prototype, "_deadline", {
                        get: $util.oneOfGetter($oneOfFields = ["deadline"]),
                        set: $util.oneOfSetter($oneOfFields)
                    });

                    // Virtual OneOf for proto3 optional field
                    $Object.defineProperty(ChannelHeader.prototype, "_errorCode", {
                        get: $util.oneOfGetter($oneOfFields = ["errorCode"]),
                        set: $util.oneOfSetter($oneOfFields)
                    });

                    // Virtual OneOf for proto3 optional field
                    $Object.defineProperty(ChannelHeader.prototype, "_errorMessage", {
                        get: $util.oneOfGetter($oneOfFields = ["errorMessage"]),
                        set: $util.oneOfSetter($oneOfFields)
                    });

                    // Virtual OneOf for proto3 optional field
                    $Object.defineProperty(ChannelHeader.prototype, "_flowId", {
                        get: $util.oneOfGetter($oneOfFields = ["flowId"]),
                        set: $util.oneOfSetter($oneOfFields)
                    });

                    // Virtual OneOf for proto3 optional field
                    $Object.defineProperty(ChannelHeader.prototype, "_flowOrigin", {
                        get: $util.oneOfGetter($oneOfFields = ["flowOrigin"]),
                        set: $util.oneOfSetter($oneOfFields)
                    });

                    // Virtual OneOf for proto3 optional field
                    $Object.defineProperty(ChannelHeader.prototype, "_source", {
                        get: $util.oneOfGetter($oneOfFields = ["source"]),
                        set: $util.oneOfSetter($oneOfFields)
                    });

                    // Virtual OneOf for proto3 optional field
                    $Object.defineProperty(ChannelHeader.prototype, "_topic", {
                        get: $util.oneOfGetter($oneOfFields = ["topic"]),
                        set: $util.oneOfSetter($oneOfFields)
                    });

                    /**
                     * Creates a new ChannelHeader instance using the specified properties.
                     * @function create
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @static
                     * @param {zlink.framework.channel.v1.ChannelHeader.$Properties=} [properties] Properties to set
                     * @returns {zlink.framework.channel.v1.ChannelHeader} ChannelHeader instance
                     * @type {{
                     *   (properties: zlink.framework.channel.v1.ChannelHeader.$Shape): zlink.framework.channel.v1.ChannelHeader & zlink.framework.channel.v1.ChannelHeader.$Shape;
                     *   (properties?: zlink.framework.channel.v1.ChannelHeader.$Properties): zlink.framework.channel.v1.ChannelHeader;
                     * }}
                     */
                    ChannelHeader.create = function(properties) {
                        return new ChannelHeader(properties);
                    };

                    /**
                     * Encodes the specified ChannelHeader message. Does not implicitly {@link zlink.framework.channel.v1.ChannelHeader.verify|verify} messages.
                     * @function encode
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @static
                     * @param {zlink.framework.channel.v1.ChannelHeader.$Properties} message ChannelHeader message or plain object to encode
                     * @param {$protobuf.Writer} [writer] Writer to encode to
                     * @returns {$protobuf.Writer} Writer
                     */
                    ChannelHeader.encode = function (message, writer, _depth) {
                        if (!writer)
                            writer = $Writer.create();
                        if (_depth === $undefined)
                            _depth = 0;
                        if (_depth > $util.recursionLimit)
                            throw $Error("max depth exceeded");
                        if (message.kind != null && $Object.hasOwnProperty.call(message, "kind") && message.kind !== 0)
                            writer.uint32(/* id 1, wireType 0 =*/8).int32(message.kind);
                        if (message.channelName != null && $Object.hasOwnProperty.call(message, "channelName") && message.channelName !== "")
                            writer.uint32(/* id 2, wireType 2 =*/18).string(message.channelName);
                        if (message.messageName != null && $Object.hasOwnProperty.call(message, "messageName") && message.messageName !== "")
                            writer.uint32(/* id 3, wireType 2 =*/26).string(message.messageName);
                        if (message.contentType != null && $Object.hasOwnProperty.call(message, "contentType") && message.contentType !== "")
                            writer.uint32(/* id 4, wireType 2 =*/34).string(message.contentType);
                        if (message.correlationId != null && $Object.hasOwnProperty.call(message, "correlationId"))
                            writer.uint32(/* id 5, wireType 2 =*/42).string(message.correlationId);
                        if (message.deadline != null && $Object.hasOwnProperty.call(message, "deadline"))
                            writer.uint32(/* id 6, wireType 2 =*/50).string(message.deadline);
                        if (message.errorCode != null && $Object.hasOwnProperty.call(message, "errorCode"))
                            writer.uint32(/* id 7, wireType 2 =*/58).string(message.errorCode);
                        if (message.errorMessage != null && $Object.hasOwnProperty.call(message, "errorMessage"))
                            writer.uint32(/* id 8, wireType 2 =*/66).string(message.errorMessage);
                        if (message.flowId != null && $Object.hasOwnProperty.call(message, "flowId"))
                            writer.uint32(/* id 9, wireType 2 =*/74).string(message.flowId);
                        if (message.flowOrigin != null && $Object.hasOwnProperty.call(message, "flowOrigin"))
                            writer.uint32(/* id 10, wireType 0 =*/80).int32(message.flowOrigin);
                        if (message.source != null && $Object.hasOwnProperty.call(message, "source"))
                            writer.uint32(/* id 11, wireType 2 =*/90).string(message.source);
                        if (message.topic != null && $Object.hasOwnProperty.call(message, "topic"))
                            writer.uint32(/* id 12, wireType 2 =*/98).string(message.topic);
                        if (message.metadata != null && message.metadata.length)
                            for (var i = 0; i < message.metadata.length; ++i)
                                $root.zlink.framework.channel.v1.MetadataEntry.encode(message.metadata[i], writer.uint32(/* id 13, wireType 2 =*/106).fork(), _depth + 1).ldelim();
                        if (message.$unknowns != null && $Object.hasOwnProperty.call(message, "$unknowns"))
                            for (var i = 0; i < message.$unknowns.length; ++i)
                                writer.raw(message.$unknowns[i]);
                        return writer;
                    };

                    /**
                     * Encodes the specified ChannelHeader message, length delimited. Does not implicitly {@link zlink.framework.channel.v1.ChannelHeader.verify|verify} messages.
                     * @function encodeDelimited
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @static
                     * @param {zlink.framework.channel.v1.ChannelHeader.$Properties} message ChannelHeader message or plain object to encode
                     * @param {$protobuf.Writer} [writer] Writer to encode to
                     * @returns {$protobuf.Writer} Writer
                     */
                    ChannelHeader.encodeDelimited = function(message, writer) {
                        return this.encode(message, (writer || $Writer.create()).fork()).ldelim();
                    };

                    /**
                     * Decodes a ChannelHeader message from the specified reader or buffer.
                     * @function decode
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @static
                     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
                     * @param {number} [length] Message length if known beforehand
                     * @returns {zlink.framework.channel.v1.ChannelHeader & zlink.framework.channel.v1.ChannelHeader.$Shape} ChannelHeader
                     * @throws {Error} If the payload is not a reader or valid buffer
                     * @throws {$protobuf.util.ProtocolError} If required fields are missing
                     */
                    ChannelHeader.decode = function (reader, length, _end, _depth, _target) {
                        if (!(reader instanceof $Reader))
                            reader = $Reader.create(reader);
                        if (_depth === $undefined)
                            _depth = 0;
                        if (_depth > $Reader.recursionLimit)
                            throw $Error("max depth exceeded");
                        var end, message, value;
                        if (length === $undefined)
                            end = reader.len;
                        else {
                            end = reader.pos + length;
                            if (end > reader.len)
                                throw $RangeError("index out of range");
                            length = reader.len;
                            reader.len = end;
                        }
                        message = _target || new $root.zlink.framework.channel.v1.ChannelHeader();
                        while (reader.pos < end) {
                            var start = reader.pos;
                            var tag = reader.tag();
                            if (tag === _end) {
                                _end = $undefined;
                                break;
                            }
                            var wireType = tag & 7;
                            switch (tag >>>= 3) {
                            case 1: {
                                    if (wireType !== 0)
                                        break;
                                    if (value = reader.int32())
                                        message.kind = value;
                                    else
                                        delete message.kind;
                                    continue;
                                }
                            case 2: {
                                    if (wireType !== 2)
                                        break;
                                    if ((value = reader.stringVerify()).length)
                                        message.channelName = value;
                                    else
                                        delete message.channelName;
                                    continue;
                                }
                            case 3: {
                                    if (wireType !== 2)
                                        break;
                                    if ((value = reader.stringVerify()).length)
                                        message.messageName = value;
                                    else
                                        delete message.messageName;
                                    continue;
                                }
                            case 4: {
                                    if (wireType !== 2)
                                        break;
                                    if ((value = reader.stringVerify()).length)
                                        message.contentType = value;
                                    else
                                        delete message.contentType;
                                    continue;
                                }
                            case 5: {
                                    if (wireType !== 2)
                                        break;
                                    message.correlationId = reader.stringVerify();
                                    message._correlationId = "correlationId";
                                    continue;
                                }
                            case 6: {
                                    if (wireType !== 2)
                                        break;
                                    message.deadline = reader.stringVerify();
                                    message._deadline = "deadline";
                                    continue;
                                }
                            case 7: {
                                    if (wireType !== 2)
                                        break;
                                    message.errorCode = reader.stringVerify();
                                    message._errorCode = "errorCode";
                                    continue;
                                }
                            case 8: {
                                    if (wireType !== 2)
                                        break;
                                    message.errorMessage = reader.stringVerify();
                                    message._errorMessage = "errorMessage";
                                    continue;
                                }
                            case 9: {
                                    if (wireType !== 2)
                                        break;
                                    message.flowId = reader.stringVerify();
                                    message._flowId = "flowId";
                                    continue;
                                }
                            case 10: {
                                    if (wireType !== 0)
                                        break;
                                    message.flowOrigin = reader.int32();
                                    message._flowOrigin = "flowOrigin";
                                    continue;
                                }
                            case 11: {
                                    if (wireType !== 2)
                                        break;
                                    message.source = reader.stringVerify();
                                    message._source = "source";
                                    continue;
                                }
                            case 12: {
                                    if (wireType !== 2)
                                        break;
                                    message.topic = reader.stringVerify();
                                    message._topic = "topic";
                                    continue;
                                }
                            case 13: {
                                    if (wireType !== 2)
                                        break;
                                    if (!(message.metadata && message.metadata.length))
                                        message.metadata = [];
                                    message.metadata.push($root.zlink.framework.channel.v1.MetadataEntry.decode(reader, reader.size(), $undefined, _depth + 1));
                                    continue;
                                }
                            }
                            reader.skipType(wireType, _depth, tag);
                            if (!reader.discardUnknown) {
                                $util.makeProp(message, "$unknowns", false);
                                (message.$unknowns || (message.$unknowns = [])).push(reader.raw(start, reader.pos));
                            }
                        }
                        if (length !== $undefined) {
                            if (reader.pos !== end)
                                throw $RangeError("index out of range");
                            reader.len = length;
                        }
                        if (_end !== $undefined)
                            throw $Error("missing end group");
                        return message;
                    };

                    /**
                     * Decodes a ChannelHeader message from the specified reader or buffer, length delimited.
                     * @function decodeDelimited
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @static
                     * @param {$protobuf.Reader|Uint8Array} reader Reader or buffer to decode from
                     * @returns {zlink.framework.channel.v1.ChannelHeader & zlink.framework.channel.v1.ChannelHeader.$Shape} ChannelHeader
                     * @throws {Error} If the payload is not a reader or valid buffer
                     * @throws {$protobuf.util.ProtocolError} If required fields are missing
                     */
                    ChannelHeader.decodeDelimited = function(reader) {
                        if (!(reader instanceof $Reader))
                            reader = new $Reader(reader);
                        return this.decode(reader, reader.size());
                    };

                    /**
                     * Verifies a ChannelHeader message.
                     * @function verify
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @static
                     * @param {Object.<string,*>} message Plain object to verify
                     * @returns {string|null} `null` if valid, otherwise the reason why it is not
                     */
                    ChannelHeader.verify = function (message, _depth) {
                        if (typeof message !== "object" || message === null)
                            return "object expected";
                        if (_depth === $undefined)
                            _depth = 0;
                        if (_depth > $util.recursionLimit)
                            return "max depth exceeded";
                        var properties = {};
                        if (message.kind != null && $Object.hasOwnProperty.call(message, "kind"))
                            if (typeof message.kind !== "number" || (message.kind | 0) !== message.kind)
                                return "kind: enum value expected";
                        if (message.channelName != null && $Object.hasOwnProperty.call(message, "channelName"))
                            if (!$util.isString(message.channelName))
                                return "channelName: string expected";
                        if (message.messageName != null && $Object.hasOwnProperty.call(message, "messageName"))
                            if (!$util.isString(message.messageName))
                                return "messageName: string expected";
                        if (message.contentType != null && $Object.hasOwnProperty.call(message, "contentType"))
                            if (!$util.isString(message.contentType))
                                return "contentType: string expected";
                        if (message.correlationId != null && $Object.hasOwnProperty.call(message, "correlationId")) {
                            properties._correlationId = 1;
                            if (!$util.isString(message.correlationId))
                                return "correlationId: string expected";
                        }
                        if (message.deadline != null && $Object.hasOwnProperty.call(message, "deadline")) {
                            properties._deadline = 1;
                            if (!$util.isString(message.deadline))
                                return "deadline: string expected";
                        }
                        if (message.errorCode != null && $Object.hasOwnProperty.call(message, "errorCode")) {
                            properties._errorCode = 1;
                            if (!$util.isString(message.errorCode))
                                return "errorCode: string expected";
                        }
                        if (message.errorMessage != null && $Object.hasOwnProperty.call(message, "errorMessage")) {
                            properties._errorMessage = 1;
                            if (!$util.isString(message.errorMessage))
                                return "errorMessage: string expected";
                        }
                        if (message.flowId != null && $Object.hasOwnProperty.call(message, "flowId")) {
                            properties._flowId = 1;
                            if (!$util.isString(message.flowId))
                                return "flowId: string expected";
                        }
                        if (message.flowOrigin != null && $Object.hasOwnProperty.call(message, "flowOrigin")) {
                            properties._flowOrigin = 1;
                            if (typeof message.flowOrigin !== "number" || (message.flowOrigin | 0) !== message.flowOrigin)
                                return "flowOrigin: enum value expected";
                        }
                        if (message.source != null && $Object.hasOwnProperty.call(message, "source")) {
                            properties._source = 1;
                            if (!$util.isString(message.source))
                                return "source: string expected";
                        }
                        if (message.topic != null && $Object.hasOwnProperty.call(message, "topic")) {
                            properties._topic = 1;
                            if (!$util.isString(message.topic))
                                return "topic: string expected";
                        }
                        if (message.metadata != null && $Object.hasOwnProperty.call(message, "metadata")) {
                            if (!$Array.isArray(message.metadata))
                                return "metadata: array expected";
                            for (var i = 0; i < message.metadata.length; ++i) {
                                var error = $root.zlink.framework.channel.v1.MetadataEntry.verify(message.metadata[i], _depth + 1);
                                if (error)
                                    return "metadata." + error;
                            }
                        }
                        return null;
                    };

                    /**
                     * Creates a ChannelHeader message from a plain object. Also converts values to their respective internal types.
                     * @function fromObject
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @static
                     * @param {Object.<string,*>} object Plain object
                     * @returns {zlink.framework.channel.v1.ChannelHeader} ChannelHeader
                     */
                    ChannelHeader.fromObject = function (object, _depth) {
                        if (object instanceof $root.zlink.framework.channel.v1.ChannelHeader)
                            return object;
                        if (!$util.isObject(object))
                            throw $TypeError(".zlink.framework.channel.v1.ChannelHeader: object expected");
                        if (_depth === $undefined)
                            _depth = 0;
                        if (_depth > $util.recursionLimit)
                            throw $Error("max depth exceeded");
                        var message = new $root.zlink.framework.channel.v1.ChannelHeader();
                        if (object.kind !== 0 && (typeof object.kind !== "string" || $root.zlink.framework.channel.v1.ChannelKind[object.kind] !== 0))
                            switch (object.kind) {
                            case "CHANNEL_KIND_UNSPECIFIED":
                            case 0:
                                message.kind = 0;
                                break;
                            case "REQUEST":
                            case 1:
                                message.kind = 1;
                                break;
                            case "RESPONSE":
                            case 2:
                                message.kind = 2;
                                break;
                            case "COMMAND":
                            case 3:
                                message.kind = 3;
                                break;
                            case "PUBLISH":
                            case 4:
                                message.kind = 4;
                                break;
                            case "ERROR":
                            case 5:
                                message.kind = 5;
                                break;
                            default:
                                if (typeof object.kind === "number" && (object.kind | 0) === object.kind)
                                    message.kind = object.kind;
                            }
                        if (object.channelName != null)
                            if (typeof object.channelName !== "string" || object.channelName.length)
                                message.channelName = $String(object.channelName);
                        if (object.messageName != null)
                            if (typeof object.messageName !== "string" || object.messageName.length)
                                message.messageName = $String(object.messageName);
                        if (object.contentType != null)
                            if (typeof object.contentType !== "string" || object.contentType.length)
                                message.contentType = $String(object.contentType);
                        if (object.correlationId != null)
                            message.correlationId = $String(object.correlationId);
                        if (object.deadline != null)
                            message.deadline = $String(object.deadline);
                        if (object.errorCode != null)
                            message.errorCode = $String(object.errorCode);
                        if (object.errorMessage != null)
                            message.errorMessage = $String(object.errorMessage);
                        if (object.flowId != null)
                            message.flowId = $String(object.flowId);
                        switch (object.flowOrigin) {
                        case "FLOW_ORIGIN_UNSPECIFIED":
                        case 0:
                            message.flowOrigin = 0;
                            break;
                        case "INBOUND":
                        case 1:
                            message.flowOrigin = 1;
                            break;
                        case "TIMER":
                        case 2:
                            message.flowOrigin = 2;
                            break;
                        case "APPLICATION":
                        case 3:
                            message.flowOrigin = 3;
                            break;
                        case "LIFECYCLE":
                        case 4:
                            message.flowOrigin = 4;
                            break;
                        default:
                            if (typeof object.flowOrigin === "number" && (object.flowOrigin | 0) === object.flowOrigin)
                                message.flowOrigin = object.flowOrigin;
                        }
                        if (object.source != null)
                            message.source = $String(object.source);
                        if (object.topic != null)
                            message.topic = $String(object.topic);
                        if (object.metadata) {
                            if (!$Array.isArray(object.metadata))
                                throw $TypeError(".zlink.framework.channel.v1.ChannelHeader.metadata: array expected");
                            message.metadata = $Array(object.metadata.length);
                            for (var i = 0; i < object.metadata.length; ++i) {
                                if (!$util.isObject(object.metadata[i]))
                                    throw $TypeError(".zlink.framework.channel.v1.ChannelHeader.metadata: object expected");
                                message.metadata[i] = $root.zlink.framework.channel.v1.MetadataEntry.fromObject(object.metadata[i], _depth + 1);
                            }
                        }
                        return message;
                    };

                    /**
                     * Creates a plain object from a ChannelHeader message. Also converts values to other types if specified.
                     * @function toObject
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @static
                     * @param {zlink.framework.channel.v1.ChannelHeader} message ChannelHeader
                     * @param {$protobuf.IConversionOptions} [options] Conversion options
                     * @returns {Object.<string,*>} Plain object
                     */
                    ChannelHeader.toObject = function (message, options, _depth) {
                        if (!options)
                            options = {};
                        if (_depth === $undefined)
                            _depth = 0;
                        if (_depth > $util.recursionLimit)
                            throw $Error("max depth exceeded");
                        var object = {};
                        if (options.arrays || options.defaults)
                            object.metadata = [];
                        if (options.defaults) {
                            object.kind = options.enums === $String ? "CHANNEL_KIND_UNSPECIFIED" : 0;
                            object.channelName = "";
                            object.messageName = "";
                            object.contentType = "";
                        }
                        if (message.kind != null && $Object.hasOwnProperty.call(message, "kind"))
                            object.kind = options.enums === $String ? $root.zlink.framework.channel.v1.ChannelKind[message.kind] === $undefined ? message.kind : $root.zlink.framework.channel.v1.ChannelKind[message.kind] : message.kind;
                        if (message.channelName != null && $Object.hasOwnProperty.call(message, "channelName"))
                            object.channelName = message.channelName;
                        if (message.messageName != null && $Object.hasOwnProperty.call(message, "messageName"))
                            object.messageName = message.messageName;
                        if (message.contentType != null && $Object.hasOwnProperty.call(message, "contentType"))
                            object.contentType = message.contentType;
                        if (message.correlationId != null && $Object.hasOwnProperty.call(message, "correlationId"))
                            object.correlationId = message.correlationId;
                        if (message.deadline != null && $Object.hasOwnProperty.call(message, "deadline"))
                            object.deadline = message.deadline;
                        if (message.errorCode != null && $Object.hasOwnProperty.call(message, "errorCode"))
                            object.errorCode = message.errorCode;
                        if (message.errorMessage != null && $Object.hasOwnProperty.call(message, "errorMessage"))
                            object.errorMessage = message.errorMessage;
                        if (message.flowId != null && $Object.hasOwnProperty.call(message, "flowId"))
                            object.flowId = message.flowId;
                        if (message.flowOrigin != null && $Object.hasOwnProperty.call(message, "flowOrigin"))
                            object.flowOrigin = options.enums === $String ? $root.zlink.framework.channel.v1.FlowOrigin[message.flowOrigin] === $undefined ? message.flowOrigin : $root.zlink.framework.channel.v1.FlowOrigin[message.flowOrigin] : message.flowOrigin;
                        if (message.source != null && $Object.hasOwnProperty.call(message, "source"))
                            object.source = message.source;
                        if (message.topic != null && $Object.hasOwnProperty.call(message, "topic"))
                            object.topic = message.topic;
                        if (message.metadata && message.metadata.length) {
                            object.metadata = $Array(message.metadata.length);
                            for (var j = 0; j < message.metadata.length; ++j)
                                object.metadata[j] = $root.zlink.framework.channel.v1.MetadataEntry.toObject(message.metadata[j], options, _depth + 1);
                        }
                        return object;
                    };

                    /**
                     * Converts this ChannelHeader to JSON.
                     * @function toJSON
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @instance
                     * @returns {Object.<string,*>} JSON object
                     */
                    ChannelHeader.prototype.toJSON = function() {
                        return ChannelHeader.toObject(this, $protobuf.util.toJSONOptions);
                    };

                    /**
                     * Gets the type url for ChannelHeader
                     * @function getTypeUrl
                     * @memberof zlink.framework.channel.v1.ChannelHeader
                     * @static
                     * @param {string} [prefix] Custom type url prefix, defaults to `"type.googleapis.com"`
                     * @returns {string} The type url
                     */
                    ChannelHeader.getTypeUrl = function(prefix) {
                        if (prefix === $undefined)
                            prefix = "type.googleapis.com";
                        return prefix + "/zlink.framework.channel.v1.ChannelHeader";
                    };

                    return ChannelHeader;
                })();

                return v1;
            })();

            return channel;
        })();

        return framework;
    })();

    return zlink;
})();

module.exports = $root;
