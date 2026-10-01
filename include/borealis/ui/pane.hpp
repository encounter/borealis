#pragma once

#include <borealis/ui/button.hpp>
#include <borealis/ui/component.hpp>
#include <borealis/ui/group_button.hpp>
#include <borealis/ui/select_button.hpp>

namespace borealis::ui {

class Pane : public FluentComponent<Pane> {
public:
    enum class Type {
        Controlled,
        Uncontrolled,
    };

    explicit Pane(Rml::Element* parent, Type type);

    bool focus() override;
    bool focus_last();

    void set_selected_item(int index);
    Component& register_control(
        Component& component, Pane& nextPane, std::function<void(Pane&)> callback);

    ControlledButton& add_button(ControlledButton::Props props) {
        return add_child<ControlledButton>(std::move(props));
    }
    GroupButton& add_group_button(GroupButton::Props props) {
        return add_child<GroupButton>(std::move(props));
    }
    Button& add_button(Rml::String text) { return add_child<Button>(std::move(text)); }
    ControlledSelectButton& add_select_button(ControlledSelectButton::Props props) {
        return add_child<ControlledSelectButton>(std::move(props));
    }
    void clear();

private:
    Type mType;
};

}  // namespace borealis::ui
