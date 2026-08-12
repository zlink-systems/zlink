import { ZLinkConfigurationException } from './ConfigurationException';

const mediaTypePunctuation = new Set(
  [..."!#$%&'*+-.^_`|~"].map((value) => value.charCodeAt(0))
);

export function normalizeCodecContentType(contentType: string): string {
  if (typeof contentType !== 'string') {
    throw invalidContentType();
  }
  let first = 0;
  let last = contentType.length;
  while (first < last && isOuterWhitespace(contentType.charCodeAt(first))) first += 1;
  while (last > first && isOuterWhitespace(contentType.charCodeAt(last - 1))) last -= 1;

  const value = contentType.slice(first, last);
  let slash = -1;
  let normalized = '';
  for (let index = 0; index < value.length; index += 1) {
    const code = value.charCodeAt(index);
    if (code === 0x2f) {
      if (slash !== -1 || index === 0 || index === value.length - 1) {
        throw invalidContentType();
      }
      slash = index;
      normalized += '/';
      continue;
    }
    if (!isTokenCharacter(code)) throw invalidContentType();
    normalized += code >= 0x41 && code <= 0x5a
      ? String.fromCharCode(code + 0x20)
      : value[index]!;
  }
  if (slash === -1) throw invalidContentType();
  return normalized;
}

export function isCanonicalCodecContentType(contentType: string): boolean {
  if (typeof contentType !== 'string' || contentType.length === 0) return false;
  let slash = -1;
  for (let index = 0; index < contentType.length; index += 1) {
    const code = contentType.charCodeAt(index);
    if (code === 0x2f) {
      if (slash !== -1 || index === 0 || index === contentType.length - 1) return false;
      slash = index;
      continue;
    }
    if (!isTokenCharacter(code) || (code >= 0x41 && code <= 0x5a)) return false;
  }
  return slash !== -1;
}

function isOuterWhitespace(code: number): boolean {
  return code === 0x20 || code === 0x09;
}

function isTokenCharacter(code: number): boolean {
  return (code >= 0x30 && code <= 0x39)
    || (code >= 0x41 && code <= 0x5a)
    || (code >= 0x61 && code <= 0x7a)
    || mediaTypePunctuation.has(code);
}

function invalidContentType(): ZLinkConfigurationException {
  return new ZLinkConfigurationException(
    'Codec content type must be a parameter-free ASCII type/subtype.'
  );
}
