#pragma once
#include "Theme.h"
#include "../engine/I18n.h"
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
    // whole pages side by side, pages one below the other at page width, or the piece on one
    // long line that scrolls from left to right
    enum class View { pages, pageWidth, continuous };
    View getView() const { return view; }
    void setView (View v);
    static juce::String viewName (View v);
    bool isZoomed() const { return view == View::pageWidth; }
    void setZoomed (bool z) { setView (z ? View::pageWidth : View::pages); }
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
        // the drawing's bounds, worked out once (JUCE adds up all elements on every call)
        juce::Rectangle<float> bounds;
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
    float zoomPageWidth() const;
    void measureLines();
    float tallestLine = 0.0f; // the tallest line of staves, as a part of the page height
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
    static juce::Rectangle<float> elementArea (Page& page, juce::Drawable* d, juce::Rectangle<float> slot);
    bool zoomed = false; // page width
    View view = View::pages;

    // the continuous view: one long line, drawn in tiles in the background
    std::shared_ptr<Notation> lineNotation;      // what is shown (piece->continuous)
    std::unique_ptr<Page> line;                  // its drawing and notes, for highlights and clicks
    std::shared_ptr<juce::Drawable> tileDrawable; // a second copy, only drawn on the background thread
    std::map<int, juce::Image> tiles;
    std::set<int> tilesPending;
    juce::String tileKey;
    int lineGeneration = 0;
    double lineX = 0, lineXTarget = 0; // how far the line is scrolled to the left (pixels)
    static constexpr float tileWidth = 1024.0f;
    // the start of the line (staff names, clefs, key and time) stays at the left edge while the
    // line scrolls, so one always sees which staff is which manual
    juce::Image headerImage;     // a copy of the first tile
    float headerFraction = -1.0f; // its width as a part of the line (-1: not worked out yet)
    juce::Range<float> headerRows { 0.0f, 1.0f }; // the staves (without tempo marks above), as parts of the height
    std::vector<juce::String> firstBarMarks;            // tempo and texts of the first bar: left out of the header
    std::vector<juce::String> firstBarSigns;            // clefs, key and time signatures: the header ends after them
    std::vector<juce::Rectangle<float>> headerMasks;    // where they are, as parts of the line's size
    void drawLineHeader (juce::Graphics& g, juce::Rectangle<float> r, float phys);
    std::shared_ptr<Notation> shownNotation() const;
    void prepareLine();
    void resetFollowState();
    juce::Rectangle<float> lineRect() const;
    double maxLineX() const;
    void paintLine (juce::Graphics& g);
    void requestTile (int index, float physicalScale);

    // the playback cursor (a bar over the line of staves, like MuseScore's): where it is at a beat
    struct Cursor
    {
        bool valid = false;
        juce::Rectangle<float> area; // on screen
        double lineX = 0;            // continuous view: its place on the long line
    };
    Cursor cursorAt (double beat);
    void drawCursor (juce::Graphics& g);
    std::vector<size_t> onsets; // timemap entries where notes start
    juce::Rectangle<float> cursorShown;
    // a steady playback clock for the desk: runs on at the music's pace between the position
    // updates of the audio thread (which come in steps) and is gently pulled towards them
    double smoothBeat = -1, beatRate = 0, lastReported = -1;
    double lineVelocity = 0; // the continuous line follows the cursor like a damped spring
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
    juce::ToggleButton followButton { tr ("Mitblättern") };
    juce::Label pageLabel;
    juce::String status;
    bool wasBusy = false;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
};
} // namespace orgel
