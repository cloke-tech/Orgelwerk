#include "MusicDesk.h"
#include "Profile.h"

namespace orgel
{
static const juce::Colour highlightColour { 0xffc0392b };

MusicDesk::MusicDesk (OrgelProcessor& p) : proc (p)
{
    prev.onClick = [this] { stopFollowing(); showPage (currentPage() - visiblePages()); };
    next.onClick = [this] { stopFollowing(); showPage (currentPage() + visiblePages()); };
    followButton.setToggleState (true, juce::dontSendNotification);
    followButton.onClick = [this] { follow = followButton.getToggleState(); };
    followButton.setColour (juce::ToggleButton::textColourId, theme::text);
    followButton.setColour (juce::ToggleButton::tickColourId, theme::gold);
    pageLabel.setJustificationType (juce::Justification::centred);
    zoomButton.setButtonText ("Seitenbreite");
    zoomButton.setTooltip ("Zwischen ganzer Seite und Seitenbreite (größere Noten, scrollt mit) umschalten");
    zoomButton.setClickingTogglesState (true);
    zoomButton.onClick = [this] {
        // stay at the same page when switching
        const int page = currentPage();
        zoomed = zoomButton.getToggleState();
        zoomButton.setButtonText (zoomed ? "Ganze Seite" : "Seitenbreite");
        turnFrom = -1;
        if (zoomed)
            scroll = scrollTarget = page;
        else
            firstPage = visiblePages() == 2 ? page - (page % 2) : page;
        updateNav();
        repaint();
    };
    pageLabel.setColour (juce::Label::textColourId, theme::text);
    for (auto* c : std::initializer_list<juce::Component*> { &prev, &next, &zoomButton, &followButton, &pageLabel })
        addAndMakeVisible (c);
    prev.setTooltip (juce::String::fromUTF8 ("Zurückblättern"));
    next.setTooltip (juce::String::fromUTF8 ("Weiterblättern"));
    updateNav();
}

void MusicDesk::setPiece (std::shared_ptr<Piece> p)
{
    const bool sameNotation = piece != nullptr && p != nullptr && piece->notation == p->notation;
    piece = std::move (p);
    if (sameNotation)
        return;
    pages.clear();
    pending.clear();
    ++generation;
    pagePool.removeAllJobs (false, 0);
    turnFrom = -1;
    scroll = scrollTarget = 0.0;
    shownPageForLabel = -1;
    followNote = {};
    active.clear();
    noteBeat.clear();
    timemapIndex = 0;
    lastBeat = -1;
    firstPage = 0;
    if (piece != nullptr && piece->notation != nullptr)
        for (auto& e : piece->notation->getTimemap())
            for (auto& id : e.on)
                if (! noteBeat.count (id))
                    noteBeat[id] = e.qstamp;
    tallestLine = 0.0f;
    measureLines();
    status = piece == nullptr ? juce::String (juce::String::fromUTF8 ("Noten hierher ziehen oder über „Öffnen“ laden"))
                              : (piece->notation == nullptr ? (piece->notationFromScore ? juce::String ("Klicken, um die Noten zu setzen")
                                                                                         : juce::String (juce::String::fromUTF8 ("Kein Notenbild verfügbar")))
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
        auto text = proc.isLoading() ? juce::String (juce::String::fromUTF8 ("Stück wird geladen…"))
                                     : (proc.isEngraving() ? juce::String (juce::String::fromUTF8 ("Noten werden gesetzt…")) : status);
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
        g.restoreState();
        prefetch();
        return;
    }
    const int n = visiblePages();
    const double turn = turnFrom >= 0 ? (juce::Time::getMillisecondCounterHiRes() - turnStart) / 280.0 : 1.0;
    if (turn < 1.0)
    {
        // page turn: the new pages glide in over the old ones and become solid
        const float e = 1.0f - (float) std::pow (1.0 - turn, 3.0);
        for (int sl = 0; sl < n; ++sl)
            drawPageAt (g, turnFrom + sl, pageSlot (sl), 1.0f, false);
        for (int sl = 0; sl < n; ++sl)
            drawPageAt (g, firstPage + sl, pageSlot (sl).translated ((1.0f - e) * (float) turnDirection * pageSlot (sl).getWidth() * 0.18f, 0.0f), e, true);
    }
    else
        for (int sl = 0; sl < n; ++sl)
            drawPageAt (g, firstPage + sl, pageSlot (sl), 1.0f, true);
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
    // the notes sounding now, drawn on top in red
    if (withHighlights)
        for (auto& id : active)
            if (auto it = page->ids.find (id); it != page->ids.end())
                drawHighlighted (g, *page, it->second, slot);
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
    if (zoomed)
    {
        const int page = currentPage();
        shownPageForLabel = page;
        prev.setEnabled (scroll > 1.0e-3);
        next.setEnabled (scroll < maxScroll() - 1.0e-3);
        pageLabel.setText (count == 0 ? juce::String() : "Seite " + juce::String (page + 1) + " von " + juce::String (count), juce::dontSendNotification);
        return;
    }
    prev.setEnabled (firstPage > 0);
    next.setEnabled (firstPage + n < count);
    if (count == 0)
        pageLabel.setText ({}, juce::dontSendNotification);
    else if (n == 2 && firstPage + 1 < count)
        pageLabel.setText ("Seiten " + juce::String (firstPage + 1) + juce::String::fromUTF8 ("–") + juce::String (firstPage + 2) + " von " + juce::String (count),
                           juce::dontSendNotification);
    else
        pageLabel.setText ("Seite " + juce::String (firstPage + 1) + " von " + juce::String (count), juce::dontSendNotification);
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

void MusicDesk::setHighlight (const juce::String& id, bool)
{
    auto area = areaOfNote (id);
    if (! area.isEmpty())
        repaint (area);
}

// screen area of a note on a visible page (empty if it is not shown)
juce::Rectangle<int> MusicDesk::areaOfNote (const juce::String& id)
{
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
    const auto fit = juce::RectanglePlacement (juce::RectanglePlacement::centred).getTransformToFit (root->getDrawableBounds(), slot);
    t = t.followedBy (juce::AffineTransform::translation (root->getDrawableBounds().getTopLeft())).followedBy (fit);
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
    if (piece == nullptr || piece->notation == nullptr)
    {
        const bool busy = proc.isLoading() || proc.isEngraving();
        if (busy != wasBusy)
            repaint();
        wasBusy = busy;
        return;
    }
    auto& tm = piece->notation->getTimemap();
    const double beat = proc.isPlaying() || proc.getPosition() > 0 ? proc.getCurrentBeat() : -1.0;
    if (std::abs (beat - lastBeat) < 1.0e-6)
        return;

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
    if (follow && newest.isNotEmpty())
    {
        const int page = piece->notation->pageOfElement (newest);
        const int n = visiblePages();
        if (! zoomed && page >= 0 && (page < firstPage || page >= firstPage + n))
            showPage (n == 2 ? page - (page % 2) : page);
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
    auto fit = juce::RectanglePlacement (juce::RectanglePlacement::centred).getTransformToFit (root->getDrawableBounds(), slot);
    const auto origin = root->getDrawableBounds().getTopLeft();
    return root->getLocalArea (d, d->getLocalBounds()).toFloat().translated (origin.x, origin.y).transformedBy (fit);
}

std::unique_ptr<juce::AccessibilityHandler> MusicDesk::createAccessibilityHandler()
{
    setTitle ("Notenpult");
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::button,
        juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [this] {
            if (piece != nullptr && piece->notation == nullptr && piece->notationFromScore)
                proc.requestEngraving();
        }));
}

void MusicDesk::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
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
