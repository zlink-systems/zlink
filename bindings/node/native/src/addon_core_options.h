/* SPDX-License-Identifier: MPL-2.0 */

#pragma once

#include "addon_common_api.h"

int set_socket_option (void *sock, int32_t opt, const void *data, size_t len);
int get_socket_option (void *sock, int32_t opt, void *data, size_t *len);
size_t initial_getopt_buffer_len (int32_t opt);
