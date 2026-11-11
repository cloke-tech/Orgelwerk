#include "PluginEditor.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

namespace orgel
{
static juce::String formatTime (double s)
{
    const int t = (int) std::floor (juce::jmax (0.0, s));
    return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
}

OrgelEditor::OrgelEditor (OrgelProcessor& p)
    : AudioProcessorEditor (&p), proc (p), leftJamb (p), rightJamb (p), desk (p), rail (p)
{
    auto& lf = getLookAndFeel();
    lf.setColour (juce::TextButton::buttonColourId, theme::wood.brighter (0.1f));
    lf.setColour (juce::TextButton::textColourOffId, theme::text);
    lf.setColour (juce::ComboBox::backgroundColourId, theme::woodDark);
    lf.setColour (juce::Slider::thumbColourId, theme::gold);
    lf.setColour (juce::Slider::trackColourId, theme::goldDark);
    lf.setColour (juce::Slider::rotarySliderFillColourId, theme::gold);
    lf.setColour (juce::Slider::textBoxTextColourId, theme::text);
    lf.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    lf.setColour (juce::PopupMenu::backgroundColourId, theme::woodDark);
    lf.setColour (juce::PopupMenu::textColourId, theme::text);
    lf.setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::goldDark);
    lf.setColour (juce::ToggleButton::textColourId, theme::text);
    lf.setColour (juce::ToggleButton::tickColourId, theme::gold);

    rewindButton.setButtonText (juce::String::fromUTF8 ("\xe2\x8f\xae"));
    playButton.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xb6"));
    stopButton.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xa0"));
    rewindButton.setTooltip ("Zum Anfang");
    playButton.setTooltip ("Abspielen / Pause (Leertaste)");
    stopButton.setTooltip ("Stopp");
    openButton.setTooltip (juce::String::fromUTF8 ("Noten öffnen: MIDI, MusicXML, MEI, ABC, Humdrum, … (oder Datei auf das Fenster ziehen)"));
    exportButton.setTooltip ("Noten oder Orgelklang in eine Datei schreiben");
    partsButton.setTooltip ("Welche Stimme auf welchem Manual oder dem Pedal spielt");
    addManualButton.setTooltip (juce::String::fromUTF8 ("Ein weiteres Manual hinzufügen"));
    registrationToggle.setTooltip (juce::String::fromUTF8 ("Register aus der Datei ziehen (Programmwechsel), wenn ein Stück geladen wird"));

    openButton.onClick = [this] { openFile(); };
    exportButton.onClick = [this] { showExportMenu(); };
    rewindButton.onClick = [this] { proc.seek (0.0); };
    playButton.onClick = [this] { togglePlay(); };
    stopButton.onClick = [this] { proc.stop(); };
    partsButton.onClick = [this] { showPartMenu(); };
    trackButton.onClick = [this] { showTrackMenu(); };
    trackButton.setTooltip (juce::String::fromUTF8 ("Registrierungen an Stellen im Stück merken; beim Abspielen wird dort umgeschaltet (Taste R)"));
    addManualButton.onClick = [this] { proc.organ.addManual(); };
    soundFontButton.onClick = [this] { showSoundFontMenu(); };
    optionsButton.onClick = [this] { showOptionsMenu(); };
    registrationToggle.setToggleState (proc.applyFileRegistration, juce::dontSendNotification);
    registrationToggle.onClick = [this] {
        proc.applyFileRegistration = registrationToggle.getToggleState();
        if (proc.applyFileRegistration)
            proc.applyPieceRegistration();
    };

    position.setRange (0.0, 1.0);
    position.onDragStart = [this] { draggingPosition = true; };
    position.onDragEnd = [this] { draggingPosition = false; proc.seek (position.getValue()); };
    position.setTooltip (juce::String::fromUTF8 ("Position im Stück"));

    speed.setRange (25.0, 200.0, 1.0);
    speed.setTextValueSuffix (" %");
    speed.setValue (proc.getSpeed() * 100.0, juce::dontSendNotification);
    speed.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
    speed.onValueChange = [this] { proc.setSpeed ((float) speed.getValue() / 100.0f); };
    speed.setDoubleClickReturnValue (true, 100.0);

    reverb.setRange (0.0, 1.0, 0.01);
    reverb.setValue (proc.organ.reverb, juce::dontSendNotification);
    reverb.onValueChange = [this] { proc.organ.reverb = (float) reverb.getValue(); };
    reverb.setTooltip ("Kirchenhall");
    volume.setRange (0.0, 1.5, 0.01);
    volume.setValue (proc.organ.master, juce::dontSendNotification);
    volume.onValueChange = [this] { proc.organ.master = (float) volume.getValue(); };
    volume.setTooltip (juce::String::fromUTF8 ("Gesamtlautstärke"));

    for (auto* l : { &timeLabel, &titleLabel, &speedLabel, &reverbLabel, &volumeLabel })
    {
        l->setColour (juce::Label::textColourId, theme::text);
        l->setFont (theme::sans (12.0f));
        addAndMakeVisible (l);
    }
    titleLabel.setFont (theme::serif (16.0f, true));
    titleLabel.setColour (juce::Label::textColourId, theme::gold);
    reverbLabel.setJustificationType (juce::Justification::centred);
    volumeLabel.setJustificationType (juce::Justification::centred);

    for (auto* c : std::initializer_list<juce::Component*> { &openButton, &exportButton, &rewindButton, &playButton, &stopButton, &position,
                                                             &speed, &partsButton, &trackButton, &registrationToggle, &addManualButton, &soundFontButton,
                                                             &optionsButton, &reverb, &volume, &markerStrip, &leftJamb, &rightJamb, &desk, &rail })
        addAndMakeVisible (c);

    proc.organ.addChangeListener (this);
    proc.addChangeListener (this);
    rebuildConsole();
    shownPiece = proc.getPiece();
    desk.setPiece (shownPiece);

    setResizable (true, true);
    setResizeLimits (1100, 720, 3200, 2200);
    setSize (1480, 940);
    setWantsKeyboardFocus (true);
    startTimerHz (30);
}

OrgelEditor::~OrgelEditor()
{
    proc.organ.removeChangeListener (this);
    proc.removeChangeListener (this);
}

//==============================================================================
juce::String OrgelEditor::structureSignature() const
{
    const juce::ScopedLock sl (proc.organ.getLock());
    juce::String s;
    for (int d = 0; d < proc.organ.getNumDivisions(); ++d)
    {
        auto& div = proc.organ.getDivision (d);
        s << div.name << ":" << (int) div.stops.size() << ":";
        for (auto& st : div.stops)
            s << st.program << "." << st.bank << ",";
        s << ";";
    }
    s << (int) proc.organ.getCouplers().size();
    return s;
}

void OrgelEditor::rebuildConsole()
{
    lastSignature = structureSignature();
    keyboards.clear();
    plates.clear();
    swells.clear();

    std::vector<int> order;
    int total = 0;
    {
        const juce::ScopedLock sl (proc.organ.getLock());
        const int ped = proc.organ.pedalIndex();
        if (ped >= 0)
            order.push_back (ped);
        for (int d = 0; d < proc.organ.getNumDivisions(); ++d)
            if (d != ped)
                order.push_back (d);
        for (int d : order)
            total += (int) proc.organ.getDivision (d).stops.size() + 1;
    }
    // pedal and the lower manuals on the left jamb, the rest on the right
    std::vector<int> left, right;
    int acc = 0;
    for (int d : order)
    {
        const int n = (int) proc.organ.getDivision (d).stops.size() + 1;
        if (left.empty() || (acc + n / 2 <= total / 2 && right.empty()))
        {
            left.push_back (d);
            acc += n;
        }
        else
            right.push_back (d);
    }
    leftJamb.setDivisions (left);
    rightJamb.setDivisions (right);
    rail.rebuild();

    for (int d = 0; d < proc.organ.getNumDivisions(); ++d)
    {
        addAndMakeVisible (keyboards.add (new KeyboardView (proc, d)));
        addAndMakeVisible (plates.add (new DivisionPlate (proc, d)));
        auto* s = swells.add (new juce::Slider (juce::Slider::LinearVertical, juce::Slider::NoTextBox));
        s->setRange (0.0, 1.5, 0.01);
        s->setValue (proc.organ.getDivision (d).volume, juce::dontSendNotification);
        s->setTooltip (juce::String::fromUTF8 ("Lautstärke ") + proc.organ.getDivision (d).name);
        s->onValueChange = [this, d, s] { proc.organ.setDivisionVolume (d, (float) s->getValue()); };
        addAndMakeVisible (s);
    }
    resized();
    repaint();
}

void OrgelEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (structureSignature() != lastSignature)
        rebuildConsole();
    else
    {
        leftJamb.refreshStates();
        rightJamb.refreshStates();
        rail.refreshStates();
        for (auto* p : plates)
            p->repaint();
    }
    auto piece = proc.getPiece();
    if (piece != shownPiece)
    {
        shownPiece = piece;
        desk.setPiece (piece);
    }
    registrationToggle.setToggleState (proc.applyFileRegistration, juce::dontSendNotification);
    auto msg = proc.takeLoadMessage();
    if (msg.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Orgelwerk", msg);
}

void OrgelEditor::timerCallback()
{
    for (auto* k : keyboards)
        k->update();
    updateFacade();
    desk.update();
    for (auto* p : plates)
        if (proc.getMidiLearnDivision() == p->division)
            p->repaint();

    const double len = proc.getLength();
    position.setEnabled (len > 0);
    if (len > 0)
    {
        position.setRange (0.0, len, 0.0);
        if (! draggingPosition)
            position.setValue (proc.getPosition(), juce::dontSendNotification);
    }
    {
        std::vector<double> times;
        if (auto pc = proc.getPiece())
            for (auto& m : proc.getMarks())
                times.push_back (pc->score->beatToSeconds (m.beat));
        markerStrip.setMarks (times, len);
    }
    timeLabel.setText (formatTime (draggingPosition ? position.getValue() : proc.getPosition()) + " / " + formatTime (len), juce::dontSendNotification);
    playButton.setButtonText (proc.isPlaying() ? juce::String::fromUTF8 ("\xe2\x8f\xb8") : juce::String::fromUTF8 ("\xe2\x96\xb6"));
    auto piece = proc.getPiece();
    titleLabel.setText (proc.isLoading() ? juce::String (juce::String::fromUTF8 ("Lade…")) : (piece != nullptr ? piece->score->title : juce::String ("Orgelwerk")),
                        juce::dontSendNotification);
}

void OrgelEditor::togglePlay()
{
    if (proc.isPlaying())
        proc.pause();
    else
        proc.play();
}

// Computer keyboard as a small manual (Manual I): A W S E D F T G Z/Y H U J K O L P,
// with , and . for octave down/up.
static int computerKeyOffset (int keyCode)
{
    static const std::pair<char, int> map[] = { { 'A', 0 }, { 'W', 1 }, { 'S', 2 }, { 'E', 3 }, { 'D', 4 }, { 'F', 5 }, { 'T', 6 },
                                                { 'G', 7 }, { 'Z', 8 }, { 'Y', 8 }, { 'H', 9 }, { 'U', 10 }, { 'J', 11 }, { 'K', 12 },
                                                { 'O', 13 }, { 'L', 14 }, { 'P', 15 } };
    for (auto& [c, o] : map)
        if (keyCode == c || keyCode == c + 32)
            return o;
    return -1;
}

bool OrgelEditor::keyStateChanged (bool)
{
    bool handled = false;
    std::set<int> now;
    for (char c : std::string ("AWSEDFTGZYHUJKOLP"))
        if (juce::KeyPress::isKeyCurrentlyDown (c) || juce::KeyPress::isKeyCurrentlyDown (c + 32))
            now.insert (computerOctave + computerKeyOffset (c));
    for (int k : computerKeysDown)
        if (! now.count (k))
        {
            proc.organ.keyUp (0, k, srcUI);
            handled = true;
        }
    for (int k : now)
        if (! computerKeysDown.count (k))
        {
            proc.organ.keyDown (0, k, srcUI);
            handled = true;
        }
    computerKeysDown = now;
    return handled;
}

bool OrgelEditor::keyPressed (const juce::KeyPress& key)
{
    if (computerKeyOffset (key.getKeyCode()) >= 0)
        return true;
    if (key.getTextCharacter() == ',' || key.getTextCharacter() == '.')
    {
        for (int k : computerKeysDown)
            proc.organ.keyUp (0, k, srcUI);
        computerKeysDown.clear();
        computerOctave = juce::jlimit (36, 84, computerOctave + (key.getTextCharacter() == '.' ? 12 : -12));
        return true;
    }
    if (key.getTextCharacter() == 'r' || key.getTextCharacter() == 'R')
    {
        proc.addMarkHere();
        return true;
    }
    if (key == juce::KeyPress::spaceKey)
    {
        togglePlay();
        return true;
    }
    if (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)
    {
        proc.seek (proc.getPosition() + (key == juce::KeyPress::leftKey ? -5.0 : 5.0));
        return true;
    }
    return false;
}

//==============================================================================
// Background of the facade (wood, cornice); the pipes themselves are drawn live so they can move.
void OrgelEditor::drawFacade (juce::Graphics& g, juce::Rectangle<float> area)
{
    theme::fillWood (g, area, theme::woodDark);
    // three towers with flats between: pipe lengths form arches. Leave head room for the bounce.
    pipes.clear();
    const int count = juce::jlimit (21, 61, (int) (area.getWidth() / 22.0f)) | 1;
    const float pw = area.getWidth() / (count + 2.0f);
    const float baseY = area.getBottom() - 6.0f;
    for (int i = 0; i < count; ++i)
    {
        const float t = (float) i / (count - 1);
        const float tower = std::abs (std::sin (t * juce::MathConstants<float>::pi * 2.0f));
        const float centre = 1.0f - std::abs (t - 0.5f) * 2.0f;
        const float len = (area.getHeight() - 14.0f) * (0.40f + 0.30f * (1.0f - tower) + 0.12f * centre);
        const float x = area.getX() + pw * (i + 1.5f);
        const float w = pw * 0.72f;
        FacadePipe p;
        p.body = { x - w / 2, baseY - len, w, len * 0.82f };
        p.mouthY = p.body.getBottom() - w * 0.9f;
        p.baseY = baseY;
        pipes.push_back (p);
    }
    // longer pipes speak lower notes: assign pitches by length
    std::vector<int> order (pipes.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = (int) i;
    std::stable_sort (order.begin(), order.end(), [this] (int a, int b) {
        return pipes[(size_t) a].body.getHeight() > pipes[(size_t) b].body.getHeight();
    });
    for (size_t r = 0; r < order.size(); ++r)
        pipes[(size_t) order[r]].note = 36 + (int) std::round ((double) r * 60.0 / std::max<size_t> (1, order.size() - 1));

    auto cornice = area.withHeight (8.0f);
    g.setGradientFill (juce::ColourGradient (theme::gold, 0, cornice.getY(), theme::goldDark, 0, cornice.getBottom(), false));
    g.fillRect (cornice);
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.fillRect (area.withY (area.getBottom() - 6.0f).withHeight (6.0f));
}

void OrgelEditor::updateFacade()
{
    if (pipes.empty())
        return;
    auto counts = proc.organ.getSoundingCounts();
    std::vector<float> target (pipes.size(), 0.0f);
    for (int k = 0; k < 128; ++k)
    {
        if (counts[(size_t) k] == 0)
            continue;
        int key = k;
        while (key < 36) key += 12;
        while (key > 96) key -= 12;
        size_t best = 0;
        for (size_t i = 1; i < pipes.size(); ++i)
            if (std::abs (pipes[i].note - key) < std::abs (pipes[best].note - key))
                best = i;
        target[best] = juce::jmin (1.0f, target[best] + 0.35f + 0.12f * counts[(size_t) k]);
    }
    const double now = juce::Time::getMillisecondCounterHiRes();
    bool changed = false;
    for (size_t i = 0; i < pipes.size(); ++i)
    {
        auto& p = pipes[i];
        // a note starts (or gets louder): the pipe jumps up and wobbles back
        if (target[i] > p.lastTarget + 0.2f)
            p.popStart = now;
        p.lastTarget = target[i];
        const float next = target[i] > p.level ? target[i] : p.level * 0.82f;
        const float clipped = next < 0.02f ? 0.0f : next;
        changed |= std::abs (clipped - p.level) > 0.005f || now - p.popStart < 700.0;
        p.level = clipped;
    }
    if (changed)
        repaint (facadeArea);
}

void OrgelEditor::drawPipe (juce::Graphics& g, const FacadePipe& p, juce::Point<float> offset, double nowMs)
{
    // cartoon squash and stretch: shoot up, overshoot, settle
    const double dt = nowMs - p.popStart;
    float stretch = 0.0f;
    if (dt >= 0.0 && dt < 700.0)
        stretch = (float) (std::exp (-dt / 150.0) * std::cos (dt / 48.0));
    // stays tall while it sounds (level), jumps and wobbles when it starts (stretch)
    const float sy = 1.0f + 0.16f * p.level + 0.24f * stretch;
    const float sx = 1.0f + 0.06f * p.level - 0.14f * stretch;

    const float baseY = p.baseY + offset.y;
    const float x = p.body.getCentreX() + offset.x;
    const float w = p.body.getWidth() * sx;
    const float bodyH = p.body.getHeight() * sy;
    const float footH = (p.baseY - p.body.getBottom()) * (1.0f + 0.15f * stretch);
    const float bodyBottom = baseY - footH;
    juce::Rectangle<float> body (x - w / 2, bodyBottom - bodyH, w, bodyH);

    juce::ColourGradient metal (juce::Colour (0xff8d9196), body.getX(), 0, juce::Colour (0xff9da1a6), body.getRight(), 0, false);
    metal.addColour (0.35, juce::Colour (0xfff3f4f6));
    metal.addColour (0.55, juce::Colour (0xffc6c9cd));
    g.setGradientFill (metal);
    g.fillRoundedRectangle (body.withTrimmedBottom (-1.0f), juce::jmin (w * 0.5f, 3.0f));
    juce::Path foot;
    foot.startNewSubPath (body.getX(), bodyBottom);
    foot.lineTo (body.getRight(), bodyBottom);
    foot.lineTo (x + w * 0.12f, baseY);
    foot.lineTo (x - w * 0.12f, baseY);
    foot.closeSubPath();
    g.fillPath (foot);

    if (p.level > 0.0f)
    {
        juce::ColourGradient shine (juce::Colours::white.withAlpha (0.0f), body.getX(), 0,
                                    juce::Colours::white.withAlpha (0.0f), body.getRight(), 0, false);
        shine.addColour (0.25, juce::Colour (0xffffe9a8).withAlpha (0.55f * p.level));
        shine.addColour (0.42, juce::Colour (0xfffff6d8).withAlpha (0.95f * p.level));
        shine.addColour (0.7, juce::Colour (0xffffd77a).withAlpha (0.45f * p.level));
        g.setGradientFill (shine);
        g.fillRect (body);
    }

    // the mouth opens while the pipe sings
    const float mouthH = w * 0.32f * (1.0f + 1.3f * p.level + 0.4f * juce::jmax (0.0f, stretch));
    const float mouthY = bodyBottom - w * 0.75f - mouthH * 0.5f;
    g.setColour (juce::Colour (0xff2a2a2e));
    g.fillEllipse (x - w * 0.3f, mouthY, w * 0.6f, mouthH);
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.drawRoundedRectangle (body, juce::jmin (w * 0.5f, 3.0f), 0.6f);

    if (p.level > 0.0f)
    {
        const float flicker = 0.85f + 0.15f * (float) std::sin (nowMs * 0.023 + x * 0.3);
        const float r = w * (1.6f + 1.6f * p.level);
        const juce::Point<float> mouth (x, mouthY + mouthH * 0.5f);
        juce::ColourGradient halo (theme::gold.brighter (0.4f).withAlpha (0.9f * p.level * flicker), mouth.x, mouth.y,
                                   theme::gold.withAlpha (0.0f), mouth.x + r, mouth.y, true);
        g.setGradientFill (halo);
        g.fillEllipse (juce::Rectangle<float> (r * 2, r * 2).withCentre (mouth));
    }
}

void OrgelEditor::paintFacadeGlow (juce::Graphics& g)
{
    const juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (facadeArea.withTrimmedTop (8));
    const double now = juce::Time::getMillisecondCounterHiRes();
    const auto offset = facadeArea.getPosition().toFloat();
    // quiet pipes first, singing pipes on top so their glow overlaps the neighbours
    for (auto& p : pipes)
        if (p.level <= 0.0f && now - p.popStart >= 700.0)
            drawPipe (g, p, offset, now);
    for (auto& p : pipes)
        if (p.level > 0.0f || now - p.popStart < 700.0)
            drawPipe (g, p, offset, now);
}

void OrgelEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::woodDark);
    // toolbar
    auto bar = getLocalBounds().removeFromTop (48).toFloat();
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1e130b), 0, 0, juce::Colour (0xff120b06), 0, bar.getBottom(), false));
    g.fillRect (bar);
    g.setColour (theme::gold.withAlpha (0.35f));
    g.drawLine (0, bar.getBottom() - 0.5f, (float) getWidth(), bar.getBottom() - 0.5f);

    if (! facadeArea.isEmpty())
    {
        if (facadeCache.getWidth() != facadeArea.getWidth() || facadeCache.getHeight() != facadeArea.getHeight())
        {
            facadeCache = juce::Image (juce::Image::RGB, facadeArea.getWidth(), facadeArea.getHeight(), true);
            juce::Graphics fg (facadeCache);
            drawFacade (fg, facadeCache.getBounds().toFloat());
        }
        g.drawImageAt (facadeCache, facadeArea.getX(), facadeArea.getY());
        paintFacadeGlow (g);
    }
    // console body behind the keyboards
    theme::fillWood (g, consoleArea.toFloat(), theme::wood);
}

void OrgelEditor::resized()
{
    auto r = getLocalBounds();
    auto bar = r.removeFromTop (48).reduced (8, 7);
    openButton.setBounds (bar.removeFromLeft (78));
    bar.removeFromLeft (4);
    exportButton.setBounds (bar.removeFromLeft (92));
    bar.removeFromLeft (12);
    rewindButton.setBounds (bar.removeFromLeft (34));
    playButton.setBounds (bar.removeFromLeft (40).reduced (1, 0));
    stopButton.setBounds (bar.removeFromLeft (34));
    bar.removeFromLeft (8);

    auto rightPart = bar.removeFromRight (juce::jmin (680, bar.getWidth() / 2));
    volume.setBounds (rightPart.removeFromRight (34));
    volumeLabel.setBounds (rightPart.removeFromRight (66));
    reverb.setBounds (rightPart.removeFromRight (34));
    reverbLabel.setBounds (rightPart.removeFromRight (36));
    rightPart.removeFromRight (6);
    optionsButton.setBounds (rightPart.removeFromRight (78));
    rightPart.removeFromRight (4);
    soundFontButton.setBounds (rightPart.removeFromRight (86));
    rightPart.removeFromRight (4);
    addManualButton.setBounds (rightPart.removeFromRight (78));
    rightPart.removeFromRight (4);
    partsButton.setBounds (rightPart.removeFromRight (72));
    rightPart.removeFromRight (4);
    trackButton.setBounds (rightPart.removeFromRight (112));
    registrationToggle.setVisible (false); // lives in the "Stimmen" menu

    auto mid = bar;
    auto top = mid.removeFromTop (mid.getHeight() / 2);
    titleLabel.setBounds (top);
    timeLabel.setBounds (mid.removeFromLeft (84));
    speedLabel.setBounds (mid.removeFromRight (150).removeFromLeft (42));
    speed.setBounds (speedLabel.getBounds().withX (speedLabel.getRight()).withWidth (108));
    position.setBounds (mid);
    markerStrip.setBounds (mid);
    markerStrip.toFront (false);

    // console
    const int H = r.getHeight();
    const int nDiv = keyboards.size();
    const int pedalH = juce::jlimit (70, 130, (int) (H * 0.12f));
    const int manualH = juce::jlimit (34, 62, (int) (H * 0.06f));
    const int nManuals = juce::jmax (0, nDiv - 1);
    auto pedalArea = r.removeFromBottom (pedalH + 12);
    auto manualsArea = r.removeFromBottom (nManuals * manualH + 10);
    auto railArea = r.removeFromBottom (34);
    consoleArea = manualsArea.getUnion (pedalArea).getUnion (railArea);
    facadeArea = r.removeFromTop (juce::jlimit (70, 170, (int) (H * 0.135f)));
    facadeCache = {};

    const int jambW = juce::jmax (220, (int) (r.getWidth() * 0.24f));
    leftJamb.setBounds (r.removeFromLeft (jambW));
    rightJamb.setBounds (r.removeFromRight (jambW));
    desk.setBounds (r.reduced (10, 0).withTrimmedBottom (6));
    rail.setBounds (railArea);

    const int plateW = 190, swellW = 26;
    const int kbW = juce::jmin (getWidth() - 2 * (plateW + swellW + 30), (int) (getWidth() * 0.66f));
    const int kbX = (getWidth() - kbW) / 2;
    manualsArea.removeFromTop (5);
    for (int i = 0; i < nDiv; ++i)
    {
        auto* kb = keyboards[i];
        const bool isPedal = proc.organ.getDivision (kb->division).pedal;
        juce::Rectangle<int> row;
        if (isPedal)
        {
            row = pedalArea.reduced (0, 6);
            const int pw = (int) (kbW * 0.9f);
            kb->setBounds ((getWidth() - pw) / 2, row.getY(), pw, row.getHeight());
        }
        else
        {
            // manual I at the bottom, higher manuals above
            const int fromBottom = kb->division;
            row = juce::Rectangle<int> (0, manualsArea.getBottom() - (fromBottom + 1) * manualH, getWidth(), manualH);
            kb->setBounds (kbX, row.getY() + 2, kbW, manualH - 4);
        }
        plates[i]->setBounds (kb->getX() - plateW - 14, row.getY(), plateW, row.getHeight());
        swells[i]->setBounds (kb->getRight() + 10, row.getY() + 2, swellW, row.getHeight() - 4);
    }
}

//==============================================================================
bool OrgelEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    auto wild = juce::StringArray::fromTokens (Formats::importWildcard(), ";", "");
    for (auto& f : files)
        for (auto& w : wild)
            if (juce::File (f).getFileName().matchesWildcard (w, true))
                return true;
    return files.size() == 1 && juce::File (files[0]).hasFileExtension ("sf2");
}

void OrgelEditor::filesDropped (const juce::StringArray& files, int, int)
{
    juce::File f (files[0]);
    if (f.hasFileExtension ("sf2"))
    {
        juce::String err;
        if (! proc.loadSoundFont (f, err))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SoundFont", err);
        return;
    }
    proc.loadFileAsync (f);
}

void OrgelEditor::openFile()
{
    chooser = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 ("Noten öffnen"), juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   Formats::importWildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
        auto f = fc.getResult();
        if (f.existsAsFile())
            proc.loadFileAsync (f);
    });
}

void OrgelEditor::showExportMenu()
{
    auto piece = proc.getPiece();
    juce::PopupMenu m;
    if (piece == nullptr)
    {
        m.addItem (-1, juce::String::fromUTF8 ("Zuerst ein Stück laden"), false);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportButton));
        return;
    }
    auto formats = Formats::exportFormats();
    const bool hasMuseScore = Formats::findMuseScore().existsAsFile();
    juce::PopupMenu notes, audio, pages;
    for (int i = 0; i < (int) formats.size(); ++i)
    {
        auto& f = formats[(size_t) i];
        const bool enabled = ! f.needsMuseScore || hasMuseScore;
        auto label = f.description + "  (." + f.extension + ")" + (enabled ? "" : juce::String::fromUTF8 (" – MuseScore fehlt"));
        if (f.isAudio)
            audio.addItem (i + 1, label, enabled);
        else if (f.id == "pdf" || f.id == "svg" || f.id == "png")
            pages.addItem (i + 1, label, enabled);
        else
            notes.addItem (i + 1, label, enabled);
    }
    m.addSectionHeader ("Noten");
    m.addSubMenu ("Notenformate", notes);
    m.addSubMenu ("Notenseiten (Bild/PDF)", pages);
    m.addSectionHeader (juce::String::fromUTF8 ("Klang (über die SF2 gerendert)"));
    m.addSubMenu ("Audio", audio);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportButton), [this, formats] (int r) {
        if (r > 0)
            exportAs (formats[(size_t) (r - 1)]);
    });
}

void OrgelEditor::exportAs (const Formats::ExportFormat& fmt)
{
    auto piece = proc.getPiece();
    if (piece == nullptr)
        return;
    auto base = piece->source.getParentDirectory().isDirectory() ? piece->source.getParentDirectory()
                                                                  : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    auto name = piece->source.getFileNameWithoutExtension();
    chooser = std::make_unique<juce::FileChooser> ("Exportieren als " + fmt.description, base.getChildFile (name + "." + fmt.extension),
                                                   "*." + fmt.extension);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, fmt, piece] (const juce::FileChooser& fc) {
        auto target = fc.getResult();
        if (target == juce::File())
            return;
        if (! target.hasFileExtension (fmt.extension))
            target = target.withFileExtension (fmt.extension);

        struct Job : public juce::ThreadWithProgressWindow
        {
            Job (OrgelProcessor& p, std::shared_ptr<Piece> pc, Formats::ExportFormat f, juce::File t)
                : juce::ThreadWithProgressWindow ("Exportiere " + t.getFileName() + juce::String::fromUTF8 ("…"), true, true), proc (p), piece (pc), fmt (f), target (t) {}
            void run() override
            {
                auto ctx = proc.makeExportContext();
                ctx.piece = piece.get();
                ctx.progress = [this] (float v) { setProgress (v); return ! threadShouldExit(); };
                setProgress (-1.0);
                ok = Formats::exportTo (fmt, target, ctx, error);
            }
            OrgelProcessor& proc;
            std::shared_ptr<Piece> piece;
            Formats::ExportFormat fmt;
            juce::File target;
            bool ok = false;
            juce::String error;
        };
        auto job = std::make_shared<Job> (proc, piece, fmt, target);
        job->launchThread();
        // report when the job is done
        auto poll = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weakPoll = poll;
        *poll = [job, weakPoll] {
            if (job->isThreadRunning())
            {
                if (auto p = weakPoll.lock())
                    juce::Timer::callAfterDelay (200, [p] { (*p)(); });
                return;
            }
            if (job->ok)
                job->target.revealToUser();
            else
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Export", job->error);
        };
        juce::Timer::callAfterDelay (200, [poll] { (*poll)(); });
    });
}

void OrgelEditor::showPartMenu()
{
    auto piece = proc.getPiece();
    juce::PopupMenu m;
    if (piece == nullptr)
    {
        m.addItem (-1, juce::String::fromUTF8 ("Kein Stück geladen"), false);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&partsButton));
        return;
    }
    auto& map = proc.getPartDivision();
    for (int p = 0; p < (int) piece->score->parts.size(); ++p)
    {
        auto& part = piece->score->parts[(size_t) p];
        juce::PopupMenu sub;
        const int current = p < (int) map.size() ? map[(size_t) p] : -1;
        for (int d = 0; d < proc.organ.getNumDivisions(); ++d)
        {
            auto& div = proc.organ.getDivision (d);
            sub.addItem (p * 100 + d + 1, div.pedal ? div.name : romanNumeral (d + 1) + " " + div.name, true, current == d);
        }
        sub.addItem (p * 100 + 99, "stumm", true, current < 0);
        auto label = part.name + "  (" + juce::String (part.count) + " Noten, "
                     + juce::MidiMessage::getMidiNoteName (part.lowest, true, true, 4) + juce::String::fromUTF8 ("–")
                     + juce::MidiMessage::getMidiNoteName (part.highest, true, true, 4) + juce::String::fromUTF8 (")  →  ")
                     + (current >= 0 ? proc.organ.getDivision (current).name : juce::String ("stumm"));
        m.addSubMenu (label, sub);
    }
    m.addSeparator();
    m.addItem (100001, "Registrierung aus der Datei beim Laden ziehen", true, proc.applyFileRegistration);
    m.addItem (100000, "Register aus der Datei jetzt ziehen");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&partsButton), [this] (int r) {
        if (r == 100001)
        {
            proc.applyFileRegistration = ! proc.applyFileRegistration;
            return;
        }
        if (r == 100000)
        {
            proc.applyPieceRegistration();
            return;
        }
        if (r <= 0)
            return;
        const int part = r / 100, d = r % 100;
        proc.setPartDivision (part, d == 99 ? -1 : d - 1);
        if (proc.applyFileRegistration)
            proc.applyPieceRegistration();
    });
}

void OrgelEditor::showTrackMenu()
{
    auto piece = proc.getPiece();
    juce::PopupMenu m;
    if (piece == nullptr)
    {
        m.addItem (-1, juce::String::fromUTF8 ("Zuerst ein Stück laden"), false);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&trackButton));
        return;
    }
    auto marks = proc.getMarks();
    const auto here = piece->score->measureAt (proc.getCurrentBeat());
    m.addItem (1, juce::String::fromUTF8 ("Aktuelle Registrierung hier merken (Takt ") + juce::String (here) + ")  [R]");
    m.addSeparator();
    if (marks.empty())
        m.addItem (-1, "Noch keine Registrierwechsel", false);
    for (int i = 0; i < (int) marks.size(); ++i)
    {
        auto& mk = marks[(size_t) i];
        const int stops = juce::StringArray::fromTokens (mk.combination.upToFirstOccurrenceOf (";", false, false), ",", "").size();
        juce::PopupMenu sub;
        sub.addItem (100 + i, "Hierhin springen");
        sub.addItem (2000 + i, "Mit aktueller Registrierung ersetzen");
        sub.addItem (4000 + i, juce::String::fromUTF8 ("Löschen"));
        m.addSubMenu ("Takt " + juce::String (piece->score->measureAt (mk.beat)) + "  ("
                          + formatTime (piece->score->beatToSeconds (mk.beat)) + ")  -  " + juce::String (stops) + " Register",
                      sub);
    }
    if (! marks.empty())
    {
        m.addSeparator();
        m.addItem (9999, juce::String::fromUTF8 ("Alle Registrierwechsel löschen"));
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&trackButton), [this, marks, piece] (int r) {
        if (r == 1) proc.addMarkHere();
        else if (r == 9999) proc.clearMarks();
        else if (r >= 4000) proc.removeMark (r - 4000);
        else if (r >= 2000) proc.updateMark (r - 2000);
        else if (r >= 100 && r - 100 < (int) marks.size())
            proc.seek (piece->score->beatToSeconds (marks[(size_t) (r - 100)].beat));
    });
}

void OrgelEditor::showSoundFontMenu()
{
    juce::PopupMenu m;
    auto sf = proc.organ.getSoundFont();
    m.addSectionHeader ("Aktuell: " + (sf ? sf->getName() : juce::String ("keine")) + " (" + juce::String (sf ? (int) sf->getPresets().size() : 0) + juce::String::fromUTF8 (" Klänge)"));
    m.addItem (1, "Eingebaute Jeux14 verwenden");
    m.addItem (2, juce::String::fromUTF8 ("SF2-Datei laden…"));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&soundFontButton), [this] (int r) {
        if (r == 1)
            proc.useBuiltInSoundFont();
        if (r == 2)
        {
            chooser = std::make_unique<juce::FileChooser> ("SoundFont laden", juce::File(), "*.sf2");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
                if (! fc.getResult().existsAsFile())
                    return;
                juce::String err;
                if (! proc.loadSoundFont (fc.getResult(), err))
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SoundFont", err);
            });
        }
    });
}

void OrgelEditor::showOptionsMenu()
{
    juce::PopupMenu m;
    if (proc.isStandalone())
        m.addItem (1, juce::String::fromUTF8 ("Audio-Einstellungen…"));
    else
        m.addItem (2, "Mit DAW-Transport mitlaufen", true, proc.hostSync);
    m.addItem (3, juce::String::fromUTF8 ("Stück in Schleife abspielen"), true, proc.getLoop());
    juce::PopupMenu transpose;
    for (int t = -6; t <= 6; ++t)
        transpose.addItem (1000 + t, (t > 0 ? "+" : "") + juce::String (t) + juce::String::fromUTF8 (" Halbtöne"), true, proc.organ.transpose == t);
    m.addSubMenu ("Transposition", transpose);
    m.addItem (6, juce::String::fromUTF8 ("Pfeifen räumlich aufstellen (Werke im Raum, C/Cis-Seiten)"), true, proc.organ.spatial);
    m.addSeparator();
    m.addItem (4, juce::String::fromUTF8 ("Orgel auf Standard zurücksetzen"));
    m.addItem (5, juce::String::fromUTF8 ("Alle Töne aus (Panik)"));
    m.addSeparator();
    auto ms = Formats::findMuseScore();
    m.addItem (-1, ms.existsAsFile() ? juce::String::fromUTF8 ("MuseScore gefunden: weitere Formate verfügbar") : juce::String::fromUTF8 ("MuseScore nicht gefunden (für .mscz, Guitar Pro, Capella …)"), false);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&optionsButton), [this] (int r) {
        if (r == 1)
            if (auto* holder = juce::StandalonePluginHolder::getInstance())
                holder->showAudioSettingsDialog();
        if (r == 2)
            proc.hostSync = ! proc.hostSync;
        if (r == 3)
            proc.setLoop (! proc.getLoop());
        if (r == 4)
            proc.organ.setDefaultDisposition();
        if (r == 5)
            proc.organ.allNotesOff();
        if (r == 6)
            proc.organ.setSpatial (! proc.organ.spatial);
        if (r >= 994 && r <= 1006)
            proc.organ.setTranspose (r - 1000);
    });
}
} // namespace orgel
