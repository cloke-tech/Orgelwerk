#pragma once
#include "Theme.h"
#include "../engine/I18n.h"
#include "../PluginProcessor.h"

namespace orgel
{
// "Klang und Raum": the church the organ stands in and how the sound reaches the listener.
// The rooms are ready-made settings (neutral without any effect, chapel ... minster) and the
// user's own, which are kept for all sessions.
class SoundPanel : public juce::Component
{
public:
    explicit SoundPanel (OrgelProcessor& p);
    void paint (juce::Graphics& g) override;
    void resized() override;
    std::function<void()> onChange; // e.g. to move the toolbar's reverb knob along

    struct Preset
    {
        juce::String name;
        float reverb = 0.35f;
        OrganEngine::Acoustics acoustics;
    };
    static std::vector<Preset> builtInPresets(); // neutral first, then chapel ... minster
    static std::vector<Preset> ownPresets();
    static void storeOwnPresets (const std::vector<Preset>& list);

private:
    void apply();
    void load();
    void choose (const Preset& p);
    void fillRooms();
    void saveOwn();
    void deleteOwn();
    int matchingItem() const; // the room the current settings are (0 if they were changed)

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
    juce::TextButton reset, save, remove;
    juce::TextEditor ownName;
    Row* rows[6] = { &reverb, &distance, &bass, &sub, &width, &treble };
    std::vector<Preset> presets; // as listed: built-in, then own (item id = index + 1)
    int numBuiltIn = 0;
};
} // namespace orgel
