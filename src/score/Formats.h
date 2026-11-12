#pragma once
#include "Notation.h"
#include "Score.h"
#include "../engine/OrganEngine.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace orgel
{
// A loaded piece: playable notes plus the engraved pages.
struct Piece
{
    std::shared_ptr<Score> score;
    std::shared_ptr<Notation> notation;
    juce::File source;
    bool notationFromScore = false; // engraved from the notes (MIDI) rather than read from a notation file
    juce::String warning;
};

namespace Formats
{
    // Import
    juce::String importWildcard();
    std::vector<std::pair<juce::String, juce::String>> importFormatList(); // (description, extensions)
    bool isMidiFile (const juce::File& f);
    bool needsMuseScore (const juce::File& f);
    juce::File findMuseScore();

    // Loads any supported file. Runs on a background thread.
    // partIsPedal is used for engraving MIDI files (which part goes to the pedal staff).
    // engraveMidi = false: MIDI files are only read (fast); their pages are set later on request
    std::unique_ptr<Piece> load (const juce::File& f, const SoundFont* sf, int numManuals, int pedalIndex, juce::String& error,
                                 bool engraveMidi = true);

    // What the score shows about the organ: the divisions' names (for "Hw.", "Pos." ...) and
    // the file's registration over time (printed as stop lists and changes).
    struct OrganInfo
    {
        std::vector<juce::String> divisionNames;
        std::vector<bool> divisionIsPedal;
        std::vector<DivisionRegistration> registration;
        std::shared_ptr<SoundFont> soundFont; // for the stop names
        bool colourManuals = false;           // notes in their manual's colour
        bool staffPerManual = true;           // own staves per manual (else the classic three)
        static OrganInfo from (OrganEngine& organ, std::vector<DivisionRegistration> registration);
    };

    // Re-engraves a MIDI-based piece, e.g. after parts were moved to other divisions.
    std::shared_ptr<Notation> engrave (const Score& score, const std::vector<bool>& partIsPedal, const std::vector<int>& partDivision,
                                       juce::String& error, const OrganInfo& organ = {});

    // Export
    struct ExportFormat
    {
        juce::String id, description, extension;
        bool needsMuseScore = false;
        bool isAudio = false;
    };
    std::vector<ExportFormat> exportFormats();

    struct ExportContext
    {
        const Piece* piece = nullptr;
        std::vector<int> partDivision;
        std::vector<bool> partIsPedal;
        std::vector<juce::String> partNames;
        OrganEngine* liveOrgan = nullptr; // copied for audio rendering
        double sampleRate = 48000.0;
        int bitDepth = 24;
        float speed = 1.0f;
        // registration track: (seconds, combination as captured by OrganEngine)
        std::vector<std::pair<double, juce::String>> registrationMarks;
        // the file's registration over time (empty: keep the organ as it is)
        std::vector<DivisionRegistration> registrationTimeline;
        bool colourManuals = false;
        bool staffPerManual = true;
        std::function<bool (float)> progress; // return false to cancel
        std::vector<int>* voiceStats = nullptr; // optional: most voices per 30 s (tests)
    };
    bool exportTo (const ExportFormat& fmt, const juce::File& target, const ExportContext& ctx, juce::String& error);

    // Renders the piece through the organ into a stereo buffer (used by audio export and tests).
    bool renderAudio (const ExportContext& ctx, juce::AudioFormatWriter& writer, juce::String& error);
} // namespace Formats
} // namespace orgel
