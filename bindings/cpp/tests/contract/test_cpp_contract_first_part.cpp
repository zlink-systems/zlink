/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink.hpp>

#include <cassert>
#include <cerrno>

namespace
{

template <typename Result>
void test_first_part ()
{
    Result result;

    try {
        (void) result.first_part ();
        assert (false && "first_part() must reject an empty result");
    }
    catch (const zlink::recv_error_t &error) {
        assert (error.result () == zlink::recv_result_t::invalid_state);
        assert (error.internal_errno () == EINVAL);
    }

    auto &parts = result.parts ();
    parts.emplace_back ();
    assert (&result.first_part () == &parts.front ());

    parts.push_back (zlink::message_t::from ("second"));
    assert (&result.first_part () == &parts.front ());
}

} // namespace

int main ()
{
    test_first_part<zlink::received_t> ();
    test_first_part<zlink::topic_message_t> ();
}
