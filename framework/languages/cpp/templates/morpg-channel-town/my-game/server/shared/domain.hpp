#pragma once
#include <string>
namespace MyGame
{
inline bool blank (const std::string &value)
{
    return value.find_first_not_of (" \t\r\n") == std::string::npos;
}
}
