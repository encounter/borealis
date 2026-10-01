#include <borealis/ui/input.hpp>

#include <borealis/ui/document.hpp>
#include <borealis/ui/ui.hpp>

#include "internal.hpp"

#include <aurora/aurora.h>
#include <aurora/input.hpp>
#include <aurora/pad.hpp>
#include <aurora/rmlui.hpp>

#include <RmlUi/Core.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <utility>
#include <vector>

namespace borealis::ui::input {
namespace {

using aurora::binding::Binding;
using aurora::binding::BindingSet;
using aurora::binding::ControlId;
using aurora::binding::kInvalidControlId;
using aurora::binding::MappingResult;
using aurora::binding::PhysicalInput;
using aurora::input::EventResult;
using aurora::input::InputEvent;
using aurora::input::InputSource;
using aurora::input::SourceId;
using aurora::rmlui::get_context;

constexpr double kRepeatInitialDelay = 0.32;
constexpr double kRepeatStartInterval = 0.12;
constexpr double kRepeatMinInterval = 0.045;
constexpr double kRepeatRampDuration = 1.0;
constexpr float kAxisPressThreshold = 16384.0f / 32767.0f;
constexpr float kAxisReleaseThreshold = 12000.0f / 32767.0f;
constexpr float kNavAxisDeadZone = 0.12f;
constexpr double kMaxNavAxisDt = 0.05;
constexpr int kMenuTapFingerCount = 3;
constexpr float kMenuTapMoveThreshold = 12.0f;
constexpr double kMenuTapMaxDownSpan = 0.18;
constexpr double kMenuTapMaxDuration = 0.55;

double now_seconds() noexcept {
    return static_cast<double>(SDL_GetTicksNS()) / 1000000000.0;
}

void send(NavCommand command) noexcept {
    auto* ctx = get_context();
    if (ctx == nullptr) {
        return;
    }
    Rml::Element* target = nullptr;
    if (command == NavCommand::Menu) {
        auto* doc = top_document();
        target = doc != nullptr ? doc->element() : nullptr;
    } else {
        ctx->ProcessMouseLeave();
        target = ctx->GetFocusElement();
    }
    if (target != nullptr) {
        target->DispatchEvent(
            kNavCommandEvent, {
                                  {"command", Rml::Variant{static_cast<int>(command)}},
                              });
    }
}

struct UiControls {
    ControlId next = kInvalidControlId;
    ControlId previous = kInvalidControlId;
    ControlId menu = kInvalidControlId;
};

const UiControls& ui_controls() noexcept {
    static const UiControls sControls{
        .next = aurora::binding::register_control({
            .name = "borealis.ui.next",
            .kind = aurora::binding::ControlKind::Button,
        }),
        .previous = aurora::binding::register_control({
            .name = "borealis.ui.previous",
            .kind = aurora::binding::ControlKind::Button,
        }),
        .menu = aurora::binding::register_control({
            .name = "borealis.ui.menu",
            .kind = aurora::binding::ControlKind::Button,
        }),
    };
    return sControls;
}

struct Contributor {
    uint64_t context = 0;
    ControlId control = kInvalidControlId;
    int direction = 0;

    auto operator<=>(const Contributor&) const = default;
};

struct ActiveNav {
    NavCommand command = NavCommand::None;
    Contributor contributor;

    auto operator<=>(const ActiveNav&) const = default;
};

using NavSet = std::set<ActiveNav>;

class NavKeys {
public:
    void press(const ActiveNav& nav) noexcept {
        auto& held = mHeld[nav.command];
        if (!held.contributors.insert(nav.contributor).second || held.contributors.size() > 1) {
            return;
        }
        const double now = now_seconds();
        held.pressedAt = now;
        held.nextRepeatAt = now + kRepeatInitialDelay;
        send(nav.command);
    }

    void release(const ActiveNav& nav) noexcept {
        const auto it = mHeld.find(nav.command);
        if (it != mHeld.end() && it->second.contributors.erase(nav.contributor) != 0 &&
            it->second.contributors.empty())
        {
            mHeld.erase(it);
        }
    }

    void update(double now) noexcept {
        std::vector<NavCommand> commands;
        for (const auto command : mHeld | std::views::keys) {
            commands.push_back(command);
        }
        for (const auto command : commands) {
            const auto it = mHeld.find(command);
            if (it == mHeld.end() || !repeats(command) || now < it->second.nextRepeatAt) {
                continue;
            }
            it->second.nextRepeatAt = now + repeat_interval(now - it->second.pressedAt);
            send(command);
        }
    }

    void clear() noexcept { mHeld.clear(); }

private:
    struct Held {
        std::set<Contributor> contributors;
        double pressedAt = 0.0;
        double nextRepeatAt = 0.0;
    };

    static bool repeats(NavCommand command) noexcept {
        switch (command) {
        case NavCommand::Up:
        case NavCommand::Down:
        case NavCommand::Left:
        case NavCommand::Right:
        case NavCommand::Next:
        case NavCommand::Previous:
            return true;
        default:
            return false;
        }
    }

    static double repeat_interval(double heldFor) noexcept {
        const double ramp = std::clamp(heldFor / kRepeatRampDuration, 0.0, 1.0);
        return kRepeatStartInterval + (kRepeatMinInterval - kRepeatStartInterval) * ramp;
    }

    std::map<NavCommand, Held> mHeld;
};

Settings sSettings;
NavKeys sNavKeys;
uint64_t sNextContext = 1;

bool is_gamepad(const PhysicalInput& input) noexcept {
    return input.control.is<PhysicalInput::GamepadButton>() ||
           input.control.is<PhysicalInput::GamepadAxis>();
}

bool is_axis(ControlId control) noexcept {
    const auto* descriptor = aurora::binding::describe_control(control);
    return descriptor != nullptr && descriptor->kind == aurora::binding::ControlKind::Axis;
}

// A control with a navigation meaning
struct NavMapping {
    ControlId control = kInvalidControlId;
    NavCommand positive = NavCommand::None;
    NavCommand negative = NavCommand::None;
};

// Navigation from one binding set
struct NavContext {
    uint64_t id = sNextContext++;
    std::shared_ptr<const BindingSet> base;
    aurora::binding::State state;
    std::vector<NavMapping> mappings;
    NavSet active;
    bool chordConsumed = false;

    [[nodiscard]] bool references(SourceId source) const noexcept {
        return base != nullptr && base->references(source);
    }

    [[nodiscard]] bool navigates(ControlId control) const noexcept {
        return std::ranges::find(mappings, control, &NavMapping::control) != mappings.end();
    }

    // Whether the event is bound to navigation
    [[nodiscard]] bool navigates(const MappingResult& result) const noexcept {
        return std::ranges::any_of(
            result.targets, [&](ControlId control) { return navigates(control); });
    }

    void assign(std::shared_ptr<const BindingSet> set) noexcept {
        release();
        base = std::move(set);
        mappings = nav_mappings(*this);
        (void)state.set_bindings(with_shoulders(base));
    }

    MappingResult process(const InputEvent& event, bool emit) noexcept {
        auto result = state.process(event);
        evaluate(emit);
        return result;
    }

    [[nodiscard]] bool r_held() const noexcept {
        const auto& c = aurora::pad::controls();
        return state.value(c.r) >= 0.5f || state.value(c.triggerR) >= kAxisPressThreshold;
    }

    [[nodiscard]] bool chord_held() const noexcept {
        return r_held() && state.value(aurora::pad::controls().start) >= 0.5f;
    }

    void evaluate(bool emit) noexcept {
        const auto& c = aurora::pad::controls();
        NavSet desired;

        if (sSettings.menuChord) {
            if (chord_held()) {
                chordConsumed = true;
                desired.insert({NavCommand::Menu, {id, c.start, 0}});
            } else if (!r_held() && state.value(c.start) < 0.5f) {
                chordConsumed = false;
            }
        }
        for (const auto& mapping : mappings) {
            if (chordConsumed && (mapping.control == c.r || mapping.control == c.triggerR)) {
                continue;
            }
            const float value = state.value(mapping.control);
            if (!is_axis(mapping.control)) {
                if (value >= 0.5f) {
                    desired.insert({mapping.positive, {id, mapping.control, 0}});
                }
                continue;
            }
            const auto direction = [&](NavCommand command, int sign) {
                const ActiveNav nav{command, {id, mapping.control, sign}};
                const float pulled = value * static_cast<float>(sign);
                if (command != NavCommand::None &&
                    (active.contains(nav) ? pulled > kAxisReleaseThreshold :
                                            pulled >= kAxisPressThreshold))
                {
                    desired.insert(nav);
                }
            };
            direction(mapping.positive, 1);
            direction(mapping.negative, -1);
        }

        for (const auto& nav : active) {
            if (!desired.contains(nav)) {
                sNavKeys.release(nav);
            }
        }
        const bool visible = any_document_visible();
        for (const auto& nav : desired) {
            if (emit && !active.contains(nav) && (visible || nav.command == NavCommand::Menu)) {
                sNavKeys.press(nav);
            }
        }
        active = std::move(desired);
    }

    void release() noexcept {
        for (const auto& nav : active) {
            sNavKeys.release(nav);
        }
        active.clear();
        chordConsumed = false;
    }

    void reset() noexcept {
        active.clear();
        chordConsumed = false;
        (void)state.reset();
    }

private:
    static std::vector<NavMapping> nav_mappings(const NavContext& context) noexcept {
        const auto& c = aurora::pad::controls();
        const auto& ui = ui_controls();
        std::vector<NavMapping> mappings{
            {c.up, NavCommand::Up},
            {c.down, NavCommand::Down},
            {c.left, NavCommand::Left},
            {c.right, NavCommand::Right},
            {c.leftX, NavCommand::Right, NavCommand::Left},
            {c.leftY, NavCommand::Up, NavCommand::Down},
            {c.a, NavCommand::Confirm},
            {c.b, NavCommand::Cancel},
            {c.l, NavCommand::Previous},
            {c.triggerL, NavCommand::Previous},
            {ui.previous, NavCommand::Previous},
            {c.r, NavCommand::Next},
            {c.triggerR, NavCommand::Next},
            {ui.next, NavCommand::Next},
            {ui.menu, NavCommand::Menu},
        };
        if (sSettings.menuControl != kInvalidControlId) {
            mappings.push_back({sSettings.menuControl, NavCommand::Menu});
        }
        // Back produces Menu until the menu control has a gamepad binding.
        const bool menuOnGamepad =
            context.base != nullptr &&
            std::ranges::any_of(context.base->bindings, [](const Binding& binding) {
                return binding.target == sSettings.menuControl && is_gamepad(binding.input);
            });
        if (!menuOnGamepad) {
            mappings.push_back(
                {aurora::pad::control_for_ext_button(PAD_BUTTON_BACK), NavCommand::Menu});
        }
        return mappings;
    }

    // Lets the shoulder buttons page wherever they aren't bound to navigation already.
    [[nodiscard]] std::shared_ptr<const BindingSet> with_shoulders(
        const std::shared_ptr<const BindingSet>& set) const noexcept {
        if (set == nullptr) {
            return set;
        }
        auto out = std::make_shared<BindingSet>(*set);
        std::set<SourceId> controllers;
        for (const auto& binding : set->bindings) {
            if (is_gamepad(binding.input)) {
                controllers.insert(binding.input.source);
            }
        }
        const std::array shoulders{
            std::pair{SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ui_controls().previous},
            std::pair{SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ui_controls().next},
        };
        for (const auto source : controllers) {
            for (const auto& [button, control] : shoulders) {
                const PhysicalInput input{
                    .source = source, .control = PhysicalInput::GamepadButton{.button = button}};
                const bool navigating =
                    std::ranges::any_of(set->bindings, [&](const Binding& binding) {
                        const auto* bound =
                            binding.input.control.get_if<PhysicalInput::GamepadButton>();
                        return binding.input.source == source && bound != nullptr &&
                               bound->button == button && navigates(binding.target);
                    });
                if (!navigating) {
                    out->bindings.push_back({.input = input, .target = control});
                }
            }
        }
        return out;
    }
};

std::array<NavContext, PAD_MAX_CONTROLLERS> sPorts;
// Controllers without a port
std::map<SourceId, NavContext> sUnassigned;
// Default navigation keys, for keys no port binds to navigation
NavContext sKeyboard;

std::shared_ptr<const BindingSet> keyboard_bindings() noexcept {
    const auto keyboard = aurora::input::keyboard_source().id;
    const auto& c = aurora::pad::controls();
    const auto& ui = ui_controls();
    const std::array<std::pair<SDL_Scancode, ControlId>, 10> keys{{
        {SDL_SCANCODE_UP, c.up},
        {SDL_SCANCODE_DOWN, c.down},
        {SDL_SCANCODE_LEFT, c.left},
        {SDL_SCANCODE_RIGHT, c.right},
        {SDL_SCANCODE_RETURN, c.a},
        {SDL_SCANCODE_KP_ENTER, c.a},
        {SDL_SCANCODE_ESCAPE, c.b},
        {SDL_SCANCODE_PAGEUP, ui.previous},
        {SDL_SCANCODE_PAGEDOWN, ui.next},
        {SDL_SCANCODE_F1, ui.menu},
    }};
    auto set = std::make_shared<BindingSet>();
    for (const auto& [scancode, target] : keys) {
        set->bindings.push_back({
            .input = {.source = keyboard, .control = PhysicalInput::Key{.scancode = scancode}},
            .target = target,
        });
    }
    return set;
}

template <typename F>
void for_each_context(F&& fn) noexcept {
    for (auto& port : sPorts) {
        fn(port);
    }
    for (auto& context : sUnassigned | std::views::values) {
        fn(context);
    }
    fn(sKeyboard);
}

// Picks up rebuilt PAD binding sets and port assignments.
void sync_contexts() noexcept {
    for (uint32_t port = 0; port < sPorts.size(); ++port) {
        if (auto set = aurora::pad::binding_set(port); set != sPorts[port].base) {
            sPorts[port].assign(std::move(set));
        }
    }
    std::erase_if(sUnassigned, [](auto& entry) {
        const bool assigned = std::ranges::any_of(
            sPorts, [&](const NavContext& port) { return port.references(entry.first); });
        if (assigned) {
            entry.second.release();
        }
        return assigned;
    });
}

NavContext* unassigned_context(SourceId source) noexcept {
    if (const auto it = sUnassigned.find(source); it != sUnassigned.end()) {
        return &it->second;
    }
    auto set = aurora::pad::controller_binding_set(source);
    if (set == nullptr) {
        return nullptr;
    }
    auto& context = sUnassigned[source];
    context.assign(std::move(set));
    return &context;
}

// The analog stick with the largest deflection (dispatched as kNavAxisEvent)
class NavAxis {
public:
    void refresh() noexcept {
        const Rml::Vector2f next = compute();
        if (next == mValue) {
            return;
        }
        mValue = next;
        if (auto* ctx = get_context()) {
            dispatch(*ctx, 0.0);
        }
    }

    // Also catches a document closing without any input.
    void update(Rml::Context& ctx, double dtSeconds) noexcept {
        refresh();
        if (mValue != Rml::Vector2f{}) {
            dispatch(ctx, dtSeconds);
        }
    }

    void clear() noexcept { mValue = {}; }

private:
    void dispatch(Rml::Context& ctx, double dtSeconds) const noexcept {
        if (auto* target = ctx.GetFocusElement()) {
            target->DispatchEvent(
                kNavAxisEvent, {
                                   {"x", Rml::Variant{mValue.x}},
                                   {"y", Rml::Variant{mValue.y}},
                                   {"dt", Rml::Variant{static_cast<float>(dtSeconds)}},
                               });
        }
    }

    static float dead_zoned(float value) noexcept {
        const float magnitude = std::abs(value);
        if (magnitude <= kNavAxisDeadZone) {
            return 0.0f;
        }
        return std::copysign((magnitude - kNavAxisDeadZone) / (1.0f - kNavAxisDeadZone), value);
    }

    static Rml::Vector2f compute() noexcept {
        if (!any_document_visible()) {
            return {};
        }
        Rml::Vector2f best;
        float bestMagnitude = 0.0f;
        const auto& c = aurora::pad::controls();
        for_each_context([&](const NavContext& context) {
            const Rml::Vector2f value{
                dead_zoned(context.state.value(c.leftX)),
                dead_zoned(-context.state.value(c.leftY)),  // PAD Y is positive up
            };
            const float magnitude = std::max(std::abs(value.x), std::abs(value.y));
            if (magnitude > bestMagnitude) {
                best = value;
                bestMagnitude = magnitude;
            }
        });
        return best;
    }

    Rml::Vector2f mValue;
};

NavAxis sNavAxis;

class MenuTap {
public:
    // True when a three-finger tap completes.
    bool process(const InputEvent::PointerChanged& pointer) noexcept {
        using Phase = InputEvent::PointerChanged::Phase;
        const Rml::Vector2f position = to_context(pointer.position);
        switch (pointer.phase) {
        case Phase::Down:
            down(pointer.pointer, position);
            return false;
        case Phase::Move:
            if (const auto* finger = find(pointer.pointer);
                finger != nullptr && moved_too_far(*finger, position))
            {
                mFailed = true;
            }
            return false;
        case Phase::Up:
            return up(pointer.pointer, position);
        case Phase::Cancel:
            clear();
            return false;
        }
        return false;
    }

    void clear() noexcept { *this = {}; }

private:
    struct Finger {
        aurora::input::PointerId id = 0;
        Rml::Vector2f startPosition;
        bool active = false;
    };

    void down(aurora::input::PointerId id, Rml::Vector2f position) noexcept {
        const double now = now_seconds();
        if (mActiveCount == 0) {
            clear();
            mFirstDownAt = now;
        }
        Finger* finger = mCandidate || find(id) != nullptr ? nullptr : find_free();
        if (finger == nullptr) {
            mFailed = true;
            return;
        }
        *finger = {.id = id, .startPosition = position, .active = true};
        ++mActiveCount;
        mFailed = mFailed || now - mFirstDownAt > kMenuTapMaxDownSpan;
        mCandidate = mActiveCount == kMenuTapFingerCount;
    }

    bool up(aurora::input::PointerId id, Rml::Vector2f position) noexcept {
        auto* finger = find(id);
        if (finger == nullptr) {
            return false;
        }
        mFailed = mFailed || !mCandidate || moved_too_far(*finger, position);
        *finger = {};
        if (--mActiveCount > 0) {
            return false;
        }
        const bool tapped =
            mCandidate && !mFailed && now_seconds() - mFirstDownAt <= kMenuTapMaxDuration;
        clear();
        if (tapped) {
            send(NavCommand::Menu);
        }
        return tapped;
    }

    Finger* find(aurora::input::PointerId id) noexcept {
        const auto it = std::ranges::find_if(
            mFingers, [id](const Finger& finger) { return finger.active && finger.id == id; });
        return it != mFingers.end() ? &*it : nullptr;
    }

    Finger* find_free() noexcept {
        const auto it = std::ranges::find(mFingers, false, &Finger::active);
        return it != mFingers.end() ? &*it : nullptr;
    }

    static Rml::Vector2f to_context(SDL_FPoint position) noexcept {
        auto* ctx = get_context();
        const AuroraWindowSize size = aurora_get_window_size();
        if (ctx == nullptr || size.width == 0 || size.height == 0) {
            return {position.x, position.y};
        }
        const auto dimensions = ctx->GetDimensions();
        return {
            position.x * static_cast<float>(dimensions.x) / static_cast<float>(size.width),
            position.y * static_cast<float>(dimensions.y) / static_cast<float>(size.height),
        };
    }

    static bool moved_too_far(const Finger& finger, Rml::Vector2f position) noexcept {
        auto* ctx = get_context();
        const float ratio =
            ctx != nullptr ? std::max(ctx->GetDensityIndependentPixelRatio(), 1.0f) : 1.0f;
        const float threshold = kMenuTapMoveThreshold * ratio;
        return (position - finger.startPosition).SquaredMagnitude() > threshold * threshold;
    }

    std::array<Finger, kMenuTapFingerCount> mFingers{};
    int mActiveCount = 0;
    double mFirstDownAt = 0.0;
    bool mCandidate = false;
    bool mFailed = false;
};

MenuTap sMenuTap;
aurora::input::LayerId sLayer = aurora::input::kInvalidLayerId;
double sLastUpdateAt = 0.0;

bool text_input_focused() noexcept {
    auto* ctx = get_context();
    auto* focus = ctx != nullptr ? ctx->GetFocusElement() : nullptr;
    if (focus == nullptr) {
        return false;
    }
    const auto& tag = focus->GetTagName();
    if (tag == "textarea") {
        return true;
    }
    if (tag != "input") {
        return false;
    }
    const auto type = focus->GetAttribute<Rml::String>("type", "text");
    return type == "text" || type == "password";
}

void handle_cancel(const InputEvent& event) noexcept {
    aurora::rmlui::process_input(event);
    for_each_context([&](NavContext& context) {
        if (context.references(event.source.id)) {
            context.process(event, false);
        }
    });
    if (event.source.kind == InputSource::Kind::Touch) {
        sMenuTap.clear();
    }
}

void handle_source_change(
    const InputEvent& event, InputEvent::SourceChanged::Change change) noexcept {
    using Change = InputEvent::SourceChanged::Change;
    sync_contexts();
    if (change == Change::Disconnected) {
        handle_cancel(event);
        if (const auto it = sUnassigned.find(event.source.id); it != sUnassigned.end()) {
            it->second.release();
            sUnassigned.erase(it);
        }
    }
    auto* ctx = get_context();
    if (event.source.kind != InputSource::Kind::Controller || ctx == nullptr ||
        ctx->GetRootElement() == nullptr)
    {
        return;
    }
    const char* type = change == Change::Connected    ? "connected" :
                       change == Change::Disconnected ? "disconnected" :
                                                        "remapped";
    Rml::Dictionary parameters;
    parameters["type"] = Rml::String(type);
    parameters["source"] = static_cast<int64_t>(event.source.id);
    ctx->GetRootElement()->DispatchEvent(kControllerChangeEvent, parameters);
}

bool handle_keyboard(const InputEvent& event) noexcept {
    const bool textFocus = text_input_focused();
    if (event.payload.is<InputEvent::TextInput>() || event.payload.is<InputEvent::TextEditing>()) {
        if (!textFocus) {
            return false;
        }
        aurora::rmlui::process_input(event);
        return true;
    }
    if (!event.payload.is<InputEvent::KeyChanged>()) {
        return false;
    }
    const bool emit = !textFocus;
    bool navigation = false;
    for (auto& port : sPorts) {
        if (port.references(event.source.id) && port.navigates(port.process(event, emit))) {
            navigation = true;
        }
    }
    if (sKeyboard.navigates(sKeyboard.process(event, emit && !navigation))) {
        navigation = true;
    }
    if (textFocus || !navigation) {
        aurora::rmlui::process_input(event);
    }
    return textFocus;
}

bool handle_pointer(const InputEvent& event) noexcept {
    const auto* pointer = event.payload.get_if<InputEvent::PointerChanged>();
    const auto result = aurora::rmlui::process_input(event);
    if (event.source.kind == InputSource::Kind::Touch && pointer != nullptr && sSettings.menuTap &&
        sMenuTap.process(*pointer))
    {
        return true;
    }
    if (pointer != nullptr && pointer->phase == InputEvent::PointerChanged::Phase::Down) {
        // Passive documents (touch overlays, toasts) let presses through unless they
        // claim the element.
        return detail::pointer_claimed(result.target);
    }
    return event.payload.is<InputEvent::Scroll>() && result.handled;
}

void handle_controller(const InputEvent& event) noexcept {
    bool assigned = false;
    for (auto& port : sPorts) {
        if (port.references(event.source.id)) {
            port.process(event, true);
            assigned = true;
        }
    }
    if (!assigned) {
        if (auto* context = unassigned_context(event.source.id)) {
            context->process(event, true);
        }
    }
}

EventResult on_event(const InputEvent& event, void*) {
    if (get_context() == nullptr) {
        return EventResult::Pass;
    }
    if (const auto* changed = event.payload.get_if<InputEvent::SourceChanged>()) {
        handle_source_change(event, changed->change);
        sNavAxis.refresh();
        return EventResult::Pass;
    }
    if (event.payload.is<InputEvent::Cancelled>()) {
        handle_cancel(event);
        sNavAxis.refresh();
        return EventResult::Pass;
    }

    const bool visibleBefore = any_document_visible();
    bool consume = false;
    switch (event.source.kind) {
    case InputSource::Kind::Keyboard:
        consume = handle_keyboard(event);
        break;
    case InputSource::Kind::Mouse:
    case InputSource::Kind::Touch:
        consume = handle_pointer(event);
        break;
    case InputSource::Kind::Controller:
        handle_controller(event);
        break;
    }
    sNavAxis.refresh();
    // Input that opened or closed a document belongs to the UI.
    return consume || visibleBefore || any_document_visible() ? EventResult::Consume :
                                                                EventResult::Pass;
}

bool captures_source(const InputSource& source, void*) {
    if (get_context() == nullptr) {
        return false;
    }
    return any_document_visible() ||
           (source.kind == InputSource::Kind::Keyboard && text_input_focused());
}

}  // namespace

void apply_settings(const Settings& settings) noexcept {
    sSettings = settings;
    // The mappings depend on the menu control.
    for_each_context([](NavContext& context) { context.assign(std::move(context.base)); });
}

const Settings& settings() noexcept {
    return sSettings;
}

void initialize() noexcept {
    if (sLayer != aurora::input::kInvalidLayerId) {
        return;
    }
    sLayer = aurora::input::register_layer({
        .label = "borealis.ui",
        .priority = aurora::input::kRmlUiLayerPriority,
        .onEvent = on_event,
        .capturesSource = captures_source,
    });
    sKeyboard.assign(keyboard_bindings());
    sync_contexts();
}

void shutdown() noexcept {
    if (sLayer != aurora::input::kInvalidLayerId) {
        aurora::input::unregister_layer(std::exchange(sLayer, aurora::input::kInvalidLayerId));
    }
    reset();
}

void reset() noexcept {
    sNavKeys.clear();
    for (auto& port : sPorts) {
        port.reset();
    }
    sKeyboard.reset();
    sUnassigned.clear();
    sNavAxis.clear();
    sMenuTap.clear();
}

void update() noexcept {
    auto* ctx = get_context();
    if (ctx == nullptr) {
        reset();
        sLastUpdateAt = 0.0;
        return;
    }
    sync_contexts();

    const double now = now_seconds();
    const double dtSeconds =
        sLastUpdateAt == 0.0 ? 0.0 : std::clamp(now - sLastUpdateAt, 0.0, kMaxNavAxisDt);
    sLastUpdateAt = now;
    sNavKeys.update(now);
    sNavAxis.update(*ctx, dtSeconds);
}

}  // namespace borealis::ui::input
