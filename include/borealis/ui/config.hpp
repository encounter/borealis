#pragma once

#include <borealis/config.hpp>
#include <borealis/ui/bool_button.hpp>
#include <borealis/ui/dropdown_button.hpp>
#include <borealis/ui/number_button.hpp>
#include <borealis/ui/string_button.hpp>

#include <algorithm>
#include <climits>
#include <concepts>
#include <functional>

/**
 * Bindings from config vars to controls. Each fills the props' unset getValue, setValue, and
 * isModified from the var, and disables the control while an overlay locks the var (in
 * addition to any isDisabled the caller supplies).
 *
 *     pane.add_child<BoolButton>(bind(settings.video.vsync, {.key = "Vertical Sync"}));
 */
namespace borealis::ui {
namespace detail {

inline std::function<bool()> config_disabled(
    const config::VarBase& var, std::function<bool()> isDisabled) {
    return [&var, isDisabled = std::move(isDisabled)] {
        return var.locked() || (isDisabled && isDisabled());
    };
}

template <typename Props, typename T>
void bind_common(config::Var<T>& var, Props& props) {
    if (!props.isModified) {
        props.isModified = [&var] { return var.modified(); };
    }
    props.isDisabled = config_disabled(var, std::move(props.isDisabled));
}

}  // namespace detail

inline BoolButton::Props bind(config::Var<bool>& var, BoolButton::Props props = {}) {
    if (!props.getValue) {
        props.getValue = [&var] { return var.get(); };
    }
    if (!props.setValue) {
        props.setValue = [&var](bool value) { var.set(value); };
    }
    detail::bind_common(var, props);
    return props;
}

/** Copies the var's min and max into the props when the var defines them. */
template <std::integral T>
    requires(!std::same_as<T, bool>)
NumberButton::Props bind(config::Var<T>& var, NumberButton::Props props = {}) {
    const auto clampToInt = [](auto value) {
        return static_cast<int>(std::clamp<long long>(static_cast<long long>(value), INT_MIN, INT_MAX));
    };
    if (!props.getValue) {
        props.getValue = [&var, clampToInt] { return clampToInt(var.get()); };
    }
    if (!props.setValue) {
        props.setValue = [&var](int value) { var.set(static_cast<T>(value)); };
    }
    if (const auto& min = var.options().min) {
        props.min = clampToInt(*min);
    }
    if (const auto& max = var.options().max) {
        props.max = clampToInt(*max);
    }
    detail::bind_common(var, props);
    return props;
}

inline StringButton::Props bind(config::Var<std::string>& var, StringButton::Props props = {}) {
    if (!props.getValue) {
        props.getValue = [&var] { return Rml::String{var.get()}; };
    }
    if (!props.setValue) {
        props.setValue = [&var](Rml::String value) { var.set(std::move(value)); };
    }
    detail::bind_common(var, props);
    return props;
}

/** The value is the selected option's index. */
inline DropdownButton::Props bind_dropdown(config::Var<int>& var, DropdownButton::Props props) {
    if (!props.getValue) {
        props.getValue = [&var] { return var.get(); };
    }
    if (!props.setValue) {
        props.setValue = [&var](int index) { var.set(index); };
    }
    detail::bind_common(var, props);
    return props;
}

/** Options are listed in the order of the enum's config_enum_values table. */
template <config::NamedEnum E>
DropdownButton::Props bind_dropdown(config::Var<E>& var, DropdownButton::Props props) {
    static constexpr auto table = config::enum_values<E>();
    if (!props.getValue) {
        props.getValue = [&var] {
            const auto index = table.index(var.get());
            return index ? static_cast<int>(*index) : -1;
        };
    }
    if (!props.setValue) {
        props.setValue = [&var](int index) {
            if (index >= 0 && static_cast<std::size_t>(index) < table.size()) {
                var.set(table.entries[static_cast<std::size_t>(index)].first);
            }
        };
    }
    detail::bind_common(var, props);
    return props;
}

}  // namespace borealis::ui
