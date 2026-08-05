// SPDX-License-Identifier: MPL-2.0

import type { CoreNativeBinding } from './binding_core';
import type { ContextNativeBinding } from './binding_context';
import type { EventingNativeBinding } from './binding_eventing';
import type { SocketNativeBinding } from './binding_socket';

export interface NativeBinding
  extends CoreNativeBinding,
    ContextNativeBinding,
    SocketNativeBinding,
    EventingNativeBinding {}
