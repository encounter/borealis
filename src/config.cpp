#include "borealis/config.hpp"
#include "borealis/config_codec.hpp"

#include "borealis/io.hpp"
#include "borealis/log.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <system_error>

#include "ascii.hpp"

namespace borealis::config {
namespace {

constexpr Log Log{"borealis::config"};
constexpr const char* kVersionKey = "$version";

std::string_view trim(std::string_view text) noexcept {
    const auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/** Parses an unsigned magnitude, with an optional 0x prefix. */
std::optional<std::uint64_t> parse_magnitude(std::string_view text) noexcept {
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value, base);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::string display(const std::filesystem::path& path) {
    return io::fs_path_to_string(path);
}

}  // namespace

namespace detail {

struct Slot {
    std::uint64_t id = 0;
    std::function<void(const void*, const void*)> callback;
    bool connected = true;
};

struct Signal {
    std::vector<std::shared_ptr<Slot>> slots;
    std::uint64_t nextId = 1;
    bool notifying = false;
};

bool ascii_iequals(std::string_view lhs, std::string_view rhs) noexcept {
    return borealis::detail::ascii_iequals(lhs, rhs);
}

std::string index_key(std::string_view pattern, std::size_t index) {
    const auto indexText = std::to_string(index);
    std::string key;
    std::size_t start = 0;
    while (true) {
        const auto found = pattern.find("{}", start);
        if (found == std::string_view::npos) {
            key.append(pattern.substr(start));
            return key;
        }
        key.append(pattern.substr(start, found - start));
        key.append(indexText);
        start = found + 2;
    }
}

std::optional<bool> parse_bool(std::string_view text) noexcept {
    text = trim(text);
    for (const std::string_view name : {"true", "1", "on", "yes"}) {
        if (ascii_iequals(text, name)) {
            return true;
        }
    }
    for (const std::string_view name : {"false", "0", "off", "no"}) {
        if (ascii_iequals(text, name)) {
            return false;
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> parse_int64(std::string_view text) noexcept {
    text = trim(text);
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    const auto magnitude = parse_magnitude(text);
    if (!magnitude) {
        return std::nullopt;
    }
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (negative) {
        if (*magnitude > kMax + 1) {
            return std::nullopt;
        }
        return *magnitude == kMax + 1 ? std::numeric_limits<std::int64_t>::min() :
                                        -static_cast<std::int64_t>(*magnitude);
    }
    if (*magnitude > kMax) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*magnitude);
}

std::optional<std::uint64_t> parse_uint64(std::string_view text) noexcept {
    text = trim(text);
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    return parse_magnitude(text);
}

std::optional<double> parse_double(std::string_view text) noexcept {
    const std::string copy{trim(text)};
    if (copy.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || errno == ERANGE) {
        return std::nullopt;
    }
    return value;
}

nlohmann::json parse_json(std::string_view text) {
    return nlohmann::json::parse(text);
}

double float_for_json(float value) noexcept {
    try {
        const auto text = fmt::format("{}", value);
        return std::strtod(text.c_str(), nullptr);
    } catch (...) {
        return value;
    }
}

}  // namespace detail

// Connection

Connection::Connection(Connection&& other) noexcept
    : mSignal{std::move(other.mSignal)}, mId{std::exchange(other.mId, 0)} {}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        disconnect();
        mSignal = std::move(other.mSignal);
        mId = std::exchange(other.mId, 0);
    }
    return *this;
}

Connection::~Connection() {
    disconnect();
}

void Connection::disconnect() {
    if (const auto signal = mSignal.lock()) {
        std::erase_if(signal->slots, [this](const std::shared_ptr<detail::Slot>& slot) {
            if (slot->id != mId) {
                return false;
            }
            slot->connected = false;
            return true;
        });
    }
    release();
}

void Connection::release() noexcept {
    mSignal.reset();
    mId = 0;
}

bool Connection::connected() const noexcept {
    const auto signal = mSignal.lock();
    return signal &&
           std::ranges::any_of(signal->slots,
               [this](const std::shared_ptr<detail::Slot>& slot) { return slot->id == mId; });
}

// Overlay

Overlay::Overlay(std::string name, OverlayOptions options)
    : mName{std::move(name)}, mOptions{options} {
    mOptions.priority = std::max(mOptions.priority, 1);
}

Overlay::~Overlay() {
    clear();
}

void Overlay::unset(VarBase& var) {
    const auto it = std::ranges::find(mVars, &var);
    if (it == mVars.end()) {
        return;
    }
    mVars.erase(it);
    var.drop_overlay(*this);
}

void Overlay::clear() {
    const auto vars = std::exchange(mVars, {});
    for (VarBase* var : vars) {
        var->drop_overlay(*this);
    }
}

bool Overlay::contains(const VarBase& var) const noexcept {
    return std::ranges::find(mVars, &var) != mVars.end();
}

// VarBase

VarBase::VarBase(Registry& registry, std::string key, const void* type,
    const detail::CodecOps* codec, bool persist)
    : mRegistry{&registry}, mKey{std::move(key)}, mType{type}, mCodec{codec}, mPersist{persist} {}

VarBase::~VarBase() = default;

Source VarBase::source() const noexcept {
    if (mTopOverlay != nullptr) {
        return Source::Overlay;
    }
    return mHasUserValue ? Source::User : Source::Default;
}

bool VarBase::locked() const noexcept {
    return mTopOverlay != nullptr && !mTopOverlay->releases_on_user_set();
}

nlohmann::json VarBase::to_json() const {
    return mCodec->encode(*this, false);
}

void VarBase::set_json(const nlohmann::json& value) {
    try {
        mCodec->decode(*this, value, false);
    } catch (const nlohmann::json::exception& e) {
        throw InvalidValueError(e.what());
    }
}

std::string VarBase::to_string() const {
    const auto json = to_json();
    return json.is_string() ? json.get<std::string>() : json.dump();
}

void VarBase::set_string(std::string_view text) {
    try {
        mCodec->parse(*this, nullptr, text);
    } catch (const nlohmann::json::exception& e) {
        throw InvalidValueError(e.what());
    }
}

Connection VarBase::on_change(std::function<void()> callback) {
    return connect([callback = std::move(callback)](const void*, const void*) { callback(); });
}

void VarBase::attach() {
    mRegistry->add(*this);
    mAttached = true;
}

void VarBase::detach() noexcept {
    if (!mAttached) {
        return;
    }
    mAttached = false;
    if (mRegistry != nullptr) {
        mRegistry->remove(*this);
    }
    mSignal.reset();
}

void VarBase::user_changed() {
    if (mAttached && mRegistry != nullptr) {
        mRegistry->user_changed(*this);
    }
}

void VarBase::notify(const void* current, const void* previous) {
    if (!mSignal || mSignal->slots.empty()) {
        return;
    }
    if (mSignal->notifying) {
        Log.debug("Skipped nested change notification for '{}'", mKey);
        return;
    }

    // Copies keep the signal and slots alive while callbacks connect, disconnect, or destroy.
    const auto signal = mSignal;
    const auto slots = signal->slots;
    struct Guard {
        detail::Signal& signal;
        ~Guard() { signal.notifying = false; }
    } guard{*signal};
    signal->notifying = true;
    for (const auto& slot : slots) {
        if (slot->connected) {
            slot->callback(current, previous);
        }
    }
}

Connection VarBase::connect(std::function<void(const void*, const void*)> callback) {
    if (!mSignal) {
        mSignal = std::make_shared<detail::Signal>();
    }
    const auto id = mSignal->nextId++;
    mSignal->slots.push_back(std::make_shared<detail::Slot>(detail::Slot{
        .id = id,
        .callback = std::move(callback),
    }));
    return Connection{mSignal, id};
}

void VarBase::track(Overlay& overlay) {
    if (!overlay.contains(*this)) {
        overlay.mVars.push_back(this);
    }
}

void VarBase::untrack(Overlay& overlay) noexcept {
    std::erase(overlay.mVars, this);
}

// Registry

struct Registry::State {
    std::map<std::string, VarBase*, std::less<>> vars;
    /** Keys with no registered var, and values that failed to decode. */
    nlohmann::json unknown = nlohmann::json::object();
    /** CLI overrides waiting for their var to register. */
    std::map<std::string, std::string, std::less<>> pendingOverrides;
    Overlay cliOverlay{"cli", OverlayOptions{
                                  .priority = std::numeric_limits<int>::max(),
                                  .releaseOnUserSet = true,
                              }};
    LoadOptions options;
    bool dirty = false;
    bool autosavePending = false;
    bool saveBlocked = false;
    std::chrono::steady_clock::time_point lastChange{};
};

Registry::Registry() : mState{std::make_unique<State>()} {}

Registry::~Registry() {
    for (const auto& [key, var] : mState->vars) {
        var->mRegistry = nullptr;
        var->mAttached = false;
    }
    mState->vars.clear();
}

Registry& Registry::global() {
    static Registry registry;
    return registry;
}

void Registry::add(VarBase& var) {
    auto& state = *mState;
    if (!state.vars.emplace(var.key(), &var).second) {
        Log.fatal("Config var '{}' is registered twice", var.key());
    }

    if (var.persistent()) {
        auto found = state.unknown.find(var.key());
        bool fromAlias = false;
        if (found == state.unknown.end()) {
            for (const auto& alias : var.aliases()) {
                found = state.unknown.find(alias);
                if (found != state.unknown.end()) {
                    fromAlias = true;
                    break;
                }
            }
        }
        if (found != state.unknown.end()) {
            try {
                var.mCodec->decode(var, *found, true);
                state.unknown.erase(found);
                if (fromAlias) {
                    // Rewrite the file under the new key.
                    state.dirty = true;
                    state.autosavePending = true;
                }
            } catch (const std::exception& e) {
                Log.warn("Ignoring invalid value for '{}': {}", var.key(), e.what());
            }
        }
    }

    if (const auto pending = state.pendingOverrides.find(var.key());
        pending != state.pendingOverrides.end())
    {
        const auto text = std::move(pending->second);
        state.pendingOverrides.erase(pending);
        try {
            var.mCodec->parse(var, &state.cliOverlay, text);
        } catch (const std::exception& e) {
            Log.error("Invalid override '{}' for '{}': {}", text, var.key(), e.what());
        }
    }
}

void Registry::remove(VarBase& var) noexcept {
    auto& state = *mState;
    const auto it = state.vars.find(var.key());
    if (it == state.vars.end() || it->second != &var) {
        return;
    }
    state.vars.erase(it);
    if (!var.persistent() || !var.has_user_value()) {
        return;
    }
    // Keep the user value so saving preserves it and a later registration restores it.
    try {
        state.unknown[var.key()] = var.mCodec->encode(var, true);
    } catch (const std::exception& e) {
        Log.error("Failed to keep the value of '{}': {}", var.key(), e.what());
    }
}

void Registry::user_changed(VarBase& var) {
    if (!var.persistent()) {
        return;
    }
    auto& state = *mState;
    state.unknown.erase(var.key());
    state.dirty = true;
    state.autosavePending = true;
    state.lastChange = std::chrono::steady_clock::now();
}

Status Registry::load(LoadOptions options) {
    auto& state = *mState;
    state.options = std::move(options);
    state.saveBlocked = false;
    const auto& path = state.options.path;
    if (path.empty()) {
        return {ErrorCode::NoPath, "No config path given"};
    }

    const auto readFailed = [&](std::string message) -> Status {
        state.saveBlocked = true;
        Log.error("Could not read config '{}': {}; saving is disabled", display(path), message);
        return {ErrorCode::ReadFailed, std::move(message)};
    };

    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        Log.info("Config '{}' does not exist; using defaults", display(path));
        return {ErrorCode::FileMissing, fmt::format("'{}' does not exist", display(path))};
    }
    if (ec) {
        return readFailed(ec.message());
    }
    if (!std::filesystem::is_regular_file(status)) {
        return readFailed("not a regular file");
    }

    std::string text;
    {
        std::ifstream stream{path, std::ios::binary};
        if (!stream) {
            return readFailed("could not open the file");
        }
        text.assign(std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{});
        if (stream.bad()) {
            return readFailed("could not read the file");
        }
    }

    nlohmann::json root;
    std::string parseError;
    try {
        root = nlohmann::json::parse(text, nullptr, true, true);
        if (!root.is_object()) {
            parseError = "the root is not an object";
        }
    } catch (const nlohmann::json::parse_error& e) {
        parseError = e.what();
    }
    if (!parseError.empty()) {
        auto backup = path;
        backup += ".bad";
        std::string moveError;
        if (io::atomic_replace(path, backup, moveError)) {
            Log.error("Could not parse config '{}' ({}); moved it to '{}' and using defaults",
                display(path), parseError, display(backup));
        } else {
            state.saveBlocked = true;
            Log.error("Could not parse config '{}' ({}) or move it aside ({}); saving is disabled",
                display(path), parseError, moveError);
        }
        return {ErrorCode::ParseFailed, parseError};
    }

    int fileVersion = 0;
    if (const auto version = root.find(kVersionKey); version != root.end()) {
        if (version->is_number_integer()) {
            fileVersion = version->get<int>();
        }
        root.erase(version);
    }
    bool rewrite = false;
    if (fileVersion < state.options.version && state.options.migrate) {
        try {
            state.options.migrate(root, fileVersion);
            rewrite = true;
        } catch (const std::exception& e) {
            Log.error("Config migration from version {} failed: {}", fileVersion, e.what());
        }
        if (!root.is_object()) {
            Log.error("Config migration replaced the root object; using defaults");
            root = nlohmann::json::object();
        }
    } else if (fileVersion > state.options.version) {
        Log.warn("Config '{}' is version {}, newer than {}; unknown settings are kept",
            display(path), fileVersion, state.options.version);
    }

    std::map<std::string, VarBase*, std::less<>> aliasTargets;
    for (const auto& [key, var] : state.vars) {
        for (const auto& alias : var->aliases()) {
            aliasTargets.emplace(alias, var);
        }
    }

    state.unknown = nlohmann::json::object();
    for (const auto& [key, value] : root.items()) {
        VarBase* var = find(key);
        bool viaAlias = false;
        if (var == nullptr) {
            if (const auto target = aliasTargets.find(key); target != aliasTargets.end()) {
                var = target->second;
                viaAlias = true;
                if (root.contains(var->key())) {
                    // The current key wins; drop the stale alias.
                    rewrite = true;
                    continue;
                }
            }
        }
        if (var == nullptr || !var->persistent()) {
            state.unknown[key] = value;
            continue;
        }
        try {
            var->mCodec->decode(*var, value, true);
            rewrite = rewrite || viaAlias;
        } catch (const std::exception& e) {
            Log.warn("Ignoring invalid value for '{}': {}", key, e.what());
            state.unknown[key] = value;
        }
    }

    state.dirty = rewrite;
    state.autosavePending = rewrite;
    state.lastChange = std::chrono::steady_clock::now();
    Log.info("Loaded config '{}'", display(path));
    return {};
}

Status Registry::save() {
    auto& state = *mState;
    const auto& path = state.options.path;
    if (path.empty()) {
        return {ErrorCode::NoPath, "No config path; call load() first"};
    }
    if (state.saveBlocked) {
        return {ErrorCode::WriteFailed,
            fmt::format("Saving is disabled because '{}' could not be read", display(path))};
    }

    nlohmann::json root = state.unknown;
    for (const auto& [key, var] : state.vars) {
        if (!var->persistent() || !var->has_user_value()) {
            continue;
        }
        try {
            root[key] = var->mCodec->encode(*var, true);
        } catch (const std::exception& e) {
            Log.error("Failed to encode '{}': {}", key, e.what());
        }
    }
    root[kVersionKey] = state.options.version;
    const auto text = root.dump(4, ' ', false, nlohmann::json::error_handler_t::replace) + '\n';

    const auto fail = [&](std::string message) -> Status {
        Log.warn("Could not save config '{}': {}", display(path), message);
        return {ErrorCode::WriteFailed, std::move(message)};
    };

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return fail(ec.message());
        }
    }
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
        if (stream) {
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            stream.close();
        }
        if (!stream) {
            std::filesystem::remove(temporary, ec);
            return fail(fmt::format("could not write '{}'", display(temporary)));
        }
    }
    std::string error;
    if (!io::atomic_replace(temporary, path, error)) {
        std::filesystem::remove(temporary, ec);
        return fail(std::move(error));
    }

    state.dirty = false;
    state.autosavePending = false;
    Log.debug("Saved config '{}'", display(path));
    return {};
}

void Registry::update() {
    auto& state = *mState;
    if (!state.autosavePending || state.options.path.empty()) {
        return;
    }
    if (std::chrono::steady_clock::now() - state.lastChange < state.options.autosaveDelay) {
        return;
    }
    // A failed autosave waits for the next change instead of retrying every frame.
    state.autosavePending = false;
    (void)save();
}

Status Registry::flush() {
    return mState->dirty ? save() : Status{};
}

bool Registry::dirty() const noexcept {
    return mState->dirty;
}

const std::filesystem::path& Registry::path() const noexcept {
    return mState->options.path;
}

Status Registry::apply_override(std::string_view key, std::string_view value) {
    if (key.empty()) {
        return {ErrorCode::InvalidValue, "Config override has an empty key"};
    }
    VarBase* var = find(key);
    if (var == nullptr) {
        Log.info("No config var '{}' yet; its override applies if one registers", key);
        mState->pendingOverrides.insert_or_assign(std::string{key}, std::string{value});
        return {};
    }
    try {
        var->mCodec->parse(*var, &mState->cliOverlay, value);
    } catch (const std::exception& e) {
        return {ErrorCode::InvalidValue,
            fmt::format("Invalid value '{}' for '{}': {}", value, key, e.what())};
    }
    return {};
}

Status Registry::apply_overrides(std::span<const std::string> assignments) {
    Status first;
    for (const std::string_view assignment : assignments) {
        const auto separator = assignment.find('=');
        Status status = separator == std::string_view::npos ?
                            Status{ErrorCode::InvalidValue,
                                fmt::format("Config override '{}' is not KEY=VALUE", assignment)} :
                            apply_override(trim(assignment.substr(0, separator)),
                                assignment.substr(separator + 1));
        if (!status && first) {
            first = std::move(status);
        }
    }
    return first;
}

Overlay& Registry::cli_overlay() noexcept {
    return mState->cliOverlay;
}

VarBase* Registry::find(std::string_view key) const {
    const auto it = mState->vars.find(key);
    return it != mState->vars.end() ? it->second : nullptr;
}

void Registry::for_each(const std::function<void(VarBase&)>& callback) const {
    for (const auto& [key, var] : mState->vars) {
        callback(*var);
    }
}

// Global shorthands

Status load(LoadOptions options) {
    return Registry::global().load(std::move(options));
}

Status save() {
    return Registry::global().save();
}

void update() {
    Registry::global().update();
}

Status flush() {
    return Registry::global().flush();
}

Status apply_overrides(std::span<const std::string> assignments) {
    return Registry::global().apply_overrides(assignments);
}

Overlay& cli_overlay() noexcept {
    return Registry::global().cli_overlay();
}

VarBase* find(std::string_view key) {
    return Registry::global().find(key);
}

void for_each(const std::function<void(VarBase&)>& callback) {
    Registry::global().for_each(callback);
}

}  // namespace borealis::config
