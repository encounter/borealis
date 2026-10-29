#pragma once

#include <RmlUi/Core/Element.h>
#include <vector>

namespace borealis::ui::detail {

bool pointer_claimed(Rml::Element* target) noexcept;
Rml::String stylesheet_links(const std::vector<Rml::String>& sheets);

}  // namespace borealis::ui::detail
