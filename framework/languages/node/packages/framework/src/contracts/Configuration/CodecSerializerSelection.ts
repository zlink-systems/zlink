import type { ZLinkMessageTypeSelector } from '../Codecs';
import type { ZLinkMessageSerializer } from '../Codecs';

export interface ZLinkCodecSerializerSelection {
  readonly selector: ZLinkMessageTypeSelector;
  readonly fallback: boolean;
}

const selectionsByRegistry = new WeakMap<
  object,
  ReadonlyMap<string, ZLinkCodecSerializerSelection>
>();

export function rememberCodecSerializerSelections(
  serializers: ReadonlyMap<string, ZLinkMessageSerializer>,
  selections: ReadonlyMap<string, ZLinkCodecSerializerSelection>
): ReadonlyMap<string, ZLinkMessageSerializer> {
  selectionsByRegistry.set(serializers as object, selections);
  return serializers;
}

export function codecSerializerSelectionsOf(
  serializers: ReadonlyMap<string, ZLinkMessageSerializer>
): ReadonlyMap<string, ZLinkCodecSerializerSelection> | undefined {
  return selectionsByRegistry.get(serializers as object);
}

export const matchEveryDeclaredMessageType: ZLinkMessageTypeSelector = () => true;
