#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include "../engine/OrganModel.h"
#include <memory>
#include <vector>

namespace orgel
{
class SoundFont;


struct ScoreNote
{
    double start = 0, length = 0; // in quarter notes
    int key = 60, velocity = 100, part = 0;
};

struct TempoChange
{
    double beat = 0;
    double bpm = 120; // quarter notes per minute
};

struct TimeSignature
{
    double beat = 0;
    int num = 4, den = 4;
};

struct KeySignature
{
    double beat = 0;
    int fifths = 0;
    bool minor = false;
};

struct RegistrationChange
{
    double beat = 0;
    int part = 0;
    Registration registration;
};

// The stops a division should have from a beat on (derived from the parts that play on it).
struct DivisionRegistration
{
    double beat = 0;
    int division = 0;
    Registration registration;
};

struct ScorePart
{
    juce::String name;
    Registration registration; // stops the file asks for at the start
    bool drums = false;
    bool pedalStops = false;   // the file's stops for it are pedal stops ("(ped.)", "Pedal", "Basse de" ...)
    int lowest = 127, highest = 0, count = 0;
    double meanPitch = 60;
};

// Notes of a piece in musical time (quarter notes), independent of any file format.
class Score
{
public:
    juce::String title, composer;
    std::vector<ScorePart> parts;
    std::vector<ScoreNote> notes;
    std::vector<TempoChange> tempos;
    std::vector<TimeSignature> timeSigs;
    std::vector<KeySignature> keySigs;
    std::vector<RegistrationChange> registrationChanges;

    void finalise();
    double lengthBeats() const;
    double lengthSeconds() const { return beatToSeconds (lengthBeats()); }
    double beatToSeconds (double beat) const;
    double secondsToBeat (double seconds) const;
    TimeSignature timeSigAt (double beat) const;
    KeySignature keySigAt (double beat) const;
    // 1-based measure number at a beat (counting from the start of the piece)
    int measureAt (double beat) const;
};

namespace ScoreIO
{
    std::shared_ptr<Score> fromMidi (const juce::MidiFile& midi, const SoundFont* sf);
    std::shared_ptr<Score> loadMidiFile (const juce::File& f, const SoundFont* sf, juce::String& error);
    std::shared_ptr<Score> fromMidiData (const void* data, size_t size, const SoundFont* sf, juce::String& error);
    // registrations: stops per part; each stop gets its own channel carrying the same notes
    // (the usual way organ MIDI files encode registration), as long as channels last
    juce::MidiFile toMidi (const Score& score, const std::vector<juce::String>& partNames = {},
                           const std::vector<Registration>& registrations = {});
    bool saveMidiFile (const Score& score, const juce::File& f, const std::vector<juce::String>& partNames = {},
                       const std::vector<Registration>& registrations = {});

    // Which organ division each part should play on: low parts on the pedal, the others on
    // manuals so that parts sounding together (or answering each other) get different manuals.
    std::vector<int> autoAssign (const Score& score, int numManuals, int pedalIndex);

    // When parts share a division, its registration changes as they take turns: the file's
    // registration of whichever part plays there (merged if several play at once).
    std::vector<DivisionRegistration> registrationTimeline (const Score& score, const std::vector<int>& partDivision);
} // namespace ScoreIO
} // namespace orgel
