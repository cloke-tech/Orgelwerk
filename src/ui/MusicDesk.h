#pragma once
#include "Theme.h"
#include "../PluginProcessor.h"
#include <set>

namespace orgel
{
// The music stand in the middle of the console: engraved pages that follow playback.
class MusicDesk : public juce::Component
{
public:
    explicit MusicDesk (OrgelProcessor& p);
    void setPiece (std::shared_ptr<Piece> piece);
    void update(); // ~30x per second
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override;
    void showPage (int first);
    bool isZoomed() const { return zoomed; }
    void setZoomed (bool z) { zoomButton.setToggleState (z, juce::sendNotification); }
    bool isFollowing() const { return follow; }
    void setFollowing (bool f) { followButton.setToggleState (f, juce::sendNotification); }

private:
    struct Page
    {
        std::unique_ptr<juce::Drawable> drawable;
        std::map<juce::String, juce::Drawable*> ids;
        // colours before highlighting (notes keep their manual colour)
        std::map<juce::DrawableShape*, std::pair<juce::FillType, juce::FillType>> shapeColours;
        std::map<juce::DrawableText*, juce::Colour> textColours;
    };
    Page* getPage (int index);
    int visiblePages() const;
    juce::Rectangle<float> pageSlot (int slot) const;
    void setHighlight (const juce::String& id, bool on);
    juce::Rectangle<float> viewArea() const;
    juce::Rectangle<float> elementArea (Page& page, juce::Drawable* d, juce::Rectangle<float> slot) const;
    bool zoomed = false;
    double scroll = 0.0; // 0..1 of the page height that is hidden above the view
    void updateNav();

    OrgelProcessor& proc;
    std::shared_ptr<Piece> piece;
    std::map<int, Page> pages;
    int firstPage = 0;
    bool follow = true;

    std::map<juce::String, double> noteBeat;
    std::set<juce::String> active;
    size_t timemapIndex = 0;
    double lastBeat = -1;

    juce::TextButton prev { "<" }, next { ">" };
    juce::TextButton zoomButton;
    juce::ToggleButton followButton { juce::String::fromUTF8 ("Mitblättern") };
    juce::Label pageLabel;
    juce::String status;
};
} // namespace orgel
