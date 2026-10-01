#pragma once

#include <borealis/ui/component.hpp>

namespace borealis::ui {

class ClampedText final : public Component {
public:
    ClampedText(Rml::Element* root, Rml::String text, int maxLines);
    void update() override;

private:
    Rml::String mSource;
    Rml::ElementText* mText;
    int mMaxLines;
    float mWidth = 0;
    Rml::FontFaceHandle mFont = 0;
};

}  // namespace borealis::ui
