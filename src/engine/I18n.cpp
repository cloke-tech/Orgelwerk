#include "I18n.h"
#include <juce_data_structures/juce_data_structures.h>
#include "BinaryData.h"
#include <cstdlib>
#include <map>

namespace orgel::i18n
{
const std::vector<Language>& languages()
{
    static const std::vector<Language> list {
        { "de", "Deutsch" }, { "en", "English" }, { "fr", "Français" },
        { "es", "Español" }, { "it", "Italiano" }, { "nl", "Nederlands" },
    };
    return list;
}

static juce::PropertiesFile& settingsFile();
} // namespace orgel::i18n

juce::PropertiesFile& orgel::appSettings()
{
    return orgel::i18n::settingsFile();
}

namespace orgel::i18n
{
static juce::PropertiesFile& settingsFile()
{
    static juce::PropertiesFile file = [] {
        juce::PropertiesFile::Options o;
        o.applicationName = "Orgelwerk";
        o.folderName = "Orgelwerk";
        o.filenameSuffix = ".settings";
        o.osxLibrarySubFolder = "Application Support";
        return juce::PropertiesFile (o);
    }();
    return file;
}

juce::String setting()
{
    return settingsFile().getValue ("language", "auto");
}

void setSetting (const juce::String& code)
{
    settingsFile().setValue ("language", code);
    settingsFile().saveIfNeeded();
}

static bool known (const juce::String& code)
{
    for (auto& l : languages())
        if (code == l.code)
            return true;
    return false;
}

juce::String current()
{
    static const juce::String chosen = [] {
        // ORGEL_LANG overrides everything (tests)
        if (auto* env = std::getenv ("ORGEL_LANG"))
            if (known (env))
                return juce::String (env);
        auto s = setting();
        if (known (s))
            return s;
        // the system's language; languages Orgelwerk does not have get English
        for (auto candidate : { juce::SystemStats::getDisplayLanguage(), juce::SystemStats::getUserLanguage() })
        {
            auto code = candidate.substring (0, 2).toLowerCase();
            if (known (code))
                return code;
        }
        return juce::String ("en");
    }();
    return chosen;
}
} // namespace orgel::i18n

namespace orgel
{
juce::String tr (const char* germanUtf8)
{
    static const std::map<juce::String, juce::String> table = [] {
        std::map<juce::String, juce::String> t;
        const auto lang = i18n::current();
        if (lang == "de")
            return t;
        int size = 0;
        if (auto* data = BinaryData::getNamedResource ((lang + "_json").toRawUTF8(), size))
        {
            auto json = juce::JSON::parse (juce::String::fromUTF8 (data, size));
            if (auto* obj = json.getDynamicObject())
                for (auto& p : obj->getProperties())
                    t[p.name.toString()] = p.value.toString();
        }
        return t;
    }();
    const auto german = juce::String::fromUTF8 (germanUtf8);
    auto it = table.find (german);
    return it != table.end() && it->second.isNotEmpty() ? it->second : german;
}
} // namespace orgel
