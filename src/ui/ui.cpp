#include <borealis/ui/document.hpp>
#include <borealis/ui/input.hpp>

#include "internal.hpp"

#include "borealis/io.hpp"

#include <aurora/aurora.h>
#include <aurora/rmlui.hpp>

#include <ranges>

namespace borealis::ui {
namespace {

bool sInitialized = false;
std::vector<std::unique_ptr<Document>> sDocumentStack;
// Documents that don't participate in the focus stack
std::vector<std::unique_ptr<Document>> sPassiveDocuments;

struct ScopedStyles {
    DocumentScope scope;
    std::string id;
    Rml::SharedPtr<Rml::StyleSheetContainer> sheet;
};

std::vector<ScopedStyles> sScopedStyles;
int sUserScale = 0;
std::string sGlobalStylesheet = "res/rml/global.rcss";
NavSoundHandler sNavSoundHandler = nullptr;

std::vector<const Rml::StyleSheetContainer*> scoped_sheets(DocumentScope scope) {
    std::vector<const Rml::StyleSheetContainer*> sheets;
    for (const auto& entry : sScopedStyles) {
        if (entry.scope == scope) {
            sheets.push_back(entry.sheet.get());
        }
    }
    return sheets;
}

void restyle_scope(DocumentScope scope) {
    const auto sheets = scoped_sheets(scope);
    const auto restyle_documents = [&sheets, scope](auto& documents) {
        for (auto& doc : documents) {
            if (doc != nullptr && doc->scope() == scope && !doc->closed()) {
                doc->restyle(sheets);
            }
        }
    };
    restyle_documents(sDocumentStack);
    restyle_documents(sPassiveDocuments);
}

}  // namespace

bool initialize() noexcept {
    if (sInitialized) {
        return true;
    }
    if (!aurora::rmlui::is_initialized()) {
        return false;
    }

    // register_icon_texture_provider();
    // register_mod_texture_provider();
    // register_remote_texture_provider();
    // Rml::StyleSheetSpecification::RegisterProperty("mod-icon-tint", "transparent", false)
    //     .AddParser("color");
    // Rml::StyleSheetSpecification::RegisterProperty("mod-icon-background", "transparent", false)
    //     .AddParser("color");
    input::initialize();
    sInitialized = true;
    return true;
}

void shutdown() noexcept {
    sDocumentStack.clear();
    sPassiveDocuments.clear();
    input::shutdown();
    sInitialized = false;
}

void handle_event(const SDL_Event& event) noexcept {
    if (!aurora::rmlui::is_initialized()) {
        return;
    }
    if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
        apply_scale();
    }
}

bool register_scoped_styles(DocumentScope scope, std::string id, const std::string& rcss) noexcept {
    auto sheet = Rml::Factory::InstanceStyleSheetString(rcss);
    if (sheet == nullptr) {
        return false;
    }
    const auto it = std::ranges::find_if(sScopedStyles,
        [scope, &id](const ScopedStyles& entry) { return entry.scope == scope && entry.id == id; });
    if (it != sScopedStyles.end()) {
        it->sheet = std::move(sheet);
    } else {
        sScopedStyles.push_back({scope, std::move(id), std::move(sheet)});
    }
    restyle_scope(scope);
    return true;
}

void unregister_scoped_styles(DocumentScope scope, std::string_view id) noexcept {
    const auto erased = std::erase_if(sScopedStyles,
        [scope, id](const ScopedStyles& entry) { return entry.scope == scope && entry.id == id; });
    if (erased != 0) {
        restyle_scope(scope);
    }
}

void apply_scoped_styles(Document& doc) noexcept {
    doc.restyle(scoped_sheets(doc.scope()));
}

Document& push_document(std::unique_ptr<Document> doc, bool show, bool passive) noexcept {
    Document& ret = *doc;
    if (passive) {
        sPassiveDocuments.push_back(std::move(doc));
    } else {
        sDocumentStack.push_back(std::move(doc));
    }
    if (show) {
        ret.show();
    }
    return ret;
}

Document& detail::pop_to_or_push_document(bool (*matches)(Document&),
    const std::function<std::unique_ptr<Document>()>& create,
    const std::function<void(Document&)>& configure) {
    Document* destination = nullptr;
    size_t destinationIndex = 0;
    for (size_t i = sDocumentStack.size(); i > 0; --i) {
        auto& document = *sDocumentStack[i - 1];
        if (!document.closed() && !document.pending_close() && matches(document)) {
            destination = &document;
            destinationIndex = i - 1;
            break;
        }
    }

    if (destination != nullptr) {
        std::vector<Document*> closing;
        for (size_t i = sDocumentStack.size(); i > destinationIndex + 1; --i) {
            closing.push_back(sDocumentStack[i - 1].get());
        }
        for (auto* document : closing) {
            if (!document->closed() && !document->pending_close()) {
                if (document->visible()) {
                    document->hide(true);
                } else {
                    document->force_hide(true);
                }
            }
        }
        configure(*destination);
    } else {
        auto document = create();
        configure(*document);
        if (auto* current = top_document()) {
            current->cover();
        }
        destination = &push_document(std::move(document), false);
    }

    destination->show();
    destination->focus();
    return *destination;
}

void bring_document_to_front(Document& doc) noexcept {
    const auto it = std::ranges::find_if(
        sDocumentStack, [&doc](const auto& entry) { return entry.get() == &doc; });
    if (it == sDocumentStack.end() || std::next(it) == sDocumentStack.end()) {
        return;
    }
    auto entry = std::move(*it);
    sDocumentStack.erase(it);
    sDocumentStack.push_back(std::move(entry));
}

void uncover_top_document() noexcept {
    if (auto* doc = top_document()) {
        doc->uncover();
    }
}

Document* find_document(DocumentScope scope) noexcept {
    for (auto& doc : std::views::reverse(sDocumentStack)) {
        if (!doc->closed() && doc->scope() == scope) {
            return doc.get();
        }
    }
    return nullptr;
}

void close_all_documents() noexcept {
    for (auto& doc : sDocumentStack) {
        if (!doc->closed()) {
            doc->force_hide(!doc->permanent());
        }
    }
}

bool any_document_visible() noexcept {
    return std::ranges::any_of(sDocumentStack,
        [](const auto& doc) { return doc && doc->visible() && !doc->pending_close(); });
}

// TODO hmm
bool game_obscured_below(const Document& doc) noexcept {
    for (const auto& entry : sDocumentStack) {
        if (entry.get() == &doc) {
            break;
        }
        if (entry->active() && entry->obscures_game()) {
            return true;
        }
    }
    return false;
}

Document* top_document() noexcept {
    for (auto& doc : std::views::reverse(sDocumentStack)) {
        if (doc->active()) {
            return doc.get();
        }
    }
    return nullptr;
}

void update() noexcept {
    if (!aurora::rmlui::is_initialized()) {
        return;
    }

    input::update();
    const auto update_documents = [](auto& documents) {
        const size_t count = documents.size();
        for (size_t i = 0; i < count && i < documents.size(); ++i) {
            Document* doc = documents[i].get();
            if (doc != nullptr && !doc->closed()) {
                doc->update();
            }
        }
    };
    update_documents(sDocumentStack);
    update_documents(sPassiveDocuments);

    // Remove closed documents
    {
        const auto [first, last] =
            std::ranges::remove_if(sDocumentStack, [](const auto& doc) { return doc->closed(); });
        sDocumentStack.erase(first, last);
    }
    {
        const auto [first, last] = std::ranges::remove_if(
            sPassiveDocuments, [](const auto& doc) { return doc->closed(); });
        sPassiveDocuments.erase(first, last);
    }

    // Keep focus on the highest active document.
    if (aurora::rmlui::get_context() != nullptr) {
        for (auto& doc : std::views::reverse(sDocumentStack)) {
            if (doc->active() && (doc->has_focus() || doc->focus())) {
                break;
            }
        }
    }
}

bool detail::pointer_claimed(Rml::Element* target) noexcept {
    if (target == nullptr) {
        return false;
    }
    for (const auto& doc : sDocumentStack) {
        if (doc != nullptr && !doc->closed() && doc->owns_element(target)) {
            return true;
        }
    }
    for (const auto& doc : sPassiveDocuments) {
        if (doc != nullptr && !doc->closed() && doc->owns_element(target) && doc->claims_pointer(target)) {
            return true;
        }
    }
    return false;
}

void set_global_stylesheet(std::string path) noexcept {
    sGlobalStylesheet = std::move(path);
}

const std::string& global_stylesheet() noexcept {
    return sGlobalStylesheet;
}

bool load_font(const std::filesystem::path& filename, bool fallback) noexcept {
    return Rml::LoadFontFace(io::fs_path_to_string(resource_path(filename)), fallback);
}

std::filesystem::path resource_path(const std::filesystem::path& filename) noexcept {
    return std::filesystem::path("res") / filename;
}

std::string escape(std::string_view str) noexcept {
    std::string result;
    result.reserve(str.size());
    for (const char c : str) {
        switch (c) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        default:
            result += c;
            break;
        }
    }
    return result;
}

Rml::Element* append(Rml::Element* parent, const Rml::String& tag) noexcept {
    if (parent == nullptr) {
        return nullptr;
    }
    auto* doc = parent->GetOwnerDocument();
    if (doc == nullptr) {
        return nullptr;
    }
    return parent->AppendChild(doc->CreateElement(tag));
}

Rml::Element* append_text(Rml::Element* parent, const Rml::String& text) noexcept {
    if (parent == nullptr) {
        return nullptr;
    }
    auto* doc = parent->GetOwnerDocument();
    if (doc == nullptr) {
        return nullptr;
    }
    return parent->AppendChild(doc->CreateTextNode(text));
}

Rml::Element* append_text_element(
    Rml::Element* parent, const Rml::String& tag, const Rml::String& text) noexcept {
    auto* element = append(parent, tag);
    append_text(element, text);
    return element;
}

void clear_children(Rml::Element* parent) noexcept {
    if (parent == nullptr) {
        return;
    }
    while (parent->GetNumChildren() > 0) {
        parent->RemoveChild(parent->GetFirstChild());
    }
}

void set_text_content(Rml::Element* parent, const Rml::String& text) noexcept {
    if (parent == nullptr) {
        return;
    }
    if (!text.empty() && parent->GetNumChildren() == 1) {
        if (auto* element = dynamic_cast<Rml::ElementText*>(parent->GetFirstChild())) {
            // RmlUi only dirties layout when the node's text changes.
            element->SetText(text);
            return;
        }
    }
    clear_children(parent);
    if (!text.empty()) {
        append_text(parent, text);
    }
}

void set_display(Rml::Element* element, Rml::Style::Display display) noexcept {
    const Rml::Property value{display};
    const auto* current = element->GetLocalProperty(Rml::PropertyId::Display);
    if (current == nullptr || *current != value) {
        element->SetProperty(Rml::PropertyId::Display, value);
    }
}

NavCommand nav_command(const Rml::Event& event) noexcept {
    if (event.GetType() != kNavCommandEvent) {
        return NavCommand::None;
    }
    const int command = event.GetParameter<int>("command", 0);
    if (command <= static_cast<int>(NavCommand::None) || command > static_cast<int>(NavCommand::Menu)) {
        return NavCommand::None;
    }
    return static_cast<NavCommand>(command);
}

Insets safe_area_insets(Rml::Context* context) noexcept {
    if (context == nullptr) {
        return {};
    }

    auto* window = aurora_get_window();
    if (window == nullptr) {
        return {};
    }

    const AuroraWindowSize windowSize = aurora_get_window_size();
    if (windowSize.width == 0 || windowSize.height == 0) {
        return {};
    }

    SDL_Rect safeRect{};
    if (!SDL_GetWindowSafeArea(window, &safeRect)) {
        return {};
    }

    const Rml::Vector2i contextSize = context->GetDimensions();
    const float scaleX = static_cast<float>(contextSize.x) / static_cast<float>(windowSize.width);
    const float scaleY = static_cast<float>(contextSize.y) / static_cast<float>(windowSize.height);

    const float safeRight = static_cast<float>(safeRect.x + safeRect.w);
    const float safeBottom = static_cast<float>(safeRect.y + safeRect.h);
    return {
        .top = std::max(0.0f, static_cast<float>(safeRect.y)) * scaleY,
        .right = std::max(0.0f, static_cast<float>(windowSize.width) - safeRight) * scaleX,
        .bottom = std::max(0.0f, static_cast<float>(windowSize.height) - safeBottom) * scaleY,
        .left = std::max(0.0f, static_cast<float>(safeRect.x)) * scaleX,
    };
}

void set_user_scale(int percent) noexcept {
    sUserScale = std::max(percent, 0);
    apply_scale();
}

void apply_scale() noexcept {
    auto scale = 0.0f;
    if (sUserScale != 0) {
        const auto displayScale = aurora_get_window_size().scale;
        scale = static_cast<float>(sUserScale) / 100.0f * (displayScale > 0.0f ? displayScale : 1.0f);
    }
    aurora::rmlui::set_ui_scale(scale);
}

void set_nav_sound_handler(NavSoundHandler handler) noexcept {
    sNavSoundHandler = handler;
}

void play_nav_sound(NavSound sound) noexcept {
    if (sNavSoundHandler != nullptr && sound != NavSound::None) {
        sNavSoundHandler(sound);
    }
}

}  // namespace borealis::ui
