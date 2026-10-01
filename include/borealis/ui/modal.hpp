#pragma once

#include <borealis/ui/button.hpp>
#include <borealis/ui/pane.hpp>
#include <borealis/ui/window.hpp>

#include <optional>

namespace borealis::ui {
class Modal;

struct ModalAction {
    Rml::String label;
    std::function<void(Modal&)> onPressed;
    std::function<bool()> isDisabled;
    Rml::String icon;
};

class Modal : public WindowSmall {
public:
    struct Props {
        Rml::String title;
        std::optional<Rml::String> bodyText;
        Rml::String bodyRml;
        std::vector<ModalAction> actions;
        std::function<void(Modal&)> onDismiss;
        Rml::String variant;
        Rml::String icon = "";
        bool isVertical = false;
    };

    explicit Modal(Props props);

    void update() override;
    bool focus() override;

    Pane& content_pane();
    void set_body(const Rml::String& bodyRml);
    void set_body_text(const Rml::String& bodyText);
    void set_icon(const Rml::String& icon);

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    void add_action(ModalAction action);
    void dismiss();

    Props mProps;
    Rml::Element* mContentRoot = nullptr;
    std::unique_ptr<Pane> mContentPane;
    std::function<void(Modal&)> mPendingAction;
    std::vector<std::unique_ptr<Button>> mButtons;
};

}  // namespace borealis::ui
