#include "Formats.h"
#include "../engine/I18n.h"
#include "BinaryData.h"
#include <set>
#include "NotationBuilder.h"
#include "Player.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace orgel::Formats
{
static const juce::StringArray midiExts { "mid", "midi", "kar", "smf", "rmi" };
static const juce::StringArray museScoreExts { "mscz", "mscx", "mscs", "gp", "gpx", "gp3", "gp4", "gp5", "gtp",
                                               "ptb", "cap", "capx", "ove", "scw", "bww", "mgu", "sgu", "bmw" };

static juce::String extOf (const juce::File& f)
{
    return f.getFileExtension().toLowerCase().trimCharactersAtStart (".");
}

bool isMidiFile (const juce::File& f) { return midiExts.contains (extOf (f)); }
bool needsMuseScore (const juce::File& f) { return museScoreExts.contains (extOf (f)); }

std::vector<std::pair<juce::String, juce::String>> importFormatList()
{
    return {
        { "MIDI", "*.mid;*.midi;*.kar;*.smf;*.rmi" },
        { "MusicXML", "*.musicxml;*.xml;*.mxl" },
        { "MEI", "*.mei" },
        { "ABC", "*.abc" },
        { "Humdrum (**kern)", "*.krn;*.hum;*.hmd" },
        { "Plaine & Easie", "*.pae" },
        { tr ("GABC (Gregorianik)"), "*.gabc" },
        { "MuseData", "*.md;*.msd" },
        { "EsAC", "*.esac" },
        { "CMME", "*.cmme" },
        { tr ("MuseScore / Guitar Pro / Capella / Overture / Bagpipe / Band-in-a-Box (über MuseScore)"),
          "*.mscz;*.mscx;*.mscs;*.gp;*.gpx;*.gp3;*.gp4;*.gp5;*.gtp;*.ptb;*.cap;*.capx;*.ove;*.scw;*.bww;*.mgu;*.sgu;*.bmw" },
    };
}

juce::String importWildcard()
{
    juce::StringArray all;
    for (auto& [desc, exts] : importFormatList())
        all.addTokens (exts, ";", "");
    return all.joinIntoString (";");
}

juce::File findMuseScore()
{
    juce::Array<juce::File> candidates;
#if JUCE_MAC
    for (auto* app : { "MuseScore 4.app", "MuseScore 3.app", "MuseScore Studio.app" })
    {
        candidates.add (juce::File ("/Applications").getChildFile (app).getChildFile ("Contents/MacOS/mscore"));
        candidates.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Applications").getChildFile (app).getChildFile ("Contents/MacOS/mscore"));
    }
#elif JUCE_WINDOWS
    for (auto base : { juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory),
                       juce::File::getSpecialLocation (juce::File::globalApplicationsDirectoryX86) })
    {
        candidates.add (base.getChildFile ("MuseScore 4/bin/MuseScore4.exe"));
        candidates.add (base.getChildFile ("MuseScore Studio 4/bin/MuseScore4.exe"));
        candidates.add (base.getChildFile ("MuseScore 3/bin/MuseScore3.exe"));
    }
#else
    for (auto* p : { "/usr/bin/mscore4portable", "/usr/bin/musescore4", "/usr/bin/mscore", "/usr/bin/musescore",
                     "/usr/local/bin/mscore", "/usr/local/bin/musescore", "/snap/bin/musescore" })
        candidates.add (juce::File (p));
    for (auto& f : juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                       .getChildFile ("Applications")
                       .findChildFiles (juce::File::findFiles, false, "MuseScore*.AppImage"))
        candidates.add (f);
#endif
    for (auto& c : candidates)
        if (c.existsAsFile())
            return c;
    return {};
}

static bool runMuseScore (const juce::File& in, const juce::File& out, juce::String& error)
{
    auto exe = findMuseScore();
    if (! exe.existsAsFile())
    {
        error = tr ("Für dieses Format wird MuseScore benötigt (kostenlos, musescore.org). Es wurde nicht gefunden.");
        return false;
    }
    juce::ChildProcess p;
    if (! p.start (juce::StringArray { exe.getFullPathName(), "-o", out.getFullPathName(), in.getFullPathName() },
                   juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        error = tr ("MuseScore konnte nicht gestartet werden.");
        return false;
    }
    auto output = p.readAllProcessOutput();
    p.waitForProcessToFinish (180000);
    if (! out.existsAsFile() || out.getSize() == 0)
    {
        error = tr ("MuseScore konnte die Datei nicht umwandeln.\n") + output.getLastCharacters (400);
        return false;
    }
    return true;
}

static NotationBuilder::Options builderOptions (const ExportContext& ctx);

//==============================================================================
// MuseScore export with the organ's sound: MuseScore plays each part through a SoundFont
// preset chosen in its mixer. The exported file names the SoundFont Orgelwerk plays (placed
// in MuseScore's SoundFonts folder if it is not there yet) and for each part the stop its
// division starts with.
static juce::File museScoreSoundFontFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("MuseScore4").getChildFile ("SoundFonts");
}

// The name MuseScore knows the SoundFont by (its file name without extension), after making
// sure a copy is in MuseScore's folder. Empty if that failed.
static juce::String soundFontForMuseScore (const SoundFont& sf)
{
    auto folder = museScoreSoundFontFolder();
    if (! folder.isDirectory() && ! folder.createDirectory())
        return {};
    const auto source = sf.getFile();
    const juce::String base = source.existsAsFile() ? source.getFileNameWithoutExtension() : juce::String ("Jeux14");
    for (auto& f : folder.findChildFiles (juce::File::findFiles, false))
        if (f.getFileNameWithoutExtension().equalsIgnoreCase (base)
            && (f.hasFileExtension ("sf2") || f.hasFileExtension ("sf3")))
            return base;
    auto target = folder.getChildFile (base + (source.existsAsFile() ? source.getFileExtension() : juce::String (".sf2")));
    const bool ok = source.existsAsFile() ? source.copyFileTo (target)
                                          : target.replaceWithData (BinaryData::Jeux14_sf2, (size_t) BinaryData::Jeux14_sf2Size);
    return ok ? base : juce::String();
}

// The stop a part is played with in MuseScore: the first one its division draws in the piece
// (or on the console), else a principal (manuals) or a pedal stop.
static RegStop presetForPart (const NotationBuilder::PartInfo& part, const ExportContext& ctx, const Score& score, const SoundFont& sf)
{
    int division = part.division;
    if (part.pedal || division < 0)
    {
        // the pedal, or all manuals on one staff: the division of that kind that plays most
        std::map<int, int> notesOn;
        for (auto& n : score.notes)
            if (n.part >= 0 && n.part < (int) ctx.partDivision.size() && ctx.partDivision[(size_t) n.part] >= 0
                && n.part < (int) ctx.partIsPedal.size() && ctx.partIsPedal[(size_t) n.part] == part.pedal)
                ++notesOn[ctx.partDivision[(size_t) n.part]];
        int most = -1;
        for (auto& [d, c] : notesOn)
            if (most < 0 || c > notesOn[most])
                most = d;
        division = most;
    }
    // in the pedal a real pedal stop (16', "(ped.)", Posaune ...) rather than a manual one it borrows
    auto pedalish = [&sf] (const RegStop& rs) {
        auto n = sf.presetName (rs.bank, rs.program);
        return n.containsIgnoreCase ("ped") || n.containsIgnoreCase ("Basse") || n.containsIgnoreCase ("Contra")
               || n.containsIgnoreCase ("Posaune") || n.containsIgnoreCase ("Subbass") || n.contains ("16") || n.contains ("32");
    };
    for (auto& e : ctx.registrationTimeline)
        if (e.division == division && ! e.registration.empty())
        {
            if (part.pedal)
                for (auto& rs : e.registration)
                    if (pedalish (rs))
                        return rs;
            return e.registration.front();
        }
    if (ctx.liveOrgan != nullptr && division >= 0)
    {
        const juce::ScopedLock sl (ctx.liveOrgan->getLock());
        if (division < ctx.liveOrgan->getNumDivisions())
            for (auto& st : ctx.liveOrgan->getDivision (division).stops)
                if (st.on)
                    return { st.bank, st.program, st.octave };
    }
    for (auto& p : sf.getPresets())
        if (part.pedal ? (p.name.containsIgnoreCase ("Subbass") || p.name.containsIgnoreCase ("(ped")) : p.name.containsIgnoreCase ("Montre"))
            return { p.bank, p.program, 0 };
    return { 0, 0, 0 };
}

// MuseScore plays one SoundFont preset per part, so a registration with several stops and
// its changes are rebuilt from hidden parts, one per stop a division uses: it holds that
// division's notes exactly while the stop is drawn (an octave stop moved by its octave).
// The visible parts are muted, as long as all their divisions are played by such layers.
struct MuseScoreLayers
{
    Score score;                       // the piece plus one part per (division, stop)
    std::vector<int> partDivision;     // the layers get divisions of their own, after the real ones
    std::vector<bool> partIsPedal;
    std::vector<juce::String> divisionNames;
    std::set<int> layerDivisions;
    std::map<int, RegStop> stopOfLayer; // layer division -> its stop
    std::set<int> realWithLayers;      // real divisions whose sound comes from layers
};

static MuseScoreLayers museScoreLayers (const Score& score, const ExportContext& ctx, const SoundFont& sf)
{
    MuseScoreLayers L;
    L.score = score;
    L.partDivision = ctx.partDivision;
    L.partIsPedal = ctx.partIsPedal;
    L.partDivision.resize (score.parts.size(), -1);
    L.partIsPedal.resize (score.parts.size(), false);
    int numDivisions = 0;
    for (int d : L.partDivision)
        numDivisions = juce::jmax (numDivisions, d + 1);
    if (ctx.liveOrgan != nullptr)
        numDivisions = juce::jmax (numDivisions, ctx.liveOrgan->getNumDivisions());
    L.divisionNames.resize ((size_t) numDivisions);
    if (ctx.liveOrgan != nullptr)
    {
        const juce::ScopedLock sl (ctx.liveOrgan->getLock());
        for (int d = 0; d < ctx.liveOrgan->getNumDivisions(); ++d)
            L.divisionNames[(size_t) d] = ctx.liveOrgan->getDivision (d).name;
    }
    std::set<int> used;
    for (auto& n : score.notes)
        if (n.part >= 0 && n.part < (int) L.partDivision.size() && L.partDivision[(size_t) n.part] >= 0
            && ! score.parts[(size_t) n.part].drums)
            used.insert (L.partDivision[(size_t) n.part]);

    int nextDivision = numDivisions;
    for (int d : used)
    {
        // the division's registration over time: (from beat, stops)
        std::vector<std::pair<double, Registration>> timeline;
        for (auto& e : ctx.registrationTimeline)
            if (e.division == d)
                timeline.push_back ({ e.beat, e.registration });
        if (timeline.empty() && ctx.liveOrgan != nullptr)
        {
            Registration drawn;
            const juce::ScopedLock sl (ctx.liveOrgan->getLock());
            if (d < ctx.liveOrgan->getNumDivisions())
                for (auto& st : ctx.liveOrgan->getDivision (d).stops)
                    if (st.on)
                        drawn.push_back ({ st.bank, st.program, st.octave });
            if (! drawn.empty())
                timeline.push_back ({ 0.0, drawn });
        }
        if (timeline.empty())
            continue;
        timeline.front().first = -1.0e9; // the first registration holds from the start
        std::vector<RegStop> stops;
        for (auto& [b, reg] : timeline)
            for (auto rs : reg)
                if (std::find (stops.begin(), stops.end(), rs) == stops.end())
                    stops.push_back (rs);
        const auto abbr = NotationBuilder::divisionAbbreviation (d < (int) L.divisionNames.size() ? L.divisionNames[(size_t) d] : juce::String(),
                                                                 d, d < (int) L.partDivision.size() && false);
        for (auto rs : stops)
        {
            const int layer = nextDivision++;
            const int partIndex = (int) L.score.parts.size();
            ScorePart part;
            part.name = abbr + " " + sf.presetName (rs.bank, rs.program);
            L.score.parts.push_back (part);
            L.partDivision.push_back (layer);
            L.partIsPedal.push_back (false);
            L.divisionNames.resize ((size_t) layer + 1);
            L.divisionNames[(size_t) layer] = part.name;
            L.layerDivisions.insert (layer);
            L.stopOfLayer[layer] = rs;
            // the stretches of time with this stop drawn
            std::vector<std::pair<double, double>> on;
            for (size_t k = 0; k < timeline.size(); ++k)
                if (std::find (timeline[k].second.begin(), timeline[k].second.end(), rs) != timeline[k].second.end())
                    on.push_back ({ timeline[k].first, k + 1 < timeline.size() ? timeline[k + 1].first : 1.0e18 });
            for (auto& n : score.notes)
            {
                if (n.part < 0 || n.part >= (int) ctx.partDivision.size() || ctx.partDivision[(size_t) n.part] != d)
                    continue;
                for (auto& [a, b] : on)
                {
                    const double s0 = juce::jmax (a, n.start), e0 = juce::jmin (b, n.start + n.length);
                    const int key = n.key + 12 * rs.octave;
                    if (e0 - s0 < 1.0e-3 || key < 0 || key > 127)
                        continue;
                    ScoreNote copy = n;
                    copy.start = s0;
                    copy.length = e0 - s0;
                    copy.key = key;
                    copy.part = partIndex;
                    L.score.notes.push_back (copy);
                }
            }
        }
        L.realWithLayers.insert (d);
    }
    L.score.finalise();
    return L;
}

// the score options for MuseScore: the layers are parts of their own (hidden later)
static NotationBuilder::Options museScoreOptions (const ExportContext& ctx, const MuseScoreLayers& layers)
{
    auto o = builderOptions (ctx);
    o.partDivision = layers.partDivision;
    o.partIsPedal = layers.partIsPedal;
    for (size_t d = 0; d < layers.divisionNames.size(); ++d)
        if (d >= o.divisionNames.size() || o.divisionNames[d].isEmpty())
        {
            o.divisionNames.resize (juce::jmax (o.divisionNames.size(), d + 1));
            o.divisionNames[d] = layers.divisionNames[d];
        }
    o.divisionIsPedal.resize (o.divisionNames.size(), false);
    o.ownPartDivisions = layers.layerDivisions;
    return o;
}

// Writes the SoundFont and the presets into the .mscz (its audiosettings.json), part by part,
// hides the layer parts and lets MuseScore leave out empty staves like Orgelwerk does.
static bool assignMuseScoreSound (const juce::File& mscz, const ExportContext& ctx, const Score& score, const MuseScoreLayers& layers)
{
    auto sf = ctx.liveOrgan != nullptr ? ctx.liveOrgan->getSoundFont() : SoundFont::loadBuiltIn();
    if (sf == nullptr)
        return false;
    const auto sfName = soundFontForMuseScore (*sf);
    if (sfName.isEmpty())
        return false;
    const auto layout = NotationBuilder::partLayout (layers.score, museScoreOptions (ctx, layers));

    juce::MemoryBlock zipData;
    if (! mscz.loadFileAsData (zipData))
        return false;
    juce::MemoryInputStream zipStream (zipData, false);
    juce::ZipFile zip (zipStream);
    // the parts in the score file, in order: (part id, instrument id)
    std::vector<std::pair<juce::String, juce::String>> parts;
    juce::var settings;
    juce::String scoreName, scoreText;
    for (int i = 0; i < zip.getNumEntries(); ++i)
    {
        auto* entry = zip.getEntry (i);
        std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (i));
        if (in == nullptr)
            continue;
        if (entry->filename.endsWithIgnoreCase (".mscx") && ! entry->filename.containsChar ('/'))
            scoreName = entry->filename, scoreText = in->readEntireStreamAsString();
        if (entry->filename == scoreName && scoreText.isNotEmpty())
            if (auto xml = juce::parseXML (scoreText))
                if (auto* sc = xml->getChildByName ("Score"))
                    for (auto* part : sc->getChildWithTagNameIterator ("Part"))
                        if (auto* inst = part->getChildByName ("Instrument"))
                            parts.push_back ({ part->getStringAttribute ("id"), inst->getStringAttribute ("id") });
        if (entry->filename == "audiosettings.json")
            settings = juce::JSON::parse (in->readEntireStreamAsString());
    }
    if (parts.empty())
        return false;
    if (! settings.isObject())
        settings = juce::var (new juce::DynamicObject());
    juce::Array<juce::var> tracks;
    std::set<juce::String> hiddenParts;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        // a hidden layer plays its stop; a visible part keeps a preset but is muted when layers play it
        const bool isLayer = i < layout.size() && layers.layerDivisions.count (layout[i].division) > 0;
        bool muted = false;
        if (i < layout.size() && ! isLayer)
        {
            muted = true;
            for (auto& n : score.notes)
                if (n.part >= 0 && n.part < (int) ctx.partDivision.size() && n.part < (int) ctx.partIsPedal.size()
                    && ctx.partIsPedal[(size_t) n.part] == layout[i].pedal
                    && (layout[i].division < 0 || ctx.partDivision[(size_t) n.part] == layout[i].division)
                    && ctx.partDivision[(size_t) n.part] >= 0 && ! layers.realWithLayers.count (ctx.partDivision[(size_t) n.part]))
                    muted = false;
        }
        const auto rs = isLayer ? layers.stopOfLayer.at (layout[i].division)
                                : (i < layout.size() ? presetForPart (layout[i], ctx, score, *sf) : RegStop { 0, 0, 0 });
        if (isLayer)
            hiddenParts.insert (parts[i].first);
        auto* attributes = new juce::DynamicObject();
        attributes->setProperty ("playbackSetupData", "keyboards.organ.pipe");
        attributes->setProperty ("soundFontName", sfName);
        attributes->setProperty ("presetName", sf->presetName (rs.bank, rs.program));
        attributes->setProperty ("presetBank", juce::String (rs.bank));
        attributes->setProperty ("presetProgram", juce::String (rs.program));
        auto* meta = new juce::DynamicObject();
        meta->setProperty ("attributes", juce::var (attributes));
        meta->setProperty ("hasNativeEditorSupport", false);
        meta->setProperty ("id", sfName + "\\" + juce::String (rs.bank) + "\\" + juce::String (rs.program));
        meta->setProperty ("type", "fluid_soundfont");
        meta->setProperty ("vendor", "Fluid");
        auto* input = new juce::DynamicObject();
        input->setProperty ("resourceMeta", juce::var (meta));
        input->setProperty ("unitConfiguration", juce::var (new juce::DynamicObject()));
        auto* output = new juce::DynamicObject();
        output->setProperty ("auxSends", juce::Array<juce::var>());
        output->setProperty ("balance", 0);
        output->setProperty ("fxChain", juce::var (new juce::DynamicObject()));
        output->setProperty ("volumeDb", 0);
        auto* soloMute = new juce::DynamicObject();
        soloMute->setProperty ("mute", muted);
        soloMute->setProperty ("solo", false);
        auto* track = new juce::DynamicObject();
        track->setProperty ("in", juce::var (input));
        track->setProperty ("instrumentId", parts[i].second);
        track->setProperty ("out", juce::var (output));
        track->setProperty ("partId", parts[i].first);
        track->setProperty ("soloMuteState", juce::var (soloMute));
        tracks.add (juce::var (track));
    }
    settings.getDynamicObject()->setProperty ("tracks", tracks);

    // the same archive with the new audiosettings.json
    juce::ZipFile::Builder builder;
    // the score file: layer parts hidden, empty staves left out (with a staff per manual)
    for (auto& id : hiddenParts)
    {
        const auto tag = "<Part id=\"" + id + "\">";
        const int at = scoreText.indexOf (tag);
        if (at >= 0)
            scoreText = scoreText.replaceSection (at + tag.length(), 0, "\n      <show>0</show>");
    }
    if (ctx.staffPerManual)
    {
        const juce::String style = "<hideEmptyStaves>1</hideEmptyStaves><dontHideStavesInFirstSystem>0</dontHideStavesInFirstSystem>";
        const int styleAt = scoreText.indexOf ("<Style>");
        const int scoreAt = scoreText.indexOf ("<Score>");
        if (styleAt >= 0)
            scoreText = scoreText.replaceSection (styleAt + 7, 0, style);
        else if (scoreAt >= 0)
            scoreText = scoreText.replaceSection (scoreAt + 7, 0, "<Style>" + style + "</Style>");
    }
    for (int i = 0; i < zip.getNumEntries(); ++i)
    {
        auto* entry = zip.getEntry (i);
        if (entry->filename == "audiosettings.json")
            continue;
        if (entry->filename == scoreName && scoreText.isNotEmpty())
        {
            builder.addEntry (new juce::MemoryInputStream (scoreText.toRawUTF8(), scoreText.getNumBytesAsUTF8(), true), 9, entry->filename, entry->fileTime);
            continue;
        }
        std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (i));
        if (in == nullptr)
            continue;
        juce::MemoryBlock data;
        in->readIntoMemoryBlock (data);
        builder.addEntry (new juce::MemoryInputStream (data, true), 9, entry->filename, entry->fileTime);
    }
    const auto json = juce::JSON::toString (settings);
    builder.addEntry (new juce::MemoryInputStream (json.toRawUTF8(), json.getNumBytesAsUTF8(), true), 9, "audiosettings.json", juce::Time::getCurrentTime());
    juce::TemporaryFile temp (mscz);
    {
        juce::FileOutputStream out (temp.getFile());
        if (! out.openedOk() || ! builder.writeToStream (out, nullptr))
            return false;
    }
    return temp.overwriteTargetFileWithTemporary();
}

OrganInfo OrganInfo::from (OrganEngine& organ, std::vector<DivisionRegistration> registration)
{
    OrganInfo info;
    {
        const juce::ScopedLock sl (organ.getLock());
        for (int d = 0; d < organ.getNumDivisions(); ++d)
        {
            info.divisionNames.push_back (organ.getDivision (d).name);
            info.divisionIsPedal.push_back (organ.getDivision (d).pedal);
        }
    }
    info.registration = std::move (registration);
    info.soundFont = organ.getSoundFont();
    return info;
}

static void applyOrganInfo (NotationBuilder::Options& opt, const OrganInfo& info)
{
    opt.divisionNames = info.divisionNames;
    opt.colourManuals = info.colourManuals;
    opt.staffPerManual = info.staffPerManual;
    opt.divisionIsPedal = info.divisionIsPedal;
    if (info.soundFont == nullptr)
        return;
    for (auto& e : info.registration)
    {
        NotationBuilder::Options::RegistrationMark m;
        m.beat = e.beat;
        m.division = e.division;
        for (auto rs : e.registration)
        {
            auto name = info.soundFont->presetName (rs.bank, rs.program);
            if (name.isNotEmpty())
                m.stops.addIfNotAlreadyThere (NotationBuilder::printedStopName (name, rs.octave));
        }
        opt.registrations.push_back (m);
    }
}

std::shared_ptr<Notation> engrave (const Score& score, const std::vector<bool>& partIsPedal, const std::vector<int>& partDivision,
                                   juce::String& error, const OrganInfo& organ)
{
    NotationBuilder::Options opt;
    opt.partIsPedal = partIsPedal;
    opt.partDivision = partDivision;
    applyOrganInfo (opt, organ);
    auto xml = NotationBuilder::toMusicXML (score, opt);
    auto n = std::make_shared<Notation>();
    n->setHideEmptyStaves (opt.staffPerManual);
    if (! n->loadData (xml, "musicxml", error))
        return {};
    return n;
}

std::shared_ptr<Notation> engraveContinuous (const Piece& piece, const std::vector<bool>& partIsPedal,
                                             const std::vector<int>& partDivision, juce::String& error, const OrganInfo& organ)
{
    juce::String data, format;
    if (piece.notationFromScore && piece.score != nullptr)
    {
        NotationBuilder::Options opt;
        opt.partIsPedal = partIsPedal;
        opt.partDivision = partDivision;
        applyOrganInfo (opt, organ);
        // each manual on its own staff here too; on one long line, a staff that is empty for the
        // whole piece is left out
        data = NotationBuilder::toMusicXML (*piece.score, opt);
        format = "musicxml";
        auto n = std::make_shared<Notation>();
        n->setContinuous (true);
        n->setHideEmptyStaves (opt.staffPerManual);
        if (! n->loadData (data, format, error))
            return {};
        return n;
    }
    else if (piece.notation != nullptr && piece.notation->getSourceData().isNotEmpty())
    {
        data = piece.notation->getSourceData();
        format = piece.notation->getSourceFormat();
    }
    else if (piece.source.existsAsFile())
    {
        auto n = std::make_shared<Notation>();
        n->setContinuous (true);
        return n->loadFile (piece.source, error) ? n : nullptr;
    }
    if (data.isEmpty())
        return {};
    auto n = std::make_shared<Notation>();
    n->setContinuous (true);
    if (! n->loadData (data, format, error))
        return {};
    return n;
}

std::unique_ptr<Piece> load (const juce::File& f, const SoundFont* sf, int numManuals, int pedalIndex, juce::String& error,
                             bool engraveMidi)
{
    auto piece = std::make_unique<Piece>();
    piece->source = f;

    if (isMidiFile (f))
    {
        piece->score = ScoreIO::loadMidiFile (f, sf, error);
        if (piece->score == nullptr)
            return {};
        auto map = ScoreIO::autoAssign (*piece->score, numManuals, pedalIndex);
        std::vector<bool> isPedal;
        for (int d : map)
            isPedal.push_back (d >= 0 && d == pedalIndex);
        piece->notationFromScore = true;
        if (! engraveMidi)
            return piece;
        juce::String engraveError;
        piece->notation = engrave (*piece->score, isPedal, map, engraveError);
        if (piece->notation == nullptr)
            piece->warning = tr ("Notenbild konnte nicht erzeugt werden: ") + engraveError;
        return piece;
    }

    juce::File notationFile = f;
    juce::TemporaryFile temp (".musicxml");
    if (needsMuseScore (f))
    {
        if (! runMuseScore (f, temp.getFile(), error))
            return {};
        notationFile = temp.getFile();
    }
    else if (! Notation::verovioExtensions().contains (extOf (f)))
    {
        error = tr ("Dieses Dateiformat wird nicht unterstützt: .") + extOf (f);
        return {};
    }

    piece->notation = std::make_shared<Notation>();
    if (! piece->notation->loadFile (notationFile, error))
        return {};
    auto midi = piece->notation->renderMidi();
    piece->score = ScoreIO::fromMidiData (midi.getData(), midi.getSize(), sf, error);
    if (piece->score == nullptr)
        return {};
    // Verovio's MIDI uses one track per staff; name the parts after the source file's title
    piece->score->title = piece->notation->getTitle().isNotEmpty() ? piece->notation->getTitle() : f.getFileNameWithoutExtension();
    if (piece->score->title == temp.getFile().getFileNameWithoutExtension())
        piece->score->title = f.getFileNameWithoutExtension();
    return piece;
}

//==============================================================================
std::vector<ExportFormat> exportFormats()
{
    return {
        { "mid", tr ("MIDI-Datei"), "mid" },
        { "musicxml", "MusicXML", "musicxml" },
        { "mxl", tr ("MusicXML komprimiert"), "mxl" },
        { "mei", "MEI", "mei" },
        { "krn", "Humdrum **kern", "krn" },
        { "abc", "ABC-Notation", "abc" },
        { "ly", "LilyPond", "ly" },
        { "pdf", tr ("PDF (Notenseiten)"), "pdf" },
        { "svg", tr ("SVG (Notenseiten)"), "svg" },
        { "png", tr ("PNG (Notenseiten)"), "png" },
        { "mscz", tr ("MuseScore (über MuseScore)"), "mscz", true },
        { "wav", tr ("WAV-Audio (Orgelklang)"), "wav", false, true },
        { "aiff", tr ("AIFF-Audio (Orgelklang)"), "aiff", false, true },
        { "flac", tr ("FLAC-Audio (Orgelklang)"), "flac", false, true },
        { "ogg", tr ("Ogg-Vorbis-Audio (Orgelklang)"), "ogg", false, true },
    };
}

static OrganInfo organInfoOf (const ExportContext& ctx)
{
    auto info = ctx.liveOrgan != nullptr ? OrganInfo::from (*ctx.liveOrgan, ctx.registrationTimeline) : OrganInfo {};
    info.colourManuals = ctx.colourManuals;
    info.staffPerManual = ctx.staffPerManual;
    return info;
}

static NotationBuilder::Options builderOptions (const ExportContext& ctx)
{
    NotationBuilder::Options o;
    o.partIsPedal = ctx.partIsPedal;
    o.partDivision = ctx.partDivision;
    applyOrganInfo (o, organInfoOf (ctx));
    return o;
}

static std::vector<juce::Image> renderPages (const Notation& n, float pixelsPerUnit)
{
    std::vector<juce::Image> images;
    for (int p = 0; p < n.getPageCount(); ++p)
    {
        auto drawable = juce::Drawable::createFromSVG (*juce::parseXML (n.getPageSVG (p)));
        if (drawable == nullptr)
            continue;
        auto bounds = drawable->getDrawableBounds();
        const int w = juce::roundToInt (2100 * pixelsPerUnit), h = juce::roundToInt (2970 * pixelsPerUnit);
        juce::Image img (juce::Image::RGB, w, h, true);
        juce::Graphics g (img);
        g.fillAll (juce::Colours::white);
        juce::ignoreUnused (bounds);
        drawable->drawWithin (g, img.getBounds().toFloat(), juce::RectanglePlacement::centred, 1.0f);
        images.push_back (img);
    }
    return images;
}

static bool writePdf (const std::vector<juce::Image>& pages, const juce::File& target, juce::String& error)
{
    target.deleteFile();
    juce::FileOutputStream out (target);
    if (! out.openedOk())
    {
        error = tr ("Datei kann nicht geschrieben werden.");
        return false;
    }
    std::vector<juce::int64> offsets;
    auto beginObj = [&] (int id) {
        if ((int) offsets.size() < id) offsets.resize ((size_t) id);
        offsets[(size_t) id - 1] = out.getPosition();
        out << id << " 0 obj\n";
    };
    out << "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n";
    const int n = (int) pages.size();
    beginObj (1);
    out << "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    beginObj (2);
    out << "<< /Type /Pages /Count " << n << " /Kids [";
    for (int i = 0; i < n; ++i)
        out << (3 + i * 3) << " 0 R ";
    out << "] >>\nendobj\n";

    for (int i = 0; i < n; ++i)
    {
        const int pageId = 3 + i * 3, contentId = pageId + 1, imageId = pageId + 2;
        auto& img = pages[(size_t) i];
        beginObj (pageId);
        out << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595.28 841.89] /Resources << /XObject << /Im0 " << imageId
            << " 0 R >> >> /Contents " << contentId << " 0 R >>\nendobj\n";
        juce::String content = "q 595.28 0 0 841.89 0 0 cm /Im0 Do Q\n";
        beginObj (contentId);
        out << "<< /Length " << (int) content.length() << " >>\nstream\n" << content << "endstream\nendobj\n";

        juce::MemoryOutputStream raw;
        {
            juce::Image::BitmapData bd (img, juce::Image::BitmapData::readOnly);
            juce::GZIPCompressorOutputStream z (raw, 9);
            juce::HeapBlock<uint8_t> row ((size_t) img.getWidth());
            for (int y = 0; y < img.getHeight(); ++y)
            {
                for (int x = 0; x < img.getWidth(); ++x)
                    row[x] = (uint8_t) juce::jlimit (0, 255, juce::roundToInt (bd.getPixelColour (x, y).getBrightness() * 255.0f));
                z.write (row, (size_t) img.getWidth());
            }
        }
        beginObj (imageId);
        out << "<< /Type /XObject /Subtype /Image /Width " << img.getWidth() << " /Height " << img.getHeight()
            << " /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode /Length " << (int) raw.getDataSize() << " >>\nstream\n";
        out.write (raw.getData(), raw.getDataSize());
        out << "\nendstream\nendobj\n";
    }
    const auto xref = out.getPosition();
    out << "xref\n0 " << (int) offsets.size() + 1 << "\n0000000000 65535 f \n";
    for (auto o : offsets)
        out << juce::String (o).paddedLeft ('0', 10) << " 00000 n \n";
    out << "trailer\n<< /Size " << (int) offsets.size() + 1 << " /Root 1 0 R >>\nstartxref\n" << (juce::int64) xref << "\n%%EOF\n";
    return true;
}

static juce::File numbered (const juce::File& target, int page, int count)
{
    if (count <= 1)
        return target;
    return target.getSiblingFile (target.getFileNameWithoutExtension() + "-" + juce::String (page + 1) + target.getFileExtension());
}

bool renderAudio (const ExportContext& ctx, juce::AudioFormatWriter& writer, juce::String& error)
{
    if (ctx.piece == nullptr || ctx.piece->score == nullptr || ctx.liveOrgan == nullptr)
    {
        error = tr ("Kein Stück geladen.");
        return false;
    }
    OrganEngine organ;
    organ.copySetupFrom (*ctx.liveOrgan);
    const int block = 512;
    organ.prepare (ctx.sampleRate, block);
    // the reverb's impulse response loads in the background
    for (int i = 0; i < 300 && ! organ.isReverbReady(); ++i)
        juce::Thread::sleep (10);

    Player player;
    player.setScore (ctx.piece->score, ctx.partDivision);
    player.speed = ctx.speed;
    std::vector<double> markTimes;
    std::vector<OrganEngine::Combination> markCombos;
    for (auto& [t, combo] : ctx.registrationMarks)
    {
        markTimes.push_back (t);
        markCombos.push_back (organ.parseCombination (combo));
    }
    player.setMarkTimes (markTimes);
    std::vector<double> timelineTimes;
    for (auto& tr : ctx.registrationTimeline)
        timelineTimes.push_back (ctx.piece->score->beatToSeconds (tr.beat));
    player.setTimelineTimes (timelineTimes);
    // registration at the start: the first entry of every division
    {
        std::set<int> done;
        for (auto& tr : ctx.registrationTimeline)
            if (done.insert (tr.division).second)
                organ.applyRegistration (tr.division, tr.registration);
    }
    // the registration in effect at the start of the piece
    for (size_t i = 0; i < markTimes.size(); ++i)
        if (markTimes[i] <= 0.0)
            organ.applyCombinationLocked (markCombos[i]);
    player.play();

    juce::AudioBuffer<float> buffer (2, block);
    std::vector<KeyEvent> events;
    std::vector<int> regs;
    const double total = player.getLength() / juce::jmax (0.1f, ctx.speed) + 4.0;
    double rendered = 0;
    int tailBlocks = (int) (4.0 * ctx.sampleRate / block);

    while (player.isPlaying() || tailBlocks-- > 0)
    {
        events.clear();
        regs.clear();
        player.process (block, ctx.sampleRate, events, regs);
        for (int r : regs)
        {
            if (r <= -2)
            {
                const juce::ScopedLock sl (organ.getLock());
                if (-r - 2 < (int) markCombos.size())
                    organ.applyCombinationLocked (markCombos[(size_t) (-r - 2)]);
                continue;
            }
            if (r < (int) ctx.registrationTimeline.size())
                organ.applyRegistration (ctx.registrationTimeline[(size_t) r].division, ctx.registrationTimeline[(size_t) r].registration);
        }
        std::stable_sort (events.begin(), events.end(), [] (auto& a, auto& b) { return a.sample < b.sample; });
        {
            const juce::ScopedLock sl (organ.getLock());
            organ.renderLocked (buffer, events);
        }
        if (! player.isPlaying() && player.needsRelease.exchange (false))
            organ.releaseSource (srcPlayer);
        if (! writer.writeFromAudioSampleBuffer (buffer, 0, block))
        {
            error = tr ("Fehler beim Schreiben der Audiodatei.");
            return false;
        }
        rendered += block / ctx.sampleRate;
        if (ctx.voiceStats != nullptr)
        {
            auto& slot = (*ctx.voiceStats)[(size_t) juce::jlimit (0, (int) ctx.voiceStats->size() - 1, (int) (rendered / 30.0))];
            slot = juce::jmax (slot, organ.activeVoiceCount());
        }
        if (ctx.progress && ! ctx.progress ((float) juce::jmin (1.0, rendered / total)))
        {
            error = tr ("Abgebrochen.");
            return false;
        }
    }
    return true;
}

bool exportTo (const ExportFormat& fmt, const juce::File& target, const ExportContext& ctx, juce::String& error)
{
    if (ctx.piece == nullptr || ctx.piece->score == nullptr)
    {
        error = tr ("Kein Stück geladen.");
        return false;
    }
    auto& score = *ctx.piece->score;
    auto notation = ctx.piece->notation;
    // pages not set yet (MIDI file, not looked at): set them now for formats that need them
    if (notation == nullptr && ctx.piece->notationFromScore
        && (fmt.id == "mei" || fmt.id == "svg" || fmt.id == "png" || fmt.id == "pdf"))
    {
        juce::String engraveError;
        notation = engrave (score, ctx.partIsPedal, ctx.partDivision, engraveError, organInfoOf (ctx));
    }
    auto writeText = [&] (const juce::String& text) {
        if (text.isEmpty())
        {
            error = tr ("Export ergab keine Daten.");
            return false;
        }
        if (! target.replaceWithText (text, false, false, "\n"))
        {
            error = tr ("Datei kann nicht geschrieben werden: ") + target.getFullPathName();
            return false;
        }
        return true;
    };
    auto needNotation = [&] {
        if (notation == nullptr || ! notation->isLoaded())
        {
            error = tr ("Für dieses Format wird ein Notenbild benötigt, das nicht erzeugt werden konnte.");
            return false;
        }
        return true;
    };

    if (fmt.id == "mid")
    {
        // the stops currently drawn on each part's division become the file's registration
        std::vector<Registration> regs;
        for (int d : ctx.partDivision)
        {
            Registration r;
            if (ctx.liveOrgan != nullptr && d >= 0 && d < ctx.liveOrgan->getNumDivisions())
            {
                const juce::ScopedLock sl (ctx.liveOrgan->getLock());
                for (auto& st : ctx.liveOrgan->getDivision (d).stops)
                    if (st.on)
                        r.push_back ({ st.bank, st.program, st.octave });
            }
            regs.push_back (r);
        }
        if (! ScoreIO::saveMidiFile (score, target, ctx.partNames, regs))
        {
            error = tr ("MIDI-Datei konnte nicht geschrieben werden.");
            return false;
        }
        return true;
    }
    if (fmt.id == "musicxml")
        return writeText (NotationBuilder::toMusicXML (score, builderOptions (ctx)));
    if (fmt.id == "abc")
        return writeText (NotationBuilder::toABC (score, builderOptions (ctx)));
    if (fmt.id == "ly")
        return writeText (NotationBuilder::toLilyPond (score, builderOptions (ctx)));
    if (fmt.id == "mxl")
    {
        auto xml = NotationBuilder::toMusicXML (score, builderOptions (ctx));
        juce::ZipFile::Builder zip;
        zip.addEntry (new juce::MemoryInputStream (juce::MemoryBlock ("application/vnd.recordare.musicxml", 34), true), 0, "mimetype", {});
        const char* container = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<container><rootfiles><rootfile full-path=\"score.musicxml\" "
                                "media-type=\"application/vnd.recordare.musicxml+xml\"/></rootfiles></container>\n";
        zip.addEntry (new juce::MemoryInputStream (juce::MemoryBlock (container, strlen (container)), true), 9, "META-INF/container.xml", {});
        auto utf8 = xml.toUTF8();
        zip.addEntry (new juce::MemoryInputStream (juce::MemoryBlock (utf8.getAddress(), utf8.sizeInBytes() - 1), true), 9, "score.musicxml", {});
        target.deleteFile();
        juce::FileOutputStream out (target);
        if (! out.openedOk() || ! zip.writeToStream (out, nullptr))
        {
            error = tr ("MXL-Datei konnte nicht geschrieben werden.");
            return false;
        }
        return true;
    }
    if (fmt.id == "mei")
        return needNotation() && writeText (notation->getMEI());
    if (fmt.id == "krn")
        return writeText (Notation::musicXmlToHumdrum (NotationBuilder::toMusicXML (score, builderOptions (ctx))));
    if (fmt.id == "svg")
    {
        if (! needNotation())
            return false;
        for (int p = 0; p < notation->getPageCount(); ++p)
            if (! numbered (target, p, notation->getPageCount()).replaceWithText (notation->getRawPageSVG (p)))
            {
                error = tr ("SVG konnte nicht geschrieben werden.");
                return false;
            }
        return true;
    }
    if (fmt.id == "png" || fmt.id == "pdf")
    {
        if (! needNotation())
            return false;
        auto pages = renderPages (*notation, fmt.id == "pdf" ? 0.95f : 0.75f); // ~240 dpi for PDF
        if (pages.empty())
        {
            error = tr ("Seiten konnten nicht gezeichnet werden.");
            return false;
        }
        if (fmt.id == "pdf")
            return writePdf (pages, target, error);
        juce::PNGImageFormat png;
        for (int p = 0; p < (int) pages.size(); ++p)
        {
            auto file = numbered (target, p, (int) pages.size());
            file.deleteFile();
            juce::FileOutputStream out (file);
            if (! out.openedOk() || ! png.writeImageToStream (pages[(size_t) p], out))
            {
                error = tr ("PNG konnte nicht geschrieben werden.");
                return false;
            }
        }
        return true;
    }
    if (fmt.id == "mscz")
    {
        // the score with hidden layer parts that play the registration (see museScoreLayers)
        auto sf = ctx.liveOrgan != nullptr ? ctx.liveOrgan->getSoundFont() : SoundFont::loadBuiltIn();
        const auto layers = museScoreLayers (score, ctx, *sf);
        juce::TemporaryFile temp (".musicxml");
        if (! temp.getFile().replaceWithText (NotationBuilder::toMusicXML (layers.score, museScoreOptions (ctx, layers))))
        {
            error = tr ("Zwischendatei konnte nicht geschrieben werden.");
            return false;
        }
        if (std::getenv ("ORGEL_KEEP_MUSESCORE_XML") != nullptr)
            temp.getFile().copyFileTo (target.withFileExtension ("musicxml"));
        target.deleteFile();
        if (! runMuseScore (temp.getFile(), target, error))
            return false;
        // the organ's SoundFont and stops for MuseScore's playback (the score is fine without)
        assignMuseScoreSound (target, ctx, score, layers);
        return true;
    }
    if (fmt.isAudio)
    {
        std::unique_ptr<juce::AudioFormat> af;
        if (fmt.id == "wav") af = std::make_unique<juce::WavAudioFormat>();
        else if (fmt.id == "aiff") af = std::make_unique<juce::AiffAudioFormat>();
        else if (fmt.id == "flac") af = std::make_unique<juce::FlacAudioFormat>();
        else af = std::make_unique<juce::OggVorbisAudioFormat>();

        target.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (target);
        if (! static_cast<juce::FileOutputStream*> (stream.get())->openedOk())
        {
            error = tr ("Datei kann nicht geschrieben werden.");
            return false;
        }
        auto options = juce::AudioFormatWriterOptions {}
                           .withSampleRate (ctx.sampleRate)
                           .withNumChannels (2)
                           .withBitsPerSample (fmt.id == "ogg" ? 16 : (fmt.id == "flac" ? juce::jmin (24, ctx.bitDepth) : ctx.bitDepth));
        if (fmt.id == "ogg")
            options = options.withQualityOptionIndex (8);
        auto writer = af->createWriterFor (stream, options);
        if (writer == nullptr)
        {
            error = tr ("Audioformat wird mit diesen Einstellungen nicht unterstützt.");
            return false;
        }
        const bool ok = renderAudio (ctx, *writer, error);
        writer.reset();
        if (! ok)
            target.deleteFile();
        return ok;
    }
    error = tr ("Unbekanntes Exportformat.");
    return false;
}
} // namespace orgel::Formats
