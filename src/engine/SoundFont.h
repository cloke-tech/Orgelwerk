#pragma once
#include <juce_core/juce_core.h>
#include <memory>
#include <vector>

struct tsf;

namespace orgel
{
struct PresetInfo
{
    int bank = 0, program = 0;
    juce::String name;
};

// A loaded SF2 file. Immutable after loading; every engine renders through its own
// tsf copy that shares the sample data with this master instance.
class SoundFont
{
public:
    ~SoundFont();

    static std::shared_ptr<SoundFont> loadBuiltIn();
    static std::shared_ptr<SoundFont> loadFromFile (const juce::File& file, juce::String& error);

    tsf* createInstance() const;

    const std::vector<PresetInfo>& getPresets() const { return presets; }
    bool hasPreset (int bank, int program) const;
    juce::String presetName (int bank, int program) const;

    // Files made for the Jeux SoundFont often address it at banks 42/43; map such
    // bank numbers back to what this file actually contains.
    bool resolvePreset (int& bank, int program) const;

    juce::String getName() const { return name; }
    juce::File getFile() const { return file; } // empty for the built-in font

private:
    SoundFont() = default;
    static std::shared_ptr<SoundFont> fromTsf (tsf* t, juce::String name, juce::File file);

    tsf* master = nullptr;
    std::vector<PresetInfo> presets;
    juce::String name;
    juce::File file;
};
} // namespace orgel
