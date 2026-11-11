#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include "engine/OrganEngine.h"
#include "score/Formats.h"
#include "score/Player.h"

namespace orgel
{
// Opens every MIDI input itself (standalone app only), so each manual can listen to its own device.
class MidiInputRouter : private juce::MidiInputCallback
{
public:
    MidiInputRouter();
    ~MidiInputRouter() override;

    struct Message
    {
        int device = 0;
        uint8_t data[3] {};
        int size = 0;
    };

    void start();
    void stop();
    void refreshDevices();
    // audio thread
    template <typename Fn>
    void drain (Fn&& fn)
    {
        const auto scope = fifo.read (fifo.getNumReady());
        for (int i = scope.startIndex1; i < scope.startIndex1 + scope.blockSize1; ++i) fn (buffer[(size_t) i]);
        for (int i = scope.startIndex2; i < scope.startIndex2 + scope.blockSize2; ++i) fn (buffer[(size_t) i]);
    }
    juce::String identifierFor (int deviceIndex) const;
    juce::String nameFor (int deviceIndex) const;
    juce::Array<juce::MidiDeviceInfo> connectedDevices() const;

private:
    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& m) override;

    juce::AbstractFifo fifo { 4096 };
    std::array<Message, 4096> buffer;
    juce::SpinLock writeLock;

    mutable juce::SpinLock tableLock;
    juce::StringArray identifiers, names; // append only: index = device number
    std::map<juce::String, std::unique_ptr<juce::MidiInput>> open;
    std::unique_ptr<juce::MidiDeviceListConnection> listConnection;
};

class OrgelProcessor : public juce::AudioProcessor,
                       public juce::ChangeBroadcaster,
                       private juce::Timer
{
public:
    OrgelProcessor();
    ~OrgelProcessor() override;

    // --- AudioProcessor ---------------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Orgelwerk"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // --- organ ------------------------------------------------------------------
    OrganEngine organ;
    bool isStandalone() const { return standalone; }
    MidiInputRouter& getMidiRouter() { return router; }
    bool loadSoundFont (const juce::File& f, juce::String& error);
    void useBuiltInSoundFont();

    // MIDI learn: the next note from any input assigns that input to the division
    void startMidiLearn (int division) { learnDivision = division; }
    int getMidiLearnDivision() const { return learnDivision.load(); }

    // registration memory ("Setzer")
    static constexpr int numCombinations = 10;
    juce::String combinations[numCombinations];
    void storeCombination (int i);
    void recallCombination (int i);

    // --- score ------------------------------------------------------------------
    void loadFileAsync (const juce::File& f);
    bool isLoading() const { return loading.load(); }
    juce::String takeLoadMessage();
    std::shared_ptr<Piece> getPiece() const;
    const std::vector<int>& getPartDivision() const { return partDivision; }
    void setPartDivision (int part, int division);
    std::vector<bool> partIsPedal() const;
    bool applyFileRegistration = true;
    void applyPieceRegistration();
    bool hostSync = false;

    void play();
    void pause();
    void stop();
    void seek (double seconds);
    bool isPlaying() const { return player.isPlaying(); }
    double getPosition() const { return player.getPosition(); }
    double getLength() const { return player.getLength(); }
    void setSpeed (float s) { player.speed = s; }
    float getSpeed() const { return player.speed.load(); }
    void setLoop (bool b) { player.loop = b; }
    bool getLoop() const { return player.loop.load(); }

    // registration track: registrations remembered at positions of the piece
    struct RegistrationMark
    {
        double beat = 0;
        juce::String combination;
    };
    std::vector<RegistrationMark> getMarks() const;
    void addMarkHere();
    void updateMark (int index);
    void removeMark (int index);
    void clearMarks();
    void applyMarkAtPosition();

    // what the UI should show while playing: current beat of the score
    double getCurrentBeat() const;

    // --- export -----------------------------------------------------------------
    Formats::ExportContext makeExportContext() const;
    double getSampleRateForExport() const { return currentSampleRate > 0 ? currentSampleRate : 48000.0; }

private:
    void timerCallback() override;
    void setPiece (std::shared_ptr<Piece> p);
    void reengrave();

    bool standalone = false;
    MidiInputRouter router;
    double currentSampleRate = 48000.0;

    Player player;
    std::shared_ptr<Piece> piece;
    std::vector<int> partDivision;
    mutable juce::CriticalSection pieceLock;

    std::vector<KeyEvent> events;
    std::vector<int> pendingRegistrationsAudio;
    juce::AbstractFifo regFifo { 256 };
    std::array<int, 256> regBuffer {};

    std::atomic<int> learnDivision { -1 };
    struct LearnResult
    {
        std::atomic<bool> ready { false };
        int division = -1, channel = 0;
        juce::String device, deviceName;
    } learnResult;

    std::atomic<bool> loading { false };
    juce::String loadMessage;
    juce::CriticalSection messageLock;
    std::unique_ptr<juce::Thread> loaderThread;
    juce::ThreadPool pool { 1 };
    bool wasHostPlaying = false;
    std::vector<RegistrationMark> marks;
    std::vector<OrganEngine::Combination> markCombos; // parsed for the audio thread
    void compileMarks();
    std::vector<DivisionRegistration> timeline; // the file's registration over time
    void compileTimeline();
    void applyFileRegistrationAt (double beat);
    int knownDivisions = -1;
    std::shared_ptr<int> alive = std::make_shared<int> (0); // async callbacks check this
    juce::StringArray restorePartMap;
    bool restoreApply = true, restorePending = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrgelProcessor)
};
} // namespace orgel
