#pragma once
#include <juce_core/juce_core.h>
#include <cstdlib>
#include <map>

// Timing of UI work, switched on with the environment variable ORGEL_PROFILE=1.
// Every two seconds it prints how often each part ran and how long it took.
namespace orgel::profile
{
inline bool enabled()
{
    static const bool on = std::getenv ("ORGEL_PROFILE") != nullptr;
    return on;
}

// set when the plug-in is created (see OrgelProcessor), for start-up timing
inline double& startMsRef()
{
    static double t = juce::Time::getMillisecondCounterHiRes();
    return t;
}
inline double startMs() { return startMsRef(); }

// one line with the time since start (for start-up steps)
inline void mark (const char* what)
{
    if (enabled())
        std::fprintf (stderr, "[profile] %s at %.0f ms\n", what, juce::Time::getMillisecondCounterHiRes() - startMs());
}

struct Stat
{
    int count = 0;
    double total = 0, max = 0;
};

inline void record (const char* name, double ms)
{
    static std::map<juce::String, Stat> stats;
    static double lastPrint = juce::Time::getMillisecondCounterHiRes();
    auto& s = stats[name];
    ++s.count;
    s.total += ms;
    s.max = juce::jmax (s.max, ms);
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastPrint > 2000.0)
    {
        juce::String line;
        for (auto& [n, st] : stats)
            line << n << " " << st.count << "x avg " << juce::String (st.total / juce::jmax (1, st.count), 2) << " max "
                 << juce::String (st.max, 2) << " ms | ";
        std::fprintf (stderr, "[profile] %s\n", line.toRawUTF8());
        stats.clear();
        lastPrint = now;
    }
}

struct Scope
{
    explicit Scope (const char* n) : name (n), start (enabled() ? juce::Time::getMillisecondCounterHiRes() : 0.0) {}
    ~Scope()
    {
        if (enabled())
            record (name, juce::Time::getMillisecondCounterHiRes() - start);
    }
    const char* name;
    double start;
};
} // namespace orgel::profile

#define ORGEL_PROFILE_SCOPE(name) const orgel::profile::Scope orgelProfileScope_ (name)
