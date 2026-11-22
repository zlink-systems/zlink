/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <string_view>

namespace zlink::detail::json_profile
{

// Framework and Stream Connector each compile the engine under their own
// namespace so a consumer of both links two distinct symbols. A target exports
// ZLINK_JSON_PROFILE_ENGINE_FRAMEWORK or _CONNECTOR; the Framework name wins when
// both arrive, and ZLINK_JSON_PROFILE_ENGINE_NS set first (by the engine's own
// library) is kept.
#ifndef ZLINK_JSON_PROFILE_ENGINE_NS
#if defined(ZLINK_JSON_PROFILE_ENGINE_CONNECTOR) && !defined(ZLINK_JSON_PROFILE_ENGINE_FRAMEWORK)
#define ZLINK_JSON_PROFILE_ENGINE_NS connector_engine
#else
#define ZLINK_JSON_PROFILE_ENGINE_NS framework_engine
#endif
#endif

class typed_sax;

namespace ZLINK_JSON_PROFILE_ENGINE_NS
{
bool read_document (std::string_view input, typed_sax &handler);
}
using ZLINK_JSON_PROFILE_ENGINE_NS::read_document;

} // namespace zlink::detail::json_profile
