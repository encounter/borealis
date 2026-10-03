#pragma once

#include "borealis/config.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

/**
 * Value serialization for borealis::config. Include this in the translation unit that defines
 * vars, and in any custom Codec specialization.
 *
 * Built in: bool, integers (with range), floating point (finite), std::string, enums with a
 * `config_enum_values(E)` table (names or integers), and any type with nlohmann to_json/from_json.
 * Specialize Codec<T> for anything else.
 */
namespace borealis::config {
namespace detail {

bool ascii_iequals(std::string_view lhs, std::string_view rhs) noexcept;
std::optional<bool> parse_bool(std::string_view text) noexcept;
std::optional<std::int64_t> parse_int64(std::string_view text) noexcept;
std::optional<std::uint64_t> parse_uint64(std::string_view text) noexcept;
std::optional<double> parse_double(std::string_view text) noexcept;
nlohmann::json parse_json(std::string_view text);
/** The double nearest the shortest decimal that round-trips the float, so 1.1f saves as 1.1. */
double float_for_json(float value) noexcept;

template <typename T>
T checked_integer(std::int64_t value) {
    if constexpr (std::is_unsigned_v<T>) {
        if (value < 0 || static_cast<std::uint64_t>(value) >
                             static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
        {
            throw InvalidValueError("integer is out of range");
        }
    } else if (value < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
               value > static_cast<std::int64_t>(std::numeric_limits<T>::max()))
    {
        throw InvalidValueError("integer is out of range");
    }
    return static_cast<T>(value);
}

template <typename T>
T checked_integer(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
        throw InvalidValueError("integer is out of range");
    }
    return static_cast<T>(value);
}

template <typename T>
T decode_integer(const nlohmann::json& json) {
    if (json.is_number_unsigned()) {
        return checked_integer<T>(json.get<std::uint64_t>());
    }
    if (json.is_number_integer()) {
        return checked_integer<T>(json.get<std::int64_t>());
    }
    if (json.is_number_float()) {
        // Accept integral floats such as 100.0 from hand-edited files.
        const double value = json.get<double>();
        constexpr double kExactLimit = 9007199254740992.0;  // 2^53
        if (std::isfinite(value) && std::trunc(value) == value && std::fabs(value) <= kExactLimit) {
            return checked_integer<T>(static_cast<std::int64_t>(value));
        }
    }
    throw InvalidValueError("expected an integer");
}

template <typename T>
T parse_integer(std::string_view text) {
    if constexpr (std::is_unsigned_v<T>) {
        if (const auto value = parse_uint64(text)) {
            return checked_integer<T>(*value);
        }
    } else if (const auto value = parse_int64(text)) {
        return checked_integer<T>(*value);
    }
    throw InvalidValueError("expected an integer");
}

template <typename T>
T checked_floating(double value) {
    if (!std::isfinite(value) || value < static_cast<double>(std::numeric_limits<T>::lowest()) ||
        value > static_cast<double>(std::numeric_limits<T>::max()))
    {
        throw InvalidValueError("expected a finite number");
    }
    return static_cast<T>(value);
}

template <typename E>
E enum_from_integer(std::underlying_type_t<E> raw) {
    for (const auto& entry : enum_values<E>().entries) {
        if (static_cast<std::underlying_type_t<E>>(entry.first) == raw) {
            return entry.first;
        }
    }
    throw InvalidValueError("enum value is out of range");
}

}  // namespace detail

template <typename T>
struct Codec {
    static nlohmann::json encode(const T& value) {
        if constexpr (std::is_enum_v<T>) {
            static_assert(NamedEnum<T>, "enum config values need a config_enum_values(E) table");
            if (const auto name = enum_values<T>().name(value)) {
                return std::string(*name);
            }
            return static_cast<std::underlying_type_t<T>>(value);
        } else if constexpr (std::is_same_v<T, float>) {
            return detail::float_for_json(value);
        } else {
            return nlohmann::json(value);
        }
    }

    /** Throws InvalidValueError or nlohmann::json::exception. */
    static T decode(const nlohmann::json& json) {
        if constexpr (std::is_same_v<T, bool>) {
            if (json.is_boolean()) {
                return json.get<bool>();
            }
            if (json.is_number_integer()) {
                const auto value = json.get<std::int64_t>();
                if (value == 0 || value == 1) {
                    return value == 1;
                }
            }
            throw InvalidValueError("expected a boolean");
        } else if constexpr (std::is_integral_v<T>) {
            return detail::decode_integer<T>(json);
        } else if constexpr (std::is_floating_point_v<T>) {
            if (!json.is_number()) {
                throw InvalidValueError("expected a number");
            }
            return detail::checked_floating<T>(json.get<double>());
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (!json.is_string()) {
                throw InvalidValueError("expected a string");
            }
            return json.get<std::string>();
        } else if constexpr (std::is_enum_v<T>) {
            static_assert(NamedEnum<T>, "enum config values need a config_enum_values(E) table");
            if (json.is_string()) {
                if (const auto value = enum_values<T>().value(json.get_ref<const std::string&>())) {
                    return *value;
                }
                throw InvalidValueError("unknown enum name '" + json.get<std::string>() + "'");
            }
            if (json.is_number()) {
                return detail::enum_from_integer<T>(
                    detail::decode_integer<std::underlying_type_t<T>>(json));
            }
            throw InvalidValueError("expected an enum name");
        } else {
            return json.get<T>();
        }
    }

    /** CLI syntax. Throws InvalidValueError. */
    static T parse(std::string_view text) {
        if constexpr (std::is_same_v<T, bool>) {
            if (const auto value = detail::parse_bool(text)) {
                return *value;
            }
            throw InvalidValueError("expected true, false, 1, 0, on, off, yes or no");
        } else if constexpr (std::is_integral_v<T>) {
            return detail::parse_integer<T>(text);
        } else if constexpr (std::is_floating_point_v<T>) {
            if (const auto value = detail::parse_double(text)) {
                return detail::checked_floating<T>(*value);
            }
            throw InvalidValueError("expected a number");
        } else if constexpr (std::is_same_v<T, std::string>) {
            return std::string(text);
        } else if constexpr (std::is_enum_v<T>) {
            static_assert(NamedEnum<T>, "enum config values need a config_enum_values(E) table");
            for (const auto& [value, name] : enum_values<T>().entries) {
                if (detail::ascii_iequals(name, text)) {
                    return value;
                }
            }
            using Underlying = std::underlying_type_t<T>;
            if constexpr (std::is_signed_v<Underlying>) {
                if (const auto raw = detail::parse_int64(text)) {
                    return detail::enum_from_integer<T>(detail::checked_integer<Underlying>(*raw));
                }
            } else if (const auto raw = detail::parse_uint64(text)) {
                return detail::enum_from_integer<T>(detail::checked_integer<Underlying>(*raw));
            }
            throw InvalidValueError("unknown enum name '" + std::string(text) + "'");
        } else {
            try {
                return decode(detail::parse_json(text));
            } catch (const nlohmann::json::exception& e) {
                throw InvalidValueError(e.what());
            }
        }
    }
};

}  // namespace borealis::config
