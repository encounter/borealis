#include "borealis/config.hpp"
#include "borealis/config_codec.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using borealis::config::ErrorCode;
using borealis::config::LoadOptions;
using borealis::config::Overlay;
using borealis::config::Registry;
using borealis::config::Source;
using borealis::config::Var;
using borealis::config::VarArray;
using nlohmann::json;

namespace test {

enum class Mode : std::uint8_t {
    Off = 0,
    Classic = 1,
    Enhanced = 2,
};

constexpr auto config_enum_values(Mode) {
    using enum Mode;
    return borealis::config::enum_table<Mode>({
        {Off, "off"},
        {Classic, "classic"},
        {Enhanced, "enhanced"},
    });
}

struct Layout {
    int x = 0;
    int y = 0;
    bool operator==(const Layout&) const = default;
};

void to_json(json& j, const Layout& layout) { j = json{{"x", layout.x}, {"y", layout.y}}; }

void from_json(const json& j, Layout& layout) {
    j.at("x").get_to(layout.x);
    j.at("y").get_to(layout.y);
}

}  // namespace test

namespace {

using test::Layout;
using test::Mode;

fs::path make_test_dir(const char* name) {
    const fs::path path = fs::temp_directory_path() / "borealis-config-test" / name;
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path);
    return path;
}

void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << text;
}

std::string read_text(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

json read_json(const fs::path& path) { return json::parse(read_text(path)); }

LoadOptions options_for(const fs::path& path) {
    return LoadOptions{.path = path, .autosaveDelay = std::chrono::milliseconds{0}};
}

TEST(Config, DefaultsSetReset) {
    Registry registry;
    Var<int> volume{registry, "audio.volume", 80};
    EXPECT_EQ(volume.get(), 80);
    EXPECT_EQ(*volume, 80);
    EXPECT_EQ(volume.source(), Source::Default);
    EXPECT_FALSE(volume.modified());
    EXPECT_FALSE(volume.has_user_value());

    volume.set(50);
    EXPECT_EQ(static_cast<int>(volume), 50);
    EXPECT_EQ(volume.source(), Source::User);
    EXPECT_TRUE(volume.modified());
    EXPECT_TRUE(registry.dirty());

    // An explicit default stays pinned as a user value.
    volume.set(80);
    EXPECT_TRUE(volume.has_user_value());
    EXPECT_FALSE(volume.modified());

    volume.reset();
    EXPECT_FALSE(volume.has_user_value());
    EXPECT_EQ(volume.get(), 80);
}

TEST(Config, ClampAndSanitize) {
    Registry registry;
    Var<int> scale{registry, "ui.scale", 100, {.min = 50, .max = 200}};
    scale.set(10);
    EXPECT_EQ(scale.get(), 50);
    scale.set(500);
    EXPECT_EQ(scale.get(), 200);

    Var<int> even{registry, "even", 2, {.sanitize = [](int value) { return value & ~1; }}};
    even.set(7);
    EXPECT_EQ(even.get(), 6);

    Overlay overlay{"test"};
    overlay.set(scale, 1);
    EXPECT_EQ(scale.get(), 50);
}

TEST(Config, SaveWritesOnlyUserValues) {
    const auto dir = make_test_dir("save");
    const auto path = dir / "nested" / "config.json";
    Registry registry;
    Var<bool> vsync{registry, "video.vsync", true};
    Var<int> volume{registry, "audio.volume", 80};
    Var<std::string> name{registry, "game.name", "Samus"};
    Var<float> sensitivity{registry, "input.sensitivity", 1.0f};
    Var<Mode> mode{registry, "video.mode", Mode::Classic};

    EXPECT_EQ(registry.load({.path = path, .version = 3}).code, ErrorCode::FileMissing);
    volume.set(50);
    name.set("Ridley");
    sensitivity.set(1.1f);
    mode.set(Mode::Enhanced);
    ASSERT_TRUE(registry.save());

    const auto saved = read_json(path);
    EXPECT_EQ(saved, (json{
                         {"$version", 3},
                         {"audio.volume", 50},
                         {"game.name", "Ridley"},
                         {"input.sensitivity", 1.1},
                         {"video.mode", "enhanced"},
                     }));
    EXPECT_FALSE(registry.dirty());
    EXPECT_FALSE(fs::exists(path.string() + ".tmp"));

    Registry reloaded;
    Var<bool> vsync2{reloaded, "video.vsync", true};
    Var<int> volume2{reloaded, "audio.volume", 80};
    Var<std::string> name2{reloaded, "game.name", "Samus"};
    Var<float> sensitivity2{reloaded, "input.sensitivity", 1.0f};
    Var<Mode> mode2{reloaded, "video.mode", Mode::Classic};
    ASSERT_TRUE(reloaded.load({.path = path, .version = 3}));
    EXPECT_FALSE(vsync2.has_user_value());
    EXPECT_EQ(volume2.get(), 50);
    EXPECT_EQ(name2.get(), "Ridley");
    EXPECT_EQ(sensitivity2.get(), 1.1f);
    EXPECT_EQ(mode2.get(), Mode::Enhanced);
    EXPECT_FALSE(reloaded.dirty());
}

TEST(Config, UnknownKeysSurvive) {
    const auto dir = make_test_dir("unknown");
    const auto path = dir / "config.json";
    write_text(path, R"({"mods.foo.enabled": true, "audio.volume": 10})");

    Registry registry;
    Var<int> volume{registry, "audio.volume", 80};
    ASSERT_TRUE(registry.load(options_for(path)));
    EXPECT_EQ(volume.get(), 10);
    volume.set(20);
    ASSERT_TRUE(registry.save());
    EXPECT_EQ(read_json(path),
        (json{{"$version", 0}, {"audio.volume", 20}, {"mods.foo.enabled", true}}));
}

TEST(Config, InvalidValuesKeepRawUntilSet) {
    const auto dir = make_test_dir("invalid");
    const auto path = dir / "config.json";
    write_text(path, R"({"video.mode": "ultra", "audio.volume": 300, "flag": "yes"})");

    Registry registry;
    Var<Mode> mode{registry, "video.mode", Mode::Classic};
    Var<std::uint8_t> volume{registry, "audio.volume", 80};
    Var<bool> flag{registry, "flag", false};
    ASSERT_TRUE(registry.load(options_for(path)));
    EXPECT_EQ(mode.get(), Mode::Classic);
    EXPECT_EQ(volume.get(), 80);
    EXPECT_FALSE(flag.get());

    // A newer build's value survives a save from this build.
    flag.set(true);
    ASSERT_TRUE(registry.save());
    auto saved = read_json(path);
    EXPECT_EQ(saved["video.mode"], "ultra");
    EXPECT_EQ(saved["audio.volume"], 300);
    EXPECT_EQ(saved["flag"], true);

    // Setting the var replaces the raw value.
    mode.set(Mode::Off);
    ASSERT_TRUE(registry.save());
    saved = read_json(path);
    EXPECT_EQ(saved["video.mode"], "off");
}

TEST(Config, ParseFailureMovesFileAside) {
    const auto dir = make_test_dir("parse");
    const auto path = dir / "config.json";
    write_text(path, "{ not json");

    Registry registry;
    Var<int> volume{registry, "audio.volume", 80};
    const auto status = registry.load(options_for(path));
    EXPECT_EQ(status.code, ErrorCode::ParseFailed);
    EXPECT_FALSE(fs::exists(path));
    EXPECT_EQ(read_text(path.string() + ".bad"), "{ not json");

    volume.set(5);
    ASSERT_TRUE(registry.save());
    EXPECT_EQ(read_json(path)["audio.volume"], 5);
    EXPECT_EQ(read_text(path.string() + ".bad"), "{ not json");
}

TEST(Config, CommentsAreAccepted) {
    const auto dir = make_test_dir("comments");
    const auto path = dir / "config.json";
    write_text(path, "{\n  // hand-edited\n  \"audio.volume\": 12\n}");

    Registry registry;
    Var<int> volume{registry, "audio.volume", 80};
    ASSERT_TRUE(registry.load(options_for(path)));
    EXPECT_EQ(volume.get(), 12);
}

TEST(Config, LateRegistrationAndUnregistration) {
    const auto dir = make_test_dir("late");
    const auto path = dir / "config.json";
    write_text(path, R"({"mods.foo.speed": 3})");

    Registry registry;
    ASSERT_TRUE(registry.load(options_for(path)));
    {
        auto speed = std::make_unique<Var<std::int64_t>>(registry, "mods.foo.speed", 1);
        EXPECT_EQ(speed->get(), 3);
        speed->set(4);
        EXPECT_EQ(registry.find("mods.foo.speed"), speed.get());
    }
    EXPECT_EQ(registry.find("mods.foo.speed"), nullptr);

    // The unregistered var's user value is kept for saving and re-registration.
    ASSERT_TRUE(registry.save());
    EXPECT_EQ(read_json(path)["mods.foo.speed"], 4);
    Var<std::int64_t> again{registry, "mods.foo.speed", 1};
    EXPECT_EQ(again.get(), 4);
}

TEST(Config, OverlaysStackByPriority) {
    Registry registry;
    Var<bool> cheats{registry, "game.cheats", false};
    Overlay speedrun{"speedrun", {.priority = 100}};
    Overlay preset{"preset", {.priority = 10}};

    cheats.set(true);
    preset.set(cheats, true);
    speedrun.set(cheats, false);
    EXPECT_FALSE(cheats.get());
    EXPECT_EQ(cheats.source(), Source::Overlay);
    EXPECT_EQ(cheats.overlay(), &speedrun);
    EXPECT_TRUE(cheats.locked());

    // A user set doesn't get through a locking overlay, but is kept underneath.
    cheats.set(true);
    EXPECT_FALSE(cheats.get());
    EXPECT_EQ(cheats.user_value(), true);

    speedrun.clear();
    EXPECT_EQ(cheats.overlay(), &preset);
    EXPECT_TRUE(cheats.get());
    preset.unset(cheats);
    EXPECT_EQ(cheats.source(), Source::User);
    EXPECT_TRUE(preset.empty());
}

TEST(Config, OverlayDestructionAndVarDestruction) {
    Registry registry;
    Var<int> value{registry, "value", 1};
    {
        Overlay overlay{"scoped"};
        overlay.set(value, 5);
        EXPECT_EQ(value.get(), 5);
    }
    EXPECT_EQ(value.get(), 1);

    Overlay overlay{"outlives"};
    {
        Var<int> shortLived{registry, "short", 1};
        overlay.set(shortLived, 2);
        EXPECT_TRUE(overlay.contains(shortLived));
    }
    EXPECT_TRUE(overlay.empty());
}

TEST(Config, CliOverridesReleaseOnUserSet) {
    Registry registry;
    Var<bool> fullscreen{registry, "video.fullscreen", false};
    Var<Mode> mode{registry, "video.mode", Mode::Classic};
    Var<double> gamma{registry, "video.gamma", 1.0};
    const std::vector<std::string> overrides{
        "video.fullscreen=on",
        "video.mode=Enhanced",
        "video.gamma=2.5",
        "mods.later=7",
    };
    ASSERT_TRUE(registry.apply_overrides(overrides));
    EXPECT_TRUE(fullscreen.get());
    EXPECT_EQ(mode.get(), Mode::Enhanced);
    EXPECT_EQ(gamma.get(), 2.5);
    EXPECT_EQ(fullscreen.overlay(), &registry.cli_overlay());
    EXPECT_FALSE(fullscreen.locked());
    EXPECT_FALSE(registry.dirty());

    fullscreen.set(false);
    EXPECT_FALSE(fullscreen.get());
    EXPECT_EQ(fullscreen.source(), Source::User);

    // Unknown keys apply when the var registers.
    Var<int> later{registry, "mods.later", 1};
    EXPECT_EQ(later.get(), 7);
    EXPECT_EQ(later.source(), Source::Overlay);
}

TEST(Config, CliOverrideErrors) {
    Registry registry;
    Var<bool> flag{registry, "flag", false};
    Var<std::uint8_t> small{registry, "small", 1};
    const std::vector<std::string> overrides{"flag=maybe", "noequals", "small=300", "flag=1"};
    const auto status = registry.apply_overrides(overrides);
    EXPECT_EQ(status.code, ErrorCode::InvalidValue);
    EXPECT_NE(status.message.find("maybe"), std::string::npos);
    // Later valid assignments still apply.
    EXPECT_TRUE(flag.get());
    EXPECT_EQ(small.get(), 1);
}

TEST(Config, CliParsing) {
    Registry registry;
    Var<int> hex{registry, "hex", 0};
    Var<std::int8_t> negative{registry, "negative", 0};
    Var<std::string> text{registry, "text", ""};
    Var<Layout> layout{registry, "layout", Layout{}};
    Var<Mode> mode{registry, "mode", Mode::Off};

    hex.set_string("0x10");
    EXPECT_EQ(hex.get(), 16);
    negative.set_string("-128");
    EXPECT_EQ(negative.get(), -128);
    EXPECT_THROW(negative.set_string("-129"), borealis::config::InvalidValueError);
    text.set_string("a,b=c");
    EXPECT_EQ(text.get(), "a,b=c");
    layout.set_string(R"({"x": 1, "y": 2})");
    EXPECT_EQ(layout.get(), (Layout{1, 2}));
    EXPECT_THROW(layout.set_string("{"), borealis::config::InvalidValueError);
    mode.set_string("2");
    EXPECT_EQ(mode.get(), Mode::Enhanced);
    EXPECT_THROW(mode.set_string("7"), borealis::config::InvalidValueError);
}

TEST(Config, DecodeRules) {
    Registry registry;
    Var<Mode> mode{registry, "mode", Mode::Off};
    Var<int> integer{registry, "integer", 0};
    Var<bool> flag{registry, "flag", false};
    Var<float> real{registry, "real", 0.0f};

    mode.set_json(1);  // integers from older files still read
    EXPECT_EQ(mode.get(), Mode::Classic);
    EXPECT_THROW(mode.set_json(9), borealis::config::InvalidValueError);
    integer.set_json(100.0);
    EXPECT_EQ(integer.get(), 100);
    EXPECT_THROW(integer.set_json(1.5), borealis::config::InvalidValueError);
    EXPECT_THROW(integer.set_json("1"), borealis::config::InvalidValueError);
    flag.set_json(1);
    EXPECT_TRUE(flag.get());
    EXPECT_THROW(flag.set_json(2), borealis::config::InvalidValueError);
    EXPECT_THROW(real.set_json("x"), borealis::config::InvalidValueError);

    EXPECT_EQ(mode.to_json(), "classic");
    EXPECT_EQ(mode.to_string(), "classic");
    EXPECT_EQ(integer.to_string(), "100");
}

TEST(Config, ChangeNotifications) {
    Registry registry;
    Var<int> value{registry, "value", 1};
    std::vector<std::pair<int, int>> changes;
    auto connection = value.on_change(
        [&](const int& current, const int& previous) { changes.emplace_back(current, previous); });

    value.set(2);
    value.set(2);  // unchanged effective value
    Overlay overlay{"test"};
    overlay.set(value, 3);
    value.set(4);  // hidden by the overlay
    overlay.clear();
    value.reset();
    EXPECT_EQ(changes, (std::vector<std::pair<int, int>>{{2, 1}, {3, 2}, {4, 3}, {1, 4}}));

    connection.disconnect();
    EXPECT_FALSE(connection.connected());
    value.set(9);
    EXPECT_EQ(changes.size(), 4u);
}

TEST(Config, ConnectionLifetime) {
    Registry registry;
    Var<int> value{registry, "value", 1};
    int calls = 0;
    {
        auto scoped = value.on_change([&] { ++calls; });
        value.set(2);
    }
    value.set(3);
    EXPECT_EQ(calls, 1);

    value.on_change([&] { ++calls; }).release();
    value.set(4);
    EXPECT_EQ(calls, 2);

    borealis::config::Connection outlives;
    {
        Var<int> shortLived{registry, "short", 1};
        outlives = shortLived.on_change([&] { ++calls; });
        EXPECT_TRUE(outlives.connected());
    }
    EXPECT_FALSE(outlives.connected());
    outlives.disconnect();
}

TEST(Config, ObserveAppliesImmediately) {
    Registry registry;
    Var<int> value{registry, "value", 7};
    std::vector<int> seen;
    auto connection = value.observe([&](int current) { seen.push_back(current); });
    value.set(8);
    EXPECT_EQ(seen, (std::vector<int>{7, 8}));
}

TEST(Config, NestedWritesDontRenotify) {
    Registry registry;
    Var<int> value{registry, "value", 0};
    Var<int> mirror{registry, "mirror", 0};
    int valueCalls = 0;
    auto clampToTen = value.on_change([&](int current) {
        ++valueCalls;
        if (current > 10) {
            value.set(10);
        }
    });
    auto mirrorValue = value.on_change([&](int current) { mirror.set(current); });

    value.set(20);
    EXPECT_EQ(value.get(), 10);
    EXPECT_EQ(valueCalls, 1);
    EXPECT_EQ(mirror.get(), 10);
}

TEST(Config, LoadNotifies) {
    const auto dir = make_test_dir("notify");
    const auto path = dir / "config.json";
    write_text(path, R"({"value": 5})");

    Registry registry;
    Var<int> value{registry, "value", 1};
    int seen = 0;
    auto connection = value.on_change([&](int current) { seen = current; });
    ASSERT_TRUE(registry.load(options_for(path)));
    EXPECT_EQ(seen, 5);
    EXPECT_FALSE(registry.dirty());
}

TEST(Config, AliasesMigrateKeys) {
    const auto dir = make_test_dir("alias");
    const auto path = dir / "config.json";
    write_text(path, R"({"game.enableFpsOverlay": true, "old.unused": 1})");

    Registry registry;
    Var<bool> overlay{registry, "video.fpsOverlay", false, {.aliases = {"game.enableFpsOverlay"}}};
    ASSERT_TRUE(registry.load(options_for(path)));
    EXPECT_TRUE(overlay.get());
    EXPECT_TRUE(registry.dirty());
    registry.update();
    EXPECT_EQ(read_json(path),
        (json{{"$version", 0}, {"old.unused", 1}, {"video.fpsOverlay", true}}));
}

TEST(Config, AliasesForLateRegistration) {
    const auto dir = make_test_dir("alias-late");
    const auto path = dir / "config.json";
    write_text(path, R"({"old.key": 4})");

    Registry registry;
    ASSERT_TRUE(registry.load(options_for(path)));
    Var<int> value{registry, "new.key", 0, {.aliases = {"old.key"}}};
    EXPECT_EQ(value.get(), 4);
    ASSERT_TRUE(registry.flush());
    EXPECT_EQ(read_json(path), (json{{"$version", 0}, {"new.key", 4}}));
}

TEST(Config, MigrationRunsForOlderFiles) {
    const auto dir = make_test_dir("migrate");
    const auto path = dir / "config.json";
    write_text(path, R"({"game.interp": true})");

    Registry registry;
    Var<Mode> interp{registry, "game.interp", Mode::Off};
    int migratedFrom = -1;
    ASSERT_TRUE(registry.load({
        .path = path,
        .version = 2,
        .migrate =
            [&](json& root, int fromVersion) {
                migratedFrom = fromVersion;
                if (root["game.interp"].is_boolean()) {
                    root["game.interp"] = root["game.interp"].get<bool>() ? "enhanced" : "off";
                }
            },
    }));
    EXPECT_EQ(migratedFrom, 0);
    EXPECT_EQ(interp.get(), Mode::Enhanced);
    ASSERT_TRUE(registry.flush());
    EXPECT_EQ(read_json(path), (json{{"$version", 2}, {"game.interp", "enhanced"}}));

    // Current files aren't migrated again.
    Registry current;
    Var<Mode> interp2{current, "game.interp", Mode::Off};
    migratedFrom = -1;
    ASSERT_TRUE(current.load({
        .path = path,
        .version = 2,
        .migrate = [&](json&, int fromVersion) { migratedFrom = fromVersion; },
    }));
    EXPECT_EQ(migratedFrom, -1);
}

TEST(Config, AutosaveDebounce) {
    const auto dir = make_test_dir("autosave");
    const auto path = dir / "config.json";

    Registry registry;
    Var<int> value{registry, "value", 0};
    ASSERT_EQ(registry.load({.path = path, .autosaveDelay = std::chrono::hours{1}}).code,
        ErrorCode::FileMissing);
    value.set(1);
    registry.update();
    EXPECT_FALSE(fs::exists(path));
    ASSERT_TRUE(registry.flush());
    EXPECT_EQ(read_json(path)["value"], 1);

    Registry immediate;
    Var<int> other{immediate, "value", 0};
    ASSERT_EQ(immediate.load(options_for(path)).code, ErrorCode::None);
    other.set(2);
    immediate.update();
    EXPECT_EQ(read_json(path)["value"], 2);
    EXPECT_FALSE(immediate.dirty());
}

TEST(Config, NonPersistentVars) {
    const auto dir = make_test_dir("transient");
    const auto path = dir / "config.json";
    write_text(path, R"({"debug.wireframe": true})");

    Registry registry;
    Var<bool> wireframe{registry, "debug.wireframe", false, {.persist = false}};
    ASSERT_TRUE(registry.load(options_for(path)));
    EXPECT_FALSE(wireframe.get());
    wireframe.set(true);
    EXPECT_FALSE(registry.dirty());
    ASSERT_TRUE(registry.save());
    // The file's entry is preserved, not overwritten by the runtime value.
    EXPECT_EQ(read_json(path), (json{{"$version", 0}, {"debug.wireframe", true}}));
}

TEST(Config, SaveWithoutLoad) {
    Registry registry;
    EXPECT_EQ(registry.save().code, ErrorCode::NoPath);
    EXPECT_TRUE(registry.flush());
}

TEST(Config, VarArrayKeys) {
    Registry registry;
    VarArray<bool, 4> leds{registry, "input.led_port{}", true};
    EXPECT_EQ(leds[2].key(), "input.led_port2");
    leds[3].set(false);
    EXPECT_FALSE(registry.find<bool>("input.led_port3")->get());
    int enabled = 0;
    for (const auto& led : leds) {
        enabled += led.get() ? 1 : 0;
    }
    EXPECT_EQ(enabled, 3);
}

TEST(Config, TypeErasedAccess) {
    Registry registry;
    Var<int> value{registry, "b.value", 1};
    Var<bool> flag{registry, "a.flag", false};
    EXPECT_EQ(registry.find<int>("b.value"), &value);
    EXPECT_EQ(registry.find<bool>("b.value"), nullptr);
    EXPECT_EQ(registry.find("missing"), nullptr);

    std::vector<std::string> keys;
    registry.for_each([&](borealis::config::VarBase& var) { keys.push_back(var.key()); });
    EXPECT_EQ(keys, (std::vector<std::string>{"a.flag", "b.value"}));

    borealis::config::VarBase& base = value;
    base.set_json(5);
    EXPECT_EQ(value.get(), 5);
    int calls = 0;
    auto connection = base.on_change([&] { ++calls; });
    base.reset();
    EXPECT_EQ(calls, 1);
}

TEST(Config, BlockedSaveAfterUnreadableFile) {
    const auto dir = make_test_dir("blocked");
    const auto path = dir / "config.json";
    fs::create_directories(path);  // a directory can't be read as the config file

    Registry registry;
    Var<int> value{registry, "value", 0};
    const auto status = registry.load(options_for(path));
    EXPECT_NE(status.code, ErrorCode::None);
    EXPECT_NE(status.code, ErrorCode::FileMissing);
    value.set(1);
    EXPECT_EQ(registry.save().code, ErrorCode::WriteFailed);
    EXPECT_TRUE(fs::is_directory(path));
}

}  // namespace
