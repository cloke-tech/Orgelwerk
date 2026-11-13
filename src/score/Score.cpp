#include "Score.h"
#include "../engine/I18n.h"
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

        Registration programsAt (const Stream& s, double beat, bool firstIfNone = false)
        {
            int bank = -1, prog = -1;
            // a program set just after the first note still counts for the beginning
            if (firstIfNone && ! s.programs.empty())
            {
                bank = std::get<1> (s.programs.front());
                prog = std::get<2> (s.programs.front());
            }
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

        // the pitch of a preset in feet from its name ("Montre 8", "Bourdon 16"); no number: 8'
        double feetOf (const juce::String& name)
        {
            for (int i = 0; i < name.length(); ++i)
                if (juce::CharacterFunctions::isDigit (name[i]))
                {
                    int v = 0;
                    while (i < name.length() && juce::CharacterFunctions::isDigit (name[i]))
                        v = v * 10 + (name[i++] - '0');
                    return v;
                }
            return 8.0;
        }

        // A SoundFont made for General MIDI (piano at 0:0) plays GM programs as they are.
        bool isGeneralMidiFont (const SoundFont& sf)
        {
            auto n = sf.presetName (0, 0);
            return n.containsIgnoreCase ("piano") || n.containsIgnoreCase ("grand") || n.containsIgnoreCase ("flügel");
        }

        // The organ stops that stand in for a General MIDI instrument: looked up in the organ
        // SoundFont by name and pitch (a flute plays on flute stops, a trumpet on a trumpet,
        // strings on gambas and celestes, a choir on the vox humana, a bass on 16' and 8').
        Registration instrumentRegistration (int program, bool low, const SoundFont& sf)
        {
            struct Want
            {
                std::vector<const char*> words;
                double feet; // 0: any
            };
            const std::vector<const char*> principal { "Principal", "Prinzipal", "Montre", "Prestant", "Octav", "Diapason", "Prin." };
            const std::vector<const char*> flute { "Flute", "Flöte", "Flûte", "Bourdon", "Gedackt", "Rohrfl", "Holzfl", "Spitzfl" };
            const std::vector<const char*> strings { "Gamba", "Viol", "Salicional", "Salicionale" };
            const std::vector<const char*> celeste { "Celeste", "Céleste", "Unda" };
            const std::vector<const char*> trumpet { "Trompette", "Trumpet", "Tromp" };
            const std::vector<const char*> bassReed { "Posaune", "Bombarde", "Basson", "Fagott", "Basse de Tromp" };
            const std::vector<const char*> oboe { "Hautbois", "Oboe", "Cor Anglais", "Schalmei" };
            const std::vector<const char*> clarinet { "Cromorne", "Krumm", "Clarinet" };
            const std::vector<const char*> vox { "Vox Humana", "Voix Humaine", "Vox" };
            const std::vector<const char*> bells { "Glocken", "Carillon", "Bells", "Zimbelstern" };
            const std::vector<const char*> cornet { "Cornet", "Sesquialt" };
            const std::vector<const char*> regal { "Regal", "Cornemuse", "Musette" };
            const std::vector<const char*> recorder { "Blockfl", "Flageolet", "Flöte 4", "Flute 4" };
            const std::vector<const char*> mixture { "Mixtur", "Fourniture", "Plein Jeu" };

            std::vector<Want> wants;
            const int family = program / 8;
            auto add = [&] (const std::vector<const char*>& w, double feet) { wants.push_back ({ w, feet }); };
            if (low || family == 4) // a bass line: 16' and 8' foundation (with a reed for brass and reeds)
            {
                add (flute, 16);
                add (principal, 16);
                add (principal, 8);
                if (family == 7 || family == 8)
                    add (bassReed, 16);
            }
            else switch (family)
            {
                case 0: add (principal, 8); add (principal, 4); break;                    // piano
                case 1: add (bells, 0); add (flute, 4); add (flute, 2); break;            // glockenspiel, celesta ...
                case 2: if (program == 19 || program == 20) { add (principal, 8); add (principal, 4); add (mixture, 0); }
                        else { add (flute, 8); add (flute, 4); }
                        break;                                                            // organs
                case 3: add (flute, 8); add (flute, 4); break;                            // guitars
                case 5: if (program == 46) { add (flute, 8); add (flute, 4); }            // harp
                        else if (program == 47) { add (flute, 16); add (principal, 8); }  // timpani
                        else { add (strings, 8); add (celeste, 8); }
                        break;                                                            // strings
                case 6: if (program >= 52 && program <= 54) { add (vox, 8); add (flute, 8); }
                        else if (program == 55) { add (principal, 8); add (principal, 4); add (mixture, 0); add (trumpet, 8); }
                        else { add (strings, 8); add (celeste, 8); }
                        break;                                                            // ensembles, choir
                case 7: if (program == 58) add (bassReed, 16); else add (trumpet, 8);
                        add (principal, 8);
                        break;                                                            // brass
                case 8: if (program == 70) add (bassReed, 16);
                        else if (program == 71) add (clarinet, 8);
                        else add (oboe, 8);
                        add (flute, 8);
                        break;                                                            // reeds
                case 9: if (program == 72) { add (flute, 4); add (flute, 2); }
                        else if (program == 74) add (recorder, 0);
                        else { add (flute, 8); add (flute, 4); }
                        break;                                                            // flutes
                case 10: add (cornet, 0); add (principal, 8); break;                      // synth lead
                case 11: add (strings, 8); add (celeste, 8); add (flute, 8); break;       // synth pad
                case 13: add (regal, 8); add (flute, 8); break;                           // ethnic (bagpipe ...)
                case 14: add (bells, 0); add (flute, 2); break;                           // percussive
                default: add (flute, 8); add (strings, 8); break;
            }
            Registration reg;
            for (auto& w : wants)
            {
                for (auto& p : sf.getPresets())
                {
                    bool match = false;
                    for (auto* word : w.words)
                        match |= p.name.containsIgnoreCase (juce::String::fromUTF8 (word));
                    // single stops only (no "Tutti", no "Montre 8 Flute 4" combinations)
                    int numbers = 0;
                    for (int i = 0; i < p.name.length(); ++i)
                        if (juce::CharacterFunctions::isDigit (p.name[i]) && (i == 0 || ! juce::CharacterFunctions::isDigit (p.name[i - 1])))
                            ++numbers;
                    if (! match || numbers > 1 || p.name.containsIgnoreCase ("Tutti") || p.name.containsIgnoreCase ("+"))
                        continue;
                    if (w.feet > 0 && std::abs (feetOf (p.name) - w.feet) > 0.01)
                        continue;
                    RegStop rs { p.bank, p.program, 0 };
                    if (std::find (reg.begin(), reg.end(), rs) == reg.end())
                    {
                        reg.push_back (rs);
                        break;
                    }
                }
            }
            return reg;
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
        // Program numbers mean organ stops only in files made for an organ SoundFont (Jeux files
        // select bank 42 or 43). In any other file they are General MIDI instruments: with an
        // organ SoundFont loaded, they are played on organ stops that sound like them.
        // program changes in another track than the notes of the same channel (type 1 files)
        for (auto& [key, st] : streams)
            if (st.programs.empty())
                for (auto& [key2, other] : streams)
                    if (key2.second == key.second && ! other.programs.empty())
                    {
                        st.programs = other.programs;
                        break;
                    }
        bool organNumbering = false;
        for (auto& [key, st] : streams)
            for (auto& [b, bk, pr] : st.programs)
                organNumbering |= bk == 42 || bk == 43;
        const bool generalMidi = sf != nullptr && ! organNumbering && ! isGeneralMidiFont (*sf);
        std::vector<Stream*> order;
        for (auto& [key, s] : streams)
            if (! s.notes.empty())
                order.push_back (&s);

        // Same rhythm and same notes, or the same line an octave or two away (a stop that
        // should sound at 16' or 4' pitch, played transposed on its own channel). Files are not
        // always exact about it (a stop moved an octave for a few bars, a note missing): two
        // channels count as one line when nearly all notes of each have a partner in the other
        // at the same time, and one octave distance clearly dominates.
        auto offsetBetween = [] (const Stream& a, const Stream& b) -> int {
            if (a.notes.empty() || b.notes.empty())
                return 99;
            const size_t na = a.notes.size(), nb = b.notes.size();
            if (std::abs ((double) na - (double) nb) > 0.03 * (double) juce::jmax (na, nb))
                return 99;
            auto slot = [] (double t) { return (long long) std::llround (t * 64.0); };
            std::map<long long, std::vector<int>> keysA, keysB;
            for (auto& n : a.notes)
                keysA[slot (n.start)].push_back (n.key);
            for (auto& n : b.notes)
                keysB[slot (n.start)].push_back (n.key);
            std::map<int, int> offsetCount;
            int matchedA = 0, matchedB = 0;
            auto partner = [] (const std::map<long long, std::vector<int>>& keys, long long t, int key, int* offsetFound) {
                for (long long dt : { 0LL, -1LL, 1LL })
                    if (auto it = keys.find (t + dt); it != keys.end())
                        for (int k : { 0, 12, -12, 24, -24 })
                            if (std::find (it->second.begin(), it->second.end(), key + k) != it->second.end())
                            {
                                if (offsetFound != nullptr)
                                    *offsetFound = k;
                                return true;
                            }
                return false;
            };
            for (auto& n : a.notes)
            {
                int k = 0;
                if (partner (keysB, slot (n.start), n.key, &k))
                {
                    ++matchedA;
                    ++offsetCount[k];
                }
            }
            for (auto& n : b.notes)
                matchedB += partner (keysA, slot (n.start), n.key, nullptr) ? 1 : 0;
            if (matchedA < 0.97 * (double) na || matchedB < 0.97 * (double) nb)
                return 99;
            int best = 0, bestCount = -1;
            for (auto& [k, c] : offsetCount)
                if (c > bestCount)
                {
                    best = k;
                    bestCount = c;
                }
            return bestCount >= 0.9 * matchedA ? best : 99;
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
            // the channel that carries the line: at the written pitch, with the most notes
            // (when the doubling is not exact, the fullest copy is the line)
            size_t ref = group.size();
            for (size_t g = 0; g < group.size(); ++g)
                if (offsets[g] == written && (ref == group.size() || group[g]->notes.size() > group[ref]->notes.size()))
                    ref = g;
            if (ref == group.size())
                ref = 0;
            const int lift = written - offsets[ref];
            auto octaveOf = [&] (size_t g) { return (offsets[g] - written) / 12; };

            ScorePart part;
            auto& first = *group[ref];
            part.drums = first.channel == 9;
            const bool multiTrack = trackNames.size() > 1 && first.trackName.isNotEmpty() && first.track > 0;
            part.name = multiTrack ? first.trackName : tr ("Kanal ") + juce::String (first.channel + 1);
            if (group.size() > 1)
            {
                juce::StringArray chans;
                for (auto* g : group)
                    chans.add (juce::String (g->channel + 1));
                part.name = multiTrack ? first.trackName : tr ("Kanäle ") + chans.joinIntoString ("+");
            }

            const double firstBeat = first.notes.front().start;
            // a low line (mostly below the tenor) is registered like a bass
            double meanKey = 0;
            for (auto& nt : first.notes)
                meanKey += nt.key + lift;
            meanKey /= juce::jmax<size_t> (1, first.notes.size());
            auto collect = [&] (double beat, bool atStart) {
                Registration reg;
                for (size_t g = 0; g < group.size(); ++g)
                    for (auto rs : programsAt (*group[g], beat, atStart))
                    {
                        if (generalMidi)
                        {
                            // a GM instrument: the organ stops that sound like it
                            for (auto stop : instrumentRegistration (rs.program, meanKey < 48.0, *sf))
                            {
                                stop.octave = octaveOf (g);
                                if (std::find (reg.begin(), reg.end(), stop) == reg.end())
                                    reg.push_back (stop);
                            }
                            continue;
                        }
                        if (sf != nullptr)
                            sf->resolvePreset (rs.bank, rs.program);
                        rs.octave = octaveOf (g);
                        if (std::find (reg.begin(), reg.end(), rs) == reg.end())
                            reg.push_back (rs);
                    }
                return reg;
            };
            part.registration = collect (firstBeat, true);
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
                auto reg = collect (b, false);
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
            error = tr ("Die MIDI-Daten sind ungültig.");
            return {};
        }
        auto s = fromMidi (mf, sf);
        if (s == nullptr || s->notes.empty())
        {
            error = tr ("Die Datei enthält keine Noten.");
            return {};
        }
        return s;
    }

    std::shared_ptr<Score> loadMidiFile (const juce::File& f, const SoundFont* sf, juce::String& error)
    {
        juce::MemoryBlock mb;
        if (! f.loadFileAsData (mb))
        {
            error = tr ("Datei kann nicht gelesen werden.");
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
