#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <deque>

namespace orgel
{
// Peak limiter with a short look-ahead: it sees a peak coming and turns the gain down
// smoothly before it arrives, so the output never goes above the ceiling.
class LookaheadLimiter
{
public:
    void prepare (double sampleRate, int numChannels)
    {
        lookahead = juce::jmax (8, (int) (sampleRate * 0.003));
        channels = juce::jlimit (1, 2, numChannels);
        delay.assign ((size_t) (lookahead * channels), 0.0f);
        writePos = 0;
        window.clear();
        sampleIndex = 0;
        gain = 1.0f;
        attackCoeff = 1.0f - std::exp (-5.0f / (float) lookahead);
        releaseCoeff = 1.0f - std::exp (-1.0f / (float) (0.15 * sampleRate));
    }

    int getLatency() const { return lookahead; }

    void process (juce::AudioBuffer<float>& buffer, int numSamples)
    {
        if (delay.empty())
            return;
        const int nch = juce::jmin (channels, buffer.getNumChannels());
        for (int i = 0; i < numSamples; ++i)
        {
            float peak = 0.0f;
            for (int c = 0; c < nch; ++c)
                peak = juce::jmax (peak, std::abs (buffer.getSample (c, i)));
            const float required = peak > ceiling ? ceiling / peak : 1.0f;

            // smallest gain needed by anything within the look-ahead window
            while (! window.empty() && window.back().second >= required)
                window.pop_back();
            window.push_back ({ sampleIndex, required });
            while (window.front().first <= sampleIndex - lookahead)
                window.pop_front();
            const float target = window.front().second;

            gain += (target < gain ? attackCoeff : releaseCoeff) * (target - gain);

            for (int c = 0; c < nch; ++c)
            {
                auto& slot = delay[(size_t) (writePos * channels + c)];
                const float delayed = slot;
                slot = buffer.getSample (c, i);
                buffer.setSample (c, i, juce::jlimit (-ceiling, ceiling, delayed * gain));
            }
            writePos = (writePos + 1) % lookahead;
            ++sampleIndex;
        }
    }

private:
    static constexpr float ceiling = 0.93f; // about -0.6 dBFS
    int lookahead = 128, channels = 2, writePos = 0;
    std::vector<float> delay;
    std::deque<std::pair<juce::int64, float>> window;
    juce::int64 sampleIndex = 0;
    float gain = 1.0f, attackCoeff = 0.1f, releaseCoeff = 0.001f;
};
} // namespace orgel
