#pragma once
#include "Score.h"
#include "../engine/OrganEngine.h"
#include <atomic>

namespace orgel
{
// Plays a Score on the organ. Runs on the audio thread (or offline), under the engine lock.
class Player
{
public:
    struct Event
    {
        double time = 0; // seconds at speed 1
        int part = 0, key = 0;
        bool on = false;
        int registrationChange = -1; // index into Score::registrationChanges
    };

    // Called with the engine lock held.
    void setScore (std::shared_ptr<const Score> s, std::vector<int> partToDivision);
    void setPartDivision (std::vector<int> partToDivision) { partDivision = std::move (partToDivision); }
    // registration marks (seconds); reported as -(index + 2) in registrationChanges
    void setMarkTimes (std::vector<double> times) { markTimes = std::move (times); }
    // the file's registration timeline (seconds); reported as the timeline index (>= 0)
    void setTimelineTimes (std::vector<double> times) { timelineTimes = std::move (times); }
    const std::vector<int>& getPartDivision() const { return partDivision; }

    // Collects the key events that fall into the next block. Registration changes found
    // in the block are reported through pendingRegistration (applied by the caller).
    void process (int numSamples, double sampleRate, std::vector<KeyEvent>& out, std::vector<int>& registrationChanges);

    void play() { playing = true; }
    void pause() { playing = false; }
    bool isPlaying() const { return playing; }
    void seek (double seconds);
    double getPosition() const { return position.load(); }
    double getLength() const { return length; }
    bool hasScore() const { return score != nullptr; }

    std::atomic<float> speed { 1.0f };
    std::atomic<bool> loop { false };
    // true when the player just stopped or jumped: the caller releases the player's keys
    std::atomic<bool> needsRelease { false };

private:
    std::shared_ptr<const Score> score;
    std::vector<int> partDivision;
    std::vector<Event> events;
    std::vector<double> markTimes, timelineTimes;
    size_t next = 0;
    std::atomic<double> position { 0.0 };
    double length = 0;
    std::atomic<bool> playing { false };
};
} // namespace orgel
