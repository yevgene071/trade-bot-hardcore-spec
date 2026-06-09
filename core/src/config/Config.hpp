#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <vector>

namespace trade_bot {

class ConfigError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Config {
  public:
    Config() = default;
    explicit Config(toml::table data) : data_(std::move(data)) {}

    void load_file(const std::string& path);
    static void load(const std::string& path);
    static Config& instance();

    /**
     * @brief Strict validation of required configuration keys.
     * Throws ConfigError if any required key is missing or has wrong type.
     */
    void validate();

    template <typename T> T get_val(std::string_view dotted_path) const {
        auto node = find_node_internal(dotted_path);
        if (!node) {
            throw ConfigError("Config key not found: " + std::string(dotted_path));
        }

        if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            if (auto arr = node.as_array()) {
                std::vector<std::string> result;
                result.reserve(arr->size());
                for (auto& el : *arr) {
                    if (auto val = el.value<std::string>()) {
                        result.push_back(*val);
                    } else {
                        throw ConfigError("Config array element type mismatch at: " +
                                          std::string(dotted_path));
                    }
                }
                return result;
            }
            throw ConfigError("Config key is not an array: " + std::string(dotted_path));
        } else if constexpr (std::is_same_v<T, std::vector<double>>) {
            if (auto arr = node.as_array()) {
                std::vector<double> result;
                result.reserve(arr->size());
                for (auto& el : *arr) {
                    if (auto val = el.value<double>()) {
                        result.push_back(*val);
                    } else if (auto int_val = el.value<int64_t>()) {
                        result.push_back(static_cast<double>(*int_val));
                    } else {
                        throw ConfigError("Config array element type mismatch at: " +
                                          std::string(dotted_path));
                    }
                }
                return result;
            }
            throw ConfigError("Config key is not an array: " + std::string(dotted_path));
        } else {
            auto val = node.value<T>();
            if (!val) {
                throw ConfigError("Config key type mismatch: " + std::string(dotted_path));
            }
            return *val;
        }
    }

    template <typename T> static T get(std::string_view dotted_path) {
        return instance().get_val<T>(dotted_path);
    }

    template <typename T> T get_val_or(std::string_view dotted_path, const T& default_value) const {
        if (!has_val(dotted_path)) {
            return default_value;
        }
        try {
            return get_val<T>(dotted_path);
        } catch (const ConfigError&) {
            return default_value;
        }
    }

    template <typename T> static T get_or(std::string_view dotted_path, const T& default_value) {
        return instance().get_val_or<T>(dotted_path, default_value);
    }

    bool has_val(std::string_view dotted_path) const {
        return static_cast<bool>(find_node_internal(dotted_path));
    }

    static bool has(std::string_view dotted_path) {
        return instance().has_val(dotted_path);
    }

  private:
    toml::node_view<const toml::node> find_node_internal(std::string_view dotted_path) const;
    toml::table data_;
    static std::unique_ptr<Config> s_instance;
};

} // namespace trade_bot
