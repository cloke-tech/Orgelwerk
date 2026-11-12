#include "MusicDesk.h"
#include "Profile.h"

namespace orgel
{
static const juce::Colour highlightColour { 0xffc0392b };

MusicDesk::MusicDesk (OrgelProcessor& p) : proc (p)
{
    prev.onClick = [this] { follow = false; followButton.setToggleState (false, juce::dontSendNotification); showPage (firstPage - visiblePages()); };
    next.onClick = [this] { follow = false; followButton.setToggleState (false, juce::dontSendNotification); showPage (firstPage + visiblePages()); };
    followButton.setToggleState (true, juce::dontSendNotification);
    followButton.onClick = [this] { follow = followButton.getToggleState(); };
    followButton.setColour (juce::ToggleButton::textColourId, theme::text);
    followButton.setColour (juce::ToggleButton::tickColourId, theme::gold);
    pageLabel.setJustificationType (juce::Justification::centred);
    zoomButton.setButtonText ("Seitenbreite");
    zoomButton.setTooltip ("Zwischen ganzer Seite und Seitenbreite (größere Noten, scrollt mit) umschalten");
    zoomButton.setClickingTogglesState (true);
    zoomButton.onClick = [this] {
        zoomed = zoomButton.getToggleState();
        scroll = scrollTarget = 0.0;
        turnFrom = -1;
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
    status = piece == nullptr ? juce::String (juce::String::fromUTF8 ("Noten hierher ziehen oder über „Öffnen“ laden"))
                              : (piece->notation == nullptr ? (piece->notationFromScore ? juce::String ("Klicken, um die Noten zu setzen")
                                                                                         : juce::String (juce::String::fromUTF8 ("Kein Notenbild verfügbar")))
                                                            : juce::String());
    updateNav();
    repaint();
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
    for (int i = firstPage; i < firstPage + 2 * n + (zoomed ? 1 : 0); ++i)
        requestPage (i, imageSize.x, imageSize.y);
    requestPage (firstPage - n, imageSize.x, imageSize.y);
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
    {
        const float w = area.getWidth(), h = w * 2970.0f / 2100.0f;
        return { area.getX(), area.getY() - (float) scroll * juce::jmax (0.0f, h - area.getHeight()), w, h };
    }
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
    auto drawPage = [&] (int index, juce::Rectangle<float> slot, float alpha, bool withHighlights) {
        if (index < 0 || index >= count)
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
    };
    const int n = visiblePages();
    const double turn = turnFrom >= 0 ? (juce::Time::getMillisecondCounterHiRes() - turnStart) / 280.0 : 1.0;
    if (turn < 1.0)
    {
        // page turn: the new pages glide in over the old ones and become solid
        const float e = 1.0f - (float) std::pow (1.0 - turn, 3.0);
        for (int sl = 0; sl < n; ++sl)
            drawPage (turnFrom + sl, pageSlot (sl), 1.0f, false);
        for (int sl = 0; sl < n; ++sl)
            drawPage (firstPage + sl, pageSlot (sl).translated ((1.0f - e) * (float) turnDirection * pageSlot (sl).getWidth() * 0.18f, 0.0f), e, true);
    }
    else
        for (int sl = 0; sl < n; ++sl)
            drawPage (firstPage + sl, pageSlot (sl), 1.0f, true);
    g.restoreState();
    prefetch();
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
    const int count = piece != nullptr && piece->notation != nullptr ? piece->notation->getPageCount() : 0;
    first = juce::jlimit (0, juce::jmax (0, count - 1), first);
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
    if (zoomed)
        scroll = scrollTarget = 0.0;
    // keep a few pages around, drop the rest
    for (auto it = pages.begin(); it != pages.end();)
        it = std::abs (it->first - firstPage) > 4 ? pages.erase (it) : std::next (it);
    updateNav();
    repaint();
}

void MusicDesk::updateNav()
{
    const int count = piece != nullptr && piece->notation != nullptr ? piece->notation->getPageCount() : 0;
    const int n = visiblePages();
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
    for (int s = 0; s < visiblePages(); ++s)
    {
        auto it = pages.find (firstPage + s);
        if (it == pages.end() || it->second.drawable == nullptr)
            continue;
        auto n = it->second.ids.find (id);
        if (n != it->second.ids.end())
            return elementArea (it->second, n->second, pageSlot (s)).expanded (3.0f).getSmallestIntegerContainer();
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
    if (std::abs (scrollTarget - scroll) > 1.0e-4)
    {
        scroll += (scrollTarget - scroll) * (1.0 - std::exp (-dt * 10.0));
        if (std::abs (scrollTarget - scroll) < 1.0e-4)
            scroll = scrollTarget;
        repaint (viewArea().expanded (8.0f).toNearestInt());
    }
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
        if (page >= 0 && (page < firstPage || page >= firstPage + n))
            showPage (n == 2 ? page - (page % 2) : page);
        if (zoomed && page == firstPage)
            if (auto* pg = findPage (page))
                if (auto it = pg->ids.find (newest); it != pg->ids.end() && pg->drawable != nullptr)
                {
                    // keep the playing system at about a third of the view height
                    const auto view = viewArea();
                    const float h = view.getWidth() * 2970.0f / 2100.0f;
                    const auto full = juce::Rectangle<float> (view.getX(), view.getY(), view.getWidth(), h);
                    const float y = elementArea (*pg, it->second, full).getCentreY() - view.getY();
                    const float range = juce::jmax (1.0f, h - view.getHeight());
                    const double target = juce::jlimit (0.0, 1.0, (double) (y - view.getHeight() * 0.35f) / range);
                    if (std::abs (target - scrollTarget) * range > view.getHeight() * 0.3f)
                        scrollTarget = target;
                }
    }
    if (changed)
        repaint();
}

juce::Rectangle<float> MusicDesk::elementArea (Page& page, juce::Drawable* d, juce::Rectangle<float> slot) const
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
    if (std::abs (w.deltaY) < 0.01f)
        return;
    if (zoomed)
    {
        follow = false;
        followButton.setToggleState (false, juce::dontSendNotification);
        const double next = scrollTarget - w.deltaY * 0.6;
        if (next < 0.0 && firstPage > 0) { showPage (firstPage - 1); scroll = scrollTarget = 1.0; }
        else if (next > 1.0 && piece != nullptr && piece->notation != nullptr && firstPage + 1 < piece->notation->getPageCount()) { showPage (firstPage + 1); scroll = scrollTarget = 0.0; }
        else scrollTarget = juce::jlimit (0.0, 1.0, next);
        repaint();
        return;
    }
    if (std::abs (w.deltaY) < 0.05f)
        return;
    follow = false;
    followButton.setToggleState (false, juce::dontSendNotification);
    showPage (firstPage + (w.deltaY < 0 ? 1 : -1));
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
    for (int s = 0; s < visiblePages(); ++s)
    {
        auto slot = pageSlot (s);
        if (! slot.contains (e.position))
            continue;
        auto* page = findPage (firstPage + s);
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
