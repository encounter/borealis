#include <borealis/ui/bool_button.hpp>

namespace borealis::ui {

BoolButton::BoolButton(Rml::Element* parent, Props props)
    : BaseControlledSelectButton(parent,
          {
              .key = std::move(props.key),
              .icon = std::move(props.icon),
          }),
      mGetValue(std::move(props.getValue)), mSetValue(std::move(props.setValue)),
      mIsDisabled(std::move(props.isDisabled)), mIsModified(std::move(props.isModified)),
      mValueOverride(std::move(props.valueOverride)) {}

bool BoolButton::modified() const {
    if (mIsModified) {
        return mIsModified();
    }
    return BaseControlledSelectButton::modified();
}

bool BoolButton::disabled() const {
    if (mIsDisabled) {
        return mIsDisabled();
    }
    return BaseControlledSelectButton::disabled();
}

Rml::String BoolButton::format_value() {
    if (mValueOverride) {
        if (std::string value = mValueOverride(); !value.empty()) {
            return value;
        }
    }

    return mGetValue() ? "On" : "Off";
}

bool BoolButton::handle_nav_command(NavCommand cmd) {
    if (cmd == NavCommand::Confirm || cmd == NavCommand::Left || cmd == NavCommand::Right) {
        const bool newValue = !mGetValue();
        mSetValue(newValue);
        play_nav_sound(newValue ? NavSound::ItemEnable : NavSound::ItemDisable);
        return true;
    }
    return false;
}

}  // namespace borealis::ui