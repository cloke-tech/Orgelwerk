#pragma once
#include <juce_core/juce_core.h>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace vrv
{
class Toolkit;
}

namespace orgel
{
// Engraved score pages (via Verovio) plus the note timing needed to follow playback.
class Notation
{
public:
    Notation();
    ~Notation();

    struct TimeEntry
    {
        double qstamp = 0; // quarter notes from the start
        std::vector<juce::String> on, off;
    };

    // Format hints: "musicxml", "mei", "abc", "humdrum", "pae", "auto"
    bool loadData (const juce::String& data, const juce::String& format, juce::String& error);
    bool loadFile (const juce::File& file, juce::String& error);
    void setLayout (int pageWidth, int pageHeight, int scale);
    // true: staves with nothing to play are left out of a line (a staff per manual);
    // false: every staff in every line (the classic organ layout). Set before loading.
    // (not called: Verovio's own choice, for notation files)
    void setHideEmptyStaves (bool hide) { hideEmptyStaves = hide ? 1 : 0; }
    // true: the whole piece on one line, as wide as it needs (a continuous view). Set before loading.
    void setContinuous (bool c) { continuous = c; }
    bool isContinuous() const { return continuous; }
    // what the notes were loaded from (empty for compressed MusicXML)
    juce::String getSourceData() const { return lastData; }
    juce::String getSourceFormat() const { return lastFormat; }

    int getPageCount() const { return (int) pages.size(); }
    juce::String getPageSVG (int page) const; // 0-based, preprocessed for JUCE
    juce::String getRawPageSVG (int page) const;
    int pageOfElement (const juce::String& id) const;
    // ids of the systems (lines of staves) on a page, top to bottom
    std::vector<juce::String> getSystemIds (int page) const;
    const std::vector<TimeEntry>& getTimemap() const { return timemap; }
    bool isLoaded() const { return ! pages.empty(); }

    juce::MemoryBlock renderMidi();
    juce::String getMEI();
    juce::String getHumdrum();
    // Humdrum via humlib's MusicXML converter (more robust than going through MEI)
    static juce::String musicXmlToHumdrum (const juce::String& musicXml);
    juce::String getTitle() const { return title; }

    static juce::File resourceDirectory();
    static juce::StringArray verovioExtensions();

    // Replaces <use> references to <symbol> glyphs by the glyph paths, which JUCE can draw.
    static juce::String inlineSymbols (const juce::String& svg);

private:
    void applyOptions();
    void renderAll (bool relayout);
    vrv::Toolkit& toolkit();

    std::unique_ptr<vrv::Toolkit> tk;
    std::vector<juce::String> pages;
    std::vector<std::vector<juce::String>> systemIds;
    std::vector<TimeEntry> timemap;
    std::map<juce::String, int> notePage;
    juce::String title;
    int pageWidth = 2100, pageHeight = 2970, scale = 40;
    int hideEmptyStaves = -1;
    bool continuous = false;
    juce::String lastData, lastFormat;
    mutable std::recursive_mutex mutex;
};
} // namespace orgel
