/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/configuration/configuration.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <utility>

extern char **environ;

namespace
{

void flatten_json (zlink::framework::configuration_model_t &model,
                   const std::string &prefix,
                   const nlohmann::json &value)
{
    if (value.is_object ()) {
        for (auto it = value.begin (); it != value.end (); ++it) {
            const auto key = prefix.empty () ? it.key () : prefix + "." + it.key ();
            flatten_json (model, key, it.value ());
        }
        return;
    }

    if (value.is_string ()) {
        model.set (prefix, value.get<std::string> ());
        return;
    }
    if (value.is_boolean ()) {
        model.set (prefix, value.get<bool> () ? "true" : "false");
        return;
    }
    if (value.is_number_integer ()) {
        model.set (prefix, std::to_string (value.get<long long> ()));
        return;
    }
    if (value.is_number_unsigned ()) {
        model.set (prefix, std::to_string (value.get<unsigned long long> ()));
        return;
    }
    if (value.is_number_float ()) {
        model.set (prefix, std::to_string (value.get<double> ()));
        return;
    }
    if (value.is_null ()) {
        model.set (prefix, "");
    }
}

std::string lower_ascii (std::string value)
{
    std::transform (value.begin (), value.end (), value.begin (),
                    [] (unsigned char ch) { return static_cast<char> (std::tolower (ch)); });
    return value;
}

} // namespace

namespace zlink::framework
{

configuration_model_t &configuration_model_t::set (std::string key, std::string value)
{
    _values[std::move (key)] = std::move (value);
    return *this;
}

bool configuration_model_t::contains (std::string_view key) const
{
    return _values.find (std::string (key)) != _values.end ();
}

bool configuration_model_t::has_section (std::string_view key) const
{
    const auto exact = std::string (key);
    if (contains (exact)) {
        return true;
    }

    const auto prefix = exact.empty () ? std::string () : exact + ".";
    for (const auto &[entry_key, _] : _values) {
        if (prefix.empty () || entry_key.rfind (prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> configuration_model_t::get (std::string_view key) const
{
    const auto found = _values.find (std::string (key));
    if (found == _values.end ()) {
        return std::nullopt;
    }
    return found->second;
}

config_builder_t &config_builder_t::load_json (std::string path)
{
    return load_json (std::move (path), optional_t::no);
}

config_builder_t &config_builder_t::load_json (std::string path, optional_t optional)
{
    _model.set ("config.json.path", path);
    std::ifstream input (path);
    if (!input) {
        if (optional == optional_t::no) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "required configuration file missing: " + path);
        }
        return *this;
    }

    nlohmann::json parsed;
    input >> parsed;
    flatten_json (_model, "", parsed);
    return *this;
}

config_builder_t &config_builder_t::load_env (std::string prefix)
{
    _model.set ("config.env.prefix", prefix);
    if (environ == nullptr) {
        return *this;
    }

    for (char **current = environ; *current != nullptr; ++current) {
        std::string entry{*current};
        if (entry.rfind (prefix, 0) != 0) {
            continue;
        }

        const auto separator = entry.find ('=');
        if (separator == std::string::npos) {
            continue;
        }

        auto key = entry.substr (0, separator);
        key.erase (0, prefix.size ());
        auto canonical_key = key;
        for (std::size_t pos = 0; (pos = canonical_key.find ("__", pos)) != std::string::npos;) {
            canonical_key.replace (pos, 2, ".");
            ++pos;
        }
        auto value = entry.substr (separator + 1);
        _model.set ("env." + key, value);
        _model.set (std::move (canonical_key), std::move (value));
    }
    return *this;
}

config_builder_t &config_builder_t::load_cli (int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        std::string arg{argv[i]};
        if (arg.rfind ("--", 0) != 0) {
            continue;
        }

        arg.erase (0, 2);
        const auto separator = arg.find ('=');
        if (separator == std::string::npos) {
            _model.set ("cli." + arg, "true");
            _model.set (std::move (arg), "true");
            continue;
        }

        auto key = arg.substr (0, separator);
        auto value = arg.substr (separator + 1);
        _model.set ("cli." + key, value);
        _model.set (std::move (key), std::move (value));
    }
    return *this;
}

config_builder_t &config_builder_t::use_environment (std::string name)
{
    _model.set ("environment.name", std::move (name));
    return *this;
}

std::string config_builder_t::environment () const
{
    return _model.get ("environment.name").value_or ("production");
}

bool config_builder_t::is_environment (std::string_view name) const
{
    return lower_ascii (environment ()) == lower_ascii (std::string (name));
}

} // namespace zlink::framework
