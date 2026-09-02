// SPDX-License-Identifier: MPL-2.0

/** One active subscription: a topic filter and whether it is a pattern. */
export interface SubscriptionEntry {
  readonly filter: string;
  readonly isPattern: boolean;
}
