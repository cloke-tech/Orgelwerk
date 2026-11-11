#define TSF_IMPLEMENTATION
#include "tsf.h"

#include "SoundFont.h"
#include "BinaryData.h"

namespace orgel
{
SoundFont::~SoundFont()
{
    if (master != nullptr)
        tsf_close (master);
}

std::shared_ptr<SoundFont> SoundFont::fromTsf (tsf* t, juce::String n, juce::File f)
{
    if (t == nullptr)
        return {};

    // Many organ SoundFonts (Jeux among them) leave attack and release at the SF2 default of
    // ~1 ms: every key release cuts the sound off and clicks. Real pipes need a few
    // milliseconds to speak and ring out briefly, so give every region a minimum.
    for (int i = 0; i < t->presetNum; ++i)
        for (int r = 0; r < t->presets[i].regionNum; ++r)
        {
            auto& env = t->presets[i].regions[r].ampenv;
            env.attack = std::max (env.attack, 0.012f);
            env.release = std::max (env.release, 0.09f);
        }

    std::shared_ptr<SoundFont> sf (new SoundFont());
    sf->master = t;
    sf->name = std::move (n);
    sf->file = std::move (f);

    for (int i = 0; i < t->presetNum; ++i)
    {
        PresetInfo p;
        p.bank = t->presets[i].bank;
        p.program = t->presets[i].preset;
        const char* raw = t->presets[i].presetName;
        auto len = (int) strnlen (raw, 20);
        auto validUtf8 = [] (const char* t, int n) {
            for (int c = 0; c < n;)
            {
                auto b = (unsigned char) t[c];
                int extra = b < 0x80 ? 0 : (b >> 5) == 6 ? 1 : (b >> 4) == 14 ? 2 : (b >> 3) == 30 ? 3 : -1;
                if (extra < 0 || c + extra >= n + (extra == 0 ? 1 : 0)) return false;
                for (int k = 1; k <= extra; ++k)
                    if (c + k >= n || ((unsigned char) t[c + k] >> 6) != 2) return false;
                c += extra + 1;
            }
            return true;
        };
        if (validUtf8 (raw, len))
            p.name = juce::String::fromUTF8 (raw, len).trim();
        else // SF2 names are usually Latin-1
            for (int c = 0; c < len; ++c)
                p.name += juce::String::charToString ((juce::juce_wchar) (unsigned char) raw[c]);
        p.name = p.name.trim();
        sf->presets.push_back (p);
    }

    std::sort (sf->presets.begin(), sf->presets.end(), [] (const PresetInfo& a, const PresetInfo& b) {
        return a.bank != b.bank ? a.bank < b.bank : a.program < b.program;
    });
    return sf;
}

std::shared_ptr<SoundFont> SoundFont::loadBuiltIn()
{
    return fromTsf (tsf_load_memory (BinaryData::Jeux14_sf2, BinaryData::Jeux14_sf2Size), "Jeux14 (eingebaut)", {});
}

std::shared_ptr<SoundFont> SoundFont::loadFromFile (const juce::File& f, juce::String& error)
{
    juce::MemoryBlock data;
    if (! f.loadFileAsData (data))
    {
        error = "Datei kann nicht gelesen werden: " + f.getFullPathName();
        return {};
    }
    auto* t = tsf_load_memory (data.getData(), (int) data.getSize());
    if (t == nullptr)
    {
        error = juce::String::fromUTF8 ("Keine gültige SF2-Datei: ") + f.getFileName();
        return {};
    }
    return fromTsf (t, f.getFileNameWithoutExtension(), f);
}

tsf* SoundFont::createInstance() const
{
    return master != nullptr ? tsf_copy (master) : nullptr;
}

bool SoundFont::hasPreset (int bank, int program) const
{
    for (auto& p : presets)
        if (p.bank == bank && p.program == program)
            return true;
    return false;
}

juce::String SoundFont::presetName (int bank, int program) const
{
    for (auto& p : presets)
        if (p.bank == bank && p.program == program)
            return p.name;
    return "Bank " + juce::String (bank) + " / " + juce::String (program);
}

bool SoundFont::resolvePreset (int& bank, int program) const
{
    if (hasPreset (bank, program))
        return true;
    for (int candidate : { bank - 42, bank % 128, 0 })
        if (candidate >= 0 && hasPreset (candidate, program))
        {
            bank = candidate;
            return true;
        }
    return false;
}
} // namespace orgel
