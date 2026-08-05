// SPDX-License-Identifier: MPL-2.0

const FIXED_C_STRING_MAX_LENGTH = 255;

export function validateCString(
  value: string,
  name: string,
  maxLength = FIXED_C_STRING_MAX_LENGTH
): string {
  if (typeof value !== 'string') {
    throw new TypeError(`${name} must be a string`);
  }
  if (value.includes('\0')) {
    throw new RangeError(`${name} must not contain embedded null characters`);
  }
  if (Buffer.byteLength(value, 'utf8') > maxLength) {
    throw new RangeError(`${name} must be at most ${maxLength} bytes`);
  }
  return value;
}
