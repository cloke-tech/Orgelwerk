#pragma once
#include "Theme.h"
#include "../engine/I18n.h"
#include "../PluginProcessor.h"

namespace orgel
{
// One drawstop knob.
class StopKnob : public juce::Component, public juce::SettableTooltipClient
{
public:
    StopKnob (OrgelProcessor& p, int division, int stop);
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void refresh();
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    const int division, stop;

private:
    void drawFace (juce::Graphics& g, juce::Rectangle<float> b, bool drawn) const;
    OrgelProcessor& proc;
    juce::String name;
    bool on = false;
    StopFamily family = StopFamily::Principal;
    juce::Image cache[2];
    juce::String cacheName[2];
};

// "+" knob that adds an SF2 preset as a new stop.
class AddStopButton : public juce::Component, public juce::SettableTooltipClient
{
public:
    AddStopButton (OrgelProcessor& p, int division);
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    static void showPresetMenu (OrgelProcessor& p, int division, juce::Component* target);

private:
    OrgelProcessor& proc;
    int division;
};

// A stop jamb: the stops of some divisions, grouped under name plates.
class StopJamb : public juce::Component
{
public:
    StopJamb (OrgelProcessor& p);
    void setDivisions (std::vector<int> divisionIndices);
    void refreshStates();
    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    struct Section
    {
        int division;
        juce::Rectangle<int> plate;
        juce::OwnedArray<StopKnob> knobs;
        std::unique_ptr<AddStopButton> add;
    };
    OrgelProcessor& proc;
    juce::OwnedArray<Section> sections;
};

// Couplers and registration pistons above the top manual.
class CouplerRail : public juce::Component
{
public:
    CouplerRail (OrgelProcessor& p);
    void rebuild();
    void refreshStates();
    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class Tab : public juce::Button
    {
    public:
        Tab (const juce::String& n) : juce::Button (n) {}
        void paintButton (juce::Graphics& g, bool over, bool down) override;
        juce::Colour onColour { 0xffb3261e };
    };
    OrgelProcessor& proc;
    juce::OwnedArray<Tab> couplers;
    juce::OwnedArray<Tab> tremulants;
    std::vector<int> tremulantDivisions;
    juce::OwnedArray<Tab> pistons;
    std::unique_ptr<Tab> setButton, zeroButton;
    std::unique_ptr<Tab> tuttiButton;
};

// A manual keyboard or the pedal board.
class KeyboardView : public juce::Component
{
public:
    KeyboardView (OrgelProcessor& p, int division);
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void update();

    const int division;

private:
    int keyAt (juce::Point<float> p) const;
    juce::Rectangle<float> keyRect (int key) const;
    static bool isBlack (int key);
    int numWhite() const;

    OrgelProcessor& proc;
    bool pedal = false;
    int low = 36, high = 96;
    std::bitset<128> held;
    int mouseKey = -1;
};

// Name plate next to a keyboard; opens the division settings.
class DivisionPlate : public juce::Component, public juce::SettableTooltipClient
{
public:
    DivisionPlate (OrgelProcessor& p, int division);
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent&) override;
    void openSettings();
    void refresh();
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    const int division;

private:
    struct Texts
    {
        bool valid = false, pedal = false, learning = false;
        juce::String title, midi;
    };
    Texts texts() const;
    OrgelProcessor& proc;
    juce::String shownKey;
};

class DivisionSettings : public juce::Component, private juce::Timer
{
public:
    DivisionSettings (OrgelProcessor& p, int division);
    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override;
    void fillDevices();
    void applyMidi();

    OrgelProcessor& proc;
    int division;
    juce::Label nameLabel { {}, tr ("Name") }, deviceLabel { {}, tr ("MIDI-Eingang") }, channelLabel { {}, tr ("Kanal") }, volumeLabel { {}, tr ("Lautstärke") };
    juce::TextEditor name;
    juce::ComboBox device, channel;
    juce::TextButton learn { tr ("MIDI lernen") }, addStop { tr ("Register hinzufügen…") }, allOff { tr ("Alle Register ab") }, remove { tr ("Manual entfernen") };
    juce::Slider volume { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::StringArray deviceIds, deviceNames;
};
} // namespace orgel
