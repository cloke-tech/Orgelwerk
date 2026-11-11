#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <bitset>
#include <memory>
#include <vector>

struct tsf;

namespace orgel
{
enum Source : uint8_t
{
    srcMidi = 1,   // external MIDI devices / host MIDI
    srcPlayer = 2, // score playback
    srcUI = 4,     // mouse on the on-screen keyboards
};

enum class StopFamily { Principal, Flute, String, Mixture, Reed, Percussion };

struct Stop
{
    juce::String name;
    int bank = 0, program = 0;
    bool on = false;

    // runtime only: one synth channel per side of the windchest
    // (C side: C D E F# G# A#, C# side: C# D# F G A B), panned apart like real pipes
    int channel = -1, channel2 = -1;
    std::bitset<128> sounding;
};

// Which MIDI input drives a division.
struct MidiAssign
{
    static constexpr const char* anyDevice = "";
    static constexpr const char* hostDevice = "host";

    juce::String device;     // "" = every input, "host" = DAW/host MIDI, otherwise device identifier
    juce::String deviceName; // for display, and to find the device again after reconnecting
    int channel = 0;         // 0 = omni, 1..16
};

struct Division
{
    juce::String name;
    bool pedal = false;
    std::vector<Stop> stops;
    MidiAssign midi;
    float volume = 1.0f;     // set in the UI
    float expression = 1.0f; // swell pedal (CC 11 / CC 7)
    bool tremulant = false;

    // runtime only: which sources hold each key
    std::array<uint8_t, 128> held {};
    // runtime only: every division has its own synth so it can sit at its own place in the room
    std::shared_ptr<::tsf> synth;
    std::bitset<256> usedChannels;
    float appliedGain = -1.0f;
    std::vector<float> delayLine; // stereo interleaved, distance delay
    int delayPos = 0;
    float lowpass[2] {};

    int lowKey() const { return 36; }
    int highKey() const { return pedal ? 67 : 96; } // C..g' pedal (32 keys), C..c'''' manual (61 keys)
};

// Plays the stops of division `from` when keys of division `to` are pressed ("II/I", "I/P").
struct Coupler
{
    int from = 0, to = 0;
    bool on = false;
};

StopFamily classifyStop (const juce::String& name);
juce::String romanNumeral (int n);
} // namespace orgel
