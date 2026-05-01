#pragma once

#include <toml++/toml.hpp>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace trade_bot {

class ConfigError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Config {
public:
    static void load(const std::string& path);
    
    /**
     * @brief Strict validation of required configuration keys.
     * Throws ConfigError if any required key is missing or has wrong type.
     */
    static void validate();

    template <typename T>
    static T get(std::string_view dotted_path) {
        auto node = find_node(dotted_path);
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
                        throw ConfigError("Config array element type mismatch at: " + std::string(dotted_path));
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

    template <typename T>
    static T get_or(std::string_view dotted_path, T default_value) {
        if (!has(dotted_path)) {
            return default_value;
        }
        try {
            return get<T>(dotted_path);
        } catch (const ConfigError&) {
            return default_value;
        }
    }

    static bool has(std::string_view dotted_path) {
        return static_cast<bool>(find_node(dotted_path));
    }

private:
    static toml::node_view<toml::node> find_node(std::string_view dotted_path);
    static toml::table s_data;
};

} // namespace trade_bot
