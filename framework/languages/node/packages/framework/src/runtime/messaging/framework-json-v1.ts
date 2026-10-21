import * as profile from '@zlink-systems/stream-wire/internal/json';

export const FRAMEWORK_JSON_V1_PROFILE = profile.FRAMEWORK_JSON_V1_PROFILE;
export const stringifyFrameworkJsonV1 = profile.stringifyFrameworkJsonV1;
const DEFAULT_PARSE_OPTIONS = Object.freeze({});

export function parseFrameworkJsonV1(
  text: string,
  options: profile.FrameworkJsonParseOptions = DEFAULT_PARSE_OPTIONS,
  schema?: Parameters<typeof profile.parseFrameworkJsonV1>[2],
  reviver?: Parameters<typeof profile.parseFrameworkJsonV1>[3]
): unknown {
  return profile.parseFrameworkJsonV1(text, options, schema, reviver);
}
export type { FrameworkJsonParseOptions } from '@zlink-systems/stream-wire/internal/json';
