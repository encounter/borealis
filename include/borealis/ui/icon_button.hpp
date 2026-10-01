#pragma once

#include <borealis/ui/button.hpp>
#include <borealis/ui/tooltip.hpp>

#include <string_view>

namespace borealis::ui {

const char* material_icon(std::string_view name);

class IconButton : public ControlledButton {
public:
    struct Props {
        Rml::String icon;
        Rml::String label;
        std::function<bool()> isSelected;
        std::function<bool()> isDisabled;
    };

    IconButton(Rml::Element* parent, Props props);
    void set_icon(std::string_view icon);
    void set_label(const Rml::String& label);
    void set_tooltip(const Rml::String& text) override;

private:
    Rml::Element* mIcon;
    Rml::String mIconName;
    Rml::String mLabel;
    Rml::String mTooltipText;
};

}  // namespace borealis::ui
