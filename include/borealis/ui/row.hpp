#pragma once

#include <borealis/ui/component.hpp>

namespace borealis::ui {

class Row : public FluentComponent<Row> {
public:
    enum class Align { Start, End, Center, SpaceBetween };

    struct Props {
        Align align = Align::Start;
        bool wrap = false;
    };

    Row(Rml::Element* parent, Props props);
    bool focus() override;
    bool focus_from(NavCommand direction) override;
    bool selected() const override;
    void set_selected(bool selected) override;

private:
    Component* mSelected = nullptr;
};

}  // namespace borealis::ui
