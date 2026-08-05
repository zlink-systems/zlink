/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <concepts>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <zlink/framework/contracts/errors/error.hpp>

namespace zlink::framework
{

enum class optional_t
{
    no = 0,
    yes = 1
};

class configuration_model_t
{
  public:
    configuration_model_t &set (std::string key, std::string value);

    bool contains (std::string_view key) const;

    bool has_section (std::string_view key) const;

    std::optional<std::string> get (std::string_view key) const;

  private:
    std::map<std::string, std::string> _values;
};

class configuration_section_t
{
  public:
    configuration_section_t (const configuration_model_t &model, std::string prefix) :
        _model (&model), _prefix (std::move (prefix))
    {
    }

    std::string key () const { return _prefix; }

    bool contains (std::string_view key) const { return _model->contains (join (key)); }

    std::optional<std::string> get (std::string_view key) const { return _model->get (join (key)); }

    std::string require (std::string_view key) const
    {
        auto value = get (key);
        if (!value) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "required configuration value missing: " + join (key));
        }
        return *value;
    }

  private:
    std::string join (std::string_view key) const
    {
        if (_prefix.empty ()) {
            return std::string (key);
        }
        if (key.empty ()) {
            return _prefix;
        }
        return _prefix + "." + std::string (key);
    }

    const configuration_model_t *_model;
    std::string _prefix;
};

template <typename T>
concept configuration_bindable = requires (const configuration_section_t &section)
{
    {
        T::bind (section)
    } -> std::same_as<T>;
};

class config_builder_t
{
  public:
    configuration_model_t &model () noexcept { return _model; }
    const configuration_model_t &model () const noexcept { return _model; }

    config_builder_t &load_json (std::string path);

    config_builder_t &load_json (std::string path, optional_t optional);

    config_builder_t &load_env (std::string prefix);

    config_builder_t &load_cli (int argc, char **argv);

    config_builder_t &use_environment (std::string name);

    std::string environment () const;

    bool is_environment (std::string_view name) const;

    configuration_section_t section (std::string prefix) const
    {
        return configuration_section_t (_model, std::move (prefix));
    }

    template <configuration_bindable T> std::optional<T> bind (std::string prefix) const
    {
        if (!_model.has_section (prefix)) {
            return std::nullopt;
        }
        return T::bind (section (std::move (prefix)));
    }

    template <configuration_bindable T> T bind_required (std::string prefix) const
    {
        auto bound = bind<T> (prefix);
        if (!bound) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "required configuration section missing: " + prefix);
        }
        return *bound;
    }

  private:
    configuration_model_t _model;
};

} // namespace zlink::framework
