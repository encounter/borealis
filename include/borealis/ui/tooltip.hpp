#pragma once

#include <borealis/ui/event.hpp>

namespace borealis::ui {

class Tooltip {
public:
    Tooltip(Rml::Element* anchor, const Rml::String& label);
    ~Tooltip();

    Tooltip(const Tooltip&) = delete;
    Tooltip& operator=(const Tooltip&) = delete;

    void set_label(const Rml::String& label);
    void update();

private:
    Rml::Element* mAnchor;
    Rml::Element* mRoot;
    bool mFollowsFocus;
    ScopedEventListener mMouseMove;
    ScopedEventListener mMouseDown;
    ScopedEventListener mNavCommand;
};

}  // namespace borealis::ui
