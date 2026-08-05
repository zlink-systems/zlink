/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/locations/location_key_codec.hpp"

#include <gtest/gtest.h>

namespace
{

TEST (ZLinkFrameworkLocationKeyCodec, MatchesDotNetCanonicalKeyBytes)
{
    using zlink::framework::actor_location_key_t;
    using zlink::framework::route_kind_t;
    using zlink::framework::route_location_key_t;
    using zlink::framework::spot_location_key_t;
    using zlink::framework::runtime::location_key_codec_t;

    EXPECT_EQ ("6:spot-a",
               location_key_codec_t::encode_spot_key (
                 spot_location_key_t{.spot_id = "spot-a"}));

    EXPECT_EQ ("4:play7:actor-1",
               location_key_codec_t::encode_actor_key (
                 actor_location_key_t{.mesh_name = "play", .actor_id = "actor-1"}));

    EXPECT_EQ ("1:113:session:alpha",
               location_key_codec_t::encode_route_key (route_location_key_t{
                 .route_kind = route_kind_t::actor_session, .route_key = "session:alpha"}));
}

} // namespace
