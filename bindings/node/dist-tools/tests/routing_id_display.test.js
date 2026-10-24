"use strict";
// SPDX-License-Identifier: MPL-2.0
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const { RoutingId } = require('@zlink-systems/zlink');
const displayCases = [
    ['6465616c65722d31', 'dealer-1'],
    ['ed959ceab880', '한글'],
    ['f09f9880', '😀'],
    ['efbfbd', '�'],
    ['c2a0', '\u00a0'],
    ['c280', 'hex:c280'],
    ['c29f', 'hex:c29f'],
    ['7f', 'hex:7f'],
    ['00', 'hex:00'],
    ['1f', 'hex:1f'],
    ['80', 'hex:80'],
    ['c080', 'hex:c080'],
    ['eda080', 'hex:eda080'],
    ['e282', 'hex:e282'],
    ['f4908080', '4103110784'],
    ['00000017', '23'],
    ['000102030405060708090a0b0c0d0e0f', '00010203-0405-0607-0809-0a0b0c0d0e0f'],
];
(0, node_test_1.default)('routing id display preserves printable UTF-8 and binary fallback order', () => {
    for (const [hex, expected] of displayCases) {
        strict_1.default.equal(RoutingId.fromHex(hex).toString(), expected, hex);
    }
});
(0, node_test_1.default)('routing id display does not allocate byte storage to validate UTF-8', () => {
    const ids = displayCases.map(([hex]) => RoutingId.fromHex(hex));
    const originalFrom = Buffer.from;
    let allocations = 0;
    let bytes = 0;
    const actual = [];
    Buffer.from = ((...args) => {
        const buffer = Reflect.apply(originalFrom, Buffer, args);
        allocations += 1;
        bytes += buffer.length;
        return buffer;
    });
    try {
        for (const id of ids)
            actual.push(id.toString());
    }
    finally {
        Buffer.from = originalFrom;
    }
    strict_1.default.deepEqual(actual, displayCases.map(([, expected]) => expected));
    strict_1.default.deepEqual({ allocations, bytes }, { allocations: 0, bytes: 0 });
});
(0, node_test_1.default)('routing id byte access preserves independent ownership', () => {
    const input = Buffer.from('peer');
    const id = RoutingId.from(input);
    input.fill(0);
    const first = id.toBytes();
    const second = id.toBytes();
    first.fill(0);
    strict_1.default.equal(second.toString(), 'peer');
    strict_1.default.equal(id.toString(), 'peer');
    strict_1.default.notStrictEqual(first, second);
});
