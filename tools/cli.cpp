// Orgelwerk command line: load a score, show what the organ makes of it, export it.
//   orgelcli info  <file>
//   orgelcli export <file> <target.ext> [<target2.ext> ...]
//   orgelcli stops <file>   (stops the piece adds, and that they leave again)
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "engine/OrganEngine.h"
#include "score/Formats.h"

using namespace orgel;

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 3)
    {
        std::cout << "usage: orgelcli info <file> | export <file> <target...>\n";
        return 1;
    }
    const juce::String cmd (argv[1]);
    const juce::File input = juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]);

    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]) == "--bank0")
            SoundFont::fallbackToBankZero = true;
    auto sf = SoundFont::loadBuiltIn();
    OrganEngine organ;
    organ.setSoundFont (sf);
    organ.setDefaultDisposition();

    juce::String error;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    // MIDI pages are set at export, with the organ's names and registration (as in the app)
    auto piece = Formats::load (input, sf.get(), organ.getNumManuals(), organ.pedalIndex(), error, false);
    if (piece == nullptr)
    {
        std::cout << "load failed: " << error << "\n";
        return 2;
    }
    std::cout << "loaded in " << (int) (juce::Time::getMillisecondCounterHiRes() - t0) << " ms: " << piece->score->title
              << "\n  notes " << piece->score->notes.size() << ", length " << piece->score->lengthSeconds() << " s, "
              << piece->score->tempos.size() << " tempo changes, " << piece->score->registrationChanges.size() << " registration changes\n";
    if (piece->warning.isNotEmpty())
        std::cout << "  warning: " << piece->warning << "\n";
    if (piece->notation != nullptr)
        std::cout << "  pages " << piece->notation->getPageCount() << ", timemap entries " << piece->notation->getTimemap().size() << "\n";

    auto map = ScoreIO::autoAssign (*piece->score, organ.getNumManuals(), organ.pedalIndex());
    for (int p = 0; p < (int) piece->score->parts.size(); ++p)
    {
        auto& part = piece->score->parts[(size_t) p];
        std::cout << "  part " << p << " '" << part.name << "' notes " << part.count << " range " << part.lowest << "-" << part.highest
                  << " -> " << (map[(size_t) p] >= 0 ? organ.getDivision (map[(size_t) p]).name : juce::String ("(stumm)")) << " | reg:";
        for (auto rs : part.registration)
            std::cout << " " << sf->presetName (rs.bank, rs.program) << " (" << rs.bank << ":" << rs.program << ")"
                      << (rs.octave != 0 ? " oct" + juce::String (rs.octave) : juce::String());
        std::cout << "\n";

    }

    for (auto& e : ScoreIO::registrationTimeline (*piece->score, map))
    {
        std::cout << "  reg @ beat " << e.beat << " (" << (int) (piece->score->beatToSeconds (e.beat)) << " s) " << organ.getDivision (e.division).name << ":";
        for (auto rs : e.registration)
            std::cout << " " << sf->presetName (rs.bank, rs.program) << (rs.octave != 0 ? " oct" + juce::String (rs.octave) : juce::String());
        std::cout << "\n";
    }
    if (cmd == "stops")
    {
        auto count = [&] {
            int n = 0;
            for (int d = 0; d < organ.getNumDivisions(); ++d)
                n += (int) organ.getDivision (d).stops.size();
            return n;
        };
        const int before = count();
        for (auto& e : ScoreIO::registrationTimeline (*piece->score, map))
            organ.applyRegistration (e.division, e.registration);
        const int during = count();
        // the old settings format: everything beyond the standard stops counts as added
        auto xml = organ.toXml();
        xml->removeAttribute ("version");
        for (auto* d : xml->getChildWithTagNameIterator ("Division"))
            for (auto* st : d->getChildWithTagNameIterator ("Stop"))
                st->removeAttribute ("temporary");
        OrganEngine restored;
        restored.setSoundFont (sf);
        restored.fromXml (*xml);
        organ.removeTemporaryStops();
        restored.removeTemporaryStops();
        int restoredCount = 0;
        for (int d = 0; d < restored.getNumDivisions(); ++d)
            restoredCount += (int) restored.getDivision (d).stops.size();
        std::cout << "  stops: " << before << " before, " << during << " with the piece, " << count() << " after the next piece, "
                  << restoredCount << " after the next piece from old settings\n";
        return count() == before && restoredCount == before ? 0 : 5;
    }
    if (cmd == "export")
    {
        Formats::ExportContext ctx;
        ctx.piece = piece.get();
        ctx.partDivision = map;
        for (int d : map)
            ctx.partIsPedal.push_back (d >= 0 && d == organ.pedalIndex());
        ctx.liveOrgan = &organ;
        ctx.sampleRate = 44100;
        ctx.registrationTimeline = ScoreIO::registrationTimeline (*piece->score, map);
        std::vector<int> voices (64, 0);
        ctx.voiceStats = &voices;
        for (int i = 3; i < argc; ++i)
        {
            // --flat: no placement in the room (for comparing with other renderers)
            if (juce::String (argv[i]) == "--bank0")
                continue;
            if (juce::String (argv[i]) == "--flat")
            {
                organ.setSpatial (false);
                continue;
            }
            // --mark <seconds> <combination>: registration track entry, e.g. --mark 10 "0:1,0:4;"
            if (juce::String (argv[i]) == "--mark" && i + 2 < argc)
            {
                ctx.registrationMarks.push_back ({ juce::String (argv[i + 1]).getDoubleValue(), juce::String (argv[i + 2]) });
                i += 2;
                continue;
            }
            juce::File target = juce::File::getCurrentWorkingDirectory().getChildFile (argv[i]);
            auto ext = target.getFileExtension().trimCharactersAtStart (".").toLowerCase();
            bool found = false;
            for (auto& f : Formats::exportFormats())
                if (f.extension == ext)
                {
                    found = true;
                    const auto t1 = juce::Time::getMillisecondCounterHiRes();
                    juce::String err;
                    const bool ok = Formats::exportTo (f, target, ctx, err);
                    std::cout << "  export " << target.getFileName() << ": " << (ok ? "ok" : ("FAILED " + err)) << " ("
                              << (int) (juce::Time::getMillisecondCounterHiRes() - t1) << " ms)\n";
                    if (f.isAudio)
                    {
                        std::cout << "  most voices per 30 s:";
                        for (int v : voices)
                            if (v > 0) std::cout << " " << v;
                        std::cout << "\n";
                        std::fill (voices.begin(), voices.end(), 0);
                    }
                }
            if (! found)
                std::cout << "  unknown export extension " << ext << "\n";
        }
    }
    return 0;
}
