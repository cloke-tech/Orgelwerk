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
    void update (double dt); // once per display frame
    ~MusicDesk() override;
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override;
    void showPage (int first);
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
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
        juce::Image image; // the page rendered once at its current size
    };
    void drawHighlighted (juce::Graphics& g, Page& page, juce::Drawable* d, juce::Rectangle<float> slot);
    juce::Rectangle<int> areaOfNote (const juce::String& id);
    Page* findPage (int index); // a page that is ready, or nullptr (it is then requested)
    void requestPage (int index, int width, int height);
    void prefetch();
    juce::ThreadPool pagePool { juce::ThreadPoolOptions{}.withThreadName ("Notenseiten").withNumberOfThreads (1) };
    std::set<int> pending;
    int generation = 0;
    juce::Point<int> imageSize; // pixel size pages are rendered at (slot size times display scale)
    int visiblePages() const;
    int pageCount() const;
    juce::Rectangle<float> pageSlot (int slot) const;
    juce::Rectangle<float> pageRect (int index) const; // where a page is drawn now (empty: not shown)
    std::vector<int> shownPages() const;
    int currentPage() const;
    float zoomPageHeight() const;
    float zoomStride() const;
    double maxScroll() const;
    void prunePages (int around);
    void stopFollowing();
    void drawPageAt (juce::Graphics& g, int index, juce::Rectangle<float> slot, float alpha, bool withHighlights);
    int shownPageForLabel = -1;
    void followInZoom();
    juce::String followNote; // the newest sounding note (page width follows it)
    bool followExact = false;
    double wheelAccumulated = 0, lastWheelTurn = 0;
    void setHighlight (const juce::String& id, bool on);
    juce::Rectangle<float> viewArea() const;
    juce::Rectangle<float> elementArea (Page& page, juce::Drawable* d, juce::Rectangle<float> slot) const;
    bool zoomed = false;
    double scroll = 0.0; // page width: pages scrolled past the top of the view (fractional)
    double scrollTarget = 0.0; // scroll glides towards this
    // page turn: the new pages slide in over the old ones
    int turnFrom = -1;
    double turnStart = 0;
    int turnDirection = 1;
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
    bool wasBusy = false;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
};
} // namespace orgel
