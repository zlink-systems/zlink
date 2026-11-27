#pragma once
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <string>
#include <string_view>

namespace MyGame
{
inline std::string new_id (std::string_view prefix)
{
    return std::string (prefix) + boost::uuids::to_string (boost::uuids::random_generator{}());
}
}
