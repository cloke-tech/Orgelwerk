#include "PluginEditor.h"
#include "ui/Profile.h"
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
    rewindButton.setTooltip (tr ("Zum Anfang"));
    playButton.setTooltip (tr ("Abspielen / Pause (Leertaste)"));
    stopButton.setTooltip (tr ("Stopp"));
    openButton.setTooltip (tr ("Noten öffnen: MIDI, MusicXML, MEI, ABC, Humdrum, … (oder Datei auf das Fenster ziehen)"));
    exportButton.setTooltip (tr ("Noten oder Orgelklang in eine Datei schreiben"));
    partsButton.setTooltip (tr ("Welche Stimme auf welchem Manual oder dem Pedal spielt"));
    addManualButton.setTooltip (tr ("Ein weiteres Manual hinzufügen"));
    registrationToggle.setTooltip (tr ("Register aus der Datei ziehen (Programmwechsel), wenn ein Stück geladen wird"));

    openButton.onClick = [this] { openFile(); };
    exportButton.onClick = [this] { showExportMenu(); };
    rewindButton.onClick = [this] { proc.seek (0.0); };
    playButton.onClick = [this] { togglePlay(); };
    stopButton.onClick = [this] { proc.stop(); };
    partsButton.onClick = [this] { showPartMenu(); };
    trackButton.onClick = [this] { showTrackMenu(); };
    trackButton.setTooltip (tr ("Registrierungen an Stellen im Stück merken; beim Abspielen wird dort umgeschaltet (Taste R)"));
    addManualButton.onClick = [this] { proc.organ.addManual(); };
    soundFontButton.onClick = [this] { showSoundFontMenu(); };
    optionsButton.onClick = [this] { showOptionsMenu(); };
    registrationToggle.setToggleState (proc.applyFileRegistration, juce::dontSendNotification);
    registrationToggle.onClick = [this] {
        proc.applyFileRegistration = registrationToggle.getToggleState();
        if (proc.applyFileRegistration)
            proc.applyPieceRegistration();
        proc.reengrave(); // the score shows the file's stops only while they are used
    };

    position.setRange (0.0, 1.0);
    position.onDragStart = [this] { draggingPosition = true; };
    position.onDragEnd = [this] { draggingPosition = false; proc.seek (position.getValue()); };
    position.setTooltip (tr ("Position im Stück"));

    speed.setRange (25.0, 200.0, 1.0);
    speed.setTextValueSuffix (" %");
    speed.setValue (proc.getSpeed() * 100.0, juce::dontSendNotification);
    speed.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
    speed.onValueChange = [this] { proc.setSpeed ((float) speed.getValue() / 100.0f); };
    speed.setDoubleClickReturnValue (true, 100.0);

    reverb.setRange (0.0, 1.0, 0.01);
    reverb.setValue (proc.organ.reverb, juce::dontSendNotification);
    reverb.onValueChange = [this] { proc.organ.reverb = (float) reverb.getValue(); };
    reverb.setTooltip (tr ("Kirchenhall"));
    volume.setRange (0.0, 1.5, 0.01);
    volume.setValue (proc.organ.master, juce::dontSendNotification);
    volume.onValueChange = [this] { proc.organ.master = (float) volume.getValue(); };
    volume.setTooltip (tr ("Gesamtlautstärke"));

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
    profile::mark ("editor: before console");
    rebuildConsole();
    profile::mark ("editor: console built");
    shownPiece = proc.getPiece();
    desk.setPiece (shownPiece);

    setResizable (true, true);
    // open at a comfortable size for this screen (at most 1480 x 940, at most 90 % of it)
    setResizeLimits (960, 640, 3200, 2200);
    {
        auto area = juce::Rectangle<int> (1440, 900);
        if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            area = display->userArea;
        setSize (juce::jlimit (960, 1480, (int) (area.getWidth() * 0.9)),
                 juce::jlimit (640, 940, (int) (area.getHeight() * 0.88) - 30));
    }
    if (proc.isStandalone())
        optionsButton.setVisible (false); // in the menu bar instead
    setWantsKeyboardFocus (true);
    vblank = juce::VBlankAttachment (this, [this] { animate(); });
    startTimerHz (15);
}

OrgelEditor::~OrgelEditor()
{
   #if JUCE_MAC
    if (juce::MenuBarModel::getMacMainMenu() == this)
        juce::MenuBarModel::setMacMainMenu (nullptr);
   #endif
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setMenuBar (nullptr);
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
        s->setTooltip (tr ("Lautstärke ") + proc.organ.getDivision (d).name);
        s->onValueChange = [this, d, s] { proc.organ.setDivisionVolume (d, (float) s->getValue()); };
        addAndMakeVisible (s);
    }
    resized();
    repaint();
}

void OrgelEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (proc.isStandalone())
        menuItemsChanged();
    if (structureSignature() != lastSignature)
        rebuildConsole();
    else
    {
        leftJamb.refreshStates();
        rightJamb.refreshStates();
        rail.refreshStates();
        for (auto* p : plates)
            p->refresh();
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

// Everything that moves runs once per display frame (60 or 120 times a second), in step with the screen.
void OrgelEditor::animate()
{
    ORGEL_PROFILE_SCOPE ("editor.animate");
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (profile::enabled() && lastAnimate > 0)
        profile::record ("frame.gap", now - lastAnimate);
    const double dt = lastAnimate > 0 ? juce::jlimit (0.0, 0.1, (now - lastAnimate) / 1000.0) : 1.0 / 60.0;
    lastAnimate = now;
    for (auto* k : keyboards)
        k->update();
    updateFacade (dt);
    desk.update (dt);
}

void OrgelEditor::timerCallback()
{
    ORGEL_PROFILE_SCOPE ("editor.timer");
    // no display frames (window hidden, or a host without them): keep things moving anyway
    if (juce::Time::getMillisecondCounterHiRes() - lastAnimate > 120.0)
        animate();
    if (profile::enabled())
    {
        // ORGEL_AUTOPLAY=1: set the pages and play as soon as a piece is there (for measuring)
        static bool started = false;
        if (! started && std::getenv ("ORGEL_AUTOPLAY") != nullptr && proc.getPiece() != nullptr && ! proc.isLoading())
        {
            started = true;
            proc.requestEngraving();
            proc.play();
        }
    }
    for (auto* p : plates)
        p->refresh();
    runDeskTest();
    runScreenshot();

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
    titleLabel.setText (proc.isLoading() ? juce::String (tr ("Lade…")) : (piece != nullptr ? piece->score->title : juce::String ("Orgelwerk")),
                        juce::dontSendNotification);
}

// ORGEL_SCREENSHOT=<folder>: plays the piece silently at ORGEL_SCREENSHOT_AT (a part of its
// length) and saves pictures of the whole window (pages, then page width), then quits. The
// volume and the view are put back before quitting, so the saved settings stay as they were.
void OrgelEditor::runScreenshot()
{
    static const char* dir = std::getenv ("ORGEL_SCREENSHOT");
    if (dir == nullptr)
        return;
    static int step = 0, wait = 0;
    static float savedMaster = 0.75f;
    static bool savedZoom = false;
    auto piece = proc.getPiece();
    if (piece == nullptr || proc.isLoading())
        return;
    auto save = [&] (const juce::String& name) {
        auto img = createComponentSnapshot (getLocalBounds(), true, 2.0f);
        juce::File f = juce::File (dir).getChildFile (name + ".png");
        f.deleteFile();
        juce::FileOutputStream out (f);
        juce::PNGImageFormat().writeImageToStream (img, out);
    };
    ++wait;
    switch (step)
    {
        case 0:
            setSize (1440, 900);
            savedMaster = proc.organ.master;
            savedZoom = desk.isZoomed();
            proc.organ.master = 0.0f; // silent
            desk.setZoomed (false);
            desk.setFollowing (true);
            proc.requestEngraving();
            step = 1;
            wait = 0;
            break;
        case 1:
            if (piece->notation == nullptr || proc.isEngraving() || wait < 10)
                break;
            {
                const char* at = std::getenv ("ORGEL_SCREENSHOT_AT");
                proc.seek ((at != nullptr ? juce::String (at).getDoubleValue() : 0.1) * proc.getLength());
            }
            proc.play();
            step = 2;
            wait = 0;
            break;
        case 2:
            if (wait < 45)
                break;
            save ("orgelwerk");
            desk.setZoomed (true);
            step = 3;
            wait = 0;
            break;
        case 3:
            if (wait < 40)
                break;
            save ("orgelwerk-seitenbreite");
            desk.setView (MusicDesk::View::continuous);
            step = 4;
            wait = 0;
            break;
        case 4:
            // the long line is set in the background first
            if (wait < 90)
                break;
            save ("orgelwerk-fortlaufend");
            proc.pause();
            desk.setView (savedZoom ? MusicDesk::View::pageWidth : MusicDesk::View::pages);
            proc.organ.master = savedMaster;
            step = 5;
            wait = 0;
            break;
        case 5:
            if (wait < 5)
                break;
            juce::JUCEApplicationBase::quit();
            step = 6;
            break;
        default:
            break;
    }
}

// ORGEL_DESKTEST=<folder>: sets the pages, switches to page width, jumps through the piece and
// saves pictures of the music desk, then quits (to check the desk without anyone at the screen).
void OrgelEditor::runDeskTest()
{
    static const char* dir = std::getenv ("ORGEL_DESKTEST");
    if (dir == nullptr)
        return;
    static int step = 0, wait = 0;
    static const double spots[] = { 0.0, 0.02, 0.05, 0.1, 0.2, 0.35, 0.5, 0.8, 0.97 };
    auto piece = proc.getPiece();
    if (piece == nullptr || proc.isLoading())
        return;
    if (step == 0)
    {
        setSize (1400, 900); // a usual window, whatever the last session left
        proc.requestEngraving();
        step = 1;
        return;
    }
    if (piece->notation == nullptr || proc.isEngraving())
        return;
    if (++wait < 12) // pages are drawn in the background meanwhile
        return;
    wait = 0;
    auto save = [&] (const juce::String& name) {
        auto img = desk.createComponentSnapshot (desk.getLocalBounds(), true, 1.0f);
        juce::File f = juce::File (dir).getChildFile (name + ".png");
        f.deleteFile();
        juce::FileOutputStream out (f);
        juce::PNGImageFormat().writeImageToStream (img, out);
    };
    const int n = (int) std::size (spots);
    if (step == 1)
    {
        desk.setZoomed (true);
        desk.setFollowing (true);
    }
    if (step >= 1 && step <= n)
    {
        if (step > 1)
            save ("zoom-" + juce::String (step - 1));
        if (step <= n)
            proc.seek (spots[step - 1] * proc.getLength());
        ++step;
        return;
    }
    if (step == n + 1)
    {
        save ("zoom-" + juce::String (n));
        desk.setZoomed (false);
        ++step;
        return;
    }
    if (step == n + 2)
    {
        save ("whole");
        juce::JUCEApplicationBase::quit();
        ++step;
    }
}

// Notes in black with the manual named at each change (as printed), or coloured by manual.
void OrgelEditor::toggleManualColours()
{
    proc.colourManuals = ! proc.colourManuals;
    proc.reengrave();
    for (auto* p : plates)
        p->repaint();
    if (proc.isStandalone())
        menuItemsChanged();
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
    if (key.getModifiers().isCommandDown())
    {
        const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter() != 0 ? key.getTextCharacter() : (juce::juce_wchar) key.getKeyCode());
        if (c == 'o') { openFile(); return true; }
        if (c == 'e') { showExportMenu(); return true; }
        if (c == ',') { menuItemSelected (4, 0); return true; }
        return false;
    }
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

void OrgelEditor::updateFacade (double dt)
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
    // the glow fades the same speed at any frame rate (it used to lose 18 % per 30 Hz tick)
    const float decay = (float) std::pow (0.82, dt * 30.0);
    juce::Rectangle<float> dirty;
    for (size_t i = 0; i < pipes.size(); ++i)
    {
        auto& p = pipes[i];
        // a note starts (or gets louder): the pipe jumps up and wobbles back
        if (target[i] > p.lastTarget + 0.2f)
            p.popStart = now;
        p.lastTarget = target[i];
        const float next = target[i] > p.level ? target[i] : p.level * decay;
        const float clipped = next < 0.02f ? 0.0f : next;
        const bool moving = std::abs (clipped - p.level) > 0.002f || now - p.popStart < 700.0 || (p.level > 0.0f) != (clipped > 0.0f);
        p.level = clipped;
        if (moving || p.wasMoving)
        {
            // the pipe's column, wide enough for the halo around its mouth
            const float halo = p.body.getWidth() * 3.4f;
            auto col = juce::Rectangle<float> (p.body.getCentreX() - halo, 0.0f, halo * 2.0f, (float) facadeArea.getHeight());
            dirty = dirty.isEmpty() ? col : dirty.getUnion (col);
        }
        p.wasMoving = moving;
    }
    if (! dirty.isEmpty())
        repaint (dirty.translated ((float) facadeArea.getX(), (float) facadeArea.getY()).getSmallestIntegerContainer().getIntersection (facadeArea));
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
    // never narrower than at rest, so the pipe always covers its own resting outline
    const float sx = 1.0f + 0.06f * p.level + 0.08f * std::abs (stretch);

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
    // resting pipes come from the cached picture; draw only the ones that move or sound
    for (auto& p : pipes)
        if (p.level > 0.0f || now - p.popStart < 700.0)
            drawPipe (g, p, offset, now);
}

void OrgelEditor::paint (juce::Graphics& g)
{
    ORGEL_PROFILE_SCOPE ("editor.paint");
    if (profile::enabled())
    {
        static bool first = true;
        if (first)
            std::fprintf (stderr, "[profile] first paint %.0f ms after start\n", juce::Time::getMillisecondCounterHiRes() - profile::startMs());
        first = false;
    }
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
            // resting pipes belong to the cached picture; only sounding ones are drawn live
            for (auto p : pipes)
            {
                p.level = 0.0f;
                p.popStart = -1.0e9;
                drawPipe (fg, p, {}, 0.0);
            }
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
    if (optionsButton.isVisible())
        optionsButton.setBounds (rightPart.removeFromRight (78));
    rightPart.removeFromRight (4);
    soundFontButton.setBounds (rightPart.removeFromRight (86));
    rightPart.removeFromRight (4);
    addManualButton.setBounds (rightPart.removeFromRight (78));
    rightPart.removeFromRight (4);
    partsButton.setBounds (rightPart.removeFromRight (72));
    rightPart.removeFromRight (4);
    trackButton.setBounds (rightPart.removeFromRight (112));
    registrationToggle.setVisible (false); // lives in the tr ("Stimmen") menu

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
    chooser = std::make_unique<juce::FileChooser> (tr ("Noten öffnen"), juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
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
        m.addItem (-1, tr ("Zuerst ein Stück laden"), false);
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
        auto label = f.description + "  (." + f.extension + ")" + (enabled ? "" : tr (" – MuseScore fehlt"));
        if (f.isAudio)
            audio.addItem (i + 1, label, enabled);
        else if (f.id == "pdf" || f.id == "svg" || f.id == "png")
            pages.addItem (i + 1, label, enabled);
        else
            notes.addItem (i + 1, label, enabled);
    }
    m.addSectionHeader (tr ("Noten"));
    m.addSubMenu (tr ("Notenformate"), notes);
    m.addSubMenu (tr ("Notenseiten (Bild/PDF)"), pages);
    m.addSectionHeader (tr ("Klang (über die SF2 gerendert)"));
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
    chooser = std::make_unique<juce::FileChooser> (tr ("Exportieren als ") + fmt.description, base.getChildFile (name + "." + fmt.extension),
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
                : juce::ThreadWithProgressWindow (tr ("Exportiere ") + t.getFileName() + juce::String::fromUTF8 ("…"), true, true), proc (p), piece (pc), fmt (f), target (t) {}
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
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Export"), job->error);
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
        m.addItem (-1, tr ("Kein Stück geladen"), false);
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
        auto label = part.name + "  (" + juce::String (part.count) + tr (" Noten, ")
                     + juce::MidiMessage::getMidiNoteName (part.lowest, true, true, 4) + juce::String::fromUTF8 ("–")
                     + juce::MidiMessage::getMidiNoteName (part.highest, true, true, 4) + juce::String::fromUTF8 (")  →  ")
                     + (current >= 0 ? proc.organ.getDivision (current).name : juce::String ("stumm"));
        m.addSubMenu (label, sub);
    }
    m.addSeparator();
    m.addItem (100001, tr ("Registrierung aus der Datei beim Laden ziehen"), true, proc.applyFileRegistration);
    m.addItem (100000, tr ("Register aus der Datei jetzt ziehen"));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&partsButton), [this] (int r) {
        if (r == 100001)
        {
            proc.applyFileRegistration = ! proc.applyFileRegistration;
            proc.reengrave();
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

//==============================================================================
void OrgelEditor::parentHierarchyChanged()
{
    if (proc.isStandalone() && ! windowSetUp && findParentComponentOfClass<juce::DocumentWindow>() != nullptr)
    {
        windowSetUp = true;
        juce::Component::SafePointer<OrgelEditor> safe (this);
        juce::MessageManager::callAsync ([safe] {
            if (safe != nullptr)
                safe->setUpStandaloneWindow();
        });
    }
}

// The standalone app: a normal window with the system's title bar, the menus in the
// macOS menu bar (or in the window on Windows/Linux), and no JUCE "Options" button.
void OrgelEditor::setUpStandaloneWindow()
{
    auto* window = findParentComponentOfClass<juce::DocumentWindow>();
    if (window == nullptr)
        return;
    for (auto* c : window->getChildren())
        if (auto* b = dynamic_cast<juce::Button*> (c))
            if (b->getButtonText() == "Options")
                b->setVisible (false);
    window->setUsingNativeTitleBar (true);
    window->setResizable (true, false);
   #if JUCE_MAC
    juce::PopupMenu appleExtras;
    appleExtras.addItem (4, tr ("Einstellungen…"));
    juce::MenuBarModel::setMacMainMenu (this, &appleExtras);
   #else
    window->setMenuBar (this);
   #endif
    // keep the window on screen and not larger than the display
    if (auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (window->getScreenBounds()))
    {
        auto area = display->userArea;
        auto bounds = window->getBounds();
        bounds.setSize (juce::jmin (bounds.getWidth(), area.getWidth()), juce::jmin (bounds.getHeight(), area.getHeight()));
        window->setBounds (bounds.constrainedWithin (area));
    }
}

juce::StringArray OrgelEditor::getMenuBarNames()
{
   #if JUCE_MAC
    return { tr ("Ablage"), tr ("Wiedergabe"), tr ("Orgel"), tr ("Ansicht") };
   #else
    return { tr ("Datei"), tr ("Wiedergabe"), tr ("Orgel"), tr ("Ansicht") };
   #endif
}

juce::PopupMenu OrgelEditor::getMenuForIndex (int index, const juce::String&)
{
    juce::PopupMenu m;
    auto piece = proc.getPiece();
    auto item = [] (int id, const juce::String& text, const juce::String& shortcut = {}, bool enabled = true, bool ticked = false) {
        juce::PopupMenu::Item it (text);
        it.itemID = id;
        it.isEnabled = enabled;
        it.isTicked = ticked;
        it.shortcutKeyDescription = shortcut;
        return it;
    };
    const juce::String cmd = juce::String::fromUTF8 ("\xe2\x8c\x98");
    if (index == 0)
    {
        m.addItem (item (1, tr ("Öffnen…"), cmd + "O"));
        juce::PopupMenu exportMenu;
        auto formats = Formats::exportFormats();
        const bool hasMuseScore = Formats::findMuseScore().existsAsFile();
        for (int i = 0; i < (int) formats.size(); ++i)
        {
            auto& f = formats[(size_t) i];
            exportMenu.addItem (item (100 + i, f.description + "  (." + f.extension + ")", {}, piece != nullptr && (! f.needsMuseScore || hasMuseScore)));
            if (f.id == "mscz" || f.id == "png")
                exportMenu.addSeparator();
        }
        m.addSubMenu (tr ("Exportieren"), exportMenu, piece != nullptr);
        m.addSeparator();
        m.addItem (item (2, tr ("SoundFont laden…")));
        m.addItem (item (3, tr ("Eingebaute SoundFont (Jeux14)")));
       #if ! JUCE_MAC
        m.addSeparator();
        m.addItem (item (4, tr ("Audio- und MIDI-Einstellungen…")));
       #endif
    }
    else if (index == 1)
    {
        m.addItem (item (10, proc.isPlaying() ? tr ("Pause") : tr ("Abspielen"), tr ("Leertaste"), piece != nullptr));
        m.addItem (item (11, tr ("Stopp"), {}, piece != nullptr));
        m.addItem (item (12, tr ("Zum Anfang"), {}, piece != nullptr));
        m.addItem (item (13, tr ("Stück in Schleife"), {}, true, proc.getLoop()));
        m.addSeparator();
        m.addItem (item (14, tr ("Registrierung hier merken"), "R", piece != nullptr));
        m.addItem (item (15, tr ("Registrierspur löschen"), {}, ! proc.getMarks().empty()));
        m.addSeparator();
        m.addItem (item (16, tr ("Registrierung aus der Datei beim Laden ziehen"), {}, true, proc.applyFileRegistration));
        m.addItem (item (17, tr ("Werke ohne Register automatisch registrieren"), {}, true, proc.autoRegister));
    }
    else if (index == 2)
    {
        m.addItem (item (20, tr ("Manual hinzufügen")));
        m.addItem (item (21, tr ("Generalauslöser (alle Register ab)")));
        m.addItem (item (22, tr ("Pfeifen räumlich aufstellen"), {}, true, proc.organ.spatial));
        juce::PopupMenu transpose;
        for (int t = -6; t <= 6; ++t)
            transpose.addItem (item (1000 + t, (t > 0 ? "+" : "") + juce::String (t) + tr (" Halbtöne"), {}, true, proc.organ.transpose == t));
        m.addSubMenu (tr ("Transposition"), transpose);
        m.addSeparator();
        m.addItem (item (25, tr ("Orgel auf Standard zurücksetzen")));
        m.addItem (item (26, tr ("Alle Töne aus (Panik)")));
    }
    else if (index == 3)
    {
        m.addItem (item (30, MusicDesk::viewName (MusicDesk::View::pages), {}, true, desk.getView() == MusicDesk::View::pages));
        m.addItem (item (34, MusicDesk::viewName (MusicDesk::View::pageWidth), {}, true, desk.getView() == MusicDesk::View::pageWidth));
        m.addItem (item (35, MusicDesk::viewName (MusicDesk::View::continuous), {}, true, desk.getView() == MusicDesk::View::continuous));
        m.addSeparator();
        m.addItem (item (31, tr ("Beim Abspielen mitblättern"), {}, true, desk.isFollowing()));
        m.addItem (item (33, tr ("Eigene Notensysteme je Manual"), {}, true, proc.staffPerManual));
        m.addItem (item (32, tr ("Manuale in den Noten farbig"), {}, true, proc.colourManuals));
        m.addSeparator();
        m.addSubMenu (tr ("Sprache"), languageMenu (2000));
    }
    return m;
}

// Languages: the system's, or one chosen here (it applies from the next start).
juce::PopupMenu OrgelEditor::languageMenu (int firstId)
{
    juce::PopupMenu m;
    const auto chosen = i18n::setting();
    m.addItem (firstId, tr ("Wie das System"), true, chosen == "auto");
    m.addSeparator();
    int id = firstId + 1;
    for (auto& l : i18n::languages())
        m.addItem (id++, juce::String::fromUTF8 (l.nativeName), true, chosen == l.code);
    return m;
}

void OrgelEditor::chooseLanguage (int index)
{
    const auto& langs = i18n::languages();
    const juce::String code = index <= 0 || index > (int) langs.size() ? juce::String ("auto") : juce::String (langs[(size_t) index - 1].code);
    if (code == i18n::setting())
        return;
    i18n::setSetting (code);
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Orgelwerk",
                                            tr ("Die Sprache wird beim nächsten Start von Orgelwerk übernommen."));
    if (proc.isStandalone())
        menuItemsChanged();
}

void OrgelEditor::menuItemSelected (int id, int)
{
    if (id >= 2000 && id < 2100)
    {
        chooseLanguage (id - 2000);
        return;
    }
    if (id >= 100 && id < 200)
    {
        auto formats = Formats::exportFormats();
        if (id - 100 < (int) formats.size())
            exportAs (formats[(size_t) (id - 100)]);
        return;
    }
    if (id >= 994 && id <= 1006)
    {
        proc.organ.setTranspose (id - 1000);
        return;
    }
    switch (id)
    {
        case 1: openFile(); break;
        case 2:
            chooser = std::make_unique<juce::FileChooser> (tr ("SoundFont laden"), juce::File(), "*.sf2");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
                juce::String err;
                if (fc.getResult().existsAsFile() && ! proc.loadSoundFont (fc.getResult(), err))
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SoundFont", err);
            });
            break;
        case 3: proc.useBuiltInSoundFont(); break;
        case 4:
            if (auto* holder = juce::StandalonePluginHolder::getInstance())
                holder->showAudioSettingsDialog();
            break;
        case 10: togglePlay(); break;
        case 11: proc.stop(); break;
        case 12: proc.seek (0.0); break;
        case 13: proc.setLoop (! proc.getLoop()); break;
        case 14: proc.addMarkHere(); break;
        case 15: proc.clearMarks(); break;
        case 16: proc.applyFileRegistration = ! proc.applyFileRegistration; proc.reengrave(); break;
        case 17: proc.autoRegister = ! proc.autoRegister; proc.fillSilentDivisions(); break;
        case 20: proc.organ.addManual(); break;
        case 21: proc.organ.clearRegistration(); break;
        case 22: proc.organ.setSpatial (! proc.organ.spatial); break;
        case 25: proc.organ.setDefaultDisposition(); break;
        case 26: proc.organ.allNotesOff(); break;
        case 30: desk.setView (MusicDesk::View::pages); break;
        case 34: desk.setView (MusicDesk::View::pageWidth); break;
        case 35: desk.setView (MusicDesk::View::continuous); break;
        case 31: desk.setFollowing (! desk.isFollowing()); break;
        case 32: toggleManualColours(); break;
        case 33: proc.staffPerManual = ! proc.staffPerManual; proc.reengrave(); menuItemsChanged(); break;
        default: break;
    }
    menuItemsChanged();
}

void OrgelEditor::showTrackMenu()
{
    auto piece = proc.getPiece();
    juce::PopupMenu m;
    if (piece == nullptr)
    {
        m.addItem (-1, tr ("Zuerst ein Stück laden"), false);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&trackButton));
        return;
    }
    auto marks = proc.getMarks();
    const auto here = piece->score->measureAt (proc.getCurrentBeat());
    m.addItem (1, tr ("Aktuelle Registrierung hier merken (Takt ") + juce::String (here) + ")  [R]");
    m.addSeparator();
    if (marks.empty())
        m.addItem (-1, tr ("Noch keine Registrierwechsel"), false);
    for (int i = 0; i < (int) marks.size(); ++i)
    {
        auto& mk = marks[(size_t) i];
        const int stops = juce::StringArray::fromTokens (mk.combination.upToFirstOccurrenceOf (";", false, false), ",", "").size();
        juce::PopupMenu sub;
        sub.addItem (100 + i, tr ("Hierhin springen"));
        sub.addItem (2000 + i, tr ("Mit aktueller Registrierung ersetzen"));
        sub.addItem (4000 + i, tr ("Löschen"));
        m.addSubMenu (tr ("Takt ") + juce::String (piece->score->measureAt (mk.beat)) + "  ("
                          + formatTime (piece->score->beatToSeconds (mk.beat)) + ")  -  " + juce::String (stops) + tr (" Register"),
                      sub);
    }
    if (! marks.empty())
    {
        m.addSeparator();
        m.addItem (9999, tr ("Alle Registrierwechsel löschen"));
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
    m.addSectionHeader (tr ("Aktuell: ") + (sf ? sf->getName() : juce::String ("keine")) + " (" + juce::String (sf ? (int) sf->getPresets().size() : 0) + tr (" Klänge)"));
    m.addItem (1, tr ("Eingebaute Jeux14 verwenden"));
    m.addItem (2, tr ("SF2-Datei laden…"));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&soundFontButton), [this] (int r) {
        if (r == 1)
            proc.useBuiltInSoundFont();
        if (r == 2)
        {
            chooser = std::make_unique<juce::FileChooser> (tr ("SoundFont laden"), juce::File(), "*.sf2");
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
        m.addItem (1, tr ("Audio-Einstellungen…"));
    else
        m.addItem (2, tr ("Mit DAW-Transport mitlaufen"), true, proc.hostSync);
    m.addItem (3, tr ("Stück in Schleife abspielen"), true, proc.getLoop());
    juce::PopupMenu transpose;
    for (int t = -6; t <= 6; ++t)
        transpose.addItem (1000 + t, (t > 0 ? "+" : "") + juce::String (t) + tr (" Halbtöne"), true, proc.organ.transpose == t);
    m.addSubMenu (tr ("Transposition"), transpose);
    m.addItem (6, tr ("Pfeifen räumlich aufstellen (Werke im Raum, C/Cis-Seiten)"), true, proc.organ.spatial);
    m.addItem (8, tr ("Eigene Notensysteme je Manual"), true, proc.staffPerManual);
    m.addItem (9, tr ("Werke ohne Register automatisch registrieren"), true, proc.autoRegister);
    m.addItem (7, tr ("Manuale in den Noten farbig"), true, proc.colourManuals);
    m.addSeparator();
    m.addItem (4, tr ("Orgel auf Standard zurücksetzen"));
    m.addItem (5, tr ("Alle Töne aus (Panik)"));
    m.addSeparator();
    m.addSubMenu (tr ("Sprache"), languageMenu (3000));
    m.addSeparator();
    auto ms = Formats::findMuseScore();
    m.addItem (-1, ms.existsAsFile() ? tr ("MuseScore gefunden: weitere Formate verfügbar") : tr ("MuseScore nicht gefunden (für .mscz, Guitar Pro, Capella …)"), false);

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
        if (r == 7)
            toggleManualColours();
        if (r == 9)
        {
            proc.autoRegister = ! proc.autoRegister;
            proc.fillSilentDivisions();
        }
        if (r == 8)
        {
            proc.staffPerManual = ! proc.staffPerManual;
            proc.reengrave();
        }
        if (r >= 994 && r <= 1006)
            proc.organ.setTranspose (r - 1000);
        if (r >= 3000 && r < 3100)
            chooseLanguage (r - 3000);
    });
}
} // namespace orgel
