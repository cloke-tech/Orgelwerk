#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>
#include <juce_dsp/juce_dsp.h>
#include "OrganModel.h"
#include "SoundFont.h"
#include "Limiter.h"
#include <atomic>

struct tsf;

namespace orgel
{
struct KeyEvent
{
    int sample = 0;
    int division = 0;
    int key = 0;
    bool on = false;
    uint8_t source = srcMidi;
};

// The organ itself: divisions with stops, couplers, and the SF2 voices that sound.
// Every public method locks; the audio thread calls render() under the same lock.
class OrganEngine : public juce::ChangeBroadcaster
{
public:
    OrganEngine();
    ~OrganEngine() override;

    juce::CriticalSection& getLock() { return lock; }

    void setSoundFont (std::shared_ptr<SoundFont> sf);
    std::shared_ptr<SoundFont> getSoundFont() const { return soundFont; }

    void prepare (double sampleRate, int maxBlockSize);
    // Caller must hold getLock(). Events must be sorted by sample.
    void renderLocked (juce::AudioBuffer<float>& out, const std::vector<KeyEvent>& events);

    // --- model -----------------------------------------------------------------
    void setDefaultDisposition();
    int getNumDivisions() const { return (int) divisions.size(); }
    const Division& getDivision (int d) const { return divisions[(size_t) d]; }
    const std::vector<Coupler>& getCouplers() const { return couplers; }
    juce::String couplerName (const Coupler& c) const;
    int getNumManuals() const;
    int pedalIndex() const;

    int addManual();
    void removeDivision (int d);
    void renameDivision (int d, const juce::String& name);
    int addStop (int d, int bank, int program, const juce::String& name, bool on = false, int octave = 0);
    void removeStop (int d, int s);
    void moveStop (int d, int s, int newIndex);
    void setStop (int d, int s, bool on);
    void toggleStop (int d, int s);
    // removes the stops that pieces brought along (see Stop::temporary)
    void removeTemporaryStops();
    bool hasStopOn (int d) const;
    // A basic registration for a division that has nothing drawn: principal 8' + 4' on a
    // manual, 16' + 8' in the pedal (flutes if there are no principals, else the first stop).
    // Returns false if nothing could be drawn (no stops at all).
    bool drawBasicRegistration (int d);
    void setCoupler (int c, bool on);
    void setDivisionVolume (int d, float v);
    void setDivisionExpression (int d, float v);
    void setTremulant (int d, bool on);
    void setMidiAssign (int d, const MidiAssign& m);
    void clearRegistration();
    // Pulls exactly the given stops (adding them to the division if missing).
    void applyRegistration (int d, const Registration& stops);

    // registration memory ("Setzer")
    juce::String captureCombination() const;
    void recallCombination (const juce::String& combination);

    float reverb = 0.35f, master = 0.75f;

    // The church and the listener: how big the room is, where one sits, and how the sound is
    // shaped (bass that fills the room, the low pedal stops one can feel, width, brilliance).
    struct Acoustics
    {
        int room = 2;          // 0 chapel, 1 village church, 2 town church, 3 cathedral, 4 minster
        // defaults matched to a recording of a real church organ (measured: ~2 s reverb,
        // ~2.6 s in the bass, treble falling off strongly towards the listener)
        float distance = 0.5f; // 0 at the console ... 1 at the back of the nave
        float bass = 0.45f;    // the fundamental range (shelf around 180 Hz), and a longer bass reverb
        float sub = 0.35f;     // the 16' and 32' range one feels (below ~45 Hz, with its overtones)
        float width = 0.5f;    // stereo width
        float treble = 0.4f;   // brilliance (0.5 = neutral)
        bool operator== (const Acoustics& o) const
        {
            return room == o.room && distance == o.distance && bass == o.bass && sub == o.sub && width == o.width && treble == o.treble;
        }
    };
    Acoustics getAcoustics() const;
    void setAcoustics (const Acoustics& a);
    static int numRooms() { return 5; }
    // pipes placed in the room (divisions at their positions, C/C# sides apart)
    bool spatial = true;
    void setSpatial (bool on);
    int transpose = 0;
    void setTranspose (int semitones);

    // --- keys ------------------------------------------------------------------
    void keyDown (int d, int key, uint8_t source);
    void keyUp (int d, int key, uint8_t source);
    void releaseSource (uint8_t source);
    void allNotesOff();
    std::bitset<128> getHeldKeys (int d) const;
    // how many stops sound each key right now (for the animated facade)
    std::array<uint8_t, 128> getSoundingCounts() const;

    // A registration that can be applied without allocating (on the audio thread).
    struct Combination
    {
        std::vector<std::vector<bool>> stops;
        std::vector<bool> couplers;
    };
    Combination parseCombination (const juce::String& text) const;
    void applyCombinationLocked (const Combination& c);
    // Is the reverb ready (impulse response loaded)? Offline rendering waits for it.
    bool isReverbReady() const;
    int getLatencySamples() const { return limiter.getLatency(); }
    int activeVoiceCount() const;

    // --- state -----------------------------------------------------------------
    std::unique_ptr<juce::XmlElement> toXml() const;
    void fromXml (const juce::XmlElement& xml);
    // copy the full organ (for offline rendering) without runtime state
    void copySetupFrom (OrganEngine& other);

    void notifyChanged() { sendChangeMessage(); }

private:
    void keyDownLocked (int d, int key, uint8_t source);
    void keyUpLocked (int d, int key, uint8_t source);
    bool keyActive (int d, int key) const;
    void reconcileKey (int targetDivision, int key);
    void reconcileStop (int d, int s);
    void reconcileDivisionStops (int d);
    void stopVoice (Division& div, Stop& st);
    static bool cisSide (int key) { return (key % 12) % 2 == 1; }
    int allocateChannel (Division& div);
    void freeChannel (Division& div, int ch);
    std::shared_ptr<tsf> makeSynth() const;
    void ensureSynth (Division& div);
    struct Placement
    {
        float panC = 0.5f, panCis = 0.5f, depth = 0.4f;
    };
    Placement placementFor (int d) const;
    void applyPlacement (int d);
    void prepareDivisionDsp (Division& div);
    void assignChannel (int d, Stop& st);
    void applyChannelVolumes (int d);
    void rebuildCouplers();
    void silenceAll();

    juce::CriticalSection lock;

    // What the keyboards and the pipe facade show, published by the audio thread after every
    // block, so the screen can read it many times a second without taking the lock.
    void publishUiState();
    static constexpr int maxUiDivisions = 16;
    std::array<std::array<std::atomic<uint64_t>, 2>, maxUiDivisions> uiHeld {};
    std::array<std::atomic<uint64_t>, 16> uiSounding {};
    std::atomic<double> uiPublishedMs { 0.0 };
    bool uiStateFresh() const { return juce::Time::getMillisecondCounterHiRes() - uiPublishedMs.load (std::memory_order_acquire) < 150.0; }
    std::shared_ptr<SoundFont> soundFont;
    double sampleRate = 48000.0;
    std::vector<float> interleaved;

    std::vector<Division> divisions;
    std::vector<Coupler> couplers;

    void buildImpulseResponse();
    void updateGains (int numSamples);
    void updateToneFilters();

    // a plain biquad (coefficients worked out off the audio thread, no allocation)
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1[2] = {}, z2[2] = {};
        float process (float x, int ch)
        {
            const float y = b0 * x + z1[ch];
            z1[ch] = b1 * x - a1 * y + z2[ch];
            z2[ch] = b2 * x - a2 * y;
            return y;
        }
        void setCoefficients (const Biquad& c) { b0 = c.b0; b1 = c.b1; b2 = c.b2; a1 = c.a1; a2 = c.a2; }
    };
    Acoustics acoustics;
    Biquad bassShelf, subShelf, trebleShelf, harmonicLow, harmonicBand, distanceShelf;
    float harmonicGain = 0, widthFactor = 1, dryFactor = 1, wetFactor = 1;
    int builtRoom = -1;
    float builtBass = -1;

    juce::dsp::Convolution convolution { juce::dsp::Convolution::NonUniform { 1024 } };
    LookaheadLimiter limiter;
    juce::AudioBuffer<float> wet, send;
    bool dspPrepared = false;
    double tremPhase = 0.0;
    juce::SmoothedValue<float> reverbSmoothed, masterSmoothed;
};
} // namespace orgel
