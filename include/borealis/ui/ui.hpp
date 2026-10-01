#pragma once

#include <RmlUi/Core.h>
#include <SDL3/SDL_events.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace borealis::ui {
class Document;

using clock = std::chrono::steady_clock;

enum class NavCommand {
    None,
    Up,
    Down,
    Left,
    Right,
    Next,      // R1
    Previous,  // L1
    Confirm,   // A
    Cancel,    // B
    Menu,      // Back/Minus, or R + Start
};

inline constexpr char kNavCommandEvent[] = "navcommand";

enum class NavSound {
    None,
    Click,              // Button clicked/pressed
    Play,               // "Play" button clicked/pressed
    BindingChanged,     // Input binding changed
    MenuOpen,           // Menu button pressed (open/close menu bar or hide/show the active window)
    MenuClose,          // Menu button pressed (open/close menu bar or hide/show the active window)
    WindowOpen,         // Window opened/closed
    WindowClose,        // Window opened/closed
    TabChanged,         // Window tab changed
    ItemFocus,          // Item within menu focused
    ItemChange,         // Item changed (e.g. number input left/right)
    ItemEnable,         // Item enabled ("On")
    ItemDisable,        // Item disabled ("Off")
    AchievementUnlock,  // Achievement unlocked
    Warning,            // Warning prompt
};

using DocumentScope = uint8_t;

inline constexpr DocumentScope kScopeNone = 0;
inline constexpr DocumentScope kScopeWindow = 1;
inline constexpr DocumentScope kScopeMenuBar = 2;
inline constexpr DocumentScope kScopeUser = 16;

struct Insets {
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;

    bool operator==(const Insets& other) const noexcept {
        return top == other.top && right == other.right && bottom == other.bottom &&
               left == other.left;
    }
};

bool initialize() noexcept;
void shutdown() noexcept;

void handle_event(const SDL_Event& event) noexcept;
void update() noexcept;

// --- Scoped styles TODO move this
bool register_scoped_styles(DocumentScope scope, std::string id, const std::string& rcss) noexcept;
void unregister_scoped_styles(DocumentScope scope, std::string_view id) noexcept;
void apply_scoped_styles(Document& doc) noexcept;

Document& push_document(
    std::unique_ptr<Document> doc, bool show = true, bool passive = false) noexcept;

namespace detail {
Document& pop_to_or_push_document(bool (*matches)(Document&),
    const std::function<std::unique_ptr<Document>()>& create,
    const std::function<void(Document&)>& configure);
}

template <typename T, typename Configure, typename... Args>
T& pop_to_or_push(Configure&& configure, Args&&... args) {
    return static_cast<T&>(detail::pop_to_or_push_document(
        [](Document& document) { return dynamic_cast<T*>(&document) != nullptr; },
        [&]() -> std::unique_ptr<Document> {
            return std::make_unique<T>(std::forward<Args>(args)...);
        },
        [&](Document& document) {
            std::invoke(std::forward<Configure>(configure), static_cast<T&>(document));
        }));
}

void bring_document_to_front(Document& doc) noexcept;
void uncover_top_document() noexcept;
Document* find_document(DocumentScope scope) noexcept;
void close_all_documents() noexcept;
bool any_document_visible() noexcept;
bool is_prelaunch_open() noexcept;
bool game_obscured_below(const Document& doc) noexcept;
Document* top_document() noexcept;

// Defaults to "res/rml/global.rcss"
void set_global_stylesheet(std::string path) noexcept;
const std::string& global_stylesheet() noexcept;

bool load_font(const std::filesystem::path& filename, bool fallback = false) noexcept;

std::filesystem::path resource_path(const std::filesystem::path& filename) noexcept;
std::string escape(std::string_view str) noexcept;
Rml::Element* append(Rml::Element* parent, const Rml::String& tag) noexcept;
Rml::Element* append_text(Rml::Element* parent, const Rml::String& text) noexcept;
Rml::Element* append_text_element(
    Rml::Element* parent, const Rml::String& tag, const Rml::String& text) noexcept;
void clear_children(Rml::Element* parent) noexcept;
void set_text_content(Rml::Element* parent, const Rml::String& text) noexcept;
void set_display(Rml::Element* element, Rml::Style::Display display) noexcept;

// For kNavCommandEvent
NavCommand nav_command(const Rml::Event& event) noexcept;
Insets safe_area_insets(Rml::Context* context) noexcept;

// void push_toast(Toast toast) noexcept;
// std::deque<Toast>& get_toasts() noexcept;
// void show_menu_notification() noexcept;
// bool consume_menu_notification_request() noexcept;

// const char* battery_icon(SDL_PowerState state, int level) noexcept;
// const char* connection_state_icon(SDL_JoystickConnectionState state) noexcept;

// UI scale as a percentage of the display scale; 0 uses the display scale.
void set_user_scale(int percent) noexcept;
void apply_scale() noexcept;

using NavSoundHandler = void (*)(NavSound sound);
void set_nav_sound_handler(NavSoundHandler handler) noexcept;
void play_nav_sound(NavSound sound) noexcept;

}  // namespace borealis::ui
