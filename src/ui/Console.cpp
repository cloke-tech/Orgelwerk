#include "Console.h"
#include "Profile.h"
#include "../score/NotationBuilder.h"

namespace orgel
{
//==============================================================================
StopKnob::StopKnob (OrgelProcessor& p, int d, int s) : division (d), stop (s), proc (p)
{
    refresh();
}

void StopKnob::refresh()
{
    const juce::ScopedLock sl (proc.organ.getLock());
    if (division >= proc.organ.getNumDivisions() || stop >= (int) proc.organ.getDivision (division).stops.size())
        return;
    auto& st = proc.organ.getDivision (division).stops[(size_t) stop];
    const bool changed = st.on != on || st.name != name;
    on = st.on;
    name = st.name;
    family = classifyStop (name);
    setTitle (name);
    setDescription (on ? "gezogen" : "abgestossen");
    setTooltip (name + "  (SF2 Bank " + juce::String (st.bank) + ", Programm " + juce::String (st.program) + ")\nRechtsklick: verschieben/entfernen");
    if (changed)
        repaint();
}

// The knob is drawn once per state into a picture and then only copied: laying out the
// engraved name is slow, and a registration change from the file flips many knobs at once.
void StopKnob::paint (juce::Graphics& g)
{
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    auto& img = cache[on ? 1 : 0];
    const int iw = juce::roundToInt ((float) getWidth() * scale), ih = juce::roundToInt ((float) getHeight() * scale);
    if (! img.isValid() || img.getWidth() != iw || img.getHeight() != ih || cacheName[on ? 1 : 0] != name)
    {
        img = juce::Image (juce::Image::ARGB, juce::jmax (1, iw), juce::jmax (1, ih), true);
        juce::Graphics ig (img);
        ig.addTransform (juce::AffineTransform::scale (scale));
        drawFace (ig, getLocalBounds().toFloat(), on);
        cacheName[on ? 1 : 0] = name;
    }
    g.drawImageTransformed (img, juce::AffineTransform::scale (1.0f / scale));
}

void StopKnob::drawFace (juce::Graphics& g, juce::Rectangle<float> b, bool on) const
{
    const float size = juce::jmin (b.getWidth(), b.getHeight());
    auto face = b.withSizeKeepingCentre (size, size).reduced (size * (on ? 0.03f : 0.09f));
    const auto c = face.getCentre();

    // shank / shadow
    if (on)
    {
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillEllipse (face.translated (size * 0.04f, size * 0.07f));
    }
    else
    {
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillEllipse (face.expanded (size * 0.02f).translated (0, size * 0.02f));
    }

    // outer ring (wood or gilded when drawn)
    juce::ColourGradient ring (on ? theme::gold.brighter (0.3f) : theme::woodLight, c.x, face.getY(),
                               on ? theme::goldDark : theme::woodDark, c.x, face.getBottom(), false);
    g.setGradientFill (ring);
    g.fillEllipse (face);

    // porcelain face
    auto inner = face.reduced (size * 0.07f);
    juce::ColourGradient porcelain (juce::Colours::white, inner.getX() + inner.getWidth() * 0.3f, inner.getY() + inner.getHeight() * 0.25f,
                                    on ? theme::ivory : theme::ivoryShade, inner.getRight(), inner.getBottom(), true);
    g.setGradientFill (porcelain);
    g.fillEllipse (inner);
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.drawEllipse (inner, 1.0f);
    if (on)
    {
        g.setColour (theme::gold.withAlpha (0.35f));
        g.drawEllipse (face.expanded (1.5f), 2.0f);
    }

    // engraved name
    g.setColour (theme::stopTextColour (family).withAlpha (on ? 1.0f : 0.75f));
    const float fs = juce::jmax (7.5f, size * 0.155f);
    g.setFont (theme::serif (fs, true));
    g.drawFittedText (name, inner.reduced (inner.getWidth() * 0.08f, inner.getHeight() * 0.12f).toNearestInt(),
                      juce::Justification::centred, 3, 0.75f);
}

std::unique_ptr<juce::AccessibilityHandler> StopKnob::createAccessibilityHandler()
{
    struct Handler : public juce::AccessibilityHandler
    {
        Handler (StopKnob& k)
            : juce::AccessibilityHandler (k, juce::AccessibilityRole::toggleButton,
                                          juce::AccessibilityActions()
                                              .addAction (juce::AccessibilityActionType::press, [&k] { k.proc.organ.toggleStop (k.division, k.stop); k.refresh(); })
                                              .addAction (juce::AccessibilityActionType::toggle, [&k] { k.proc.organ.toggleStop (k.division, k.stop); k.refresh(); })),
              knob (k) {}
        juce::AccessibleState getCurrentState() const override
        {
            auto st = juce::AccessibilityHandler::getCurrentState().withCheckable();
            return knob.on ? st.withChecked() : st;
        }
        StopKnob& knob;
    };
    return std::make_unique<Handler> (*this);
}

void StopKnob::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        m.addSectionHeader (name);
        m.addItem (1, "Nach vorne");
        m.addItem (2, "Nach hinten");
        m.addSeparator();
        m.addItem (3, "Register entfernen");
        juce::Component::SafePointer<StopKnob> safe (this);
        auto& p = proc;
        const int d = division, s = stop;
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [&p, d, s] (int r) {
            if (r == 1) p.organ.moveStop (d, s, s - 1);
            if (r == 2) p.organ.moveStop (d, s, s + 1);
            if (r == 3) p.organ.removeStop (d, s);
        });
        return;
    }
    proc.organ.toggleStop (division, stop);
    refresh();
}

//==============================================================================
AddStopButton::AddStopButton (OrgelProcessor& p, int d) : proc (p), division (d)
{
    setTooltip (juce::String::fromUTF8 ("Register aus der SoundFont hinzufügen"));
}

void AddStopButton::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    const float size = juce::jmin (b.getWidth(), b.getHeight()) * 0.62f;
    auto r = b.withSizeKeepingCentre (size, size);
    g.setColour (theme::gold.withAlpha (isMouseOver() ? 0.9f : 0.45f));
    g.drawEllipse (r, 1.5f);
    g.setFont (theme::sans (size * 0.6f, true));
    g.drawText ("+", r, juce::Justification::centred);
}

void AddStopButton::showPresetMenu (OrgelProcessor& p, int division, juce::Component* target)
{
    auto sf = p.organ.getSoundFont();
    if (sf == nullptr)
        return;
    juce::PopupMenu menu;
    std::map<int, juce::PopupMenu> banks;
    std::map<int, std::map<int, juce::PopupMenu>> chunks;
    auto& presets = sf->getPresets();
    for (int i = 0; i < (int) presets.size(); ++i)
    {
        auto& pr = presets[(size_t) i];
        chunks[pr.bank][pr.program / 32].addItem (i + 1, pr.name + "  (" + juce::String (pr.bank) + ":" + juce::String (pr.program) + ")");
    }
    for (auto& [bank, c] : chunks)
    {
        juce::PopupMenu bm;
        for (auto& [chunk, sub] : c)
            bm.addSubMenu ("Programm " + juce::String (chunk * 32) + juce::String::fromUTF8 ("–") + juce::String (chunk * 32 + 31), sub);
        menu.addSubMenu ("Bank " + juce::String (bank), bm);
    }
    // also by stop family, which is how organists think
    juce::PopupMenu families;
    const juce::String familyNames[] = { "Prinzipale", juce::String::fromUTF8 ("Flöten"), "Streicher", "Mixturen & Aliquoten", "Zungen", "Glocken & Effekte" };
    for (int f = 0; f < 6; ++f)
    {
        juce::PopupMenu fm;
        for (int i = 0; i < (int) presets.size(); ++i)
            if ((int) classifyStop (presets[(size_t) i].name) == f)
                fm.addItem (i + 1, presets[(size_t) i].name);
        families.addSubMenu (familyNames[f], fm);
    }
    menu.addSeparator();
    menu.addSubMenu ("Nach Registerfamilie", families);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target), [&p, division, sf] (int r) {
        if (r <= 0)
            return;
        auto& pr = sf->getPresets()[(size_t) (r - 1)];
        p.organ.addStop (division, pr.bank, pr.program, pr.name, true);
    });
}

std::unique_ptr<juce::AccessibilityHandler> AddStopButton::createAccessibilityHandler()
{
    setTitle (juce::String::fromUTF8 ("Register hinzufügen"));
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::button,
        juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [this] { showPresetMenu (proc, division, this); }));
}

void AddStopButton::mouseDown (const juce::MouseEvent&)
{
    showPresetMenu (proc, division, this);
}

//==============================================================================
StopJamb::StopJamb (OrgelProcessor& p) : proc (p)
{
    // kept as a picture: only knobs that change are drawn again
    setBufferedToImage (true);
}

void StopJamb::setDivisions (std::vector<int> indices)
{
    sections.clear();
    const juce::ScopedLock sl (proc.organ.getLock());
    for (int d : indices)
    {
        auto* s = sections.add (new Section());
        s->division = d;
        for (int i = 0; i < (int) proc.organ.getDivision (d).stops.size(); ++i)
            addAndMakeVisible (s->knobs.add (new StopKnob (proc, d, i)));
        s->add = std::make_unique<AddStopButton> (proc, d);
        addAndMakeVisible (*s->add);
    }
    resized();
    repaint();
}

void StopJamb::refreshStates()
{
    for (auto* s : sections)
        for (auto* k : s->knobs)
            k->refresh();
}

void StopJamb::paint (juce::Graphics& g)
{
    ORGEL_PROFILE_SCOPE ("jamb.paint");
    auto b = getLocalBounds().toFloat();
    theme::fillWood (g, b, theme::wood, true);
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.drawRect (b, 2.0f);
    const juce::ScopedLock sl (proc.organ.getLock());
    for (auto* s : sections)
        if (s->division < proc.organ.getNumDivisions())
        {
            auto& d = proc.organ.getDivision (s->division);
            auto label = d.pedal ? d.name : romanNumeral (s->division + 1) + juce::String::fromUTF8 (" · ") + d.name;
            theme::drawPlate (g, s->plate.toFloat(), label, juce::jmin (15.0f, s->plate.getHeight() * 0.7f));
        }
}

void StopJamb::resized()
{
    auto area = getLocalBounds().reduced (8, 8);
    if (sections.isEmpty())
        return;
    const int plateH = 20, gap = 8;
    int knob = 24;
    for (int d = 96; d >= 24; --d)
    {
        const int cols = juce::jmax (1, area.getWidth() / d);
        int total = 0;
        for (auto* s : sections)
        {
            const int n = s->knobs.size() + 1;
            total += plateH + 4 + ((n + cols - 1) / cols) * d + gap;
        }
        if (total <= area.getHeight())
        {
            knob = d;
            break;
        }
    }
    const int cols = juce::jmax (1, area.getWidth() / knob);
    const int xOff = (area.getWidth() - cols * knob) / 2;
    int y = area.getY();
    for (auto* s : sections)
    {
        s->plate = juce::Rectangle<int> (area.getX() + area.getWidth() / 6, y, area.getWidth() * 2 / 3, plateH);
        y += plateH + 4;
        const int n = s->knobs.size() + 1;
        for (int i = 0; i < n; ++i)
        {
            juce::Rectangle<int> r (area.getX() + xOff + (i % cols) * knob, y + (i / cols) * knob, knob, knob);
            if (i < s->knobs.size())
                s->knobs[i]->setBounds (r);
            else
                s->add->setBounds (r);
        }
        y += ((n + cols - 1) / cols) * knob + gap;
    }
}

//==============================================================================
void CouplerRail::Tab::paintButton (juce::Graphics& g, bool over, bool)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f);
    const bool on = getToggleState();
    if (on)
        r = r.translated (0, 2.0f);
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.fillRoundedRectangle (r.translated (0, on ? 1.0f : 3.0f), 3.0f);
    juce::ColourGradient grad (on ? onColour.brighter (0.2f) : theme::ivory, r.getX(), r.getY(),
                               on ? onColour.darker (0.3f) : theme::ivoryShade, r.getX(), r.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (r, 3.0f);
    if (over)
    {
        g.setColour (theme::gold.withAlpha (0.6f));
        g.drawRoundedRectangle (r, 3.0f, 1.2f);
    }
    g.setColour (on ? juce::Colours::white : juce::Colour (0xff2a1c08));
    g.setFont (theme::serif (juce::jmin (14.0f, r.getHeight() * 0.55f), true));
    g.drawFittedText (getButtonText(), r.toNearestInt(), juce::Justification::centred, 1);
}

CouplerRail::CouplerRail (OrgelProcessor& p) : proc (p)
{
    setBufferedToImage (true);
    setButton = std::make_unique<Tab> ("S");
    setButton->setTooltip (juce::String::fromUTF8 ("Setzer: S drücken, dann eine Nummer, um die aktuelle Registrierung zu speichern"));
    setButton->setClickingTogglesState (true);
    setButton->onColour = juce::Colour (0xff2a6ab3);
    addAndMakeVisible (*setButton);
    zeroButton = std::make_unique<Tab> ("0");
    zeroButton->setTooltip (juce::String::fromUTF8 ("Generalauslöser: alle Register und Koppeln ab"));
    zeroButton->onClick = [this] { proc.organ.clearRegistration(); };
    addAndMakeVisible (*zeroButton);
    for (int i = 0; i < 8; ++i)
    {
        auto* t = pistons.add (new Tab (juce::String (i + 1)));
        t->setTooltip ("Kombination " + juce::String (i + 1) + " abrufen (mit S: speichern)");
        t->onClick = [this, i] {
            if (setButton->getToggleState())
            {
                proc.storeCombination (i);
                setButton->setToggleState (false, juce::dontSendNotification);
            }
            else
                proc.recallCombination (i);
            refreshStates();
        };
        addAndMakeVisible (t);
    }
    rebuild();
}

void CouplerRail::rebuild()
{
    couplers.clear();
    const juce::ScopedLock sl (proc.organ.getLock());
    auto& cs = proc.organ.getCouplers();
    for (int i = 0; i < (int) cs.size(); ++i)
    {
        auto* t = couplers.add (new Tab (proc.organ.couplerName (cs[(size_t) i])));
        t->setTooltip ("Koppel " + proc.organ.couplerName (cs[(size_t) i]) + ": "
                       + proc.organ.getDivision (cs[(size_t) i].from).name + " spielt auf " + proc.organ.getDivision (cs[(size_t) i].to).name + " mit");
        t->setToggleState (cs[(size_t) i].on, juce::dontSendNotification);
        t->onClick = [this, i] {
            bool now = false;
            {
                const juce::ScopedLock l (proc.organ.getLock());
                if (i < (int) proc.organ.getCouplers().size())
                    now = ! proc.organ.getCouplers()[(size_t) i].on;
            }
            proc.organ.setCoupler (i, now);
        };
        addAndMakeVisible (t);
    }
    tremulants.clear();
    tremulantDivisions.clear();
    for (int d = 0; d < proc.organ.getNumDivisions(); ++d)
    {
        if (proc.organ.getDivision (d).pedal)
            continue;
        auto* t = tremulants.add (new Tab ("Trem. " + romanNumeral (d + 1)));
        t->onColour = juce::Colour (0xff2a6ab3);
        t->setTooltip ("Tremulant " + proc.organ.getDivision (d).name);
        t->setToggleState (proc.organ.getDivision (d).tremulant, juce::dontSendNotification);
        t->onClick = [this, d] {
            bool now = false;
            {
                const juce::ScopedLock l (proc.organ.getLock());
                if (d < proc.organ.getNumDivisions())
                    now = ! proc.organ.getDivision (d).tremulant;
            }
            proc.organ.setTremulant (d, now);
        };
        tremulantDivisions.push_back (d);
        addAndMakeVisible (t);
    }
    resized();
}

void CouplerRail::refreshStates()
{
    const juce::ScopedLock sl (proc.organ.getLock());
    auto& cs = proc.organ.getCouplers();
    for (int i = 0; i < couplers.size() && i < (int) cs.size(); ++i)
        couplers[i]->setToggleState (cs[(size_t) i].on, juce::dontSendNotification);
    for (int i = 0; i < tremulants.size() && i < (int) tremulantDivisions.size(); ++i)
        if (tremulantDivisions[(size_t) i] < proc.organ.getNumDivisions())
            tremulants[i]->setToggleState (proc.organ.getDivision (tremulantDivisions[(size_t) i]).tremulant, juce::dontSendNotification);
    for (int i = 0; i < pistons.size(); ++i)
        pistons[i]->setAlpha (proc.combinations[i].isNotEmpty() ? 1.0f : 0.6f);
}

void CouplerRail::paint (juce::Graphics& g)
{
    theme::fillWood (g, getLocalBounds().toFloat(), theme::woodDark);
    g.setColour (theme::gold.withAlpha (0.5f));
    g.drawLine (0, 0.5f, (float) getWidth(), 0.5f);
}

void CouplerRail::resized()
{
    auto r = getLocalBounds().reduced (6, 4);
    const int tabW = juce::jmin (54, juce::jmax (36, r.getHeight() * 2));
    auto right = r.removeFromRight ((pistons.size() + 2) * (tabW - 10) + 12);
    for (auto* t : couplers)
        t->setBounds (r.removeFromLeft (tabW).reduced (1, 0));
    r.removeFromLeft (10);
    for (auto* t : tremulants)
        t->setBounds (r.removeFromLeft (tabW + 14).reduced (1, 0));
    const int pw = tabW - 10;
    setButton->setBounds (right.removeFromLeft (pw).reduced (1, 0));
    for (auto* t : pistons)
        t->setBounds (right.removeFromLeft (pw).reduced (1, 0));
    zeroButton->setBounds (right.removeFromLeft (pw).reduced (1, 0));
}

//==============================================================================
KeyboardView::KeyboardView (OrgelProcessor& p, int d) : division (d), proc (p)
{
    const juce::ScopedLock sl (proc.organ.getLock());
    auto& div = proc.organ.getDivision (d);
    pedal = div.pedal;
    low = div.lowKey();
    high = div.highKey();
}

bool KeyboardView::isBlack (int key)
{
    const int pc = key % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

int KeyboardView::numWhite() const
{
    int n = 0;
    for (int k = low; k <= high; ++k)
        n += isBlack (k) ? 0 : 1;
    return n;
}

juce::Rectangle<float> KeyboardView::keyRect (int key) const
{
    const float w = getWidth() / (float) numWhite();
    int whiteIndex = 0;
    for (int k = low; k < key; ++k)
        whiteIndex += isBlack (k) ? 0 : 1;
    const float h = (float) getHeight();
    if (! isBlack (key))
        return { whiteIndex * w, 0, w, h };
    const float bw = w * (pedal ? 0.55f : 0.6f);
    return { whiteIndex * w - bw / 2, 0, bw, h * (pedal ? 0.55f : 0.62f) };
}

int KeyboardView::keyAt (juce::Point<float> p) const
{
    for (int k = low; k <= high; ++k)
        if (isBlack (k) && keyRect (k).contains (p))
            return k;
    for (int k = low; k <= high; ++k)
        if (! isBlack (k) && keyRect (k).contains (p))
            return k;
    return -1;
}

void KeyboardView::paint (juce::Graphics& g)
{
    ORGEL_PROFILE_SCOPE ("keyboard.paint");
    auto b = getLocalBounds().toFloat();
    g.setColour (theme::woodDark.darker (0.5f));
    g.fillRect (b);

    for (int k = low; k <= high; ++k)
    {
        if (isBlack (k))
            continue;
        auto r = keyRect (k).reduced (pedal ? 2.5f : 0.6f, 0);
        const bool down = held[(size_t) k];
        if (pedal)
        {
            juce::ColourGradient grad (down ? theme::activeKey : juce::Colour (0xffc89a62), r.getX(), r.getY(),
                                       down ? theme::activeKey.darker (0.3f) : juce::Colour (0xff8a5a2c), r.getX(), r.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (r, 2.0f);
        }
        else
        {
            juce::ColourGradient grad (down ? theme::activeKey.brighter (0.2f) : theme::ivory, r.getX(), r.getY(),
                                       down ? theme::activeKey : theme::ivoryShade, r.getX(), r.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRect (r);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.drawRect (r, 0.6f);
        }
        if (k % 12 == 0 && ! pedal && r.getWidth() > 10)
        {
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.setFont (theme::sans (juce::jmin (10.0f, r.getWidth() * 0.6f)));
            g.drawText ("C" + juce::String (k / 12 - 1), r.removeFromBottom (14), juce::Justification::centred);
        }
    }
    for (int k = low; k <= high; ++k)
    {
        if (! isBlack (k))
            continue;
        auto r = keyRect (k);
        const bool down = held[(size_t) k];
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRect (r.translated (1.5f, 1.5f));
        juce::ColourGradient grad (down ? theme::activeKey.darker (0.1f) : juce::Colour (0xff3a332e), r.getX(), r.getY(),
                                   down ? theme::activeKey.darker (0.5f) : theme::ebony, r.getX(), r.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (r, 1.5f);
    }
    // key slip / shadow at the top
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.5f), 0, 0, juce::Colours::transparentBlack, 0, 6, false));
    g.fillRect (b.withHeight (6));
}

void KeyboardView::update()
{
    auto h = proc.organ.getHeldKeys (division);
    if (h != held)
    {
        held = h;
        repaint();
    }
}

void KeyboardView::mouseDown (const juce::MouseEvent& e)
{
    mouseKey = keyAt (e.position);
    if (mouseKey >= 0)
        proc.organ.keyDown (division, mouseKey, srcUI);
    update();
}

void KeyboardView::mouseDrag (const juce::MouseEvent& e)
{
    const int k = keyAt (e.position);
    if (k == mouseKey)
        return;
    if (mouseKey >= 0)
        proc.organ.keyUp (division, mouseKey, srcUI);
    mouseKey = k;
    if (mouseKey >= 0)
        proc.organ.keyDown (division, mouseKey, srcUI);
    update();
}

void KeyboardView::mouseUp (const juce::MouseEvent&)
{
    if (mouseKey >= 0)
        proc.organ.keyUp (division, mouseKey, srcUI);
    mouseKey = -1;
    update();
}

//==============================================================================
DivisionPlate::DivisionPlate (OrgelProcessor& p, int d) : division (d), proc (p)
{
    setBufferedToImage (true);
    setTooltip (juce::String::fromUTF8 ("Klicken: Name, MIDI-Eingang und Kanal, Lautstärke"));
}

DivisionPlate::Texts DivisionPlate::texts() const
{
    Texts t;
    const juce::ScopedLock sl (proc.organ.getLock());
    if (division >= proc.organ.getNumDivisions())
        return t;
    auto& d = proc.organ.getDivision (division);
    t.valid = true;
    t.pedal = d.pedal;
    t.learning = proc.getMidiLearnDivision() == division;
    t.title = d.pedal ? d.name : romanNumeral (division + 1) + "  " + d.name;
    if (t.learning)
        t.midi = juce::String::fromUTF8 ("Taste drücken…");
    else
    {
        auto dev = d.midi.device.isEmpty() ? juce::String (juce::String::fromUTF8 ("Alle Eingänge"))
                                           : (d.midi.device == MidiAssign::hostDevice ? juce::String ("DAW") : d.midi.deviceName);
        t.midi = dev + juce::String::fromUTF8 (" · ") + (d.midi.channel == 0 ? juce::String ("Omni") : "Kanal " + juce::String (d.midi.channel));
    }
    return t;
}

// repaints only when something on the plate changed (it is kept as a picture)
void DivisionPlate::refresh()
{
    auto t = texts();
    const auto key = t.title + "|" + t.midi + (t.learning ? "L" : "") + (proc.colourManuals ? "C" : "");
    if (key != shownKey)
    {
        shownKey = key;
        repaint();
    }
}

void DivisionPlate::paint (juce::Graphics& g)
{
    const auto t = texts();
    if (! t.valid)
        return;
    const auto& title = t.title;
    const auto& midi = t.midi;
    auto r = getLocalBounds().toFloat().reduced (3.0f, 3.0f);
    auto plate = r.removeFromTop (juce::jmin (22.0f, r.getHeight() * 0.55f));
    theme::drawPlate (g, plate, title, 13.0f);
    // the colour this manual's notes have on the music desk (when the notes are coloured)
    if (! t.pedal && proc.colourManuals)
    {
        auto dot = plate.removeFromLeft (plate.getHeight()).reduced (5.0f);
        g.setColour (juce::Colour::fromString ("FF" + NotationBuilder::manualColour (division).substring (1)));
        g.fillEllipse (dot);
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.drawEllipse (dot, 1.2f);
    }
    g.setColour (t.learning ? theme::activeKey : theme::text.withAlpha (0.8f));
    g.setFont (theme::sans (11.0f));
    g.drawFittedText (midi, r.toNearestInt(), juce::Justification::centred, 2);
}

void DivisionPlate::mouseDown (const juce::MouseEvent&)
{
    openSettings();
}

void DivisionPlate::openSettings()
{
    auto content = std::make_unique<DivisionSettings> (proc, division);
    content->setSize (330, 300);
    // inside the editor: works the same in the app and in every DAW
    if (auto* editor = findParentComponentOfClass<juce::AudioProcessorEditor>())
        juce::CallOutBox::launchAsynchronously (std::move (content), editor->getLocalArea (this, getLocalBounds()), editor);
    else
        juce::CallOutBox::launchAsynchronously (std::move (content), getScreenBounds(), nullptr);
}

std::unique_ptr<juce::AccessibilityHandler> DivisionPlate::createAccessibilityHandler()
{
    {
        const juce::ScopedLock sl (proc.organ.getLock());
        if (division < proc.organ.getNumDivisions())
            setTitle (proc.organ.getDivision (division).name + " Einstellungen");
    }
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::button,
        juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [this] { openSettings(); }));
}

//==============================================================================
DivisionSettings::DivisionSettings (OrgelProcessor& p, int d) : proc (p), division (d)
{
    for (auto* l : { &nameLabel, &deviceLabel, &channelLabel, &volumeLabel })
    {
        l->setColour (juce::Label::textColourId, theme::text);
        addAndMakeVisible (l);
    }
    {
        const juce::ScopedLock sl (proc.organ.getLock());
        auto& div = proc.organ.getDivision (d);
        name.setText (div.name, false);
        volume.setRange (0.0, 1.5, 0.01);
        volume.setValue (div.volume, juce::dontSendNotification);
        channel.addItem (juce::String::fromUTF8 ("Omni (alle Kanäle)"), 1);
        for (int c = 1; c <= 16; ++c)
            channel.addItem ("Kanal " + juce::String (c), c + 1);
        channel.setSelectedId (div.midi.channel + 1, juce::dontSendNotification);
        remove.setEnabled (! div.pedal && proc.organ.getNumManuals() > 1);
    }
    fillDevices();

    name.onReturnKey = name.onFocusLost = [this] { proc.organ.renameDivision (division, name.getText()); };
    device.onChange = channel.onChange = [this] { applyMidi(); };
    volume.onValueChange = [this] { proc.organ.setDivisionVolume (division, (float) volume.getValue()); };
    learn.onClick = [this] { proc.startMidiLearn (division); learn.setButtonText (juce::String::fromUTF8 ("Taste am Gerät drücken…")); };
    addStop.onClick = [this] { AddStopButton::showPresetMenu (proc, division, &addStop); };
    allOff.onClick = [this] {
        const int n = (int) proc.organ.getDivision (division).stops.size();
        for (int s = 0; s < n; ++s)
            proc.organ.setStop (division, s, false);
    };
    remove.onClick = [this] {
        const int d = division;
        auto& p = proc;
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
        p.organ.removeDivision (d);
    };
    for (auto* c : std::initializer_list<juce::Component*> { &name, &device, &channel, &learn, &addStop, &allOff, &remove, &volume })
        addAndMakeVisible (c);
    startTimerHz (5);
}

void DivisionSettings::fillDevices()
{
    device.clear (juce::dontSendNotification);
    deviceIds = { MidiAssign::anyDevice };
    deviceNames = { juce::String::fromUTF8 ("Alle Eingänge") };
    if (! proc.isStandalone())
    {
        deviceIds.add (MidiAssign::hostDevice);
        deviceNames.add ("DAW / Host");
    }
    else
        for (auto& info : proc.getMidiRouter().connectedDevices())
        {
            deviceIds.add (info.identifier);
            deviceNames.add (info.name);
        }
    MidiAssign current;
    {
        const juce::ScopedLock sl (proc.organ.getLock());
        current = proc.organ.getDivision (division).midi;
    }
    if (! deviceIds.contains (current.device))
    {
        deviceIds.add (current.device);
        deviceNames.add (current.deviceName + " (nicht verbunden)");
    }
    for (int i = 0; i < deviceIds.size(); ++i)
        device.addItem (deviceNames[i], i + 1);
    device.setSelectedId (deviceIds.indexOf (current.device) + 1, juce::dontSendNotification);
}

void DivisionSettings::applyMidi()
{
    MidiAssign a;
    const int i = device.getSelectedId() - 1;
    a.device = deviceIds[i];
    a.deviceName = deviceNames[i].upToFirstOccurrenceOf (" (nicht verbunden)", false, false);
    a.channel = juce::jmax (0, channel.getSelectedId() - 1);
    proc.organ.setMidiAssign (division, a);
}

void DivisionSettings::timerCallback()
{
    if (proc.getMidiLearnDivision() != division && learn.getButtonText() != "MIDI lernen")
    {
        learn.setButtonText ("MIDI lernen");
        fillDevices();
        const juce::ScopedLock sl (proc.organ.getLock());
        channel.setSelectedId (proc.organ.getDivision (division).midi.channel + 1, juce::dontSendNotification);
    }
}

void DivisionSettings::paint (juce::Graphics& g)
{
    g.fillAll (theme::woodDark);
}

void DivisionSettings::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto row = [&r] { auto x = r.removeFromTop (28); r.removeFromTop (6); return x; };
    auto a = row(); nameLabel.setBounds (a.removeFromLeft (100)); name.setBounds (a);
    a = row(); deviceLabel.setBounds (a.removeFromLeft (100)); device.setBounds (a);
    a = row(); channelLabel.setBounds (a.removeFromLeft (100)); channel.setBounds (a);
    learn.setBounds (row());
    a = row(); volumeLabel.setBounds (a.removeFromLeft (100)); volume.setBounds (a);
    a = row(); addStop.setBounds (a.removeFromLeft (a.getWidth() / 2).reduced (2, 0)); allOff.setBounds (a.reduced (2, 0));
    remove.setBounds (row());
}
} // namespace orgel
