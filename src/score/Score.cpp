#include "Score.h"
#include "../engine/SoundFont.h"
#include <map>
#include <set>

namespace orgel
{
void Score::finalise()
{
    std::sort (notes.begin(), notes.end(), [] (const ScoreNote& a, const ScoreNote& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.part != b.part) return a.part < b.part;
        return a.key > b.key;
    });
    std::stable_sort (tempos.begin(), tempos.end(), [] (auto& a, auto& b) { return a.beat < b.beat; });
    std::stable_sort (timeSigs.begin(), timeSigs.end(), [] (auto& a, auto& b) { return a.beat < b.beat; });
    std::stable_sort (keySigs.begin(), keySigs.end(), [] (auto& a, auto& b) { return a.beat < b.beat; });
    std::stable_sort (registrationChanges.begin(), registrationChanges.end(), [] (auto& a, auto& b) { return a.beat < b.beat; });

    if (tempos.empty() || tempos.front().beat > 0)
        tempos.insert (tempos.begin(), TempoChange { 0, tempos.empty() ? 120.0 : tempos.front().bpm });
    if (timeSigs.empty() || timeSigs.front().beat > 0)
        timeSigs.insert (timeSigs.begin(), TimeSignature {});
    if (keySigs.empty() || keySigs.front().beat > 0)
        keySigs.insert (keySigs.begin(), KeySignature {});

    for (auto& p : parts)
    {
        p.lowest = 127;
        p.highest = 0;
        p.count = 0;
        p.meanPitch = 0;
    }
    for (auto& n : notes)
    {
        if (n.part < 0 || n.part >= (int) parts.size())
            continue;
        auto& p = parts[(size_t) n.part];
        p.lowest = juce::jmin (p.lowest, n.key);
        p.highest = juce::jmax (p.highest, n.key);
        p.count++;
        p.meanPitch += n.key;
    }
    for (auto& p : parts)
        p.meanPitch = p.count > 0 ? p.meanPitch / p.count : 60.0;
}

double Score::lengthBeats() const
{
    double end = 0;
    for (auto& n : notes)
        end = juce::jmax (end, n.start + n.length);
    return end;
}

double Score::beatToSeconds (double beat) const
{
    double t = 0, b = 0, bpm = tempos.empty() ? 120.0 : tempos.front().bpm;
    for (auto& tc : tempos)
    {
        if (tc.beat >= beat)
            break;
        t += (tc.beat - b) * 60.0 / bpm;
        b = tc.beat;
        bpm = tc.bpm;
    }
    return t + (beat - b) * 60.0 / bpm;
}

double Score::secondsToBeat (double seconds) const
{
    double t = 0, b = 0, bpm = tempos.empty() ? 120.0 : tempos.front().bpm;
    for (auto& tc : tempos)
    {
        const double tNext = t + (tc.beat - b) * 60.0 / bpm;
        if (tNext >= seconds)
            break;
        t = tNext;
        b = tc.beat;
        bpm = tc.bpm;
    }
    return b + (seconds - t) * bpm / 60.0;
}

TimeSignature Score::timeSigAt (double beat) const
{
    TimeSignature r;
    for (auto& ts : timeSigs)
        if (ts.beat <= beat + 1.0e-6)
            r = ts;
    return r;
}

KeySignature Score::keySigAt (double beat) const
{
    KeySignature r;
    for (auto& ks : keySigs)
        if (ks.beat <= beat + 1.0e-6)
            r = ks;
    return r;
}

int Score::measureAt (double beat) const
{
    int measure = 1;
    double pos = 0;
    for (size_t i = 0; i < timeSigs.size() || i == 0; ++i)
    {
        const TimeSignature ts = timeSigs.empty() ? TimeSignature {} : timeSigs[i];
        const double len = juce::jmax (0.25, ts.num * 4.0 / juce::jmax (1, ts.den));
        const double end = i + 1 < timeSigs.size() ? timeSigs[i + 1].beat : 1.0e12;
        while (pos + len <= beat + 1.0e-6 && pos + len <= end + 1.0e-6)
        {
            pos += len;
            ++measure;
        }
        if (beat < end)
            break;
        pos = end;
    }
    return measure;
}

//==============================================================================
namespace ScoreIO
{
    namespace
    {
        struct Stream
        {
            int track = 0, channel = 0;
            std::vector<ScoreNote> notes;
            // program state over time: (beat, bank, program)
            std::vector<std::tuple<double, int, int>> programs;
            juce::String trackName;
        };

        Registration programsAt (const Stream& s, double beat)
        {
            int bank = -1, prog = -1;
            for (auto& [b, bk, pr] : s.programs)
                if (b <= beat + 1.0e-6)
                {
                    bank = bk;
                    prog = pr;
                }
            if (prog < 0)
                return {};
            return { RegStop { juce::jmax (0, bank), prog, 0 } };
        }
    } // namespace

    // Many MIDI files declare C major (or nothing) whatever the piece is in. If the notes
    // clearly suggest another key, use that for spelling and the key signature.
    void estimateKeyIfMissing (Score& score)
    {
        if (score.keySigs.size() > 1 || (! score.keySigs.empty() && (score.keySigs[0].fifths != 0 || score.keySigs[0].minor)))
            return;
        double w[12] = {};
        for (auto& n : score.notes)
            if (n.part >= 0 && n.part < (int) score.parts.size() && ! score.parts[(size_t) n.part].drums)
                w[n.key % 12] += n.length;
        static const int majorScale[7] = { 0, 2, 4, 5, 7, 9, 11 };
        auto outside = [&] (int fifths) {
            const int tonic = ((fifths * 7) % 12 + 12) % 12;
            double inScale = 0, total = 0;
            for (int pc = 0; pc < 12; ++pc)
                total += w[pc];
            for (int d : majorScale)
                inScale += w[(tonic + d) % 12];
            inScale += w[(tonic + 8) % 12]; // leading tone of the relative minor
            return total - inScale;
        };
        int best = 0;
        for (int f = -6; f <= 6; ++f)
            if (outside (f) < outside (best) - 1.0e-9)
                best = f;
        if (best == 0 || outside (best) > 0.75 * outside (0))
            return;
        const int majorTonic = ((best * 7) % 12 + 12) % 12;
        const int minorTonic = (majorTonic + 9) % 12;
        KeySignature ks;
        ks.fifths = best;
        ks.minor = w[minorTonic] > w[majorTonic];
        score.keySigs = { ks };
    }

    std::shared_ptr<Score> fromMidi (const juce::MidiFile& midiIn, const SoundFont* sf)
    {
        juce::MidiFile midi (midiIn);
        double ticksPerBeat = midi.getTimeFormat();
        if (ticksPerBeat <= 0)
        {
            // SMPTE time: work in seconds at a nominal 120 bpm
            midi.convertTimestampTicksToSeconds();
            ticksPerBeat = 0.5;
        }

        auto score = std::make_shared<Score>();
        std::map<std::pair<int, int>, Stream> streams;
        bool usesBankSelect = false;
        juce::StringArray trackNames;

        for (int t = 0; t < midi.getNumTracks(); ++t)
        {
            auto* seq = midi.getTrack (t);
            juce::String trackName;
            int bankMsb[16] = {}, bankLsb[16] = {};
            std::array<std::vector<std::pair<double, int>>, 128 * 16> open;

            for (auto* ev : *seq)
            {
                auto& m = ev->message;
                const double beat = m.getTimeStamp() / ticksPerBeat;

                if (m.isMetaEvent())
                {
                    if (m.isTempoMetaEvent())
                        score->tempos.push_back ({ beat, 60.0 / juce::jmax (1.0e-6, m.getTempoSecondsPerQuarterNote()) });
                    else if (m.isTimeSignatureMetaEvent())
                    {
                        int num = 4, den = 4;
                        m.getTimeSignatureInfo (num, den);
                        score->timeSigs.push_back ({ beat, num, den });
                    }
                    else if (m.isKeySignatureMetaEvent())
                        score->keySigs.push_back ({ beat, m.getKeySignatureNumberOfSharpsOrFlats(), ! m.isKeySignatureMajorKey() });
                    else if (m.isTrackNameEvent() && trackName.isEmpty())
                        trackName = m.getTextFromTextMetaEvent().trim();
                    continue;
                }

                const int ch = m.getChannel() - 1;
                if (ch < 0 || ch > 15)
                    continue;
                auto& s = streams[{ t, ch }];
                s.track = t;
                s.channel = ch;

                if (m.isController())
                {
                    if (m.getControllerNumber() == 0 || m.getControllerNumber() == 32)
                        usesBankSelect = true;
                    if (m.getControllerNumber() == 0)
                        bankMsb[ch] = m.getControllerValue();
                    else if (m.getControllerNumber() == 32)
                        bankLsb[ch] = m.getControllerValue();
                }
                else if (m.isProgramChange())
                {
                    int bank = bankMsb[ch] != 0 ? bankMsb[ch] : bankLsb[ch];
                    s.programs.emplace_back (beat, bank, m.getProgramChangeNumber());
                }
                else if (m.isNoteOn())
                    open[(size_t) (ch * 128 + m.getNoteNumber())].push_back ({ beat, m.getVelocity() });
                else if (m.isNoteOff())
                {
                    auto& stack = open[(size_t) (ch * 128 + m.getNoteNumber())];
                    if (! stack.empty())
                    {
                        auto [start, vel] = stack.front();
                        stack.erase (stack.begin());
                        ScoreNote n;
                        n.start = start;
                        n.length = juce::jmax (1.0 / 64.0, beat - start);
                        n.key = m.getNoteNumber();
                        n.velocity = vel;
                        s.notes.push_back (n);
                    }
                }
            }
            for (auto& [key, s] : streams)
                if (s.track == t)
                    s.trackName = trackName;
            trackNames.add (trackName);
        }

        if (midi.getNumTracks() > 0)
            score->title = trackNames[0];

        // Organ files often double one line on several channels, one channel per stop.
        // Merge streams with identical notes into one part with a combined registration.
        std::vector<Stream*> order;
        for (auto& [key, s] : streams)
            if (! s.notes.empty())
                order.push_back (&s);

        // Same rhythm and same notes, or the same line an octave or two away (a stop that
        // should sound at 16' or 4' pitch, played transposed on its own channel).
        auto offsetBetween = [] (const Stream& a, const Stream& b) -> int {
            if (a.notes.size() != b.notes.size() || a.notes.empty())
                return 99;
            const int k = b.notes.front().key - a.notes.front().key;
            if (k % 12 != 0 || std::abs (k) > 24)
                return 99;
            for (size_t i = 0; i < a.notes.size(); ++i)
                if (std::abs (a.notes[i].start - b.notes[i].start) > 1.0e-6 || b.notes[i].key - a.notes[i].key != k)
                    return 99;
            return k;
        };
        for (auto* s : order)
            std::sort (s->notes.begin(), s->notes.end(), [] (auto& a, auto& b) {
                return a.start != b.start ? a.start < b.start : a.key < b.key;
            });

        auto outOfRange = [] (const Stream& st, int shift) {
            int n = 0;
            for (auto& nt : st.notes)
                n += (nt.key + shift < 36 || nt.key + shift > 96) ? 1 : 0;
            return n;
        };

        std::vector<bool> used (order.size(), false);
        for (size_t i = 0; i < order.size(); ++i)
        {
            if (used[i])
                continue;
            std::vector<Stream*> group { order[i] };
            std::vector<int> offsets { 0 };
            for (size_t j = i + 1; j < order.size(); ++j)
                if (! used[j] && order[j]->channel != 9 && order[i]->channel != 9)
                {
                    const int k = offsetBetween (*order[i], *order[j]);
                    if (k != 99)
                    {
                        group.push_back (order[j]);
                        offsets.push_back (k);
                        used[j] = true;
                    }
                }

            // Pedal stops in this group? Then the line belongs in the pedal compass (C..g').
            bool pedalGroup = false;
            if (sf != nullptr)
                for (auto* g : group)
                    for (auto rs : programsAt (*g, g->notes.front().start))
                    {
                        sf->resolvePreset (rs.bank, rs.program);
                        auto n = sf->presetName (rs.bank, rs.program);
                        pedalGroup |= (n.containsIgnoreCase ("(ped") || n.containsIgnoreCase ("ped.") || n.containsIgnoreCase ("pedal")) || n.startsWithIgnoreCase ("Basse de") || n.containsIgnoreCase ("Contra")
                                      || n.containsIgnoreCase ("Subbass") || n.containsIgnoreCase ("Bourdon 32")
                                      || n.containsIgnoreCase ("Bombarde") || n.containsIgnoreCase ("Posaune");
                    }
            const int top = pedalGroup ? 67 : 96;

            // The written pitch of the line: the octave at which the fewest notes fall outside
            // the keyboard. Octaves no channel actually plays (a line transposed to fake a 16'
            // stop) are only chosen if that brings a real share of the notes back in.
            auto& base = *group.front();
            int written = 0;
            double bestScore = 1.0e18;
            std::vector<int> candidates;
            for (int o : offsets)
                for (int m = -2; m <= 2; ++m)
                    candidates.push_back (o + 12 * m);
            std::sort (candidates.begin(), candidates.end());
            candidates.erase (std::unique (candidates.begin(), candidates.end()), candidates.end());
            for (int w : candidates)
            {
                int outside = 0;
                for (auto& nt : base.notes)
                    outside += (nt.key + w < 36 || nt.key + w > top) ? 1 : 0;
                const bool played = std::find (offsets.begin(), offsets.end(), w) != offsets.end();
                const double score = outside + (played ? 0.0 : juce::jmax (3.0, 0.1 * (double) base.notes.size())) + std::abs (w) * 1.0e-3;
                if (score < bestScore)
                {
                    bestScore = score;
                    written = w;
                }
            }
            size_t ref = 0;
            for (size_t g = 0; g < group.size(); ++g)
                if (offsets[g] == written)
                    ref = g;
            const int lift = written - offsets[ref];
            auto octaveOf = [&] (size_t g) { return (offsets[g] - written) / 12; };

            ScorePart part;
            auto& first = *group[ref];
            part.drums = first.channel == 9;
            const bool multiTrack = trackNames.size() > 1 && first.trackName.isNotEmpty() && first.track > 0;
            part.name = multiTrack ? first.trackName : "Kanal " + juce::String (first.channel + 1);
            if (group.size() > 1)
            {
                juce::StringArray chans;
                for (auto* g : group)
                    chans.add (juce::String (g->channel + 1));
                part.name = multiTrack ? first.trackName : juce::String::fromUTF8 ("Kanäle ") + chans.joinIntoString ("+");
            }

            const double firstBeat = first.notes.front().start;
            auto collect = [&] (double beat) {
                Registration reg;
                for (size_t g = 0; g < group.size(); ++g)
                    for (auto rs : programsAt (*group[g], beat))
                    {
                        if (sf != nullptr)
                            sf->resolvePreset (rs.bank, rs.program);
                        rs.octave = octaveOf (g);
                        if (std::find (reg.begin(), reg.end(), rs) == reg.end())
                            reg.push_back (rs);
                    }
                return reg;
            };
            part.registration = collect (firstBeat);
            if (sf != nullptr)
                for (auto& rs : part.registration)
                {
                    auto n = sf->presetName (rs.bank, rs.program);
                    part.pedalStops |= (n.containsIgnoreCase ("(ped") || n.containsIgnoreCase ("ped.") || n.containsIgnoreCase ("pedal")) || n.startsWithIgnoreCase ("Basse de")
                                       || n.containsIgnoreCase ("Contra") || n.containsIgnoreCase ("Subbass")
                                       || n.containsIgnoreCase ("Bourdon 32") || n.containsIgnoreCase ("Bombarde")
                                       || n.containsIgnoreCase ("Posaune");
                }

            // A line played in octaves throughout (one channel doing 16' + 8'): keep the upper
            // note as the written line and let the stops sound an octave lower as well.
            std::set<std::pair<double, int>> lowerOfPair; // (start, key) of notes to drop
            {
                std::map<double, std::vector<int>> byStart;
                for (auto& n : first.notes)
                    byStart[n.start].push_back (n.key);
                int pairs = 0;
                for (auto& [t, keys] : byStart)
                    if (keys.size() == 2 && std::abs (keys[0] - keys[1]) == 12)
                        ++pairs;
                if (pairs >= 8 && pairs >= (int) (0.6 * (double) byStart.size()))
                {
                    for (auto& [t, keys] : byStart)
                        if (keys.size() == 2 && std::abs (keys[0] - keys[1]) == 12)
                            lowerOfPair.insert ({ t, juce::jmin (keys[0], keys[1]) });
                    Registration withSub = part.registration;
                    for (auto rs : part.registration)
                    {
                        rs.octave -= 1;
                        if (std::find (withSub.begin(), withSub.end(), rs) == withSub.end())
                            withSub.push_back (rs);
                    }
                    part.registration = withSub;
                }
            }

            const int partIndex = (int) score->parts.size();
            score->parts.push_back (part);
            for (auto n : first.notes)
            {
                if (lowerOfPair.count ({ n.start, n.key }))
                    continue;
                n.part = partIndex;
                n.key += lift;
                score->notes.push_back (n);
            }

            // later program changes become registration changes during playback
            std::vector<double> changeBeats;
            for (auto* g : group)
                for (auto& [b, bk, pr] : g->programs)
                    if (b > firstBeat + 1.0e-6)
                        changeBeats.push_back (b);
            std::sort (changeBeats.begin(), changeBeats.end());
            changeBeats.erase (std::unique (changeBeats.begin(), changeBeats.end()), changeBeats.end());
            Registration last = part.registration;
            for (double b : changeBeats)
            {
                auto reg = collect (b);
                if (! lowerOfPair.empty())
                {
                    auto base = reg;
                    for (auto rs : base)
                    {
                        rs.octave -= 1;
                        if (std::find (reg.begin(), reg.end(), rs) == reg.end())
                            reg.push_back (rs);
                    }
                }
                if (reg != last && ! reg.empty())
                {
                    score->registrationChanges.push_back ({ b, partIndex, reg });
                    last = reg;
                }
            }
        }

        // Program numbers only mean organ stops in files made for an organ SoundFont (they
        // select banks). In ordinary General-MIDI files they are instruments (flute, strings,
        // drums ...): play those with the organ's current registration instead.
        if (! usesBankSelect)
        {
            for (auto& p : score->parts)
            {
                p.registration.clear();
                p.pedalStops = false;
            }
            score->registrationChanges.clear();
        }
        score->finalise();
        estimateKeyIfMissing (*score);
        // drop duplicate meta events that multi-track files repeat in every track
        auto dedupe = [] (auto& v, auto eq) {
            v.erase (std::unique (v.begin(), v.end(), eq), v.end());
        };
        dedupe (score->tempos, [] (auto& a, auto& b) { return std::abs (a.beat - b.beat) < 1.0e-9 && a.bpm == b.bpm; });
        dedupe (score->timeSigs, [] (auto& a, auto& b) { return std::abs (a.beat - b.beat) < 1.0e-9; });
        dedupe (score->keySigs, [] (auto& a, auto& b) { return std::abs (a.beat - b.beat) < 1.0e-9; });
        return score;
    }

    // Many MIDI files in the wild are slightly off: bytes after the last track, a format-0
    // header with several tracks, a RIFF (.rmi) wrapper, a track that claims to be longer than
    // the file. Rebuild a clean file from the tracks that are there.
    static juce::MemoryBlock repairMidi (const void* data, size_t size)
    {
        auto* b = static_cast<const uint8_t*> (data);
        auto be32 = [b] (size_t p) { return (uint32_t) b[p] << 24 | (uint32_t) b[p + 1] << 16 | (uint32_t) b[p + 2] << 8 | b[p + 3]; };
        size_t head = 0;
        while (head + 14 <= size && std::memcmp (b + head, "MThd", 4) != 0)
            ++head;
        if (head + 14 > size)
            return {};
        const size_t headerLen = juce::jmax<uint32_t> (6, be32 (head + 4));
        const uint16_t division = (uint16_t) (b[head + 12] << 8 | b[head + 13]);
        std::vector<std::pair<size_t, size_t>> tracks; // (start of data, length)
        size_t p = head + 8 + headerLen;
        while (p + 8 <= size)
        {
            if (std::memcmp (b + p, "MTrk", 4) != 0)
            {
                ++p; // skip junk until the next track
                continue;
            }
            const size_t len = juce::jmin<size_t> (be32 (p + 4), size - (p + 8));
            tracks.push_back ({ p + 8, len });
            p += 8 + len;
        }
        if (tracks.empty())
            return {};
        juce::MemoryOutputStream out;
        out.write ("MThd", 4);
        out.writeIntBigEndian (6);
        out.writeShortBigEndian ((short) (tracks.size() > 1 ? 1 : 0));
        out.writeShortBigEndian ((short) tracks.size());
        out.writeShortBigEndian ((short) division);
        for (auto [start, len] : tracks)
        {
            out.write ("MTrk", 4);
            // make sure every track ends with an end-of-track event
            const bool hasEnd = len >= 3 && b[start + len - 3] == 0xff && b[start + len - 2] == 0x2f && b[start + len - 1] == 0x00;
            out.writeIntBigEndian ((int) (len + (hasEnd ? 0 : 4)));
            out.write (b + start, len);
            if (! hasEnd)
            {
                const uint8_t eot[] = { 0x00, 0xff, 0x2f, 0x00 };
                out.write (eot, 4);
            }
        }
        return out.getMemoryBlock();
    }

    std::shared_ptr<Score> fromMidiData (const void* data, size_t size, const SoundFont* sf, juce::String& error)
    {
        juce::MidiFile mf;
        bool ok = false;
        {
            juce::MemoryInputStream in (data, size, false);
            ok = mf.readFrom (in, true);
        }
        if (! ok)
        {
            auto repaired = repairMidi (data, size);
            juce::MemoryInputStream in (repaired, false);
            mf.clear();
            ok = repaired.getSize() > 0 && mf.readFrom (in, true);
        }
        if (! ok)
        {
            error = juce::String::fromUTF8 ("Die MIDI-Daten sind ungültig.");
            return {};
        }
        auto s = fromMidi (mf, sf);
        if (s == nullptr || s->notes.empty())
        {
            error = juce::String::fromUTF8 ("Die Datei enthält keine Noten.");
            return {};
        }
        return s;
    }

    std::shared_ptr<Score> loadMidiFile (const juce::File& f, const SoundFont* sf, juce::String& error)
    {
        juce::MemoryBlock mb;
        if (! f.loadFileAsData (mb))
        {
            error = "Datei kann nicht gelesen werden.";
            return {};
        }
        auto s = fromMidiData (mb.getData(), mb.getSize(), sf, error);
        if (s != nullptr && s->title.isEmpty())
            s->title = f.getFileNameWithoutExtension();
        return s;
    }

    juce::MidiFile toMidi (const Score& score, const std::vector<juce::String>& partNames, const std::vector<Registration>& registrations)
    {
        constexpr int ppq = 480;
        auto tick = [] (double beat) { return std::round (beat * ppq); };
        juce::MidiFile mf;
        mf.setTicksPerQuarterNote (ppq);

        juce::MidiMessageSequence meta;
        if (score.title.isNotEmpty())
            meta.addEvent (juce::MidiMessage::textMetaEvent (3, score.title), 0);
        for (auto& t : score.tempos)
            meta.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::round (60000000.0 / t.bpm)), tick (t.beat));
        for (auto& t : score.timeSigs)
            meta.addEvent (juce::MidiMessage::timeSignatureMetaEvent (t.num, t.den), tick (t.beat));
        for (auto& k : score.keySigs)
            meta.addEvent (juce::MidiMessage::keySignatureMetaEvent (k.fifths, k.minor), tick (k.beat));
        meta.addEvent (juce::MidiMessage::endOfTrack(), tick (score.lengthBeats()));
        mf.addTrack (meta);

        // channels in use (1-based), skipping 10 (drums)
        std::vector<int> freeChannels;
        for (int c = 1; c <= 16; ++c)
            if (c != 10)
                freeChannels.push_back (c);
        size_t nextChannel = 0;
        int playable = 0;
        for (auto& p : score.parts)
            playable += p.drums ? 0 : 1;

        for (int p = 0; p < (int) score.parts.size(); ++p)
        {
            auto& part = score.parts[(size_t) p];
            Registration reg = p < (int) registrations.size() && ! registrations[(size_t) p].empty() ? registrations[(size_t) p] : part.registration;
            // keep at least one channel for every part that is still to come
            const size_t remainingParts = (size_t) juce::jmax (0, playable - 1);
            const size_t available = freeChannels.size() > nextChannel + remainingParts ? freeChannels.size() - nextChannel - remainingParts : 1;
            if (! part.drums)
                --playable;

            std::vector<std::pair<int, RegStop>> channels; // channel -> stop
            if (part.drums)
                channels.push_back ({ 10, RegStop { -1, -1, 0 } });
            else if (reg.empty())
                channels.push_back ({ freeChannels[juce::jmin (nextChannel++, freeChannels.size() - 1)], RegStop { -1, -1, 0 } });
            else
                for (size_t r = 0; r < reg.size() && r < available; ++r)
                    channels.push_back ({ freeChannels[juce::jmin (nextChannel++, freeChannels.size() - 1)], reg[r] });

            juce::MidiMessageSequence seq;
            auto name = p < (int) partNames.size() && partNames[(size_t) p].isNotEmpty() ? partNames[(size_t) p] : part.name;
            seq.addEvent (juce::MidiMessage::textMetaEvent (3, name), 0);
            for (auto& [ch, bp] : channels)
                if (bp.program >= 0)
                {
                    seq.addEvent (juce::MidiMessage::controllerEvent (ch, 0, bp.bank), 0);
                    seq.addEvent (juce::MidiMessage::controllerEvent (ch, 32, 0), 0);
                    seq.addEvent (juce::MidiMessage::programChange (ch, bp.program), 0);
                }
            for (auto& n : score.notes)
                if (n.part == p)
                    for (auto& [ch, bp] : channels)
                    {
                        // a stop an octave down is written as the line an octave down on its channel
                        const int key = juce::jlimit (0, 127, n.key + 12 * bp.octave);
                        seq.addEvent (juce::MidiMessage::noteOn (ch, key, (juce::uint8) juce::jlimit (1, 127, n.velocity)), tick (n.start));
                        seq.addEvent (juce::MidiMessage::noteOff (ch, key), tick (n.start + n.length));
                    }
            seq.updateMatchedPairs();
            mf.addTrack (seq);
        }
        return mf;
    }

    bool saveMidiFile (const Score& score, const juce::File& f, const std::vector<juce::String>& partNames, const std::vector<Registration>& registrations)
    {
        auto mf = toMidi (score, partNames, registrations);
        f.deleteFile();
        juce::FileOutputStream out (f);
        return out.openedOk() && mf.writeTo (out, 1);
    }

    namespace
    {
        using Segments = std::vector<std::pair<double, double>>;

        // stretches in which a part plays: a rest of more than two bars ends a stretch
        Segments segmentsOf (const Score& score, int part)
        {
            Segments segs;
            for (auto& n : score.notes)
            {
                if (n.part != part)
                    continue;
                if (segs.empty() || n.start > segs.back().second + 8.0)
                    segs.push_back ({ n.start, n.start + n.length });
                else
                    segs.back().second = juce::jmax (segs.back().second, n.start + n.length);
            }
            return segs;
        }

        double overlap (const Segments& a, const Segments& b)
        {
            double total = 0;
            for (auto& x : a)
                for (auto& y : b)
                    total += juce::jmax (0.0, juce::jmin (x.second, y.second) - juce::jmax (x.first, y.first));
            return total;
        }

        // one part takes over from the other within a beat: no time to change stops
        bool answersQuickly (const Segments& a, const Segments& b)
        {
            for (auto& x : a)
                for (auto& y : b)
                    if (std::abs (x.first - y.second) < 2.0 || std::abs (y.first - x.second) < 2.0)
                        return true;
            return false;
        }

        Registration registrationAt (const Score& score, int part, double beat)
        {
            Registration reg = score.parts[(size_t) part].registration;
            for (auto& rc : score.registrationChanges)
                if (rc.part == part && rc.beat <= beat + 1.0e-6)
                    reg = rc.registration;
            return reg;
        }
    } // namespace

    std::vector<int> autoAssign (const Score& score, int numManuals, int pedalIndex)
    {
        const int numParts = (int) score.parts.size();
        std::vector<int> result ((size_t) numParts, -1);
        std::vector<int> candidates;
        for (int p = 0; p < numParts; ++p)
            if (! score.parts[(size_t) p].drums && score.parts[(size_t) p].count > 0)
                candidates.push_back (p);
        if (candidates.empty() || numManuals <= 0)
            return result;

        // pedal: named so, or a part that stays in the pedal range (several parts may be pedal)
        bool anyHighPart = false;
        for (int p : candidates)
            anyHighPart |= score.parts[(size_t) p].highest > 67;
        std::vector<int> manualParts;
        for (int p : candidates)
        {
            auto& part = score.parts[(size_t) p];
            int above = 0, total = 0;
            for (auto& n : score.notes)
                if (n.part == p)
                {
                    ++total;
                    above += n.key > 67 ? 1 : 0;
                }
            const bool named = part.name.containsIgnoreCase ("pedal") || part.name.startsWithIgnoreCase ("ped.")
                               || (part.pedalStops && above <= total / 20);
            const bool lowOnly = anyHighPart && candidates.size() >= 2 && part.highest <= 67 && part.meanPitch < 52;
            if (pedalIndex >= 0 && (named || lowOnly))
                result[(size_t) p] = pedalIndex;
            else
                manualParts.push_back (p);
        }

        // manuals: the busiest part on manual I; parts that sound together or answer each other
        // closely go to different manuals; parts with other stops prefer a manual of their own
        std::stable_sort (manualParts.begin(), manualParts.end(), [&] (int a, int b) {
            return score.parts[(size_t) a].count > score.parts[(size_t) b].count;
        });
        std::vector<Segments> segs ((size_t) numParts);
        for (int p : manualParts)
            segs[(size_t) p] = segmentsOf (score, p);
        std::vector<std::vector<int>> onManual ((size_t) numManuals);
        for (int p : manualParts)
        {
            int best = 0;
            double bestCost = 1.0e18;
            for (int m = 0; m < numManuals; ++m)
            {
                double cost = m * 0.01;
                for (int q : onManual[(size_t) m])
                {
                    // staves of one instrument (same name, e.g. right and left hand) belong together
                    if (score.parts[(size_t) q].name == score.parts[(size_t) p].name && ! score.parts[(size_t) p].name.startsWith ("Kan"))
                    {
                        cost -= 50.0;
                        continue;
                    }
                    cost += 100.0 * overlap (segs[(size_t) p], segs[(size_t) q]) / juce::jmax (1.0, (double) score.parts[(size_t) p].count);
                    // answering within a beat with other stops (echo effects): impossible on one manual.
                    // Between movements or after a rest the organist can simply change the stops.
                    if (answersQuickly (segs[(size_t) p], segs[(size_t) q])
                        && score.parts[(size_t) q].registration != score.parts[(size_t) p].registration)
                        cost += 5.0;
                }
                if (cost < bestCost)
                {
                    bestCost = cost;
                    best = m;
                }
            }
            result[(size_t) p] = best;
            onManual[(size_t) best].push_back (p);
        }
        return result;
    }

    std::vector<DivisionRegistration> registrationTimeline (const Score& score, const std::vector<int>& partDivision)
    {
        std::vector<DivisionRegistration> timeline;
        const int numParts = (int) score.parts.size();
        std::map<int, std::vector<int>> partsOn;
        std::vector<Segments> segs ((size_t) numParts);
        for (int p = 0; p < numParts && p < (int) partDivision.size(); ++p)
            if (partDivision[(size_t) p] >= 0 && ! score.parts[(size_t) p].registration.empty())
            {
                partsOn[partDivision[(size_t) p]].push_back (p);
                segs[(size_t) p] = segmentsOf (score, p);
            }

        for (auto& [division, parts] : partsOn)
        {
            // moments at which something may change: a part starts playing, or the file changes its stops
            std::vector<double> moments;
            for (int p : parts)
            {
                for (auto& sg : segs[(size_t) p])
                    moments.push_back (sg.first);
                for (auto& rc : score.registrationChanges)
                    if (rc.part == p)
                        moments.push_back (rc.beat);
            }
            std::sort (moments.begin(), moments.end());
            moments.erase (std::unique (moments.begin(), moments.end(), [] (double a, double b) { return std::abs (a - b) < 1.0e-6; }), moments.end());

            Registration last;
            for (double t : moments)
            {
                Registration reg;
                auto addFrom = [&] (int p) {
                    for (auto bp : registrationAt (score, p, t))
                        if (std::find (reg.begin(), reg.end(), bp) == reg.end())
                            reg.push_back (bp);
                };
                for (int p : parts)
                    for (auto& sg : segs[(size_t) p])
                        if (sg.first <= t + 1.0 && sg.second > t)
                        {
                            addFrom (p);
                            break;
                        }
                if (reg.empty())
                    continue;
                if (reg != last)
                {
                    timeline.push_back ({ t, division, reg });
                    last = reg;
                }
            }
        }
        std::stable_sort (timeline.begin(), timeline.end(), [] (auto& a, auto& b) { return a.beat < b.beat; });
        return timeline;
    }
} // namespace ScoreIO
} // namespace orgel
