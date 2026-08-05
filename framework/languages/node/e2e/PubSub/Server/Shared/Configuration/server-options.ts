export function objectValues(value: unknown): Record<string, unknown> {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) throw new Error("Configuration section 'e2e' must be an object.");
  return value as Record<string, unknown>;
}

export function optional(values: Record<string, unknown>, name: string): string | undefined {
  const value = values[name];
  return typeof value === 'string' && value.length > 0 ? value : undefined;
}

export function required(values: Record<string, unknown>, name: string): string {
  const value = optional(values, name);
  if (value === undefined) throw new Error(`Configuration value 'e2e.${name}' must be a non-empty string.`);
  return value;
}
