#pragma once
#include <juce_core/juce_core.h>
#include <vector>

// Languages of the user interface. Texts are written in German in the code; tr() returns them
// in the chosen language (the system's language unless one is picked in the menu).
namespace orgel::i18n
{
struct Language
{
    const char* code;       // "de", "en" ...
    const char* nativeName; // "Deutsch", "English" ...
};
const std::vector<Language>& languages();

juce::String setting();                     // "auto" or a language code
void setSetting (const juce::String& code); // stored for the next start
juce::String current();                     // the language in use
} // namespace orgel::i18n

namespace juce
{
class PropertiesFile;
}

namespace orgel
{
// settings of the program as a whole (language, own rooms), shared by the app and the plugins
juce::PropertiesFile& appSettings();

// the text in the current language (the German text if there is no translation)
juce::String tr (const char* germanUtf8);
} // namespace orgel
