#include "Player.h"

namespace orgel
{
void Player::setScore (std::shared_ptr<const Score> s, std::vector<int> map)
{
    score = std::move (s);
    partDivision = std::move (map);
    events.clear();
    next = 0;
    position = 0.0;
    playing = false;
    length = 0;
    if (score == nullptr)
        return;

    events.reserve (score->notes.size() * 2 + score->registrationChanges.size());
    for (auto& n : score->notes)
    {
        const double t0 = score->beatToSeconds (n.start);
        // a hair shorter so repeated notes re-strike audibly
        const double t1 = juce::jmax (t0 + 0.01, score->beatToSeconds (n.start + n.length) - 0.004);
        events.push_back ({ t0, n.part, n.key, true, -1 });
        events.push_back ({ t1, n.part, n.key, false, -1 });
        length = juce::jmax (length, t1);
    }
    for (int i = 0; i < (int) score->registrationChanges.size(); ++i)
    {
        auto& rc = score->registrationChanges[(size_t) i];
        // just before the notes of that beat
        events.push_back ({ score->beatToSeconds (rc.beat) - 0.0005, rc.part, 0, false, i });
    }
    std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b) {
        if (a.time != b.time) return a.time < b.time;
        return ! a.on && b.on; // releases first
    });
    length += 0.5;
}

void Player::seek (double seconds)
{
    seconds = juce::jlimit (0.0, length, seconds);
    position = seconds;
    next = (size_t) (std::lower_bound (events.begin(), events.end(), seconds,
                                       [] (const Event& e, double t) { return e.time < t; })
                     - events.begin());
    needsRelease = true;
}

void Player::process (int numSamples, double sampleRate, std::vector<KeyEvent>& out, std::vector<int>& registrationChanges)
{
    if (! playing || score == nullptr || sampleRate <= 0)
        return;

    const double spd = juce::jlimit (0.1, 4.0, (double) speed.load());
    const double start = position.load();
    const double end = start + numSamples / sampleRate * spd;

    for (size_t m = 0; m < markTimes.size(); ++m)
        if (markTimes[m] >= start && markTimes[m] < end)
            registrationChanges.push_back (-(int) m - 2);

    while (next < events.size() && events[next].time < end)
    {
        auto& e = events[next++];
        if (e.registrationChange >= 0)
        {
            registrationChanges.push_back (e.registrationChange);
            continue;
        }
        const int div = e.part < (int) partDivision.size() ? partDivision[(size_t) e.part] : -1;
        if (div < 0)
            continue;
        const int sample = juce::jlimit (0, numSamples - 1, (int) ((e.time - start) / spd * sampleRate));
        out.push_back ({ sample, div, e.key, e.on, srcPlayer });
    }
    position = end;

    if (end >= length)
    {
        if (loop)
            seek (0.0);
        else
        {
            playing = false;
            position = 0.0;
            next = 0;
            needsRelease = true;
        }
    }
}
} // namespace orgel
