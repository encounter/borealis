#pragma once

#include <borealis/ui/button.hpp>
#include <borealis/ui/document.hpp>
#include <borealis/ui/tab_bar.hpp>

#include <memory>

namespace borealis::ui {

class MenuBar : public Document {
public:
    struct Props {
        std::vector<Rml::String> styleSheets;
    };

    MenuBar() : MenuBar(Props{}) {}
    explicit MenuBar(Props props);

    MenuBar(const MenuBar&) = delete;
    MenuBar& operator=(const MenuBar&) = delete;

    void show() override;
    void hide(bool close) override;
    void update() override;
    bool focus() override;
    bool visible() const override;
    bool permanent() const override { return true; }

    static void refresh_tabs();

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;
    virtual void build_tabs() = 0;

    Rml::Element* mRoot;
    std::unique_ptr<TabBar> mTabBar;

private:
    void update_safe_area() noexcept;

    Insets mTabBarPadding;
    float mTopMargin = 0.f;
    Rml::String mFocusedTabTitle;
};

}  // namespace borealis::ui
