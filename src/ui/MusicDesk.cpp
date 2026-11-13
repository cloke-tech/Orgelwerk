#include "MusicDesk.h"
#include "Profile.h"

namespace orgel
{
static const juce::Colour highlightColour { 0xffc0392b };

MusicDesk::MusicDesk (OrgelProcessor& p) : proc (p)
{
    prev.onClick = [this] {
        stopFollowing();
        if (view == View::continuous)
            lineXTarget = juce::jlimit (0.0, maxLineX(), lineXTarget - viewArea().getWidth() * 0.8);
        else
            showPage (currentPage() - visiblePages());
    };
    next.onClick = [this] {
        stopFollowing();
        if (view == View::continuous)
            lineXTarget = juce::jlimit (0.0, maxLineX(), lineXTarget + viewArea().getWidth() * 0.8);
        else
            showPage (currentPage() + visiblePages());
    };
    followButton.setToggleState (true, juce::dontSendNotification);
    followButton.onClick = [this] { follow = followButton.getToggleState(); };
    followButton.setColour (juce::ToggleButton::textColourId, theme::text);
    followButton.setColour (juce::ToggleButton::tickColourId, theme::gold);
    pageLabel.setJustificationType (juce::Justification::centred);
    zoomButton.setButtonText (viewName (view) + juce::String::fromUTF8 (" \xe2\x96\xbe"));
    zoomButton.setTooltip (tr ("Ansicht der Noten: ganze Seiten, Seitenbreite oder fortlaufend"));
    zoomButton.onClick = [this] {
        juce::PopupMenu m;
        for (auto v : { View::pages, View::pageWidth, View::continuous })
            m.addItem (1 + (int) v, viewName (v), true, view == v);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&zoomButton), [this] (int r) {
            if (r > 0)
                setView ((View) (r - 1));
        });
    };
    pageLabel.setColour (juce::Label::textColourId, theme::text);
    for (auto* c : std::initializer_list<juce::Component*> { &prev, &next, &zoomButton, &followButton, &pageLabel })
        addAndMakeVisible (c);
    prev.setTooltip (tr ("Zurückblättern"));
    next.setTooltip (tr ("Weiterblättern"));
    updateNav();
}

juce::String MusicDesk::viewName (View v)
{
    switch (v)
    {
        case View::pageWidth: return tr ("Seitenbreite");
        case View::continuous: return tr ("Fortlaufend");
        default: return tr ("Ganze Seite");
    }
}

void MusicDesk::setView (View v)
{
    if (v == view)
        return;
    // stay at the same page when switching between pages and page width
    const int page = currentPage();
    view = v;
    zoomed = v == View::pageWidth;
    zoomButton.setButtonText (viewName (view) + juce::String::fromUTF8 (" \xe2\x96\xbe"));
    turnFrom = -1;
    if (v == View::pageWidth)
        scroll = scrollTarget = page;
    else if (v == View::pages)
        firstPage = page;
    else
        prepareLine();
    resetFollowState();
    updateNav();
    repaint();
}

// the engraving the current view shows (the long line in the continuous view)
std::shared_ptr<Notation> MusicDesk::shownNotation() const
{
    if (piece == nullptr)
        return {};
    return view == View::continuous ? lineNotation : piece->notation;
}

// what is sounding is looked up again in the engraving now shown
void MusicDesk::resetFollowState()
{
    active.clear();
    timemapIndex = 0;
    lastBeat = -1;
    followNote = {};
    noteBeat.clear();
    onsets.clear();
    if (auto n = shownNotation())
    {
        auto& tm = n->getTimemap();
        for (size_t i = 0; i < tm.size(); ++i)
        {
            for (auto& id : tm[i].on)
                if (! noteBeat.count (id))
                    noteBeat[id] = tm[i].qstamp;
            if (! tm[i].on.empty())
                onsets.push_back (i);
        }
    }
}

// The cursor stands at the notes being played and moves on evenly towards the next ones (as
// far as they are on the same line), over the full height of that line of staves.
MusicDesk::Cursor MusicDesk::cursorAt (double beat)
{
    Cursor c;
    auto n = shownNotation();
    if (n == nullptr || beat < 0 || onsets.empty())
        return c;
    auto& tm = n->getTimemap();
    auto it = std::upper_bound (onsets.begin(), onsets.end(), beat + 1.0e-4, [&tm] (double b, size_t e) { return b < tm[e].qstamp; });
    const size_t k = it == onsets.begin() ? 0 : (size_t) (it - onsets.begin()) - 1;

    // a notehead of an onset, the line of staves around it and the page it is on
    auto locate = [&] (size_t onsetIndex, juce::Rectangle<float>& note, juce::Rectangle<float>& system, int& pageIndex) {
        const auto& entry = tm[onsets[onsetIndex]];
        for (auto& id : entry.on)
        {
            Page* pg = nullptr;
            juce::Rectangle<float> slot;
            if (view == View::continuous)
            {
                pg = line.get();
                slot = lineRect();
                pageIndex = 0;
            }
            else
            {
                pageIndex = n->pageOfElement (id);
                slot = pageRect (pageIndex);
                pg = slot.isEmpty() ? nullptr : findPage (pageIndex);
            }
            if (pg == nullptr || pg->drawable == nullptr)
                return false;
            auto d = pg->ids.find (id);
            if (d == pg->ids.end())
                continue;
            note = elementArea (*pg, d->second, slot);
            system = slot;
            for (auto& sid : n->getSystemIds (pageIndex))
                if (auto s2 = pg->ids.find (sid); s2 != pg->ids.end())
                {
                    auto sys = elementArea (*pg, s2->second, slot);
                    if (sys.getY() - 4.0f <= note.getCentreY() && note.getCentreY() <= sys.getBottom() + 4.0f)
                    {
                        system = sys;
                        break;
                    }
                }
            return true;
        }
        return false;
    };
    juce::Rectangle<float> noteA, sysA, noteB, sysB;
    int pageA = -1, pageB = -1;
    if (! locate (k, noteA, sysA, pageA))
        return c;
    float x = noteA.getCentreX();
    const double qA = tm[onsets[k]].qstamp;
    if (beat > qA && k + 1 < onsets.size() && locate (k + 1, noteB, sysB, pageB) && pageB == pageA
        && std::abs (sysB.getY() - sysA.getY()) < 2.0f && noteB.getCentreX() > x)
    {
        const double qB = tm[onsets[k + 1]].qstamp;
        const double f = juce::jlimit (0.0, 1.0, (beat - qA) / juce::jmax (1.0e-6, qB - qA));
        x += (float) f * (noteB.getCentreX() - x);
    }
    const float w = juce::jmax (5.0f, noteA.getWidth() * 1.3f);
    const float pad = sysA.getHeight() * 0.03f + 2.0f;
    c.valid = true;
    c.area = { x - w / 2, sysA.getY() - pad, w, sysA.getHeight() + 2 * pad };
    if (view == View::continuous)
        c.lineX = x - lineRect().getX();
    return c;
}

void MusicDesk::drawCursor (juce::Graphics& g)
{
    auto c = cursorAt (lastBeat);
    if (! c.valid)
        return;
    g.setColour (juce::Colour (0x4d2f80ed));
    g.fillRoundedRectangle (c.area, 2.0f);
    g.setColour (juce::Colour (0xb32f80ed));
    g.drawRoundedRectangle (c.area, 2.0f, 1.2f);
}

// The continuous view needs the piece set on one long line: asked for once, then read in the
// background (twice: one copy for the screen's highlights, one the tile drawing may use freely).
void MusicDesk::prepareLine()
{
    if (piece == nullptr)
        return;
    auto n = piece->continuous;
    if (n == nullptr)
    {
        proc.requestContinuous();
        return;
    }
    if (n == lineNotation)
        return;
    lineNotation = n;
    line.reset();
    tileDrawable.reset();
    tiles.clear();
    tilesPending.clear();
    lineX = lineXTarget = 0;
    headerImage = {};
    headerFraction = -1.0f;
    headerMasks.clear();
    firstBarMarks.clear();
    firstBarSigns.clear();
    const int gen = ++lineGeneration;
    resetFollowState();
    juce::Component::SafePointer<MusicDesk> safe (this);
    pagePool.addJob ([n, gen, safe] {
        const auto svg = n->getPageSVG (0);
        auto page = std::make_shared<Page>();
        std::shared_ptr<juce::Drawable> forTiles;
        std::vector<juce::String> marks, signs;
        if (auto xml = juce::parseXML (svg))
        {
            // tempo, texts and dynamics of the first bar (the header at the left edge leaves them out)
            std::function<juce::XmlElement*(juce::XmlElement&)> firstMeasure = [&] (juce::XmlElement& e) -> juce::XmlElement* {
                if (e.getStringAttribute ("class") == "measure")
                    return &e;
                for (auto* c : e.getChildIterator())
                    if (auto* m = firstMeasure (*c))
                        return m;
                return nullptr;
            };
            std::function<void (juce::XmlElement&)> collect = [&] (juce::XmlElement& e) {
                const auto cls = e.getStringAttribute ("class");
                if (cls == "tempo" || cls == "dir" || cls == "dynam" || cls == "reh" || cls == "harm")
                    marks.push_back (e.getStringAttribute ("id"));
                else if (cls == "clef" || cls == "keySig" || cls == "meterSig" || cls == "meterSigGrp" || cls == "mensur")
                    signs.push_back (e.getStringAttribute ("id"));
                else
                    for (auto* c : e.getChildIterator())
                        collect (*c);
            };
            if (auto* m = firstMeasure (*xml))
                collect (*m);
            page->drawable = juce::Drawable::createFromSVG (*xml);
            forTiles = juce::Drawable::createFromSVG (*xml);
        }
        if (page->drawable != nullptr)
        {
            std::function<void (juce::Drawable*)> walk = [&] (juce::Drawable* d) {
                if (d->getComponentID().isNotEmpty())
                    page->ids[d->getComponentID()] = d;
                for (auto* c : d->getChildren())
                    if (auto* cd = dynamic_cast<juce::Drawable*> (c))
                        walk (cd);
            };
            walk (page->drawable.get());
            page->bounds = page->drawable->getDrawableBounds();
        }
        juce::MessageManager::callAsync ([safe, page, forTiles, gen, marks, signs] {
            if (safe == nullptr || gen != safe->lineGeneration || page->drawable == nullptr)
                return;
            safe->firstBarMarks = marks;
            safe->firstBarSigns = signs;
            safe->line = std::make_unique<Page> (std::move (*page));
            safe->tileDrawable = forTiles;
            safe->updateNav();
            safe->repaint();
        });
    });
}

// where the whole line is drawn now: as tall as the desk allows, scrolled by lineX
juce::Rectangle<float> MusicDesk::lineRect() const
{
    auto va = viewArea();
    if (line == nullptr || line->drawable == nullptr)
        return {};
    auto b = line->bounds;
    if (b.getHeight() <= 0)
        return {};
    const float s = va.getHeight() * 0.94f / b.getHeight();
    return { va.getX() - (float) lineX, va.getCentreY() - b.getHeight() * s / 2, b.getWidth() * s, b.getHeight() * s };
}

double MusicDesk::maxLineX() const
{
    return juce::jmax (0.0, (double) lineRect().getWidth() - viewArea().getWidth());
}

void MusicDesk::paintLine (juce::Graphics& g)
{
    auto va = viewArea();
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRect (va.translated (4, 5));
    g.setColour (juce::Colour (0xfffdfbf5));
    g.fillRect (va);
    if (line == nullptr)
    {
        g.setColour (juce::Colours::grey);
        g.setFont (theme::serif (18.0f));
        g.drawFittedText (tr ("Noten werden gesetzt…"), va.reduced (30).toNearestInt(), juce::Justification::centred, 2);
        return;
    }
    const auto r = lineRect();
    const float phys = g.getInternalContext().getPhysicalPixelScaleFactor();
    // tiles belong to one size: after a resize they are drawn again
    const auto key = juce::String (r.getHeight(), 2) + "/" + juce::String (phys, 2) + "/" + juce::String (lineGeneration);
    if (key != tileKey)
    {
        tileKey = key;
        tiles.clear();
        tilesPending.clear();
        headerImage = {};
    }
    const juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (va.toNearestInt());
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
    const int first = juce::jmax (0, (int) std::floor (lineX / tileWidth));
    const int last = (int) std::floor ((lineX + va.getWidth()) / tileWidth);
    const int count = (int) std::ceil (r.getWidth() / tileWidth);
    for (int i = first; i <= last && i < count; ++i)
    {
        if (auto it = tiles.find (i); it != tiles.end())
        {
            // on whole screen pixels and without smoothing: the tile is copied 1:1
            const float x = std::round ((r.getX() + i * tileWidth) * phys) / phys;
            const float y = std::round (r.getY() * phys) / phys;
            g.drawImageTransformed (it->second, juce::AffineTransform::scale (1.0f / phys).translated (x, y));
        }
        else
            requestTile (i, phys);
    }
    // the next tiles in reading direction, so scrolling finds them ready
    for (int i = last + 1; i <= last + 2 && i < count; ++i)
        requestTile (i, phys);
    // drop tiles far away
    for (auto it = tiles.begin(); it != tiles.end();)
        it = (it->first < first - 3 || it->first > last + 6) ? tiles.erase (it) : std::next (it);
    drawLineHeader (g, r, phys);
    drawCursor (g);
}

void MusicDesk::drawLineHeader (juce::Graphics& g, juce::Rectangle<float> r, float phys)
{
    if (auto it = tiles.find (0); it != tiles.end())
        headerImage = it->second;
    if (headerFraction < 0.0f && ! onsets.empty() && lineNotation != nullptr && r.getWidth() > 0)
    {
        // up to just before the first note
        headerFraction = 0.0f;
        for (auto& id : lineNotation->getTimemap()[onsets.front()].on)
            if (auto d = line->ids.find (id); d != line->ids.end())
            {
                const auto note = elementArea (*line, d->second, r);
                // a little before it: ornaments and accidentals stand left of the note
                headerFraction = juce::jmax (0.0f, (note.getX() - note.getHeight() * 0.8f - 2.0f - r.getX()) / r.getWidth());
                break;
            }
        // better: just after the clefs, key and time signatures (however many accidentals there are)
        float signsRight = std::numeric_limits<float>::lowest();
        for (auto& id : firstBarSigns)
            if (auto d = line->ids.find (id); d != line->ids.end())
                signsRight = juce::jmax (signsRight, elementArea (*line, d->second, r).getRight());
        if (signsRight > r.getX())
            headerFraction = (signsRight + 5.0f - r.getX()) / r.getWidth();
        for (auto& sid : lineNotation->getSystemIds (0))
            if (auto d = line->ids.find (sid); d != line->ids.end())
            {
                const auto sys = elementArea (*line, d->second, r).expanded (0.0f, 4.0f);
                headerRows = { (sys.getY() - r.getY()) / r.getHeight(), (sys.getBottom() - r.getY()) / r.getHeight() };
                break;
            }
        for (auto& id : firstBarMarks)
            if (auto d = line->ids.find (id); d != line->ids.end())
            {
                const auto m = elementArea (*line, d->second, r).expanded (2.0f);
                headerMasks.push_back ({ (m.getX() - r.getX()) / r.getWidth(), (m.getY() - r.getY()) / r.getHeight(),
                                         m.getWidth() / r.getWidth(), m.getHeight() / r.getHeight() });
            }
    }
    if (lineX < 0.5 || headerFraction <= 0.0f)
        return;
    if (! headerImage.isValid())
    {
        requestTile (0, phys);
        return;
    }
    const auto va = viewArea();
    const float w = juce::jmin (headerFraction * r.getWidth(), tileWidth, va.getWidth() * 0.3f);
    {
        const juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (juce::Rectangle<float> (va.getX(), va.getY(), w, va.getHeight()).toNearestInt());
        g.setColour (juce::Colour (0xfffdfbf5));
        g.fillRect (va);
        // only the staves: tempo marks and texts above them belong to the first bar
        g.reduceClipRegion (juce::Rectangle<float> (va.getX(), r.getY() + headerRows.getStart() * r.getHeight(), w,
                                                    headerRows.getLength() * r.getHeight()).toNearestInt());
        const float x = std::round (va.getX() * phys) / phys;
        const float y = std::round (r.getY() * phys) / phys;
        g.drawImageTransformed (headerImage, juce::AffineTransform::scale (1.0f / phys).translated (x, y));
        g.setColour (juce::Colour (0xfffdfbf5));
        for (auto& m : headerMasks)
            g.fillRect (va.getX() + m.getX() * r.getWidth(), r.getY() + m.getY() * r.getHeight(), m.getWidth() * r.getWidth(),
                        m.getHeight() * r.getHeight());
    }
    // a soft edge: the music slides in under the header
    const float edge = va.getX() + w;
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.12f), edge, 0.0f,
                                             juce::Colours::transparentBlack, edge + 10.0f, 0.0f, false));
    g.fillRect (juce::Rectangle<float> (edge, va.getY(), 10.0f, va.getHeight()));
}

void MusicDesk::requestTile (int index, float physicalScale)
{
    if (tileDrawable == nullptr || tilesPending.count (index) || tiles.count (index))
        return;
    tilesPending.insert (index);
    const auto r = lineRect();
    const auto bounds = line != nullptr ? line->bounds : tileDrawable->getDrawableBounds();
    const float s = r.getHeight() / bounds.getHeight();
    auto drawable = tileDrawable;
    const auto key = tileKey;
    juce::Component::SafePointer<MusicDesk> safe (this);
    pagePool.addJob ([drawable, bounds, s, physicalScale, index, key, safe, height = r.getHeight()] {
        const int w = juce::roundToInt (tileWidth * physicalScale), h = juce::jmax (1, juce::roundToInt (height * physicalScale));
        juce::Image img (juce::Image::RGB, w, h, false);
        {
            juce::Graphics g (img);
            g.fillAll (juce::Colour (0xfffdfbf5));
            // only the measures inside this tile are drawn (the others fall outside the clip)
            auto t = juce::AffineTransform::translation (-bounds.getX(), -bounds.getY())
                         .scaled (s * physicalScale)
                         .translated (-(float) index * tileWidth * physicalScale, 0.0f);
            drawable->draw (g, 1.0f, t);
        }
        juce::MessageManager::callAsync ([safe, img, index, key] {
            if (safe == nullptr || key != safe->tileKey)
                return;
            safe->tilesPending.erase (index);
            safe->tiles[index] = img;
            safe->repaint (safe->viewArea().toNearestInt());
        });
    });
}

void MusicDesk::setPiece (std::shared_ptr<Piece> p)
{
    const bool sameNotation = piece != nullptr && p != nullptr && piece->notation == p->notation;
    piece = std::move (p);
    if (sameNotation)
    {
        // the same pages (perhaps the long line for the continuous view has arrived)
        if (view == View::continuous)
            prepareLine();
        return;
    }
    pages.clear();
    pending.clear();
    ++generation;
    pagePool.removeAllJobs (false, 0);
    // the long line is made again for the new engraving (jobs for it were just dropped)
    lineNotation = nullptr;
    line.reset();
    tileDrawable.reset();
    tiles.clear();
    tilesPending.clear();
    ++lineGeneration;
    turnFrom = -1;
    scroll = scrollTarget = 0.0;
    shownPageForLabel = -1;
    firstPage = 0;
    if (view == View::continuous)
        prepareLine();
    resetFollowState();
    tallestLine = 0.0f;
    measureLines();
    status = piece == nullptr ? juce::String (tr ("Noten hierher ziehen oder über „Öffnen“ laden"))
                              : (piece->notation == nullptr ? (piece->notationFromScore ? juce::String (tr ("Klicken, um die Noten zu setzen"))
                                                                                         : juce::String (tr ("Kein Notenbild verfügbar")))
                                                            : juce::String());
    updateNav();
    repaint();
}

// How tall the tallest line of staves is (as a part of the page height), measured once in the
// background: page width shrinks the pages so that it fits the desk.
void MusicDesk::measureLines()
{
    if (piece == nullptr || piece->notation == nullptr)
        return;
    auto notation = piece->notation;
    const int gen = generation;
    juce::Component::SafePointer<MusicDesk> safe (this);
    pagePool.addJob ([notation, gen, safe] {
        float tallest = 0.0f;
        for (int p = 0; p < notation->getPageCount(); ++p)
        {
            Page page;
            if (auto xml = juce::parseXML (notation->getPageSVG (p)))
                page.drawable = juce::Drawable::createFromSVG (*xml);
            if (page.drawable == nullptr)
                continue;
            std::function<void (juce::Drawable*)> walk = [&] (juce::Drawable* d) {
                if (d->getComponentID().isNotEmpty())
                    page.ids[d->getComponentID()] = d;
                for (auto* c : d->getChildren())
                    if (auto* cd = dynamic_cast<juce::Drawable*> (c))
                        walk (cd);
            };
            walk (page.drawable.get());
            page.bounds = page.drawable->getDrawableBounds();
            const juce::Rectangle<float> unit (0.0f, 0.0f, 2100.0f, 2970.0f);
            for (auto& sid : notation->getSystemIds (p))
                if (auto it = page.ids.find (sid); it != page.ids.end())
                    tallest = juce::jmax (tallest, elementArea (page, it->second, unit).getHeight() / 2970.0f);
        }
        juce::MessageManager::callAsync ([safe, gen, tallest] {
            if (safe == nullptr || gen != safe->generation)
                return;
            // a little room for the text above the top staff
            safe->tallestLine = juce::jmin (1.0f, tallest * 1.08f);
            safe->updateNav();
            safe->repaint();
        });
    });
}

MusicDesk::~MusicDesk()
{
    pagePool.removeAllJobs (true, 4000);
}

MusicDesk::Page* MusicDesk::findPage (int index)
{
    auto it = pages.find (index);
    return it != pages.end() && it->second.drawable != nullptr ? &it->second : nullptr;
}

// Pages are read and drawn on a background thread, so turning a page never stops the animations.
void MusicDesk::requestPage (int index, int width, int height)
{
    if (piece == nullptr || piece->notation == nullptr || ! juce::isPositiveAndBelow (index, piece->notation->getPageCount()) || width <= 0 || height <= 0)
        return;
    if (auto it = pages.find (index); it != pages.end() && it->second.image.getWidth() == width && it->second.image.getHeight() == height)
        return;
    if (pending.count (index))
        return;
    pending.insert (index);
    auto notation = piece->notation;
    const int gen = generation;
    juce::Component::SafePointer<MusicDesk> safe (this);
    pagePool.addJob ([notation, index, width, height, gen, safe] {
        auto page = std::make_shared<Page>();
        {
            ORGEL_PROFILE_SCOPE ("desk.buildPage");
            if (auto xml = juce::parseXML (notation->getPageSVG (index)))
                page->drawable = juce::Drawable::createFromSVG (*xml);
        }
        if (page->drawable != nullptr)
        {
            std::function<void (juce::Drawable*)> walk = [&] (juce::Drawable* d) {
                if (d->getComponentID().isNotEmpty())
                    page->ids[d->getComponentID()] = d;
                for (auto* c : d->getChildren())
                    if (auto* cd = dynamic_cast<juce::Drawable*> (c))
                        walk (cd);
            };
            walk (page->drawable.get());
            page->bounds = page->drawable->getDrawableBounds();
            ORGEL_PROFILE_SCOPE ("desk.renderPage");
            page->image = juce::Image (juce::Image::RGB, width, height, false);
            juce::Graphics ig (page->image);
            ig.fillAll (juce::Colour (0xfffdfbf5));
            page->drawable->drawWithin (ig, page->image.getBounds().toFloat(), juce::RectanglePlacement::centred, 1.0f);
        }
        juce::MessageManager::callAsync ([safe, page, index, gen] {
            if (safe == nullptr)
                return;
            safe->pending.erase (index);
            if (gen != safe->generation)
                return;
            auto& slot = safe->pages[index];
            // the new picture replaces an older one; the notes for highlighting come with it
            slot = std::move (*page);
            safe->repaint();
        });
    });
}

// the pages around the ones shown, so the next turn is instant
void MusicDesk::prefetch()
{
    if (piece == nullptr || piece->notation == nullptr || imageSize.x <= 0)
        return;
    const int n = visiblePages();
    const int from = zoomed ? (int) std::floor (scroll) : firstPage;
    for (int i = from; i < from + 2 * n + (zoomed ? 1 : 0); ++i)
        requestPage (i, imageSize.x, imageSize.y);
    requestPage (from - n, imageSize.x, imageSize.y);
}

int MusicDesk::pageCount() const
{
    return piece != nullptr && piece->notation != nullptr ? piece->notation->getPageCount() : 0;
}

// Page width: all pages one below the other, scrolled like a document. "scroll" counts pages
// from the top of the first one (2.5 = the middle of the third page is at the top of the view).
// The page is as wide as the desk, unless the tallest line of staves would then not fit
// in the desk: then the page is made smaller, so every line can be seen whole.
float MusicDesk::zoomPageWidth() const
{
    const auto view = viewArea();
    float w = view.getWidth();
    if (tallestLine > 0.0f)
        w = juce::jmin (w, view.getHeight() * 0.94f / (tallestLine * 2970.0f / 2100.0f));
    return juce::jmax (100.0f, w);
}

float MusicDesk::zoomPageHeight() const
{
    return zoomPageWidth() * 2970.0f / 2100.0f;
}

float MusicDesk::zoomStride() const
{
    return zoomPageHeight() + 14.0f;
}

double MusicDesk::maxScroll() const
{
    const auto view = viewArea();
    const double total = pageCount() * (double) zoomStride() - 14.0;
    return juce::jmax (0.0, (total - view.getHeight()) / zoomStride());
}

juce::Rectangle<float> MusicDesk::pageRect (int index) const
{
    if (zoomed)
    {
        auto area = viewArea();
        return { area.getCentreX() - zoomPageWidth() / 2, area.getY() + (float) ((index - scroll) * zoomStride()), zoomPageWidth(), zoomPageHeight() };
    }
    const int slot = index - firstPage;
    if (slot < 0 || slot >= visiblePages())
        return {};
    return pageSlot (slot);
}

std::vector<int> MusicDesk::shownPages() const
{
    std::vector<int> out;
    const int count = pageCount();
    if (zoomed)
    {
        const auto view = viewArea();
        for (int i = juce::jmax (0, (int) std::floor (scroll)); i < count; ++i)
        {
            auto r = pageRect (i);
            if (r.getY() > view.getBottom())
                break;
            if (r.getBottom() >= view.getY())
                out.push_back (i);
        }
        return out;
    }
    for (int s = 0; s < visiblePages(); ++s)
        if (firstPage + s < count)
            out.push_back (firstPage + s);
    return out;
}

// the page the reader is looking at (the one in the middle of the view)
int MusicDesk::currentPage() const
{
    if (! zoomed)
        return firstPage;
    const double centre = scroll + viewArea().getHeight() * 0.5 / zoomStride();
    return juce::jlimit (0, juce::jmax (0, pageCount() - 1), (int) std::floor (centre));
}

void MusicDesk::stopFollowing()
{
    follow = false;
    followButton.setToggleState (false, juce::dontSendNotification);
}

int MusicDesk::visiblePages() const
{
    if (zoomed)
        return 1;
    auto area = getLocalBounds().reduced (12).withTrimmedBottom (30);
    const float pageAspect = 2100.0f / 2970.0f;
    return area.getWidth() >= 2.0f * pageAspect * area.getHeight() * 0.92f ? 2 : 1;
}

juce::Rectangle<float> MusicDesk::viewArea() const
{
    return getLocalBounds().toFloat().reduced (14.0f).withTrimmedBottom (32.0f);
}

juce::Rectangle<float> MusicDesk::pageSlot (int slot) const
{
    auto area = viewArea();
    if (zoomed)
        return { area.getCentreX() - zoomPageWidth() / 2, area.getY(), zoomPageWidth(), zoomPageHeight() };
    const int n = visiblePages();
    const float pageAspect = 2100.0f / 2970.0f;
    float h = area.getHeight();
    float w = h * pageAspect;
    if (w * n + 10.0f * (n - 1) > area.getWidth())
    {
        w = (area.getWidth() - 10.0f * (n - 1)) / n;
        h = w / pageAspect;
    }
    const float total = w * n + 10.0f * (n - 1);
    const float x = area.getCentreX() - total / 2 + slot * (w + 10.0f);
    return { x, area.getY() + (area.getHeight() - h) / 2, w, h };
}

void MusicDesk::paint (juce::Graphics& g)
{
    ORGEL_PROFILE_SCOPE ("desk.paint");
    auto b = getLocalBounds().toFloat();
    // music stand
    theme::fillWood (g, b, theme::woodLight.darker (0.15f));
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.drawRect (b, 2.0f);
    auto ledge = b.removeFromBottom (30.0f);
    theme::fillWood (g, ledge, theme::wood);
    g.setColour (theme::gold.withAlpha (0.4f));
    g.drawLine (ledge.getX(), ledge.getY(), ledge.getRight(), ledge.getY(), 1.0f);

    if (view == View::continuous && piece != nullptr)
    {
        paintLine (g);
        return;
    }
    const int count = piece != nullptr && piece->notation != nullptr ? piece->notation->getPageCount() : 0;
    if (count == 0)
    {
        auto slot = pageSlot (0);
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.fillRect (slot.translated (4, 5));
        g.setColour (juce::Colour (0xfffbf8f0));
        g.fillRect (slot);
        g.setColour (juce::Colours::grey);
        g.setFont (theme::serif (18.0f));
        const bool clickable = piece != nullptr && piece->notationFromScore && ! proc.isEngraving();
        auto text = proc.isLoading() ? juce::String (tr ("Stück wird geladen…"))
                                     : (proc.isEngraving() ? juce::String (tr ("Noten werden gesetzt…")) : status);
        if (clickable)
        {
            g.setColour (theme::gold.withAlpha (isMouseOver() ? 0.25f : 0.12f));
            g.fillRoundedRectangle (slot.reduced (24).withSizeKeepingCentre (slot.getWidth() - 48, 90), 8.0f);
            g.setColour (juce::Colour (0xff5a3a10));
        }
        g.drawFittedText (text, slot.reduced (30).toNearestInt(), juce::Justification::centred, 4);
        return;
    }
    g.saveState();
    g.reduceClipRegion (viewArea().expanded (6.0f).toNearestInt());
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    {
        auto slot0 = pageSlot (0);
        imageSize = { juce::roundToInt (slot0.getWidth() * scale), juce::roundToInt (slot0.getHeight() * scale) };
    }
    if (zoomed)
    {
        for (int index : shownPages())
            drawPageAt (g, index, pageRect (index), 1.0f, true);
        drawCursor (g);
        g.restoreState();
        prefetch();
        return;
    }
    const int n = visiblePages();
    const double turn = turnFrom >= 0 ? (juce::Time::getMillisecondCounterHiRes() - turnStart) / 280.0 : 1.0;
    if (turn < 1.0 && n == 2 && std::abs (firstPage - turnFrom) == 1)
    {
        // turning by one page: the row of pages slides over by one slot, the page leaving fades
        // out on one side while the next one fades in on the other
        const float e = 1.0f - (float) std::pow (1.0 - turn, 3.0);
        const float pos = (float) turnFrom + (float) (firstPage - turnFrom) * e;
        const float stride = pageSlot (1).getX() - pageSlot (0).getX();
        for (int i = juce::jmin (turnFrom, firstPage); i <= juce::jmax (turnFrom, firstPage) + 1; ++i)
        {
            const float rel = (float) i - pos;
            const float alpha = juce::jlimit (0.0f, 1.0f, 1.0f - juce::jmax (0.0f, -rel, rel - 1.0f));
            if (alpha > 0.0f)
                drawPageAt (g, i, pageSlot (0).translated (rel * stride, 0.0f), alpha, alpha >= 1.0f);
        }
    }
    else if (turn < 1.0)
    {
        // page turn: the new pages glide in over the old ones and become solid
        const float e = 1.0f - (float) std::pow (1.0 - turn, 3.0);
        for (int sl = 0; sl < n; ++sl)
            drawPageAt (g, turnFrom + sl, pageSlot (sl), 1.0f, false);
        for (int sl = 0; sl < n; ++sl)
            drawPageAt (g, firstPage + sl, pageSlot (sl).translated ((1.0f - e) * (float) turnDirection * pageSlot (sl).getWidth() * 0.18f, 0.0f), e, true);
    }
    else
    {
        for (int sl = 0; sl < n; ++sl)
            drawPageAt (g, firstPage + sl, pageSlot (sl), 1.0f, true);
        drawCursor (g);
    }
    g.restoreState();
    prefetch();
}

void MusicDesk::drawPageAt (juce::Graphics& g, int index, juce::Rectangle<float> slot, float alpha, bool withHighlights)
{
    if (index < 0 || index >= pageCount() || slot.isEmpty())
        return;
    g.setColour (juce::Colours::black.withAlpha (0.35f * alpha));
    g.fillRect (slot.translated (4, 5));
    g.setColour (juce::Colour (0xfffdfbf5).withAlpha (alpha));
    g.fillRect (slot);
    auto* page = findPage (index);
    if (page == nullptr || ! page->image.isValid())
    {
        requestPage (index, imageSize.x, imageSize.y);
        return;
    }
    // the engraved page is rendered once per size in the background and here only copied;
    // after a resize the old picture is stretched until the new one is ready
    if (page->image.getWidth() != imageSize.x || page->image.getHeight() != imageSize.y)
        requestPage (index, imageSize.x, imageSize.y);
    g.setOpacity (alpha);
    g.drawImage (page->image, slot);
    g.setOpacity (1.0f);
    juce::ignoreUnused (withHighlights);
}

void MusicDesk::resized()
{
    auto ledge = getLocalBounds().removeFromBottom (30).reduced (6, 3);
    prev.setBounds (ledge.removeFromLeft (34));
    next.setBounds (ledge.removeFromRight (34));
    followButton.setBounds (ledge.removeFromRight (120));
    zoomButton.setBounds (ledge.removeFromLeft (100).reduced (4, 0));
    pageLabel.setBounds (ledge);
    updateNav();
}

void MusicDesk::showPage (int first)
{
    const int count = pageCount();
    first = juce::jlimit (0, juce::jmax (0, count - 1), first);
    if (zoomed)
    {
        // glide down (or up) to the top of that page
        scrollTarget = juce::jlimit (0.0, maxScroll(), (double) first);
        firstPage = first;
        prunePages (first);
        updateNav();
        return;
    }
    if (first == firstPage)
        return;
    // glide only between pages that are ready; a far jump just shows the new page
    if (findPage (firstPage) != nullptr && findPage (first) != nullptr && std::abs (first - firstPage) <= 2 * visiblePages())
    {
        turnFrom = firstPage;
        turnDirection = first > firstPage ? 1 : -1;
        turnStart = juce::Time::getMillisecondCounterHiRes();
    }
    else
        turnFrom = -1;
    firstPage = first;
    prunePages (first);
    updateNav();
    repaint();
}

// keep a few pages around the shown ones, drop the rest
void MusicDesk::prunePages (int around)
{
    for (auto it = pages.begin(); it != pages.end();)
        it = std::abs (it->first - around) > 4 ? pages.erase (it) : std::next (it);
}

void MusicDesk::updateNav()
{
    const int count = pageCount();
    const int n = visiblePages();
    if (view == View::continuous)
    {
        prev.setEnabled (lineX > 0.5);
        next.setEnabled (lineX < maxLineX() - 0.5);
        const double total = juce::jmax (1.0, (double) lineRect().getWidth());
        pageLabel.setText (line == nullptr ? juce::String() : juce::String (juce::roundToInt ((lineX + viewArea().getWidth() * 0.3) * 100.0 / total)) + " %",
                           juce::dontSendNotification);
        return;
    }
    if (zoomed)
    {
        const int page = currentPage();
        shownPageForLabel = page;
        prev.setEnabled (scroll > 1.0e-3);
        next.setEnabled (scroll < maxScroll() - 1.0e-3);
        pageLabel.setText (count == 0 ? juce::String() : tr ("Seite ") + juce::String (page + 1) + tr (" von ") + juce::String (count), juce::dontSendNotification);
        return;
    }
    prev.setEnabled (firstPage > 0);
    next.setEnabled (firstPage + n < count);
    if (count == 0)
        pageLabel.setText ({}, juce::dontSendNotification);
    else if (n == 2 && firstPage + 1 < count)
        pageLabel.setText (tr ("Seiten ") + juce::String (firstPage + 1) + juce::String::fromUTF8 ("–") + juce::String (firstPage + 2) + tr (" von ") + juce::String (count),
                           juce::dontSendNotification);
    else
        pageLabel.setText (tr ("Seite ") + juce::String (firstPage + 1) + tr (" von ") + juce::String (count), juce::dontSendNotification);
}

static void highlight (juce::Drawable* d, bool on,
                       std::map<juce::DrawableShape*, std::pair<juce::FillType, juce::FillType>>& shapes,
                       std::map<juce::DrawableText*, juce::Colour>& texts)
{
    if (auto* sh = dynamic_cast<juce::DrawableShape*> (d))
    {
        if (on)
        {
            if (! shapes.count (sh))
                shapes[sh] = { sh->getFill(), sh->getStrokeFill() };
            sh->setFill (highlightColour);
            if (! sh->getStrokeFill().isInvisible())
                sh->setStrokeFill (highlightColour);
        }
        else if (auto it = shapes.find (sh); it != shapes.end())
        {
            sh->setFill (it->second.first);
            sh->setStrokeFill (it->second.second);
        }
    }
    if (auto* t = dynamic_cast<juce::DrawableText*> (d))
    {
        if (on)
        {
            if (! texts.count (t))
                texts[t] = t->getColour();
            t->setColour (highlightColour);
        }
        else if (auto it = texts.find (t); it != texts.end())
            t->setColour (it->second);
    }
    for (auto* ch : d->getChildren())
        if (auto* cd = dynamic_cast<juce::Drawable*> (ch))
            highlight (cd, on, shapes, texts);
}

void MusicDesk::setHighlight (const juce::String&, bool)
{
    // the cursor shows what is playing (see update)
}

// screen area of a note on a visible page (empty if it is not shown)
juce::Rectangle<int> MusicDesk::areaOfNote (const juce::String& id)
{
    if (view == View::continuous)
    {
        if (line != nullptr)
            if (auto n = line->ids.find (id); n != line->ids.end())
                return elementArea (*line, n->second, lineRect()).expanded (3.0f).getSmallestIntegerContainer()
                    .getIntersection (viewArea().expanded (6.0f).toNearestInt());
        return {};
    }
    for (int index : shownPages())
    {
        auto it = pages.find (index);
        if (it == pages.end() || it->second.drawable == nullptr)
            continue;
        auto n = it->second.ids.find (id);
        if (n != it->second.ids.end())
            return elementArea (it->second, n->second, pageRect (index)).expanded (3.0f).getSmallestIntegerContainer()
                .getIntersection (viewArea().expanded (6.0f).toNearestInt());
    }
    return {};
}

void MusicDesk::drawHighlighted (juce::Graphics& g, Page& page, juce::Drawable* d, juce::Rectangle<float> slot)
{
    auto* root = page.drawable.get();
    // from the note's own coordinates up to the page, then onto the slot
    juce::AffineTransform t;
    for (juce::Component* c = d; c != nullptr && c != root; c = c->getParentComponent())
        t = t.followedBy (juce::AffineTransform::translation ((float) c->getX(), (float) c->getY()).followedBy (c->getTransform()));
    const auto fit = juce::RectanglePlacement (juce::RectanglePlacement::centred).getTransformToFit (page.bounds, slot);
    t = t.followedBy (juce::AffineTransform::translation (page.bounds.getTopLeft())).followedBy (fit);
    const juce::Graphics::ScopedSaveState save (g);
    g.addTransform (t);
    highlight (d, true, page.shapeColours, page.textColours);
    d->paintEntireComponent (g, false);
    highlight (d, false, page.shapeColours, page.textColours);
}

void MusicDesk::update (double dt)
{
    // page turn and scrolling glide; both are only repainted while they move
    if (turnFrom >= 0)
    {
        if (juce::Time::getMillisecondCounterHiRes() - turnStart > 280.0)
            turnFrom = -1;
        repaint (viewArea().expanded (8.0f).toNearestInt());
    }
    if (zoomed && std::abs (scrollTarget - scroll) > 1.0e-5)
    {
        // eases out: quick at first, settling softly (about a third of a second)
        scroll += (scrollTarget - scroll) * (1.0 - std::exp (-dt * 9.0));
        if (std::abs (scrollTarget - scroll) * zoomStride() < 0.5)
            scroll = scrollTarget;
        repaint (viewArea().expanded (8.0f).toNearestInt());
        if (currentPage() != shownPageForLabel)
            updateNav();
    }
    followInZoom();
    const bool playingAlong = view == View::continuous && follow && proc.isPlaying();
    if (view == View::continuous && ! playingAlong && std::abs (lineXTarget - lineX) > 0.3)
    {
        // the line glides along with the music
        lineX += (lineXTarget - lineX) * (1.0 - std::exp (-dt * 5.0));
        repaint (viewArea().expanded (8.0f).toNearestInt());
        updateNav();
    }
    auto shown = shownNotation();
    if (shown == nullptr)
    {
        const bool busy = proc.isLoading() || proc.isEngraving() || proc.isEngravingContinuous();
        if (busy != wasBusy)
            repaint();
        wasBusy = busy;
        return;
    }
    auto& tm = shown->getTimemap();
    const double reported = proc.isPlaying() || proc.getPosition() > 0 ? proc.getCurrentBeat() : -1.0;
    double beat = reported;
    if (proc.isPlaying() && reported >= 0 && smoothBeat >= 0 && std::abs (reported - smoothBeat) < 1.0)
    {
        // the pace in beats per second, measured from the reported positions
        if (reported != lastReported && lastReported >= 0 && dt > 0)
        {
            const double measured = (reported - lastReported) / dt;
            beatRate = beatRate <= 0 ? measured : beatRate + (measured - beatRate) * 0.05;
        }
        smoothBeat += beatRate * dt;
        smoothBeat += (reported - smoothBeat) * (1.0 - std::exp (-dt * 4.0));
        beat = smoothBeat;
    }
    else
    {
        smoothBeat = reported;
        beatRate = 0;
    }
    lastReported = reported;
    if (std::abs (beat - lastBeat) < 1.0e-6)
        return;
    {
        // the cursor moves with the music; the continuous line follows it evenly, a third
        // into the view (gently smoothed, as playback reports its position in steps)
        auto c = cursorAt (beat);
        if (playingAlong && c.valid)
        {
            const auto va = viewArea();
            const double target = juce::jlimit (0.0, maxLineX(), c.lineX - va.getWidth() * 0.3);
            const double before = lineX;
            if (std::abs (target - lineX) > va.getWidth())
            {
                lineX = target; // jumped (seek)
                lineVelocity = 0;
            }
            else
            {
                // critically damped spring: the speed changes smoothly, never in jolts
                constexpr double omega = 3.5;
                const double h = juce::jmin (dt, 0.05);
                lineVelocity += (omega * omega * (target - lineX) - 2.0 * omega * lineVelocity) * h;
                lineX += lineVelocity * h;
            }
            if (profile::enabled())
            {
                static double lastStep = 0;
                const double step = lineX - before;
                if (std::abs (step) < 50.0 && std::abs (lastStep) < 50.0)
                {
                    profile::record ("line.step", std::abs (step));
                    profile::record ("line.stepChange", std::abs (step - lastStep));
                }
                lastStep = step;
            }
            lineXTarget = lineX;
            repaint (va.expanded (8.0f).toNearestInt());
            updateNav();
        }
        else if (c.valid || ! cursorShown.isEmpty())
        {
            auto dirty = c.valid ? c.area : juce::Rectangle<float>();
            dirty = dirty.isEmpty() ? cursorShown : (cursorShown.isEmpty() ? dirty : dirty.getUnion (cursorShown));
            repaint (dirty.expanded (3.0f).getSmallestIntegerContainer());
        }
        cursorShown = c.valid ? c.area : juce::Rectangle<float>();
    }

    std::set<juce::String> now;
    if (beat < lastBeat || lastBeat < 0)
    {
        timemapIndex = 0;
    }
    else
        now = active;
    if (beat >= 0)
        while (timemapIndex < tm.size() && tm[timemapIndex].qstamp <= beat + 1.0e-4)
        {
            for (auto& id : tm[timemapIndex].off)
                now.erase (id);
            for (auto& id : tm[timemapIndex].on)
                now.insert (id);
            ++timemapIndex;
        }
    lastBeat = beat;

    bool changed = false;
    juce::String newest;
    for (auto& id : active)
        if (! now.count (id))
        {
            setHighlight (id, false);
            changed = true;
        }
    for (auto& id : now)
        if (! active.count (id))
        {
            setHighlight (id, true);
            changed = true;
            newest = id;
        }
    active = std::move (now);

    changed = false; // note areas were repainted individually by setHighlight
    if (view == View::continuous)
        return; // the line follows the cursor (above)
    if (follow && newest.isNotEmpty() && piece->notation != nullptr)
    {
        const int page = piece->notation->pageOfElement (newest);
        const int n = visiblePages();
        // two pages: the page being played stays on the left, the next one is already on the right
        if (! zoomed && page >= 0 && (n == 2 ? page != firstPage : (page < firstPage || page >= firstPage + n)))
            showPage (page);
        if (zoomed)
            followNote = newest;
    }
    if (changed)
        repaint();
}

// Page width: keeps the whole line of staves that is playing in view. When it leaves the view,
// the view glides until that line's top is just below the top edge (like turning to the next
// line when reading). Checked every frame, so it also corrects itself once a page that was
// still being drawn arrives.
void MusicDesk::followInZoom()
{
    if (! zoomed || ! follow || followNote.isEmpty() || piece == nullptr || piece->notation == nullptr)
        return;
    const int page = piece->notation->pageOfElement (followNote);
    if (page < 0)
        return;
    const auto view = viewArea();
    // the line of staves (system) the note is in, in pixels from its page top
    juce::Range<double> line (0.0, 1.0);
    bool exact = false;
    if (auto* pg = findPage (page))
        if (auto it = pg->ids.find (followNote); it != pg->ids.end())
        {
            const auto pageArea = juce::Rectangle<float> (view.getCentreX() - zoomPageWidth() / 2, 0.0f, zoomPageWidth(), zoomPageHeight());
            const auto note = elementArea (*pg, it->second, pageArea);
            line = { note.getY(), note.getBottom() };
            for (auto& sid : piece->notation->getSystemIds (page))
                if (auto s2 = pg->ids.find (sid); s2 != pg->ids.end())
                {
                    const auto sys = elementArea (*pg, s2->second, pageArea);
                    if (sys.getY() - 4.0f <= note.getCentreY() && note.getCentreY() <= sys.getBottom() + 4.0f)
                    {
                        line = { sys.getY(), sys.getBottom() };
                        break;
                    }
                }
            exact = true;
        }
    // the whole line in view: nothing to do. Else its top goes just below the top of the view
    // (a line taller than the view shows from its top).
    const double margin = view.getHeight() * 0.04;
    const double top = (page - scrollTarget) * zoomStride() + line.getStart();    // with the glide under way
    const double bottom = (page - scrollTarget) * zoomStride() + line.getEnd();
    const bool inView = top >= margin * 0.5 && bottom <= view.getHeight() - margin * 0.5;
    if (! inView)
        scrollTarget = juce::jlimit (0.0, maxScroll(), page + (line.getStart() - margin) / zoomStride());
    followExact = exact;
}

juce::Rectangle<float> MusicDesk::elementArea (Page& page, juce::Drawable* d, juce::Rectangle<float> slot)
{
    auto* root = page.drawable.get();
    auto fit = juce::RectanglePlacement (juce::RectanglePlacement::centred).getTransformToFit (page.bounds, slot);
    const auto origin = page.bounds.getTopLeft();
    return root->getLocalArea (d, d->getLocalBounds()).toFloat().translated (origin.x, origin.y).transformedBy (fit);
}

std::unique_ptr<juce::AccessibilityHandler> MusicDesk::createAccessibilityHandler()
{
    setTitle (tr ("Notenpult"));
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::button,
        juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [this] {
            if (piece != nullptr && piece->notation == nullptr && piece->notationFromScore)
                proc.requestEngraving();
        }));
}

void MusicDesk::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    if (view == View::continuous)
    {
        // sideways (or the wheel turned): the line moves left and right
        const float d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
        if (std::abs (d) < 1.0e-4)
            return;
        stopFollowing();
        const double base = w.isSmooth ? lineX : lineXTarget;
        lineXTarget = juce::jlimit (0.0, maxLineX(), base - d * (w.isSmooth ? 600.0 : 420.0));
        if (w.isSmooth)
            lineX = lineXTarget;
        repaint();
        updateNav();
        return;
    }
    if (std::abs (w.deltaY) < 1.0e-4)
        return;
    if (zoomed)
    {
        // scrolls like a document: a trackpad moves the page directly, a wheel glides
        stopFollowing();
        const double pixels = w.deltaY * (w.isSmooth ? 600.0 : 420.0);
        const double base = w.isSmooth ? scroll : scrollTarget;
        scrollTarget = juce::jlimit (0.0, maxScroll(), base - pixels / zoomStride());
        if (w.isSmooth)
            scroll = scrollTarget;
        firstPage = currentPage();
        repaint();
        updateNav();
        return;
    }
    // whole pages: one turn per gesture, however many events a trackpad sends
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastWheelTurn < 450.0)
        return;
    wheelAccumulated += w.deltaY;
    if (std::abs (wheelAccumulated) < 0.12)
        return;
    stopFollowing();
    showPage (firstPage + (wheelAccumulated < 0 ? visiblePages() : -visiblePages()));
    wheelAccumulated = 0;
    lastWheelTurn = now;
}

void MusicDesk::mouseDown (const juce::MouseEvent& e)
{
    // a MIDI file shows its pages only once asked for
    if (piece != nullptr && piece->notation == nullptr && piece->notationFromScore)
    {
        proc.requestEngraving();
        repaint();
        return;
    }
    // click on a note: continue playback from there
    if (view == View::continuous && line != nullptr && piece != nullptr && piece->score != nullptr)
    {
        const auto r = lineRect();
        double bestDist = 1.0e9;
        juce::String best;
        for (auto& [id, d] : line->ids)
        {
            if (! noteBeat.count (id))
                continue;
            auto area = elementArea (*line, d, r);
            const double dist = area.getCentre().getDistanceFrom (e.position);
            if (area.expanded (6.0f).contains (e.position) && dist < bestDist)
            {
                bestDist = dist;
                best = id;
            }
        }
        if (best.isNotEmpty())
        {
            proc.seek (piece->score->beatToSeconds (noteBeat[best]));
            lastBeat = -1;
            follow = true;
            followButton.setToggleState (true, juce::dontSendNotification);
        }
        return;
    }
    if (piece == nullptr || piece->notation == nullptr || piece->score == nullptr)
        return;
    for (int index : shownPages())
    {
        auto slot = pageRect (index);
        if (! slot.contains (e.position) || ! viewArea().contains (e.position))
            continue;
        auto* page = findPage (index);
        if (page == nullptr || page->drawable == nullptr)
            return;
        double bestDist = 1.0e9;
        juce::String best;
        for (auto& [id, d] : page->ids)
        {
            if (! noteBeat.count (id))
                continue;
            auto area = elementArea (*page, d, slot);
            const double dist = area.getCentre().getDistanceFrom (e.position);
            if (area.expanded (6.0f).contains (e.position) && dist < bestDist)
            {
                bestDist = dist;
                best = id;
            }
        }
        if (best.isNotEmpty())
        {
            proc.seek (piece->score->beatToSeconds (noteBeat[best]));
            lastBeat = -1;
            follow = true;
            followButton.setToggleState (true, juce::dontSendNotification);
        }
        return;
    }
}
} // namespace orgel
