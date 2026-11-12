#include "Formats.h"
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
        { "GABC (Gregorianik)", "*.gabc" },
        { "MuseData", "*.md;*.msd" },
        { "EsAC", "*.esac" },
        { "CMME", "*.cmme" },
        { juce::String::fromUTF8 ("MuseScore / Guitar Pro / Capella / Overture / Bagpipe / Band-in-a-Box (über MuseScore)"),
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
        error = juce::String::fromUTF8 ("Für dieses Format wird MuseScore benötigt (kostenlos, musescore.org). Es wurde nicht gefunden.");
        return false;
    }
    juce::ChildProcess p;
    if (! p.start (juce::StringArray { exe.getFullPathName(), "-o", out.getFullPathName(), in.getFullPathName() },
                   juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        error = "MuseScore konnte nicht gestartet werden.";
        return false;
    }
    auto output = p.readAllProcessOutput();
    p.waitForProcessToFinish (180000);
    if (! out.existsAsFile() || out.getSize() == 0)
    {
        error = "MuseScore konnte die Datei nicht umwandeln.\n" + output.getLastCharacters (400);
        return false;
    }
    return true;
}

std::shared_ptr<Notation> engrave (const Score& score, const std::vector<bool>& partIsPedal, const std::vector<int>& partDivision,
                                   juce::String& error)
{
    NotationBuilder::Options opt;
    opt.partIsPedal = partIsPedal;
    opt.partDivision = partDivision;
    auto xml = NotationBuilder::toMusicXML (score, opt);
    auto n = std::make_shared<Notation>();
    if (! n->loadData (xml, "musicxml", error))
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
            piece->warning = "Notenbild konnte nicht erzeugt werden: " + engraveError;
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
        error = juce::String::fromUTF8 ("Dieses Dateiformat wird nicht unterstützt: .") + extOf (f);
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
        { "mid", "MIDI-Datei", "mid" },
        { "musicxml", "MusicXML", "musicxml" },
        { "mxl", "MusicXML komprimiert", "mxl" },
        { "mei", "MEI", "mei" },
        { "krn", "Humdrum **kern", "krn" },
        { "abc", "ABC-Notation", "abc" },
        { "ly", "LilyPond", "ly" },
        { "pdf", "PDF (Notenseiten)", "pdf" },
        { "svg", "SVG (Notenseiten)", "svg" },
        { "png", "PNG (Notenseiten)", "png" },
        { "mscz", juce::String::fromUTF8 ("MuseScore (über MuseScore)"), "mscz", true },
        { "wav", "WAV-Audio (Orgelklang)", "wav", false, true },
        { "aiff", "AIFF-Audio (Orgelklang)", "aiff", false, true },
        { "flac", "FLAC-Audio (Orgelklang)", "flac", false, true },
        { "ogg", "Ogg-Vorbis-Audio (Orgelklang)", "ogg", false, true },
    };
}

static NotationBuilder::Options builderOptions (const ExportContext& ctx)
{
    NotationBuilder::Options o;
    o.partIsPedal = ctx.partIsPedal;
    o.partDivision = ctx.partDivision;
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
        error = "Datei kann nicht geschrieben werden.";
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
        error = juce::String::fromUTF8 ("Kein Stück geladen.");
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
            error = "Fehler beim Schreiben der Audiodatei.";
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
            error = "Abgebrochen.";
            return false;
        }
    }
    return true;
}

bool exportTo (const ExportFormat& fmt, const juce::File& target, const ExportContext& ctx, juce::String& error)
{
    if (ctx.piece == nullptr || ctx.piece->score == nullptr)
    {
        error = juce::String::fromUTF8 ("Kein Stück geladen.");
        return false;
    }
    auto& score = *ctx.piece->score;
    auto notation = ctx.piece->notation;
    // pages not set yet (MIDI file, not looked at): set them now for formats that need them
    if (notation == nullptr && ctx.piece->notationFromScore
        && (fmt.id == "mei" || fmt.id == "svg" || fmt.id == "png" || fmt.id == "pdf"))
    {
        juce::String engraveError;
        notation = engrave (score, ctx.partIsPedal, ctx.partDivision, engraveError);
    }
    auto writeText = [&] (const juce::String& text) {
        if (text.isEmpty())
        {
            error = "Export ergab keine Daten.";
            return false;
        }
        if (! target.replaceWithText (text, false, false, "\n"))
        {
            error = "Datei kann nicht geschrieben werden: " + target.getFullPathName();
            return false;
        }
        return true;
    };
    auto needNotation = [&] {
        if (notation == nullptr || ! notation->isLoaded())
        {
            error = juce::String::fromUTF8 ("Für dieses Format wird ein Notenbild benötigt, das nicht erzeugt werden konnte.");
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
            error = "MIDI-Datei konnte nicht geschrieben werden.";
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
            error = "MXL-Datei konnte nicht geschrieben werden.";
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
                error = "SVG konnte nicht geschrieben werden.";
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
            error = "Seiten konnten nicht gezeichnet werden.";
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
                error = "PNG konnte nicht geschrieben werden.";
                return false;
            }
        }
        return true;
    }
    if (fmt.id == "mscz")
    {
        juce::TemporaryFile temp (".musicxml");
        if (! temp.getFile().replaceWithText (NotationBuilder::toMusicXML (score, builderOptions (ctx))))
        {
            error = "Zwischendatei konnte nicht geschrieben werden.";
            return false;
        }
        target.deleteFile();
        return runMuseScore (temp.getFile(), target, error);
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
            error = "Datei kann nicht geschrieben werden.";
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
            error = juce::String::fromUTF8 ("Audioformat wird mit diesen Einstellungen nicht unterstützt.");
            return false;
        }
        const bool ok = renderAudio (ctx, *writer, error);
        writer.reset();
        if (! ok)
            target.deleteFile();
        return ok;
    }
    error = "Unbekanntes Exportformat.";
    return false;
}
} // namespace orgel::Formats
