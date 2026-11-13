#pragma once
#include "Theme.h"
#include "../engine/I18n.h"
#include "../PluginProcessor.h"

namespace orgel
{
// "Klang und Raum": the church the organ stands in and how the sound reaches the listener.
class SoundPanel : public juce::Component
{
public:
    explicit SoundPanel (OrgelProcessor& p);
    void paint (juce::Graphics& g) override;
    void resized() override;
    static juce::String roomName (int room);
    std::function<void()> onChange; // e.g. to move the toolbar's reverb knob along

private:
    void apply();
    void chooseRoom (int r);
    // what suits each room: reverb amount and the sound of a typical seat in it
    struct Preset
    {
        float reverb;
        OrganEngine::Acoustics acoustics;
    };
    static Preset presetFor (int room);
    void load();

    OrgelProcessor& proc;
    juce::Label title;
    juce::ComboBox room;
    struct Row
    {
        juce::Label label;
        juce::Slider slider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    };
    Row reverb, distance, bass, sub, width, treble;
    juce::Label roomLabel;
    juce::TextButton reset;
    Row* rows[6] = { &reverb, &distance, &bass, &sub, &width, &treble };
};
} // namespace orgel
