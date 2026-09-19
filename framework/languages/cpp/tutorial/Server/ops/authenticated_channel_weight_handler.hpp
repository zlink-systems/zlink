#pragma once

#include "channel_weight_handler.hpp"

#include <optional>
#include <string_view>

// The tutorial has no configuration file, so its documented admin credential
// is deliberately hard-coded in the example instead of being externalized.
class authenticated_channel_weight_handler_t
{
  public:
    explicit authenticated_channel_weight_handler_t (fw::route_mesh_runtime_options_t &mesh) :
        _weight (mesh)
    {
    }

    fw::http_response_t handle (const fw::http_request_t &request)
    {
        const auto authorization = find_header (request, "authorization");
        if (!authorization || *authorization != expected_authorization)
            return fw::http_response_t{401, ""}.header (
              "www-authenticate", "Basic realm=\"tutorial-admin\"");

        return _weight.handle (request);
    }

  private:
    static constexpr std::string_view admin_user = "ops";
    static constexpr std::string_view admin_password = "tutorial-admin";
    static constexpr std::string_view expected_authorization =
      "Basic b3BzOnR1dG9yaWFsLWFkbWlu";

    static bool same_name (std::string_view left, std::string_view right)
    {
        if (left.size () != right.size ())
            return false;
        for (std::size_t i = 0; i < left.size (); ++i) {
            const auto lower = [] (char value) {
                return value >= 'A' && value <= 'Z' ? static_cast<char> (value - 'A' + 'a') : value;
            };
            if (lower (left[i]) != lower (right[i]))
                return false;
        }
        return true;
    }

    static std::optional<std::string_view> find_header (const fw::http_request_t &request,
                                                        std::string_view name)
    {
        for (const auto &[key, value] : request.headers) {
            if (same_name (key, name))
                return value;
        }
        return std::nullopt;
    }

    channel_weight_handler_t _weight;
};
