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
        std::vector<juce::String> divisionNames; // indexed by division ("Hauptwerk", "Pedal"...)
        std::vector<bool> divisionIsPedal;
        juce::String manualName = "Manual", pedalName = "Pedal";
        // false: black notes, the manual named at every change (as printed organ music);
        // true: notes in their manual's colour as well
        bool colourManuals = false;
        // true: every manual gets its own grand staff (named after it), the pedal its own staff,
        // and staves are hidden in lines where they have nothing to play;
        // false: the classic three staves (right hand, left hand, pedal)
        bool staffPerManual = false;
        // the registration the file asks for, from a beat on (stop names as printed)
        struct RegistrationMark
        {
            double beat = 0;
            int division = 0;
            juce::StringArray stops;
        };
        std::vector<RegistrationMark> registrations;
    };

    // "Prin.(pos.)8+4" sounding an octave lower -> "Prin. 16' + 8'" (with real foot marks)
    juce::String printedStopName (const juce::String& presetName, int octave);
    // how organ editions name a division: "Hw.", "Pos.", "Sw.", "Ped." or "Man. II"
    juce::String divisionAbbreviation (const juce::String& name, int division, bool pedal);

    juce::String toMusicXML (const Score& score, const Options& options);
    // The parts the score is written in, top to bottom (MusicXML P1, P2 ...): the division a
    // part belongs to (-1: all manuals together) and whether it is the pedal.
    struct PartInfo
    {
        int division = -1;
        bool pedal = false;
    };
    std::vector<PartInfo> partLayout (const Score& score, const Options& options);
    juce::String toABC (const Score& score, const Options& options);
    juce::String toLilyPond (const Score& score, const Options& options);
    // colour used for the notes of a manual (0 = manual I, black)
    juce::String manualColour (int division);
} // namespace NotationBuilder
} // namespace orgel
