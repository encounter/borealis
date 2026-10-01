#pragma once

#include <borealis/ui/nav_group.hpp>
#include <borealis/ui/popover.hpp>

namespace borealis::ui {

class ContextMenu : public Popover {
public:
    struct Item {
        Rml::String text;
        Rml::String icon;
        std::function<void()> onPressed;
        bool enabled = true;
        bool destructive = false;
        bool separatorBefore = false;
        bool selected = false;
    };

    class Binding {
    public:
        Binding(Document& owner, Rml::Element* root, Rml::String selector,
            std::function<std::vector<Item>(Rml::Element*)> items);
        ~Binding();

        void dismiss();

    private:
        ContextMenu* mMenu = nullptr;
        ScopedEventListener mMouseDown;
    };

    ContextMenu(Rml::Element* anchor, std::vector<Item> items,
        std::optional<Rml::Vector2f> position = std::nullopt);

    bool focus() override;

private:
    NavGroup mNavigation;
    Component* mInitialFocus = nullptr;
};

}  // namespace borealis::ui
