#pragma once
#include "Score.h"

namespace orgel
{
// Turns performed notes (a Score) into written notation: measures, voices, note values,
// ties, pitch spelling, accidentals and beams. Writes MusicXML, ABC and LilyPond.
namespace NotationBuilder
{
    struct Options
    {
        std::vector<bool> partIsPedal; // parts that go to the pedal staff
        // which manual each part plays on: notes get that manual's colour, and "Man. II" etc.
        // is written where a hand changes manual
        std::vector<int> partDivision;
        std::vector<juce::String> divisionLabels; // indexed by division
        juce::String manualName = "Manual", pedalName = "Pedal";
    };

    juce::String toMusicXML (const Score& score, const Options& options);
    juce::String toABC (const Score& score, const Options& options);
    juce::String toLilyPond (const Score& score, const Options& options);
    // colour used for the notes of a manual (0 = manual I, black)
    juce::String manualColour (int division);
} // namespace NotationBuilder
} // namespace orgel
