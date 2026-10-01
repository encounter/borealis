#pragma once

#include <aurora/binding.hpp>

namespace borealis::ui::input {

// Dispatched on the focused element with "x" and "y" (dead-zoned, [-1, 1], positive
// right/down) and "dt" for analog navigation.
inline constexpr char kNavAxisEvent[] = "navaxis";

// Dispatched on the root element when a controller connects, disconnects or is
// remapped, with "type" ("connected", "disconnected", "remapped") and "source" (the
// aurora::input::SourceId).
inline constexpr char kControllerChangeEvent[] = "controllerchange";

struct Settings {
    // Defaults to controller Back until a Menu control is bound.
    aurora::binding::ControlId menuControl = aurora::binding::kInvalidControlId;
    bool menuChord = false; // R + Start for Menu
    bool menuTap = false; // Three-finger tap for Menu
};

void apply_settings(const Settings& settings) noexcept;
const Settings& settings() noexcept;

void initialize() noexcept;
void shutdown() noexcept;
void update() noexcept;
void reset() noexcept;

}  // namespace borealis::ui::input
