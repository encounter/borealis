#pragma once

#include <nlohmann/json_fwd.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

/**
 * Typed configuration variables with JSON persistence.
 *
 * A port declares Var<T> members in a settings struct and defines them, with their keys and
 * defaults, in one translation unit that includes <borealis/config_codec.hpp>. Vars register
 * themselves on construction. Code that reads and writes them only needs this header.
 *
 * A var's effective value is the highest-priority Overlay value, else the user value, else
 * the default. Only user values are persisted. All access is main-thread only.
 */
namespace borealis::config {

template <typename T>
concept Value = std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T> &&
    std::copy_constructible<T> && std::equality_comparable<T>;

class Overlay;
class Registry;
class VarBase;
template <Value T>
class Var;

/** Serialization for T. Defined in <borealis/config_codec.hpp>; specialize for custom types. */
template <typename T>
struct Codec;

/** Thrown when a value can't be decoded or parsed for a var. */
class InvalidValueError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * Enum names for persistence and CLI parsing. Provide them with an ADL-visible function next
 * to the enum:
 *
 *     constexpr auto config_enum_values(Resampler) {
 *         using enum Resampler;
 *         return borealis::config::enum_table<Resampler>({{Bilinear, "bilinear"}, {Area, "area"}});
 *     }
 *
 * Values outside the table are rejected on load.
 */
template <typename E, std::size_t N>
struct EnumTable {
    std::array<std::pair<E, std::string_view>, N> entries{};

    constexpr std::optional<std::string_view> name(E value) const noexcept {
        for (const auto& [entryValue, entryName] : entries) {
            if (entryValue == value) {
                return entryName;
            }
        }
        return std::nullopt;
    }

    constexpr std::optional<E> value(std::string_view name) const noexcept {
        for (const auto& [entryValue, entryName] : entries) {
            if (entryName == name) {
                return entryValue;
            }
        }
        return std::nullopt;
    }

    constexpr std::optional<std::size_t> index(E value) const noexcept {
        for (std::size_t i = 0; i < N; ++i) {
            if (entries[i].first == value) {
                return i;
            }
        }
        return std::nullopt;
    }

    static constexpr std::size_t size() noexcept { return N; }
};

template <typename E, std::size_t N>
constexpr EnumTable<E, N> enum_table(const std::pair<E, std::string_view> (&entries)[N]) {
    EnumTable<E, N> table;
    for (std::size_t i = 0; i < N; ++i) {
        table.entries[i] = entries[i];
    }
    return table;
}

/** An enum with a `config_enum_values(E)` table. */
template <typename E>
concept NamedEnum = std::is_enum_v<E> && requires { config_enum_values(E{}); };

/** The table returned by `config_enum_values(E)`. */
template <NamedEnum E>
constexpr auto enum_values() {
    return config_enum_values(E{});
}

template <typename T>
struct VarOptions {
    /** Arithmetic T only. Clamps on every write path. */
    std::optional<T> min{};
    std::optional<T> max{};
    /** Fixup applied after clamping on every write path. */
    T (*sanitize)(T) = nullptr;
    /** Former keys, read when the key itself is absent and dropped on the next save. */
    std::vector<std::string> aliases{};
    /** false keeps the var out of the config file. Overlays, callbacks and CLI still work. */
    bool persist = true;
};

enum class Source : std::uint8_t {
    Default,
    User,
    Overlay,
};

struct OverlayOptions {
    /** Higher wins. The user layer is 0; values below 1 are raised to 1. */
    int priority = 100;
    /** A user set() or reset() of a var drops this overlay's value for it. */
    bool releaseOnUserSet = false;
};

namespace detail {
struct Signal;

/** Codec entry points, captured where Var<T> is constructed. */
struct CodecOps {
    nlohmann::json (*encode)(const VarBase& var, bool userValue);
    void (*decode)(VarBase& var, const nlohmann::json& value, bool fromFile);
    void (*parse)(VarBase& var, Overlay* overlay, std::string_view text);
};

/** Identifies T for checked downcasts. */
template <typename T>
struct TypeTag {
    static constexpr char id = 0;
};

struct CodecAccess;

std::string index_key(std::string_view pattern, std::size_t index);
}  // namespace detail

/** Keeps a change callback connected. Disconnects on destruction. */
class [[nodiscard]] Connection {
public:
    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    ~Connection();

    void disconnect();
    /** Keeps the callback for the var's lifetime and empties this handle. */
    void release() noexcept;
    bool connected() const noexcept;
    explicit operator bool() const noexcept { return connected(); }

private:
    friend class VarBase;
    Connection(std::weak_ptr<detail::Signal> signal, std::uint64_t id) noexcept
        : mSignal{std::move(signal)}, mId{id} {}

    std::weak_ptr<detail::Signal> mSignal;
    std::uint64_t mId = 0;
};

/** A named set of temporary values that take precedence over user values. */
class Overlay {
public:
    explicit Overlay(std::string name, OverlayOptions options = {});
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    /** Clears its values. */
    ~Overlay();

    template <Value T>
    void set(Var<T>& var, std::type_identity_t<T> value);
    void unset(VarBase& var);
    void clear();

    bool contains(const VarBase& var) const noexcept;
    bool empty() const noexcept { return mVars.empty(); }
    const std::string& name() const noexcept { return mName; }
    int priority() const noexcept { return mOptions.priority; }
    bool releases_on_user_set() const noexcept { return mOptions.releaseOnUserSet; }

private:
    friend class VarBase;

    std::string mName;
    OverlayOptions mOptions;
    std::vector<VarBase*> mVars;
};

/** Type-erased view of a Var<T>. */
class VarBase {
public:
    VarBase(const VarBase&) = delete;
    VarBase& operator=(const VarBase&) = delete;
    virtual ~VarBase();

    const std::string& key() const noexcept { return mKey; }
    Registry* registry() const noexcept { return mRegistry; }
    Source source() const noexcept;
    /** The overlay supplying the effective value, or null. */
    const Overlay* overlay() const noexcept { return mTopOverlay; }
    /** True while an overlay that a user set() doesn't release supplies the value. */
    bool locked() const noexcept;
    bool has_user_value() const noexcept { return mHasUserValue; }
    bool persistent() const noexcept { return mPersist; }
    /** Drops the user value. */
    virtual void reset() = 0;
    virtual std::span<const std::string> aliases() const noexcept = 0;

    /** The effective value. */
    nlohmann::json to_json() const;
    /** Sets the user value. Throws InvalidValueError. */
    void set_json(const nlohmann::json& value);
    /** The effective value; strings are returned raw, other types as JSON. */
    std::string to_string() const;
    /** Sets the user value from CLI syntax. Throws InvalidValueError. */
    void set_string(std::string_view text);

    Connection on_change(std::function<void()> callback);

    /** Checked downcast; null when the var doesn't hold a T. */
    template <Value T>
    Var<T>* as() noexcept {
        return mType == &detail::TypeTag<T>::id ? static_cast<Var<T>*>(this) : nullptr;
    }
    template <Value T>
    const Var<T>* as() const noexcept {
        return mType == &detail::TypeTag<T>::id ? static_cast<const Var<T>*>(this) : nullptr;
    }

protected:
    VarBase(Registry& registry, std::string key, const void* type, const detail::CodecOps* codec,
        bool persist);

    /** Registers the var. Called once the derived var is fully constructed. */
    void attach();
    /** Unregisters the var, stashing its user value. Called before derived members die. */
    void detach() noexcept;
    /** Records a user-layer change for persistence. */
    void user_changed();
    void notify(const void* current, const void* previous);
    Connection connect(std::function<void(const void*, const void*)> callback);
    void track(Overlay& overlay);
    void untrack(Overlay& overlay) noexcept;
    /** Removes the overlay's value and recomputes. The overlay has already untracked it. */
    virtual void drop_overlay(const Overlay& overlay) = 0;

    const Overlay* mTopOverlay = nullptr;
    bool mHasUserValue = false;

private:
    friend class Overlay;
    friend class Registry;
    friend struct detail::CodecAccess;

    Registry* mRegistry;
    std::string mKey;
    const void* mType;
    const detail::CodecOps* mCodec;
    bool mPersist;
    bool mAttached = false;
    std::shared_ptr<detail::Signal> mSignal;
};

template <Value T>
class Var final : public VarBase {
public:
    using value_type = T;

    Var(std::string key, T defaultValue, VarOptions<T> options = {});
    Var(Registry& registry, std::string key, T defaultValue, VarOptions<T> options = {});
    ~Var() override;

    const T& get() const noexcept { return mCurrent; }
    const T& operator*() const noexcept { return mCurrent; }
    const T* operator->() const noexcept { return &mCurrent; }
    operator const T&() const noexcept { return mCurrent; }

    /** Sets the user value. Releases this var from overlays that release on user set. */
    void set(T value) { set_user(std::move(value), false); }
    void reset() override;
    const T& default_value() const noexcept { return mDefault; }
    const std::optional<T>& user_value() const noexcept { return mUser; }
    /** True when the effective value differs from the default. */
    bool modified() const { return mCurrent != mDefault; }
    const VarOptions<T>& options() const noexcept { return mOptions; }
    std::span<const std::string> aliases() const noexcept override { return mOptions.aliases; }

    /**
     * Calls back whenever the effective value changes. Accepts (), (const T&), or
     * (const T& value, const T& previous).
     */
    template <typename F>
    Connection on_change(F&& callback);

    /** Calls back now with the current value, then on every change. */
    template <typename F>
    Connection observe(F&& callback);

private:
    friend class Overlay;
    friend struct detail::CodecAccess;

    struct OverlayValue {
        Overlay* overlay;
        T value;
    };

    T constrain(T value) const;
    void set_user(T value, bool fromFile);
    void set_overlay(Overlay& overlay, T value);
    void drop_overlay(const Overlay& overlay) override;
    void release_overlays() noexcept;
    void recompute();

    VarOptions<T> mOptions;
    T mDefault;
    std::optional<T> mUser;
    /** Highest priority first; ties go to the most recently added. */
    std::vector<OverlayValue> mOverlays;
    T mCurrent;
};

/** N vars keyed by substituting the index for each "{}" in a pattern. */
template <Value T, std::size_t N>
class VarArray {
public:
    VarArray(std::string_view pattern, const T& defaultValue, const VarOptions<T>& options = {});
    VarArray(Registry& registry, std::string_view pattern, const T& defaultValue,
        const VarOptions<T>& options = {})
        : mVars{make(registry, pattern, defaultValue, options, std::make_index_sequence<N>{})} {}

    Var<T>& operator[](std::size_t index) noexcept { return mVars[index]; }
    const Var<T>& operator[](std::size_t index) const noexcept { return mVars[index]; }
    auto begin() noexcept { return mVars.begin(); }
    auto end() noexcept { return mVars.end(); }
    auto begin() const noexcept { return mVars.begin(); }
    auto end() const noexcept { return mVars.end(); }
    static constexpr std::size_t size() noexcept { return N; }

private:
    template <std::size_t... I>
    static std::array<Var<T>, N> make(Registry& registry, std::string_view pattern,
        const T& defaultValue, const VarOptions<T>& options, std::index_sequence<I...>) {
        return {Var<T>{registry, detail::index_key(pattern, I), defaultValue, options}...};
    }

    std::array<Var<T>, N> mVars;
};

struct LoadOptions {
    std::filesystem::path path;
    /** Stored as "$version". Files without it are version 0. */
    int version = 0;
    /** Runs on the raw root object, before values are applied, when the file is older. */
    std::function<void(nlohmann::json& root, int fromVersion)> migrate;
    /** update() saves once this long has passed since the last user change. */
    std::chrono::milliseconds autosaveDelay{1000};
};

enum class ErrorCode {
    None,
    /** Non-fatal: defaults stay in effect and the next save creates the file. */
    FileMissing,
    /** save() before load() supplied a path. */
    NoPath,
    ReadFailed,
    ParseFailed,
    WriteFailed,
    InvalidValue,
};

struct Status {
    ErrorCode code = ErrorCode::None;
    std::string message;

    explicit operator bool() const noexcept { return code == ErrorCode::None; }
};

/** Owns var registration and the config file. Ports use the global one via free functions. */
class Registry {
public:
    Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    /** Vars still registered are detached; they must not be used afterwards. */
    ~Registry();

    static Registry& global();

    /**
     * Reads the file and applies its values. A file that can't be read or parsed is left in
     * place (parse failures are moved to "<path>.bad" first) and saving stays disabled until
     * the move succeeds, so the user's data is never overwritten.
     */
    Status load(LoadOptions options);
    Status save();
    /** Saves when there are unsaved changes and the autosave delay has passed. Call per frame. */
    void update();
    /** Saves now if there are unsaved changes. */
    Status flush();
    bool dirty() const noexcept;
    const std::filesystem::path& path() const noexcept;

    /** Sets a value in cli_overlay(). Unknown keys apply when the var registers. */
    Status apply_override(std::string_view key, std::string_view value);
    /** Applies KEY=VALUE assignments; reports the first failure after trying all. */
    Status apply_overrides(std::span<const std::string> assignments);
    /** Holds command-line overrides: highest priority, released by a user set(). */
    Overlay& cli_overlay() noexcept;

    VarBase* find(std::string_view key) const;
    template <Value T>
    Var<T>* find(std::string_view key) const {
        VarBase* var = find(key);
        return var ? var->as<T>() : nullptr;
    }
    /** Visits registered vars in key order. */
    void for_each(const std::function<void(VarBase&)>& callback) const;

private:
    friend class VarBase;
    struct State;

    void add(VarBase& var);
    void remove(VarBase& var) noexcept;
    void user_changed(VarBase& var);

    std::unique_ptr<State> mState;
};

// Shorthands for Registry::global().
Status load(LoadOptions options);
Status save();
void update();
Status flush();
Status apply_overrides(std::span<const std::string> assignments);
Overlay& cli_overlay() noexcept;
VarBase* find(std::string_view key);
template <Value T>
Var<T>* find(std::string_view key) {
    return Registry::global().find<T>(key);
}
void for_each(const std::function<void(VarBase&)>& callback);

namespace detail {
/** nlohmann::json, made dependent so it only has to be complete where a Var<T> is constructed. */
template <typename T>
using JsonFor = std::conditional_t<std::is_void_v<T>, void, nlohmann::json>;

/** Bridges VarBase's codec entry points to Codec<T>, which <borealis/config_codec.hpp> defines. */
struct CodecAccess {
    template <Value T>
    static JsonFor<T> encode(const VarBase& base, bool userValue) {
        const auto& var = static_cast<const Var<T>&>(base);
        return Codec<T>::encode(userValue && var.mUser ? *var.mUser : var.mCurrent);
    }

    template <Value T>
    static void decode(VarBase& base, const nlohmann::json& value, bool fromFile) {
        static_cast<Var<T>&>(base).set_user(Codec<T>::decode(value), fromFile);
    }

    template <Value T>
    static void parse(VarBase& base, Overlay* overlay, std::string_view text) {
        auto& var = static_cast<Var<T>&>(base);
        T value = Codec<T>::parse(text);
        if (overlay != nullptr) {
            var.set_overlay(*overlay, std::move(value));
        } else {
            var.set_user(std::move(value), false);
        }
    }

    template <Value T>
    static constexpr CodecOps ops{&encode<T>, &decode<T>, &parse<T>};
};
}  // namespace detail

template <Value T>
void Overlay::set(Var<T>& var, std::type_identity_t<T> value) {
    var.set_overlay(*this, std::move(value));
}

template <Value T>
Var<T>::Var(std::string key, T defaultValue, VarOptions<T> options)
    : Var(Registry::global(), std::move(key), std::move(defaultValue), std::move(options)) {}

template <Value T>
Var<T>::Var(Registry& registry, std::string key, T defaultValue, VarOptions<T> options)
    : VarBase(registry, std::move(key), &detail::TypeTag<T>::id, &detail::CodecAccess::ops<T>,
          options.persist),
      mOptions(std::move(options)), mDefault(constrain(std::move(defaultValue))),
      mCurrent(mDefault) {
    attach();
}

template <Value T>
Var<T>::~Var() {
    for (auto& entry : mOverlays) {
        untrack(*entry.overlay);
    }
    mOverlays.clear();
    detach();
}

template <Value T>
void Var<T>::reset() {
    if (!mUser) {
        return;
    }
    mUser.reset();
    mHasUserValue = false;
    release_overlays();
    user_changed();
    recompute();
}

template <Value T>
template <typename F>
Connection Var<T>::on_change(F&& callback) {
    using Fn = std::decay_t<F>;
    static_assert(std::is_invocable_v<Fn&, const T&, const T&> ||
                      std::is_invocable_v<Fn&, const T&> || std::is_invocable_v<Fn&>,
        "on_change callbacks take (), (const T&), or (const T&, const T&)");
    return connect([fn = Fn(std::forward<F>(callback))](
                       const void* current, const void* previous) mutable {
        if constexpr (std::is_invocable_v<Fn&, const T&, const T&>) {
            fn(*static_cast<const T*>(current), *static_cast<const T*>(previous));
        } else if constexpr (std::is_invocable_v<Fn&, const T&>) {
            fn(*static_cast<const T*>(current));
        } else {
            fn();
        }
    });
}

template <Value T>
template <typename F>
Connection Var<T>::observe(F&& callback) {
    static_assert(std::is_invocable_v<F&, const T&> || std::is_invocable_v<F&>,
        "observe callbacks take () or (const T&)");
    if constexpr (std::is_invocable_v<F&, const T&>) {
        callback(mCurrent);
    } else {
        callback();
    }
    return on_change(std::forward<F>(callback));
}

template <Value T>
T Var<T>::constrain(T value) const {
    if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
        if (mOptions.min && value < *mOptions.min) {
            value = *mOptions.min;
        }
        if (mOptions.max && value > *mOptions.max) {
            value = *mOptions.max;
        }
    }
    if (mOptions.sanitize != nullptr) {
        value = mOptions.sanitize(std::move(value));
    }
    return value;
}

template <Value T>
void Var<T>::set_user(T value, bool fromFile) {
    value = constrain(std::move(value));
    const bool changed = !mUser || *mUser != value;
    mUser = std::move(value);
    mHasUserValue = true;
    if (!fromFile) {
        release_overlays();
        if (changed) {
            user_changed();
        }
    }
    recompute();
}

template <Value T>
void Var<T>::set_overlay(Overlay& overlay, T value) {
    value = constrain(std::move(value));
    for (auto& entry : mOverlays) {
        if (entry.overlay == &overlay) {
            entry.value = std::move(value);
            recompute();
            return;
        }
    }
    const auto position = std::find_if(mOverlays.begin(), mOverlays.end(),
        [&](const OverlayValue& entry) { return entry.overlay->priority() <= overlay.priority(); });
    mOverlays.insert(position, OverlayValue{&overlay, std::move(value)});
    track(overlay);
    recompute();
}

template <Value T>
void Var<T>::drop_overlay(const Overlay& overlay) {
    std::erase_if(
        mOverlays, [&](const OverlayValue& entry) { return entry.overlay == &overlay; });
    recompute();
}

template <Value T>
void Var<T>::release_overlays() noexcept {
    std::erase_if(mOverlays, [this](const OverlayValue& entry) {
        if (!entry.overlay->releases_on_user_set()) {
            return false;
        }
        untrack(*entry.overlay);
        return true;
    });
}

template <Value T>
void Var<T>::recompute() {
    mTopOverlay = mOverlays.empty() ? nullptr : mOverlays.front().overlay;
    const T& next = !mOverlays.empty() ? mOverlays.front().value : mUser ? *mUser : mDefault;
    if (next == mCurrent) {
        return;
    }
    T previous = std::exchange(mCurrent, next);
    notify(&mCurrent, &previous);
}

template <Value T, std::size_t N>
VarArray<T, N>::VarArray(std::string_view pattern, const T& defaultValue,
    const VarOptions<T>& options)
    : VarArray(Registry::global(), pattern, defaultValue, options) {}

}  // namespace borealis::config
