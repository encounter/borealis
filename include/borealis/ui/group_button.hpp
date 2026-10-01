#pragma once

#include <borealis/ui/select_button.hpp>

namespace borealis::ui {

class GroupButton : public SelectButton {
public:
    struct Props {
        Rml::String text;
        std::function<bool()> isSelected;
        std::function<bool()> isDisabled;
    };

    GroupButton(Rml::Element* parent, Props props);

    void update() override;
    bool selected() const override;
    bool disabled() const override;

private:
    std::function<bool()> mIsSelected;
    std::function<bool()> mIsDisabled;
};

}  // namespace borealis::ui
