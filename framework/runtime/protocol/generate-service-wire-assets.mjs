#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.dirname(fileURLToPath(import.meta.url));
const schemaPath = path.join(root, "service-wire-v1.schema.json");
const check = process.argv.includes("--check");
const schema = JSON.parse(fs.readFileSync(schemaPath, "utf8"));

const snake = (value) => value.replace(/([a-z0-9])([A-Z])/g, "$1_$2").toUpperCase();
const pascal = (value) => value[0].toUpperCase() + value.slice(1);
const commands = [...schema.commands].sort((a, b) => a.id - b.id);
const flags = [...schema.flags].sort((a, b) => a.bit - b.bit);
const numericBounds = schema.bounds.filter((entry) => Number.isSafeInteger(entry.value));
const frameworkErrors = [...schema.types.find((entry) => entry.name === "framework-error-code").values]
  .sort((a, b) => a.value - b.value);
const terminalFailureIntegrity = schema.semanticConstraints.find(
  (entry) => entry.kind === "terminal-failure-integrity",
);
const frameworkMultipartV1Profile = schema.frameworkMultipartV1Profile;

function head(command, flagsValue = 0) {
  return [...schema.protocol.magic, schema.protocol.wireMajor, command, flagsValue];
}

function u64(value) {
  let current = BigInt(value);
  const bytes = Array(8).fill(0);
  for (let index = 7; index >= 0; --index) {
    bytes[index] = Number(current & 0xffn);
    current >>= 8n;
  }
  return bytes;
}

const probeId = 0x0102030405060708n;
const probe = [...head(5), ...u64(probeId)];
const ack = [...head(6), ...u64(probeId)];
const fixtures = {
  schema: "zlink-service-wire-decoder-fixtures-v1",
  wireMajor: schema.protocol.wireMajor,
  probeId: probeId.toString(),
  canonical: [
    { name: "livenessProbe", commandId: 5, bytes: probe },
    { name: "livenessAck", commandId: 6, bytes: ack, echoesProbeIdFrom: "livenessProbe" },
  ],
  malformed: [
    { name: "wrongMagic", bytes: [0, schema.protocol.magic[1], ...probe.slice(2)], error: "invalid-magic" },
    { name: "unknownCommand", bytes: [...head(7), ...u64(probeId)], error: "unknown-command" },
    { name: "forbiddenFlag", bytes: [...head(5, 1), ...u64(probeId)], error: "forbidden-flag" },
    { name: "zeroProbeId", bytes: [...head(5), ...u64(0n)], error: "invalid-field" },
    { name: "truncatedProbeId", bytes: probe.slice(0, -1), error: "truncated-field" },
    { name: "trailingByte", bytes: [...probe, 0], error: "trailing-byte" },
  ],
  frameworkErrors: {
    publicMapping: terminalFailureIntegrity.publicMapping,
    canonical: frameworkErrors.map((entry) => ({
      name: entry.name,
      wireValue: entry.value,
      publicValue: entry.name === "none" ? null : entry.value - 1,
    })),
    reservedWireValues: terminalFailureIntegrity.reservedWireValues,
    malformed: [
      {
        name: "reservedPublicOnlyFirst",
        wireValue: terminalFailureIntegrity.reservedWireValues.first,
        error: "unknown-framework-error",
      },
      {
        name: "reservedPublicOnlyLast",
        wireValue: terminalFailureIntegrity.reservedWireValues.last,
        error: "unknown-framework-error",
      },
      {
        name: "unknownAfterRelocationDataLost",
        wireValue: frameworkErrors.at(-1).value + 1,
        error: "unknown-framework-error",
      },
    ],
  },
};

const cpp = `// Generated from service-wire-v1.schema.json. Do not edit.\n#pragma once\n\n#include <cstddef>\n#include <cstdint>\n\nnamespace zlink::framework::runtime::protocol {\ninline constexpr std::uint8_t magic[] = {${schema.protocol.magic.join(", ")}};\ninline constexpr std::uint8_t wire_major = ${schema.protocol.wireMajor};\ninline constexpr const char required_capability[] = "${schema.protocol.requiredCapability}";\ninline constexpr const char framework_multipart_packet_name[] = "${frameworkMultipartV1Profile.packetName}";\ninline constexpr const char framework_multipart_content_type[] = "${frameworkMultipartV1Profile.contentType}";\nenum class command : std::uint8_t {\n${commands.map((entry) => `    ${entry.name} = ${entry.id},`).join("\n")}\n};\nenum class flag : std::uint8_t {\n${flags.map((entry) => `    ${entry.name} = ${entry.bit},`).join("\n")}\n};\nenum class framework_error_code : std::uint32_t {\n${frameworkErrors.map((entry) => `    ${entry.name} = ${entry.value},`).join("\n")}\n};\n${numericBounds.map((entry) => `inline constexpr std::uint64_t ${entry.name} = ${entry.value}ULL;`).join("\n")}\n} // namespace zlink::framework::runtime::protocol\n`;

const dotnet = `// Generated from service-wire-v1.schema.json. Do not edit.\nnamespace Systems.Zlink.Framework.Runtime.Protocol;\n\ninternal static class ServiceWireConstants\n{\n    internal const byte Magic0 = ${schema.protocol.magic[0]};\n    internal const byte Magic1 = ${schema.protocol.magic[1]};\n    internal const byte WireMajor = ${schema.protocol.wireMajor};\n    internal const string RequiredCapability = "${schema.protocol.requiredCapability}";\n    internal const string FrameworkMultipartPacketName = "${frameworkMultipartV1Profile.packetName}";\n    internal const string FrameworkMultipartContentType = "${frameworkMultipartV1Profile.contentType}";\n    internal enum Command : byte\n    {\n${commands.map((entry) => `        ${pascal(entry.name)} = ${entry.id},`).join("\n")}\n    }\n    [System.Flags]\n    internal enum Flag : byte\n    {\n        None = 0,\n${flags.map((entry) => `        ${pascal(entry.name)} = ${entry.bit},`).join("\n")}\n    }\n    internal enum FrameworkErrorCode : uint\n    {\n${frameworkErrors.map((entry) => `        ${pascal(entry.name)} = ${entry.value},`).join("\n")}\n    }\n}\n`;

const java = `// Generated from service-wire-v1.schema.json. Do not edit.\npackage systems.zlink.framework.runtime.protocol;\n\npublic final class ServiceWireConstants {\n    public static final int MAGIC_0 = ${schema.protocol.magic[0]};\n    public static final int MAGIC_1 = ${schema.protocol.magic[1]};\n    public static final int WIRE_MAJOR = ${schema.protocol.wireMajor};\n    public static final String REQUIRED_CAPABILITY = "${schema.protocol.requiredCapability}";\n    public static final String FRAMEWORK_MULTIPART_PACKET_NAME = "${frameworkMultipartV1Profile.packetName}";\n    public static final String FRAMEWORK_MULTIPART_CONTENT_TYPE = "${frameworkMultipartV1Profile.contentType}";\n${commands.map((entry) => `    public static final int COMMAND_${snake(entry.name)} = ${entry.id};`).join("\n")}\n${flags.map((entry) => `    public static final int FLAG_${snake(entry.name)} = ${entry.bit};`).join("\n")}\n${frameworkErrors.map((entry) => `    public static final long FRAMEWORK_ERROR_${snake(entry.name)} = ${entry.value}L;`).join("\n")}\n    private ServiceWireConstants() {}\n}\n`;

const node = `// Generated from service-wire-v1.schema.json. Do not edit.\nexport const SERVICE_WIRE_MAGIC = [${schema.protocol.magic.join(", ")}] as const;\nexport const SERVICE_WIRE_MAJOR = ${schema.protocol.wireMajor} as const;\nexport const SERVICE_WIRE_REQUIRED_CAPABILITY = "${schema.protocol.requiredCapability}" as const;\nexport const SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME = "${frameworkMultipartV1Profile.packetName}" as const;\nexport const SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE = "${frameworkMultipartV1Profile.contentType}" as const;\nexport const ServiceWireCommand = {\n${commands.map((entry) => `  ${entry.name}: ${entry.id},`).join("\n")}\n} as const;\nexport const ServiceWireFlag = {\n${flags.map((entry) => `  ${entry.name}: ${entry.bit},`).join("\n")}\n} as const;\nexport const ServiceWireFrameworkErrorCode = {\n${frameworkErrors.map((entry) => `  ${entry.name}: ${entry.value},`).join("\n")}\n} as const;\n`;

const nodeJs = `"use strict";\nObject.defineProperty(exports, "__esModule", { value: true });\nexports.ServiceWireFrameworkErrorCode = exports.ServiceWireFlag = exports.ServiceWireCommand = exports.SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE = exports.SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME = exports.SERVICE_WIRE_REQUIRED_CAPABILITY = exports.SERVICE_WIRE_MAJOR = exports.SERVICE_WIRE_MAGIC = void 0;\n// Generated from service-wire-v1.schema.json. Do not edit.\nexports.SERVICE_WIRE_MAGIC = [${schema.protocol.magic.join(", ")}];\nexports.SERVICE_WIRE_MAJOR = ${schema.protocol.wireMajor};\nexports.SERVICE_WIRE_REQUIRED_CAPABILITY = "${schema.protocol.requiredCapability}";\nexports.SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME = "${frameworkMultipartV1Profile.packetName}";\nexports.SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE = "${frameworkMultipartV1Profile.contentType}";\nexports.ServiceWireCommand = {\n${commands.map((entry) => `    ${entry.name}: ${entry.id},`).join("\n")}\n};\nexports.ServiceWireFlag = {\n${flags.map((entry) => `    ${entry.name}: ${entry.bit},`).join("\n")}\n};\nexports.ServiceWireFrameworkErrorCode = {\n${frameworkErrors.map((entry) => `    ${entry.name}: ${entry.value},`).join("\n")}\n};\n`;

const nodeDeclarations = `export declare const SERVICE_WIRE_MAGIC: readonly [${schema.protocol.magic.join(", ")}];\nexport declare const SERVICE_WIRE_MAJOR: ${schema.protocol.wireMajor};\nexport declare const SERVICE_WIRE_REQUIRED_CAPABILITY: "${schema.protocol.requiredCapability}";\nexport declare const SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME: "${frameworkMultipartV1Profile.packetName}";\nexport declare const SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE: "${frameworkMultipartV1Profile.contentType}";\nexport declare const ServiceWireCommand: {\n${commands.map((entry) => `    readonly ${entry.name}: ${entry.id};`).join("\n")}\n};\nexport declare const ServiceWireFlag: {\n${flags.map((entry) => `    readonly ${entry.name}: ${entry.bit};`).join("\n")}\n};\nexport declare const ServiceWireFrameworkErrorCode: {\n${frameworkErrors.map((entry) => `    readonly ${entry.name}: ${entry.value};`).join("\n")}\n};\n`;

const outputs = new Map([
  [path.join(root, "generated/cpp/service_wire_constants.hpp"), cpp],
  [path.join(root, "generated/dotnet/ServiceWireConstants.g.cs"), dotnet],
  [path.join(root, "generated/jvm/ServiceWireConstants.java"), java],
  [path.join(root, "generated/node/service_wire_constants.ts"), node],
  [path.join(root, "generated/node/service_wire_constants.js"), nodeJs],
  [path.join(root, "generated/node/service_wire_constants.d.ts"), nodeDeclarations],
  [path.join(
    root,
    "../../languages/node/packages/framework/src/runtime/foundation/service-wire-constants.generated.ts",
  ), node],
  [path.join(root, "golden/service-decoder-fixtures-v1.json"), `${JSON.stringify(fixtures, null, 2)}\n`],
]);

let failed = false;
for (const [output, content] of outputs) {
  if (check) {
    if (!fs.existsSync(output) || fs.readFileSync(output, "utf8") !== content) {
      console.error(`generated service wire asset is stale: ${path.relative(root, output)}`);
      failed = true;
    }
    continue;
  }
  fs.mkdirSync(path.dirname(output), { recursive: true });
  fs.writeFileSync(output, content);
}
if (failed) process.exit(1);
const fixtureCount = fixtures.canonical.length + fixtures.malformed.length
  + fixtures.frameworkErrors.canonical.length + fixtures.frameworkErrors.malformed.length;
console.log(`${check ? "verified" : "generated"} service wire assets: commands=${commands.length} flags=${flags.length} fixtures=${fixtureCount}`);
