#include "NotationBuilder.h"
#include <map>
#include <set>
#include <tuple>
#include <array>
#include <regex>
#include <cctype>
#include "../engine/OrganModel.h"

namespace orgel::NotationBuilder
{
namespace
{
    constexpr int U = 24; // grid units per quarter note: 32nd notes (3) and triplets (8, 4)

    struct QNote
    {
        int key = 60, step = 0, alter = 0, octave = 4;
        int showAccidental = -99; // -99 = none, otherwise the alter to print
        bool tieStart = false, tieStop = false;
        int division = -1;        // organ division (manual) that plays it
        int ornament = 0;         // 0 none, 1 trill, 2 mordent (lower), 3 inverted mordent (upper)
    };

    struct QEvent
    {
        int start = 0, dur = 0; // relative to measure start
        bool rest = false;
        int typeUnits = U, dots = 0;
        bool triplet = false, tupletStart = false, tupletStop = false;
        std::vector<QNote> notes;
        int beam = 0; // 0 none, 1 begin, 2 continue, 3 end
        int stem = 0; // 0 auto, 1 up, 2 down
        bool wholeMeasureRest = false;
        bool hidden = false; // rest that would collide with another voice's notes
    };

    struct QMeasure
    {
        int start = 0, length = 4 * U;
        int num = 4, den = 4;
        int fifths = 0;
        bool minor = false;
        bool timeChange = false, keyChange = false;
        std::vector<std::pair<int, double>> tempos; // (position in measure, bpm)
    };

    struct QStaff
    {
        bool bass = false;
        int part = 0; // index into QScore::parts
        bool pedal = false;
        int numVoices = 1;
        std::vector<char> clefBass; // per measure: bass clef (else treble)
        std::vector<std::vector<std::vector<QEvent>>> voices; // [voice][measure] -> events
    };

    struct QScore
    {
        juce::String title, composer;
        std::vector<QMeasure> measures;
        std::vector<QStaff> staves;
        bool hasPedal = false;
        int manualsUsed = 0;
        // the parts: one for all manuals (or one per manual), then the pedal
        struct QPart
        {
            juce::String name, abbreviation;
            bool pedal = false;
            int division = -1; // the manual of a part per manual
        };
        std::vector<QPart> parts;
        bool perManual = false;
        int partOf (int division, bool pedal) const
        {
            for (int p = 0; p < (int) parts.size(); ++p)
                if (pedal ? parts[(size_t) p].pedal : (! parts[(size_t) p].pedal && (! perManual || parts[(size_t) p].division == division)))
                    return p;
            return 0;
        }
        double firstTempo = 120;
    };

    // ---- pitch spelling ------------------------------------------------------
    const int stepPc[7] = { 0, 2, 4, 5, 7, 9, 11 };
    const int stepFifth[7] = { 0, 2, 4, -1, 1, 3, 5 };
    const char* stepName = "CDEFGAB";

    void spell (int key, int fifths, QNote& n)
    {
        const int pc = ((key % 12) + 12) % 12;
        double best = 1.0e9;
        for (int s = 0; s < 7; ++s)
        {
            int alter = pc - stepPc[s];
            if (alter > 6) alter -= 12;
            if (alter < -6) alter += 12;
            if (std::abs (alter) > 1)
                continue;
            const int pos = stepFifth[s] + 7 * alter;
            double cost = 0;
            if (pos > fifths + 5) cost = pos - (fifths + 5);
            else if (pos < fifths - 1) cost = ((fifths - 1) - pos) * 1.3;
            if (alter != 0) cost += 0.01;
            if (cost < best)
            {
                best = cost;
                n.step = s;
                n.alter = alter;
            }
        }
        n.octave = (key - n.alter - stepPc[n.step]) / 12 - 1;
        n.key = key;
    }

    int keySigAlter (int step, int fifths)
    {
        // order of sharps: F C G D A E B ; flats: B E A D G C F
        static const int sharpOrder[7] = { 3, 0, 4, 1, 5, 2, 6 };
        static const int flatOrder[7] = { 6, 2, 5, 1, 4, 0, 3 };
        for (int i = 0; i < std::abs (fifths) && i < 7; ++i)
            if ((fifths > 0 ? sharpOrder[i] : flatOrder[i]) == step)
                return fifths > 0 ? 1 : -1;
        return 0;
    }

    // ---- note values -----------------------------------------------------------
    struct Value
    {
        int units, typeUnits, dots;
        bool triplet = false;
    };
    const Value values[] = { { 4 * U, 4 * U, 0 }, { 3 * U, 2 * U, 1 }, { 2 * U, 2 * U, 0 }, { 3 * U / 2, U, 1 }, { U, U, 0 },
                             { 3 * U / 4, U / 2, 1 }, { U / 2, U / 2, 0 }, { 3 * U / 8, U / 4, 1 }, { U / 4, U / 4, 0 }, { U / 8, U / 8, 0 } };
    // triplet values (three in the time of two), written inside one beat
    const Value tripletValues[] = { { 2 * U / 3, U, 0, true }, { U / 3, U / 2, 0, true }, { U / 6, U / 4, 0, true } };
    using TripletBeats = std::map<int, int>; // beat (absolute, in quarters) -> triplet grid in units

    // How a note or rest is written with tied values, following the usual engraving rules:
    // the longest value that fits is taken from the left, a value starts at most half its
    // own length off the grid (8th-quarter-8th, 16th-dotted 8th), only notes starting on a
    // beat may hold over the middle of a 4/4 bar, and rests never hold over a beat.
    std::vector<Value> decompose (int pos, int len, const QMeasure& m, const TripletBeats& triplets, bool rest = false)
    {
        const bool compound = m.den == 8 && m.num % 3 == 0 && m.num > 3;
        const int beat = compound ? 3 * U / 2 : juce::jmax (1, 4 * U / juce::jmax (1, m.den));
        const int nominal = m.num * 4 * U / juce::jmax (1, m.den);
        // inside a triplet beat (not on its start)?
        auto tripletGrid = [&] (int p) {
            const int a = m.start + p;
            auto it = triplets.find (a / U);
            return it == triplets.end() ? 0 : it->second;
        };
        auto insideTriplet = [&] (int p) { return (m.start + p) % U != 0 && tripletGrid (p) > 0; };
        int halfBar = 0;
        if (m.length == nominal)
        {
            if (! compound && m.num % 2 == 0 && m.num >= 4)
                halfBar = m.length / 2;
            else if (compound && m.num == 12)
                halfBar = 3 * U;
        }
        auto crosses = [] (int s, int e, int grid) { return grid > 0 && (s / grid) != ((e - 1) / grid); };
        auto fits = [&] (int s, int l, const Value& v) {
            if (v.units != l || (v.units == 4 * U && m.length != 4 * U))
                return false;
            const int e = s + l;
            if (insideTriplet (s) || insideTriplet (e))
                return false;
            if (rest)
            {
                if (v.dots > 0 && ! (compound && v.units == 3 * U / 2))
                    return false;
                if (s % juce::jmin (v.units, beat * 2) != 0)
                    return false;
                if (s % beat != 0 && crosses (s, e, beat))
                    return false;
                return ! (halfBar > 0 && s % halfBar != 0 && crosses (s, e, halfBar));
            }
            const int align = juce::jmax (1, v.dots ? v.typeUnits / 2 : v.units / 2);
            if (s % align != 0)
                return false;
            if (s % beat != 0 && crosses (s, e, beat))
            {
                if (compound || s % juce::jmax (1, beat / 2) != 0)
                    return false;
            }
            return ! (halfBar > 0 && s % beat != 0 && crosses (s, e, halfBar));
        };
        std::vector<Value> out;
        while (len > 0)
        {
            const Value* chosen = nullptr;
            for (int l = len; l >= 1 && chosen == nullptr; --l)
                for (auto& v : values)
                    if (fits (pos, l, v))
                    {
                        chosen = &v;
                        break;
                    }
            if (chosen == nullptr && tripletGrid (pos) > 0)
            {
                // a piece of a triplet beat: triplet values up to the end of the beat
                const int a = m.start + pos;
                const int room = juce::jmin (len, (a / U + 1) * U - a);
                for (auto& v : tripletValues)
                    if (v.units <= room && (a % U) % v.units == 0)
                    {
                        chosen = &v;
                        break;
                    }
            }
            Value piece = chosen != nullptr ? *chosen : Value { juce::jmin (len, U / 8), U / 8, 0 };
            out.push_back (piece);
            pos += piece.units;
            len -= piece.units;
        }
        return out;
    }

    // ---- ornaments ---------------------------------------------------------------
    // Trills and mordents that a performer (or a sequencer file) played out as fast notes are
    // written the way they are printed: one note with "tr" or a mordent sign.
    //  - trill: at least four fast notes swinging between two neighbouring pitches; the
    //    written note is the lower one and lasts as long as the whole trill
    //  - mordent: main note, its neighbour, main note again - the first two fast, the last
    //    one held
    // "fast" means a 32nd or quicker. The notes that are absorbed are not written.
    void detectOrnaments (const Score& score, std::vector<bool>& written, std::vector<double>& start,
                          std::vector<double>& end, std::vector<int>& ornamentOf)
    {
        // a trill moves in 32nds or faster; a mordent may be a little slower (16th triplets)
        constexpr double fast = 0.18, trillFast = 0.15, joinGap = 0.06;
        std::map<int, std::vector<int>> byPart;
        for (int i = 0; i < (int) score.notes.size(); ++i)
            if (written[(size_t) i])
                byPart[score.notes[(size_t) i].part].push_back (i);
        auto len = [&] (int i) { return end[(size_t) i] - start[(size_t) i]; };
        auto key = [&] (int i) { return score.notes[(size_t) i].key; };
        for (auto& [part, idx] : byPart)
        {
            std::sort (idx.begin(), idx.end(), [&] (int a, int b) { return start[(size_t) a] < start[(size_t) b]; });
            std::vector<bool> used (idx.size(), false);
            // the note that follows idx[k] directly with a neighbouring pitch (wanted: that key, or any if 0)
            auto follower = [&] (size_t k, int wanted) -> int {
                const int i = idx[k];
                for (size_t m = k + 1; m < idx.size() && start[(size_t) idx[m]] <= end[(size_t) i] + joinGap; ++m)
                {
                    const int j = idx[m];
                    if (used[m] || start[(size_t) j] < end[(size_t) i] - joinGap || start[(size_t) j] <= start[(size_t) i])
                        continue;
                    const int d = std::abs (key (j) - key (i));
                    if ((wanted == 0 && (d == 1 || d == 2)) || key (j) == wanted)
                        return (int) m;
                }
                return -1;
            };
            for (size_t k = 0; k < idx.size(); ++k)
            {
                if (used[k] || len (idx[k]) > fast)
                    continue;
                std::vector<size_t> chain { k };
                int other = 0;
                while (true)
                {
                    const size_t last = chain.back();
                    if (len (idx[last]) > fast)
                        break; // a held note ends the figure
                    const int want = chain.size() >= 2 ? key (idx[chain[chain.size() - 2]]) : other;
                    const int m = follower (last, want);
                    if (m < 0)
                        break;
                    chain.push_back ((size_t) m);
                }
                const int a = key (idx[k]);
                bool trillSpeed = true;
                for (size_t c = 0; c + 1 < chain.size(); ++c)
                    trillSpeed &= len (idx[chain[c]]) <= trillFast;
                if (chain.size() >= 4 && trillSpeed)
                {
                    const int b = key (idx[chain[1]]);
                    const int mainKey = juce::jmin (a, b);
                    size_t mainPos = chain[0];
                    for (size_t c : chain)
                        if (key (idx[c]) == mainKey)
                        {
                            mainPos = c;
                            break;
                        }
                    const int mainNote = idx[mainPos];
                    const double s0 = start[(size_t) idx[chain.front()]], e0 = end[(size_t) idx[chain.back()]];
                    for (size_t c : chain)
                    {
                        used[c] = true;
                        if (c != mainPos)
                            written[(size_t) idx[c]] = false;
                    }
                    start[(size_t) mainNote] = s0;
                    end[(size_t) mainNote] = e0;
                    ornamentOf[(size_t) mainNote] = 1;
                    continue;
                }
                if (chain.size() >= 3 && (chain.size() == 3 || len (idx[chain[2]]) > fast || ! trillSpeed) && key (idx[chain[2]]) == a && len (idx[chain[1]]) <= fast
                    && len (idx[chain[2]]) > 2.0 * len (idx[k]))
                {
                    const int mainNote = idx[chain[2]];
                    const double s0 = start[(size_t) idx[k]];
                    written[(size_t) idx[k]] = false;
                    written[(size_t) idx[chain[1]]] = false;
                    used[k] = used[chain[1]] = used[chain[2]] = true;
                    start[(size_t) mainNote] = s0;
                    ornamentOf[(size_t) mainNote] = key (idx[chain[1]]) < a ? 2 : 3;
                }
            }
        }
    }

    // ---- building --------------------------------------------------------------
    struct Chord
    {
        int qs, qe;
        std::vector<int> keys;
    };

    QScore build (const Score& score, const Options& opt)
    {
        QScore q;
        q.title = score.title;
        q.composer = score.composer;
        q.firstTempo = score.tempos.empty() ? 120.0 : score.tempos.front().bpm;

        // Which notes are written: drum tracks and parts that are switched off are not.
        std::vector<bool> written (score.notes.size(), true);
        for (size_t i = 0; i < score.notes.size(); ++i)
        {
            const int p = score.notes[i].part;
            if (p >= 0 && p < (int) score.parts.size() && score.parts[(size_t) p].drums)
                written[i] = false;
            if (p >= 0 && p < (int) opt.partDivision.size() && opt.partDivision[(size_t) p] < 0)
                written[i] = false;
        }

        // Quantisation of performed notes.
        // 1. Chord tones played slightly apart (rolled, or just not exactly together) start
        //    together: onsets that follow each other within 0.07 beats form one onset.
        // 2. Each beat gets the coarsest grid its onsets allow: a 16th grid unless the beat
        //    really moves in 32nds.
        // 3. Note ends are rounded more coarsely the longer the note is: a held chord that
        //    is let go a bit early or late still ends on the beat.
        std::vector<double> start (score.notes.size()), end (score.notes.size());
        std::vector<int> qStart (score.notes.size(), 0), qEnd (score.notes.size(), 0);
        std::vector<int> ornamentOf (score.notes.size(), 0);
        TripletBeats triplets;
        {
            std::vector<int> order;
            for (int i = 0; i < (int) score.notes.size(); ++i)
            {
                start[(size_t) i] = score.notes[(size_t) i].start;
                end[(size_t) i] = score.notes[(size_t) i].start + score.notes[(size_t) i].length;
            }
            detectOrnaments (score, written, start, end, ornamentOf);
            for (int i = 0; i < (int) score.notes.size(); ++i)
                if (written[(size_t) i])
                    order.push_back (i);
            std::sort (order.begin(), order.end(), [&] (int a, int b) { return start[(size_t) a] < start[(size_t) b]; });
            auto offGrid = [] (double t, double perBeat) { return std::abs (t * perBeat - std::round (t * perBeat)) / perBeat; };
            for (size_t i = 0; i < order.size();)
            {
                size_t j = i + 1;
                while (j < order.size() && start[(size_t) order[j]] - start[(size_t) order[j - 1]] <= 0.07
                       && start[(size_t) order[j]] - start[(size_t) order[i]] <= 0.2)
                    ++j;
                if (j - i > 1)
                {
                    // the onset nearest to the 16th grid is where the chord was meant to be
                    double best = start[(size_t) order[i]];
                    for (size_t k = i; k < j; ++k)
                        if (offGrid (start[(size_t) order[k]], 4.0) < offGrid (best, 4.0) - 1.0e-6)
                            best = start[(size_t) order[k]];
                    for (size_t k = i; k < j; ++k)
                        start[(size_t) order[k]] = best;
                }
                i = j;
            }
            // grid per beat: 16ths if the onsets sit near them, 8th triplets (or 16th
            // triplets) if they sit near thirds, else 32nds
            std::map<int, std::vector<double>> onsetsInBeat;
            for (int i : order)
            {
                const double t = start[(size_t) i];
                const int b = (int) std::floor (t + 0.04);
                auto& v = onsetsInBeat[b];
                const double f = t - b;
                if (std::find_if (v.begin(), v.end(), [f] (double x) { return std::abs (x - f) < 0.03; }) == v.end())
                    v.push_back (f);
            }
            std::map<int, int> beatGrid; // beat -> grid in units
            for (auto& [b, fs] : onsetsInBeat)
            {
                double e16 = 0, e8t = 0, e16t = 0;
                for (double f : fs)
                {
                    e16 = juce::jmax (e16, offGrid (f, 4.0));
                    e8t = juce::jmax (e8t, offGrid (f, 3.0));
                    e16t = juce::jmax (e16t, offGrid (f, 6.0));
                }
                int g = U / 4;
                if (e16 > 0.08)
                {
                    g = U / 8;
                    if (fs.size() >= 2 && e8t <= 0.05)
                        g = U / 3;
                    else if (fs.size() >= 3 && e16t <= 0.03)
                        g = U / 6;
                }
                beatGrid[b] = g;
                if (g == U / 3 || g == U / 6)
                    triplets[b] = g;
            }
            auto gridAt = [&] (double t) {
                auto it = beatGrid.find ((int) std::floor (t + 0.04));
                return it == beatGrid.end() ? U / 4 : it->second;
            };
            for (int i : order)
            {
                const int g = gridAt (start[(size_t) i]);
                const int qs = juce::jmax (0, (int) std::round (start[(size_t) i] * U / g) * g);
                const double lenU = (end[(size_t) i] - start[(size_t) i]) * U;
                const int eg = lenU >= 6 * U ? U : lenU >= 3 * U ? U / 2 : lenU >= 3 * U / 4 ? U / 4 : gridAt (end[(size_t) i]);
                int qe = (int) std::round (end[(size_t) i] * U / eg) * eg;
                // an end inside a triplet beat sits on that beat's triplet grid
                if (qe % U != 0)
                    if (auto it = triplets.find (qe / U); it != triplets.end())
                        qe = (int) std::round (qe / (double) it->second) * it->second;
                qe = juce::jmax (qe, qs + g);
                qStart[(size_t) i] = qs;
                qEnd[(size_t) i] = qe;
            }
        }

        int endUnits = 1;
        for (size_t i = 0; i < score.notes.size(); ++i)
            if (written[i])
                endUnits = juce::jmax (endUnits, qEnd[i]);

        // measures from the time signatures
        {
            int pos = 0;
            size_t tsIndex = 0;
            TimeSignature ts = score.timeSigs.empty() ? TimeSignature {} : score.timeSigs.front();
            KeySignature lastKey { -1, -99, false };
            TimeSignature lastTs { -1, 0, 0 };
            while (pos < endUnits)
            {
                while (tsIndex + 1 < score.timeSigs.size() && (int) std::round (score.timeSigs[tsIndex + 1].beat * U) <= pos)
                    ts = score.timeSigs[++tsIndex];
                QMeasure m;
                m.start = pos;
                m.num = juce::jlimit (1, 32, ts.num);
                m.den = ts.den == 1 || ts.den == 2 || ts.den == 4 || ts.den == 8 || ts.den == 16 ? ts.den : 4;
                // sequencers often write doubled meters (8/4, 16/8); engrave them as 4/4 and 8/8
                if ((m.num == 8 && m.den == 4) || (m.num == 16 && m.den == 8) || (m.num == 16 && m.den == 16))
                    m.num /= 2;
                m.length = juce::jmax (1, m.num * 4 * U / m.den);
                // a time signature that starts mid-measure shortens this measure
                if (tsIndex + 1 < score.timeSigs.size())
                {
                    const int nextTs = (int) std::round (score.timeSigs[tsIndex + 1].beat * U);
                    if (nextTs > pos && nextTs < pos + m.length)
                        m.length = nextTs - pos;
                }
                auto ks = score.keySigAt (pos / (double) U);
                m.fifths = juce::jlimit (-7, 7, ks.fifths);
                m.minor = ks.minor;
                m.timeChange = q.measures.empty() || m.num != lastTs.num || m.den != lastTs.den;
                m.keyChange = q.measures.empty() || m.fifths != lastKey.fifths;
                lastTs = { 0, m.num, m.den };
                lastKey = { 0, m.fifths, m.minor };
                q.measures.push_back (m);
                pos += m.length;
            }
            double lastBpm = -1;
            for (auto& t : score.tempos)
            {
                const int tp = (int) std::round (t.beat * U);
                if (std::abs (t.bpm - lastBpm) < 0.01)
                    continue;
                lastBpm = t.bpm;
                for (auto& m : q.measures)
                    if (tp >= m.start && tp < m.start + m.length)
                    {
                        m.tempos.push_back ({ tp - m.start, t.bpm });
                        break;
                    }
            }
        }
        const int totalUnits = q.measures.empty() ? 0 : q.measures.back().start + q.measures.back().length;

        // Parts: the manuals on a grand staff (all together, or one grand staff per manual when
        // asked for: each manual then has its own staves, shown only where it plays), and the
        // pedal on one staff. Manuals in console order from the top: III, II, I.
        std::vector<int> manualGroups; // divisions with their own part, top to bottom
        {
            std::set<int> manuals;
            for (int i = 0; i < (int) score.notes.size(); ++i)
            {
                const int p = score.notes[(size_t) i].part;
                if (! written[(size_t) i] || (p < (int) opt.partIsPedal.size() && opt.partIsPedal[(size_t) p]))
                    continue;
                manuals.insert (p < (int) opt.partDivision.size() ? opt.partDivision[(size_t) p] : 0);
            }
            q.manualsUsed = (int) manuals.size();
            q.perManual = opt.staffPerManual && manuals.size() > 1;
            if (q.perManual)
                manualGroups.assign (manuals.rbegin(), manuals.rend());
            else
                manualGroups.push_back (-1);
        }
        auto groupOf = [&] (int division) {
            for (int g = 0; g < (int) manualGroups.size(); ++g)
                if (manualGroups[(size_t) g] == division || manualGroups[(size_t) g] == -1)
                    return g;
            return 0;
        };
        const int groups = (int) manualGroups.size();
        const int pedalStaff = 2 * groups;

        // Hands: notes that start together are split between right and left hand so that
        // each hand stays near where it was and neither has to stretch further than a tenth.
        std::vector<int> staffOf (score.notes.size(), 0);
        {
            std::map<std::pair<int, int>, std::vector<int>> onsets; // (group, quantised start) -> manual notes
            for (int i = 0; i < (int) score.notes.size(); ++i)
            {
                auto& n = score.notes[(size_t) i];
                if (! written[(size_t) i])
                    continue;
                const bool pedal = n.part < (int) opt.partIsPedal.size() && opt.partIsPedal[(size_t) n.part];
                if (pedal)
                {
                    staffOf[(size_t) i] = pedalStaff;
                    q.hasPedal = true;
                }
                else
                    onsets[{ groupOf (n.part < (int) opt.partDivision.size() ? opt.partDivision[(size_t) n.part] : 0), qStart[(size_t) i] }].push_back (i);
            }
            std::vector<double> rightOf ((size_t) groups, 67.0), leftOf ((size_t) groups, 50.0);
            for (auto& [gt, idx] : onsets)
            {
                const int g = gt.first;
                double& right = rightOf[(size_t) g];
                double& left = leftOf[(size_t) g];
                std::sort (idx.begin(), idx.end(), [&] (int a, int b) { return score.notes[(size_t) a].key < score.notes[(size_t) b].key; });
                const int n = (int) idx.size();
                auto key = [&] (int k) { return score.notes[(size_t) idx[(size_t) k]].key; };
                double bestCost = 1.0e18;
                int bestSplit = 0;
                for (int split = 0; split <= n; ++split) // notes [0, split) left hand, [split, n) right hand
                {
                    double cost = 0;
                    if (split > 0)
                    {
                        double mean = 0;
                        for (int k = 0; k < split; ++k)
                        {
                            mean += key (k);
                            cost += juce::jmax (0, key (k) - 63) * 1.4; // left hand far up: unusual
                        }
                        mean /= split;
                        cost += 0.5 * std::abs (mean - left);
                        cost += juce::jmax (0, key (split - 1) - key (0) - 14) * 4.0;
                    }
                    if (split < n)
                    {
                        double mean = 0;
                        for (int k = split; k < n; ++k)
                        {
                            mean += key (k);
                            cost += juce::jmax (0, 58 - key (k)) * 1.4; // right hand far down: unusual
                        }
                        mean /= (n - split);
                        cost += 0.5 * std::abs (mean - right);
                        cost += juce::jmax (0, key (n - 1) - key (split) - 14) * 4.0;
                    }
                    if (split == 0 || split == n)
                        cost += juce::jmax (0, n - 2) * 2.5;
                    if (cost < bestCost)
                    {
                        bestCost = cost;
                        bestSplit = split;
                    }
                }
                double lm = 0, rm = 0;
                for (int k = 0; k < n; ++k)
                {
                    staffOf[(size_t) idx[(size_t) k]] = 2 * g + (k < bestSplit ? 1 : 0);
                    (k < bestSplit ? lm : rm) += key (k);
                }
                if (bestSplit > 0) left = 0.6 * left + 0.4 * (lm / bestSplit);
                if (bestSplit < n) right = 0.6 * right + 0.4 * (rm / (n - bestSplit));
                // hands drift back towards their home range when one is idle
                left = 0.97 * left + 0.03 * 50;
                right = 0.97 * right + 0.03 * 67;
            }
        }
        // Pedal parts doubling each other an octave apart (a 16' made from an 8' stop on another
        // channel): write the line once, not as octaves.
        std::vector<bool> hidden (score.notes.size(), false);
        {
            std::map<std::pair<int, int>, std::vector<int>> pedalAt; // (start, end) -> notes
            for (int i = 0; i < (int) score.notes.size(); ++i)
                if (staffOf[(size_t) i] == pedalStaff && written[(size_t) i])
                {
                    pedalAt[{ qStart[(size_t) i], qEnd[(size_t) i] }].push_back (i);
                }
            for (auto& [t, idx] : pedalAt)
                for (int a : idx)
                    for (int b : idx)
                        if (score.notes[(size_t) a].part != score.notes[(size_t) b].part
                            && score.notes[(size_t) b].key == score.notes[(size_t) a].key - 12)
                            hidden[(size_t) b] = true;
        }
        const int numStaves = q.hasPedal ? pedalStaff + 1 : pedalStaff;
        std::vector<std::vector<std::pair<int, int>>> staffNotes ((size_t) numStaves); // per staff: (index into score.notes, key)
        for (int i = 0; i < (int) score.notes.size(); ++i)
            if (! hidden[(size_t) i] && written[(size_t) i])
                staffNotes[(size_t) staffOf[(size_t) i]].push_back ({ i, score.notes[(size_t) i].key });

        for (int g = 0; g < groups; ++g)
        {
            QScore::QPart part;
            const int d = manualGroups[(size_t) g];
            part.division = d;
            if (d < 0)
            {
                part.name = opt.manualName;
                part.abbreviation = "Man.";
            }
            else
            {
                const auto name = juce::isPositiveAndBelow (d, (int) opt.divisionNames.size()) ? opt.divisionNames[(size_t) d] : juce::String();
                part.abbreviation = divisionAbbreviation (name, d, false);
                part.name = name.isNotEmpty() ? romanNumeral (d + 1) + ". " + name : "Manual " + romanNumeral (d + 1);
            }
            q.parts.push_back (part);
        }
        if (q.hasPedal)
            q.parts.push_back ({ opt.pedalName, "Ped.", true, -1 });

        for (int s = 0; s < numStaves; ++s)
        {
            QStaff st;
            st.pedal = s == pedalStaff;
            st.bass = st.pedal || s % 2 == 1;
            st.part = st.pedal ? groups : s / 2;

            // chords: same start and end
            // quantise, then close small gaps before the next onset of the same part
            // (performed non-legato would otherwise turn into a rest after every note)
            struct QN { int qs, qe, key, part; };
            std::vector<QN> qn;
            for (auto [idx, key] : staffNotes[(size_t) s])
            {
                auto& n = score.notes[(size_t) idx];
                const int qs = juce::jlimit (0, juce::jmax (0, totalUnits - 1), qStart[(size_t) idx]);
                const int qe = juce::jlimit (qs + 1, totalUnits, qEnd[(size_t) idx]);
                qn.push_back ({ qs, qe, key, n.part });
            }
            std::map<int, std::vector<int>> onsetsByPart;
            for (auto& n : qn)
                onsetsByPart[n.part].push_back (n.qs);
            for (auto& [part, on] : onsetsByPart)
            {
                std::sort (on.begin(), on.end());
                on.erase (std::unique (on.begin(), on.end()), on.end());
            }
            for (auto& n : qn)
            {
                auto& on = onsetsByPart[n.part];
                auto it = std::upper_bound (on.begin(), on.end(), n.qs);
                if (it == on.end())
                    continue;
                const int next = *it, gap = next - n.qe, len = n.qe - n.qs;
                if (gap > 0 && gap <= juce::jmax (U / 4, len) && gap <= U)
                    n.qe = next;
                // legato playing lets a note ring into the next one; cut that small overlap
                // so a running line stays in one voice
                else if (gap < 0 && -gap <= juce::jmax (1, len / 3))
                    n.qe = next;
            }
            // A short note on its own (staccato, an echo) is written as an 8th or 16th up to
            // the next 8th of the bar with a rest after it, not as a lone 32nd.
            for (auto& n : qn)
            {
                if (n.qe - n.qs >= U / 2)
                    continue;
                auto& on = onsetsByPart[n.part];
                auto it = std::upper_bound (on.begin(), on.end(), n.qs);
                const auto tri = triplets.find (n.qs / U);
                const int step = tri != triplets.end() ? tri->second : U / 2;
                int target = (n.qs / step + 1) * step;
                if (it != on.end())
                    target = juce::jmin (target, *it);
                n.qe = juce::jlimit (n.qe, totalUnits, target);
            }
            // Notes struck together and let go at nearly the same time form one chord:
            // their ends are made equal, so the chord is not spread over several voices.
            {
                std::map<int, std::vector<QN*>> byStart;
                for (auto& n : qn)
                    byStart[n.qs].push_back (&n);
                std::map<int, std::vector<int>> onsetsOfKey;
                for (auto& n : qn)
                    onsetsOfKey[n.key].push_back (n.qs);
                for (auto& [k, on] : onsetsOfKey)
                    std::sort (on.begin(), on.end());
                for (auto& [qs, group] : byStart)
                {
                    std::sort (group.begin(), group.end(), [] (QN* a, QN* b) { return a->qe < b->qe; });
                    for (size_t i = 0; i < group.size();)
                    {
                        size_t j = i + 1;
                        const int tol = juce::jmax (U / 2, (group[i]->qe - qs) / 4);
                        while (j < group.size() && group[j]->qe - group[i]->qe <= tol)
                            ++j;
                        const int e = group[j - 1]->qe;
                        for (size_t k = i; k < j; ++k)
                        {
                            auto& on = onsetsOfKey[group[k]->key];
                            auto it = std::upper_bound (on.begin(), on.end(), qs);
                            group[k]->qe = it == on.end() ? e : juce::jmin (e, *it);
                        }
                        i = j;
                    }
                }
            }

            std::map<std::pair<int, int>, int> ornamentAt; // (start, key) -> ornament
            for (auto [idx, key] : staffNotes[(size_t) s])
                if (ornamentOf[(size_t) idx] != 0)
                    ornamentAt[{ qStart[(size_t) idx], key }] = ornamentOf[(size_t) idx];
            std::map<std::pair<int, int>, int> divisionOf; // (start, key) -> division
            for (auto& n : qn)
                divisionOf[{ n.qs, n.key }] = n.part < (int) opt.partDivision.size() ? opt.partDivision[(size_t) n.part] : -1;
            // Voices, the way a copyist writes them:
            // 1. Lines: every note continues the free line nearest in pitch (a melody, the alto,
            //    one note of held chords...). Lines know nothing about rhythm yet.
            // 2. Per bar the lines are split into an upper voice (stems up) and a lower voice
            //    (stems down), or kept in one voice when they all move in the same rhythm.
            //    The split chosen is the one where the lines put together clash least.
            // 3. Inside a voice, notes with the same start and end are chords. A note that the
            //    voice cannot hold (another line moves under it) goes to a third voice when it
            //    is long, or is written shorter when it only overlaps a little.
            struct VoiceState
            {
                int lastEnd = 0;
                std::vector<Chord> chords;
                double pitchSum = 0;
                int pitchCount = 0;
            };
            enum { upperSlot = 0, lowerSlot = 1, upperExtra = 2, lowerExtra = 3, numSlots = 4 };
            std::vector<int> lineOf (qn.size(), -1);
            {
                std::vector<int> order (qn.size());
                for (size_t i = 0; i < qn.size(); ++i)
                    order[i] = (int) i;
                std::sort (order.begin(), order.end(), [&] (int a2, int b2) {
                    if (qn[(size_t) a2].qs != qn[(size_t) b2].qs) return qn[(size_t) a2].qs < qn[(size_t) b2].qs;
                    return qn[(size_t) a2].key > qn[(size_t) b2].key;
                });
                struct LineState { int lastEnd, lastKey; };
                std::vector<LineState> lines;
                for (int i : order)
                {
                    auto& n = qn[(size_t) i];
                    int best = -1;
                    double bestCost = 1.0e9;
                    for (int l = 0; l < (int) lines.size(); ++l)
                        if (lines[(size_t) l].lastEnd <= n.qs)
                        {
                            const double idle = (n.qs - lines[(size_t) l].lastEnd) / (double) (4 * U);
                            const double cost = std::abs (lines[(size_t) l].lastKey - n.key) + 2.0 * juce::jmin (idle, 4.0);
                            if (cost < bestCost)
                            {
                                bestCost = cost;
                                best = l;
                            }
                        }
                    if (best < 0 || bestCost > 14.0)
                    {
                        lines.push_back ({ 0, n.key });
                        best = (int) lines.size() - 1;
                    }
                    lines[(size_t) best] = { n.qe, n.key };
                    lineOf[(size_t) i] = best;
                }
            }
            // how much the notes put into one voice would clash: overlapping notes that are
            // not one chord
            auto clash = [&] (const std::vector<int>& idx) {
                double c = 0;
                for (size_t x = 0; x < idx.size(); ++x)
                    for (size_t y = x + 1; y < idx.size(); ++y)
                    {
                        auto& a2 = qn[(size_t) idx[x]];
                        auto& b2 = qn[(size_t) idx[y]];
                        if (a2.qs == b2.qs && a2.qe == b2.qe)
                            continue;
                        const int ov = juce::jmin (a2.qe, b2.qe) - juce::jmax (a2.qs, b2.qs);
                        if (ov > 0)
                            c += ov + U / 4; // every clash costs something, long ones more
                    }
                return c;
            };
            std::vector<int> slotOf (qn.size(), upperSlot);
            {
                std::vector<std::vector<int>> byMeasure (q.measures.size());
                for (size_t i = 0; i < qn.size(); ++i)
                {
                    int lo = 0, hi = (int) q.measures.size() - 1;
                    while (lo < hi)
                    {
                        const int mid = (lo + hi + 1) / 2;
                        if (q.measures[(size_t) mid].start <= qn[i].qs) lo = mid; else hi = mid - 1;
                    }
                    byMeasure[(size_t) lo].push_back ((int) i);
                }
                std::vector<int> carried; // notes from earlier bars still sounding
                for (size_t mi = 0; mi < q.measures.size(); ++mi)
                {
                    const int ms = q.measures[mi].start;
                    carried.erase (std::remove_if (carried.begin(), carried.end(), [&] (int i) { return qn[(size_t) i].qe <= ms; }), carried.end());
                    auto& here = byMeasure[mi];
                    if (here.empty())
                        continue;
                    std::map<int, std::vector<int>> lineNotes;
                    for (int i : here)
                        lineNotes[lineOf[(size_t) i]].push_back (i);
                    std::vector<std::pair<double, int>> ranked; // (mean pitch, line)
                    for (auto& [l, idx] : lineNotes)
                    {
                        double sum = 0;
                        for (int i : idx)
                            sum += qn[(size_t) i].key;
                        ranked.push_back ({ sum / (double) idx.size(), l });
                    }
                    std::sort (ranked.begin(), ranked.end(), [] (auto& x, auto& y) { return x.first > y.first; });
                    const int L = (int) ranked.size();
                    int bestK = L;
                    double bestCost = 1.0e18;
                    for (int k = L; k >= 1; --k) // k lines in the upper voice, k == L: one voice
                    {
                        std::vector<int> up, low;
                        for (int r = 0; r < L; ++r)
                            for (int i : lineNotes[ranked[(size_t) r].second])
                                (r < k ? up : low).push_back (i);
                        for (int i : carried)
                            (slotOf[(size_t) i] % 2 == 0 ? up : low).push_back (i);
                        double cost = clash (up) + clash (low) + (k < L ? U / 2 : 0);
                        // the lower voice should stay below the upper one
                        if (k < L)
                            cost += juce::jmax (0.0, ranked[(size_t) k].first - ranked[(size_t) k - 1].first) * U;
                        if (cost < bestCost - 1.0e-9)
                        {
                            bestCost = cost;
                            bestK = k;
                        }
                    }
                    for (int r = 0; r < L; ++r)
                        for (int i : lineNotes[ranked[(size_t) r].second])
                            slotOf[(size_t) i] = r < bestK ? upperSlot : lowerSlot;
                    for (int i : here)
                        if (qn[(size_t) i].qe > ms + q.measures[mi].length)
                            carried.push_back (i);
                }
            }
            // chords per slot, then each slot made playable as one rhythm
            std::vector<VoiceState> vs (numSlots);
            {
                std::array<std::map<std::pair<int, int>, std::vector<int>>, numSlots> chordMaps;
                for (size_t i = 0; i < qn.size(); ++i)
                {
                    auto& keys = chordMaps[(size_t) slotOf[i]][{ qn[i].qs, qn[i].qe }];
                    if (std::find (keys.begin(), keys.end(), qn[i].key) == keys.end())
                        keys.push_back (qn[i].key);
                }
                auto place = [&] (int slot, Chord c) {
                    auto& v = vs[(size_t) slot];
                    v.chords.push_back (c);
                    v.lastEnd = juce::jmax (v.lastEnd, c.qe);
                };
                for (int slot : { upperSlot, lowerSlot })
                {
                    const int extra = slot + 2;
                    std::vector<Chord> cs;
                    for (auto& [range, keys] : chordMaps[(size_t) slot])
                    {
                        Chord c { range.first, range.second, keys };
                        std::sort (c.keys.begin(), c.keys.end());
                        cs.push_back (c);
                    }
                    // in start order; at the same start the longer chord first
                    std::stable_sort (cs.begin(), cs.end(), [] (const Chord& x, const Chord& y) {
                        return x.qs != y.qs ? x.qs < y.qs : x.qe > y.qe;
                    });
                    for (auto& c : cs)
                    {
                        auto& v = vs[(size_t) slot];
                        if (v.chords.empty() || v.chords.back().qe <= c.qs)
                        {
                            place (slot, c);
                            continue;
                        }
                        auto& prev = v.chords.back();
                        if (prev.qs == c.qs)
                        {
                            // struck together, let go at different times
                            if (prev.qe - c.qe <= juce::jmax (U / 4, (c.qe - c.qs) / 3))
                            {
                                for (int k : c.keys)
                                    if (std::find (prev.keys.begin(), prev.keys.end(), k) == prev.keys.end())
                                        prev.keys.push_back (k);
                                std::sort (prev.keys.begin(), prev.keys.end());
                                continue;
                            }
                            // the longer one is held in an extra voice when that is free
                            if (vs[(size_t) extra].lastEnd <= prev.qs)
                            {
                                Chord held = prev;
                                v.chords.pop_back();
                                place (extra, held);
                                v.lastEnd = v.chords.empty() ? 0 : v.chords.back().qe;
                                place (slot, c);
                                continue;
                            }
                            // else the shorter rhythm wins: the held notes are written shorter
                            for (int k : c.keys)
                                if (std::find (prev.keys.begin(), prev.keys.end(), k) == prev.keys.end())
                                    prev.keys.push_back (k);
                            std::sort (prev.keys.begin(), prev.keys.end());
                            prev.qe = c.qe;
                            v.lastEnd = prev.qe;
                            continue;
                        }
                        // c starts while prev still sounds
                        const int lost = prev.qe - c.qs;
                        if (lost >= U && vs[(size_t) extra].lastEnd <= prev.qs)
                        {
                            Chord held = prev;
                            v.chords.pop_back();
                            place (extra, held);
                            v.lastEnd = v.chords.empty() ? 0 : v.chords.back().qe;
                            place (slot, c);
                            continue;
                        }
                        prev.qe = c.qs; // a little overlap: written shorter
                        v.lastEnd = c.qs;
                        place (slot, c);
                    }
                }
                // A note in an extra voice moves to the other main voice when that one is silent
                // there and the pitches stay in order (upper voice above lower voice): two voices
                // read better than three, and a third voice with stems the same way collides.
                for (int slot : { upperExtra, lowerExtra })
                {
                    const int target = slot == upperExtra ? lowerSlot : upperSlot;
                    const int same = slot - 2;
                    auto& cs = vs[(size_t) slot].chords;
                    for (size_t ci = 0; ci < cs.size();)
                    {
                        auto& c = cs[ci];
                        bool free = true, ordered = true;
                        for (auto& o : vs[(size_t) target].chords)
                            if (o.qs < c.qe && o.qe > c.qs)
                                free = false;
                        for (auto& o : vs[(size_t) same].chords)
                            if (o.qs < c.qe && o.qe > c.qs)
                                ordered &= target == upperSlot ? c.keys.front() >= o.keys.back() : c.keys.back() <= o.keys.front();
                        if (free && ordered)
                        {
                            auto& tv = vs[(size_t) target].chords;
                            tv.insert (std::upper_bound (tv.begin(), tv.end(), c, [] (const Chord& x, const Chord& y) { return x.qs < y.qs; }), c);
                            cs.erase (cs.begin() + (long) ci);
                        }
                        else
                            ++ci;
                    }
                }
                // the extra voices themselves may not overlap
                for (int slot : { upperExtra, lowerExtra })
                {
                    auto& cs = vs[(size_t) slot].chords;
                    std::stable_sort (cs.begin(), cs.end(), [] (const Chord& x, const Chord& y) { return x.qs < y.qs; });
                    std::vector<Chord> out;
                    for (auto& c : cs)
                    {
                        if (! out.empty() && out.back().qe > c.qs)
                        {
                            if (out.back().qs == c.qs)
                            {
                                for (int k : c.keys)
                                    if (std::find (out.back().keys.begin(), out.back().keys.end(), k) == out.back().keys.end())
                                        out.back().keys.push_back (k);
                                std::sort (out.back().keys.begin(), out.back().keys.end());
                                out.back().qe = juce::jmin (out.back().qe, c.qe);
                                continue;
                            }
                            out.back().qe = c.qs;
                        }
                        out.push_back (c);
                    }
                    cs = std::move (out);
                }
                for (auto& v : vs)
                {
                    v.lastEnd = v.chords.empty() ? 0 : v.chords.back().qe;
                    for (auto& c : v.chords)
                        for (int k : c.keys)
                        {
                            v.pitchSum += k;
                            v.pitchCount++;
                        }
                }
                // unused extra voices at the end are dropped (the slot numbers keep the stems right)
                while (vs.size() > 1 && vs.back().chords.empty())
                    vs.pop_back();
            }
            // A rest of up to a beat inside a voice, while another voice holds a note across
            // it, could only be printed hidden: the note before it is written longer instead,
            // as a copyist would, so no note is left with a flag and nothing after it.
            for (int v = 0; v < (int) vs.size(); ++v)
            {
                auto& cs = vs[(size_t) v].chords;
                for (size_t i = 0; i + 1 < cs.size(); ++i)
                {
                    const int g0 = cs[i].qe, g1 = cs[i + 1].qs;
                    if (g1 <= g0 || g1 - g0 > U)
                        continue;
                    bool covered = false;
                    for (int w = 0; w < (int) vs.size() && ! covered; ++w)
                        if (w != v)
                            for (auto& c : vs[(size_t) w].chords)
                                if (c.qs <= g0 && c.qe >= g1)
                                {
                                    covered = true;
                                    break;
                                }
                    if (covered)
                        cs[i].qe = g1;
                }
            }
            if (st.bass)
            {
                int sum = 0, cnt = 0;
                for (auto& v : vs)
                    for (auto& c : v.chords)
                        for (int k : c.keys)
                        {
                            sum += k;
                            ++cnt;
                        }
                st.bass = cnt == 0 || sum / (double) cnt < 60.0;
            }

            st.numVoices = (int) vs.size();
            st.voices.assign (vs.size(), std::vector<std::vector<QEvent>> (q.measures.size()));

            for (int v = 0; v < (int) vs.size(); ++v)
            {
                auto& voiceChords = vs[(size_t) v].chords;
                size_t ci = 0;
                for (int mi = 0; mi < (int) q.measures.size(); ++mi)
                {
                    auto& m = q.measures[(size_t) mi];
                    const int ms = m.start, me = m.start + m.length;
                    auto& evs = st.voices[(size_t) v][(size_t) mi];
                    while (ci < voiceChords.size() && voiceChords[ci].qe <= ms)
                        ++ci;
                    int pos = ms;
                    bool any = false;
                    for (size_t k = ci; k < voiceChords.size() && voiceChords[k].qs < me; ++k)
                    {
                        auto& c = voiceChords[k];
                        const int s0 = juce::jmax (c.qs, ms), e0 = juce::jmin (c.qe, me);
                        if (s0 > pos)
                            for (auto& val : decompose (pos - ms, s0 - pos, m, triplets, true))
                            {
                                QEvent r;
                                r.start = pos - ms;
                                r.dur = val.units;
                                r.rest = true;
                                r.typeUnits = val.typeUnits;
                                r.dots = val.dots;
                            r.triplet = val.triplet;
                                r.triplet = val.triplet;
                                evs.push_back (r);
                                pos += val.units;
                            }
                        auto pieces = decompose (s0 - ms, e0 - s0, m, triplets);
                        for (size_t p = 0; p < pieces.size(); ++p)
                        {
                            QEvent e;
                            e.start = pos - ms;
                            e.dur = pieces[p].units;
                            e.typeUnits = pieces[p].typeUnits;
                            e.dots = pieces[p].dots;
                            e.triplet = pieces[p].triplet;
                            for (int key : c.keys)
                            {
                                QNote n;
                                spell (key, m.fifths, n);
                                {
                                    auto it = divisionOf.find ({ c.qs, key });
                                    if (it == divisionOf.end()) // joined into another chord: nearest earlier start
                                        for (auto& [k2, d2] : divisionOf)
                                            if (k2.second == key && k2.first <= c.qs)
                                                n.division = d2;
                                    if (it != divisionOf.end())
                                        n.division = it->second;
                                }
                                n.tieStop = p > 0 || (p == 0 && c.qs < ms);
                                if (! n.tieStop)
                                    if (auto it = ornamentAt.find ({ c.qs, key }); it != ornamentAt.end())
                                        n.ornament = it->second;
                                n.tieStart = p + 1 < pieces.size() || (p + 1 == pieces.size() && c.qe > me);
                                e.notes.push_back (n);
                            }
                            std::reverse (e.notes.begin(), e.notes.end()); // low to high
                            evs.push_back (e);
                            pos += pieces[p].units;
                        }
                        any = true;
                    }
                    if (any && pos < me)
                        for (auto& val : decompose (pos - ms, me - pos, m, triplets, true))
                        {
                            QEvent r;
                            r.start = pos - ms;
                            r.dur = val.units;
                            r.rest = true;
                            r.typeUnits = val.typeUnits;
                            r.dots = val.dots;
                            r.triplet = val.triplet;
                            evs.push_back (r);
                            pos += val.units;
                        }
                    if (! any && v == 0)
                    {
                        QEvent r;
                        r.start = 0;
                        r.dur = m.length;
                        r.rest = true;
                        r.wholeMeasureRest = true;
                        r.typeUnits = 4 * U;
                        evs.push_back (r);
                    }
                }
            }

            // stems, beams, accidentals per measure
            for (int mi = 0; mi < (int) q.measures.size(); ++mi)
            {
                auto& m = q.measures[(size_t) mi];
                int activeVoices = 0;
                for (auto& v : st.voices)
                    for (auto& e : v[(size_t) mi])
                        if (! e.rest)
                        {
                            ++activeVoices;
                            break;
                        }
                const bool compound = m.den == 8 && m.num % 3 == 0 && m.num > 3;
                const int group = compound || m.den == 8 ? 3 * U / 2 : U;

                for (int v = 0; v < (int) st.voices.size(); ++v)
                {
                    auto& evs = st.voices[(size_t) v][(size_t) mi];
                    for (auto& e : evs)
                        e.stem = activeVoices > 1 && ! e.rest ? (v % 2 == 0 ? 1 : 2) : 0;

                    // triplet brackets: one group per beat
                    for (size_t i = 0; i < evs.size(); ++i)
                    {
                        if (! evs[i].triplet)
                            continue;
                        const int b = (m.start + evs[i].start) / U;
                        size_t j = i;
                        while (j + 1 < evs.size() && evs[j + 1].triplet && (m.start + evs[j + 1].start) / U == b)
                            ++j;
                        evs[i].tupletStart = true;
                        evs[j].tupletStop = true;
                        i = j;
                    }

                    size_t i = 0;
                    while (i < evs.size())
                    {
                        auto beamable = [&] (const QEvent& e) { return ! e.rest && e.typeUnits <= U / 2; };
                        if (! beamable (evs[i]))
                        {
                            ++i;
                            continue;
                        }
                        size_t j = i + 1;
                        const int g = evs[i].start / group;
                        while (j < evs.size() && beamable (evs[j]) && evs[j].start / group == g)
                            ++j;
                        if (j - i >= 2)
                        {
                            evs[i].beam = 1;
                            for (size_t k = i + 1; k + 1 < j; ++k)
                                evs[k].beam = 2;
                            evs[j - 1].beam = 3;
                        }
                        i = j;
                    }
                }

                // rests are only printed where no other voice of this staff is sounding;
                // secondary voices never print rests
                for (int v = 0; v < (int) st.voices.size(); ++v)
                    for (auto& e : st.voices[(size_t) v][(size_t) mi])
                    {
                        if (! e.rest)
                            continue;
                        if (v > 0)
                        {
                            e.hidden = true;
                            continue;
                        }
                        for (int w = 1; w < (int) st.voices.size() && ! e.hidden; ++w)
                            for (auto& o : st.voices[(size_t) w][(size_t) mi])
                                if (! o.rest && o.start < e.start + e.dur && o.start + o.dur > e.start)
                                {
                                    e.hidden = true;
                                    break;
                                }
                    }

                // accidentals: walk all notes of the staff in time order
                std::vector<std::pair<int, QNote*>> ordered;
                for (auto& v : st.voices)
                    for (auto& e : v[(size_t) mi])
                        for (auto& n : e.notes)
                            ordered.push_back ({ e.start, &n });
                std::stable_sort (ordered.begin(), ordered.end(), [] (auto& a, auto& b) { return a.first < b.first; });
                std::map<int, int> state; // step*16+octave -> alter in effect
                for (auto& [pos, n] : ordered)
                {
                    const int slot = n->step * 16 + n->octave;
                    const int current = state.count (slot) ? state[slot] : keySigAlter (n->step, m.fifths);
                    if (n->tieStop && pos == 0)
                    {
                        state[slot] = n->alter;
                        continue;
                    }
                    if (n->alter != current)
                        n->showAccidental = n->alter;
                    state[slot] = n->alter;
                }
            }
            // Clef changes on the manual staves: a hand that stays high (or low) for more than
            // a bar gets the other clef instead of a stack of ledger lines.
            {
                // ledger lines a note needs in each clef (diatonic steps from the staff)
                const size_t nm = q.measures.size();
                std::vector<double> inBass (nm, 0), inTreble (nm, 0);
                std::vector<int> count (nm, 0);
                auto ledgers = [] (int d, int bottom, int top) {
                    return d > top + 1 ? (d - top) / 2 : d < bottom - 1 ? (bottom - d) / 2 : 0;
                };
                for (size_t mi = 0; mi < nm; ++mi)
                    for (auto& voice : st.voices)
                        for (auto& e : voice[mi])
                            for (auto& n : e.notes)
                            {
                                const int d = n.step + 7 * n.octave;
                                inBass[mi] += ledgers (d, 4 + 7 * 2, 5 + 7 * 3);   // G2 .. A3
                                inTreble[mi] += ledgers (d, 2 + 7 * 4, 3 + 7 * 5); // E4 .. F5
                                ++count[mi];
                            }
                // the other clef must save most notes a ledger line, in this bar and the next
                // (a bar with only a note or two keeps the clef it has)
                auto wantsTreble = [&] (size_t mi) { return count[mi] >= 3 && inBass[mi] - inTreble[mi] >= 0.75 * count[mi]; };
                auto wantsBass = [&] (size_t mi) { return count[mi] >= 3 && inTreble[mi] - inBass[mi] >= 0.75 * count[mi]; };
                std::vector<double> mean (nm, -1);
                for (size_t mi = 0; mi < nm; ++mi)
                    if (count[mi] > 0)
                        mean[mi] = 0;
                st.clefBass.assign (nm, st.bass ? 1 : 0);
                bool bass = st.bass;
                if (! st.pedal)
                    for (size_t mi = 0; mi < nm; ++mi)
                    {
                        if (mean[mi] >= 0)
                        {
                            size_t next = mi + 1;
                            while (next < nm && mean[next] < 0)
                                ++next;
                            if (bass && wantsTreble (mi) && (next >= nm || wantsTreble (next)))
                                bass = false;
                            else if (! bass && wantsBass (mi) && (next >= nm || wantsBass (next)))
                                bass = true;
                        }
                        st.clefBass[mi] = bass ? 1 : 0;
                    }
                if (nm > 0)
                    st.bass = st.clefBass[0] != 0;
            }
            q.staves.push_back (std::move (st));
        }
        return q;
    }

    // Is the triplet group starting at evs[i] marked with a "3"? j becomes its last event.
    bool tupletShown (const std::vector<QEvent>& evs, size_t i, size_t& j, bool& bracket)
    {
        if (! evs[i].tupletStart)
            return false;
        j = i;
        while (j + 1 < evs.size() && ! evs[j].tupletStop)
            ++j;
        int visibleNotes = 0;
        bool hiddenRest = false;
        bracket = evs[i].beam != 1 || evs[j].beam != 3;
        for (size_t k = i; k <= j; ++k)
        {
            bracket |= evs[k].rest || evs[k].beam == 0;
            hiddenRest |= evs[k].rest && evs[k].hidden;
            visibleNotes += evs[k].rest ? 0 : 1;
        }
        if (hiddenRest)
            bracket = false;
        return ! hiddenRest || visibleNotes >= 2;
    }

    juce::String typeName (int typeUnits)
    {
        switch (typeUnits)
        {
            case 4 * U: return "whole";
            case 2 * U: return "half";
            case U: return "quarter";
            case U / 2: return "eighth";
            case U / 4: return "16th";
            default: return "32nd";
        }
    }

    juce::String esc (const juce::String& s)
    {
        return s.replace ("&", "&amp;").replace ("<", "&lt;").replace (">", "&gt;").replace ("\"", "&quot;");
    }

    juce::String keyName (int fifths, bool minor, bool lily)
    {
        static const char* major[] = { "Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#" };
        static const char* minorK[] = { "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#", "G#", "D#", "A#" };
        juce::String k = minor ? minorK[fifths + 7] : major[fifths + 7];
        if (! lily)
            return k + (minor ? "m" : "");
        auto lower = k.substring (0, 1).toLowerCase();
        if (k.endsWith ("b")) lower << "es";
        if (k.endsWith ("#")) lower << "is";
        return lower + (minor ? " \\minor" : " \\major");
    }
} // namespace

juce::String manualColour (int division)
{
    static const char* colours[] = { "#000000", "#1E4FA8", "#1E7A3A", "#7A2E8C", "#B05A00", "#8C1E3A" };
    return colours[juce::jlimit (0, 5, division)];
}

//==============================================================================
juce::String printedStopName (const juce::String& presetName, int octave)
{
    // drop qualifiers such as "(ped.)", "(pos.)", "(g.o.)"
    juce::String name;
    for (int i = 0; i < presetName.length(); ++i)
    {
        if (presetName[i] == '(')
        {
            const int close = presetName.indexOfChar (i, ')');
            if (close > i && close - i <= 8)
            {
                i = close;
                continue;
            }
        }
        name << juce::String::charToString (presetName[i]);
    }
    name = name.trim();
    static const std::pair<double, const char*> nice[] = {
        { 64, "64" }, { 32, "32" }, { 21.333, "21 1/3" }, { 16, "16" }, { 10.667, "10 2/3" }, { 8, "8" }, { 6.4, "6 2/5" },
        { 5.333, "5 1/3" }, { 4, "4" }, { 3.2, "3 1/5" }, { 2.667, "2 2/3" }, { 2.286, "2 2/7" }, { 2, "2" }, { 1.6, "1 3/5" },
        { 1.333, "1 1/3" }, { 1.143, "1 1/7" }, { 1, "1" }, { 0.8, "4/5" }, { 0.667, "2/3" }, { 0.5, "1/2" }
    };
    // every footage in the name ("16", "8+4", "2 2/3", "Montre 8 Flute 4") gets its foot mark,
    // moved by the octave the stop sounds in
    const std::string text = name.toStdString();
    static const std::regex number (R"((\d+)(?: (\d+)/(\d+))?)");
    std::string out;
    size_t last = 0;
    bool any = false;
    for (auto it = std::sregex_iterator (text.begin(), text.end(), number); it != std::sregex_iterator(); ++it)
    {
        const auto& m = *it;
        const size_t pos = (size_t) m.position (0);
        // a number glued to letters after it ("Mixture3rangs") is not a footage
        const size_t after = pos + (size_t) m.length (0);
        if (after < text.size() && std::isalpha ((unsigned char) text[after]))
            continue;
        double v = std::stod (m.str (1));
        if (m[2].matched && std::stod (m.str (3)) > 0)
            v += std::stod (m.str (2)) / std::stod (m.str (3));
        v *= std::pow (2.0, -octave);
        std::string shownValue;
        for (auto& [nv, txt] : nice)
            if (std::abs (nv - v) < 0.02 * nv)
                shownValue = txt;
        if (shownValue.empty())
            shownValue = juce::String (v, 2).toStdString();
        out += text.substr (last, pos - last);
        if (! out.empty() && (std::isalpha ((unsigned char) out.back()) || out.back() == '.'))
            out += ' ';
        out += shownValue + "\u2032";
        last = after;
        any = true;
    }
    out += text.substr (last);
    auto result = juce::String (juce::CharPointer_UTF8 (out.c_str()));
    if (! any)
        return octave != 0 ? result + (octave < 0 ? " (8va bassa)" : " (8va)") : result;
    // "8'+4'" -> "8' + 4'"
    result = result.replace ("+", " + ");
    while (result.contains ("  "))
        result = result.replace ("  ", " ");
    return result.trim();
}

juce::String divisionAbbreviation (const juce::String& name, int division, bool pedal)
{
    const auto n = name.toLowerCase();
    static const std::pair<const char*, const char*> known[] = {
        { "hauptwerk", "Hw." }, { "rückpositiv", "Rp." }, { "rueckpositiv", "Rp." }, { "brustpositiv", "Bp." },
        { "oberwerk", "Ow." }, { "brustwerk", "Bw." }, { "kronwerk", "Kw." }, { "fernwerk", "Fw." }, { "schwell", "Sw." },
        { "grand orgue", "G.O." }, { "grand-orgue", "G.O." }, { "great", "Gt." }, { "positif", "Pos." }, { "positiv", "Pos." },
        { "récit", "Réc." }, { "recit", "Réc." }, { "swell", "Sw." }, { "choir", "Ch." }, { "solo", "Solo" },
        { "bombarde", "Bomb." }, { "hoofdwerk", "Hw." }, { "rugwerk", "Rw." }, { "zwelwerk", "Zw." }, { "bovenwerk", "Bw." },
        { "órgano mayor", "O.M." }, { "organo mayor", "O.M." }, { "grand'organo", "G.O." }, { "grand’organo", "G.O." },
        { "espressivo", "Espr." }, { "expresivo", "Expr." }, { "écho", "Écho" }, { "echo", "Echo" }, { "eco", "Eco" },
        { "pedal", "Ped." }, { "pédale", "Ped." }, { "pedale", "Ped." }, { "pedaal", "Ped." }
    };
    for (auto& [word, abbr] : known)
        if (n.contains (juce::String::fromUTF8 (word)))
            return juce::String::fromUTF8 (abbr);
    return pedal ? juce::String ("Ped.") : "Man. " + romanNumeral (division + 1);
}

namespace
{
    // Registration as organ editions print it: the full list where a division first plays,
    // later only what changes ("+ Trompette 8', - Cornet") at the first note after the change.
    // Changes a moment apart are taken together, and a change nobody plays is not printed.
    struct RegistrationText
    {
        int measure = 0, pos = 0, part = 0;
        juce::String text, division; // division: "Hw." etc.
    };
    std::vector<RegistrationText> registrationTexts (const Score& score, const Options& opt, const QScore& q)
    {
        std::vector<RegistrationText> out;
        if (opt.registrations.empty() || q.measures.empty())
            return out;
        std::map<int, std::vector<const Options::RegistrationMark*>> byDivision;
        for (auto& m : opt.registrations)
            byDivision[m.division].push_back (&m);
        for (auto& [d, marks] : byDivision)
        {
            std::stable_sort (marks.begin(), marks.end(), [] (auto* a, auto* b) { return a->beat < b->beat; });
            // where this division plays
            std::vector<double> onsets;
            for (auto& n : score.notes)
                if (n.part >= 0 && n.part < (int) opt.partDivision.size() && opt.partDivision[(size_t) n.part] == d
                    && ! (n.part < (int) score.parts.size() && score.parts[(size_t) n.part].drums))
                    onsets.push_back (n.start);
            std::sort (onsets.begin(), onsets.end());
            const bool pedal = juce::isPositiveAndBelow (d, (int) opt.divisionIsPedal.size()) && opt.divisionIsPedal[(size_t) d];
            const auto name = juce::isPositiveAndBelow (d, (int) opt.divisionNames.size()) ? opt.divisionNames[(size_t) d] : juce::String();
            const auto abbr = divisionAbbreviation (name, d, pedal);
            juce::StringArray shown;
            int lastMeasure = -1000;
            const double barBeats = q.measures.front().length / (double) U;
            for (size_t i = 0; i < marks.size(); ++i)
            {
                // a change that is replaced within two bars is not worth printing (the next
                // one says where it all leads)
                if (i + 1 < marks.size() && marks[i + 1]->beat - marks[i]->beat < 2.0 * barBeats)
                    continue;
                const double next = i + 1 < marks.size() ? marks[i + 1]->beat : 1.0e18;
                auto it = std::lower_bound (onsets.begin(), onsets.end(), marks[i]->beat - 0.25);
                if (it == onsets.end() || *it >= next)
                    continue; // nothing is played with it
                const auto& now = marks[i]->stops;
                juce::String text;
                if (shown.isEmpty())
                    text = now.joinIntoString (", ");
                else
                {
                    juce::StringArray added, removed;
                    for (auto& s : now)
                        if (! shown.contains (s))
                            added.add ("+ " + s);
                    for (auto& s : shown)
                        if (! now.contains (s))
                            removed.add (juce::String::fromUTF8 ("\u2212 ") + s);
                    if (added.isEmpty() && removed.isEmpty())
                        continue;
                    // a small registration, or most of it new: say it in full
                    if (now.size() <= 2 || added.size() + removed.size() >= juce::jmax (3, now.size()))
                        text = now.joinIntoString (", ");
                    else
                    {
                        added.addArray (removed);
                        text = added.joinIntoString (", ");
                    }
                }
                if (now.isEmpty())
                    continue;
                const int at = juce::jmax (0, (int) std::round (*it * 4.0) * (U / 4));
                int mi = 0;
                while (mi + 1 < (int) q.measures.size() && q.measures[(size_t) mi + 1].start <= at)
                    ++mi;
                // at most one registration per division every four bars; what changes in
                // between is said with the next one
                if (mi - lastMeasure < 4)
                    continue;
                lastMeasure = mi;
                shown = now;
                // with a part per manual the staff's name already says which division it is
                out.push_back ({ mi, juce::jmin (at - q.measures[(size_t) mi].start, q.measures[(size_t) mi].length - 1), q.partOf (d, pedal),
                                 q.perManual ? text : abbr + " " + text, abbr });
            }
        }
        std::stable_sort (out.begin(), out.end(), [] (auto& a, auto& b) { return std::tie (a.measure, a.pos) < std::tie (b.measure, b.pos); });
        return out;
    }

    // the registration texts that belong to an event (top staff of its part, first voice)
    juce::StringArray registrationAt (const std::vector<RegistrationText>& texts, const QScore& q, int staff, int voice, int measure, const QEvent& e)
    {
        juce::StringArray out;
        if (voice != 0 || ! juce::isPositiveAndBelow (staff, (int) q.staves.size()))
            return out;
        const int part = q.staves[(size_t) staff].part;
        for (int s = 0; s < staff; ++s)
            if (q.staves[(size_t) s].part == part)
                return out; // not the top staff of its part
        for (auto& t : texts)
            if (t.measure == measure && t.part == part && t.pos >= e.start && t.pos < e.start + juce::jmax (1, e.dur))
                out.add (t.text);
        return out;
    }
} // namespace

std::vector<PartInfo> partLayout (const Score& score, const Options& opt)
{
    auto q = build (score, opt);
    std::vector<PartInfo> out;
    for (auto& p : q.parts)
        out.push_back ({ p.division, p.pedal });
    return out;
}

juce::String toMusicXML (const Score& score, const Options& opt)
{
    auto q = build (score, opt);
    const auto regTexts = registrationTexts (score, opt, q);
    juce::MemoryOutputStream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 4.0 Partwise//EN\" \"http://www.musicxml.org/dtds/partwise.dtd\">\n"
      << "<score-partwise version=\"4.0\">\n";
    if (q.title.isNotEmpty())
        o << "  <work><work-title>" << esc (q.title) << "</work-title></work>\n";
    o << "  <identification>";
    if (q.composer.isNotEmpty())
        o << "<creator type=\"composer\">" << esc (q.composer) << "</creator>";
    o << "<encoding><software>Orgelwerk</software></encoding></identification>\n";
    o << "  <part-list>\n";
    const int numParts = (int) q.parts.size();
    // a part per manual: the whole organ in a bracket (that is also what lets Verovio leave out
    // staves with nothing to play)
    if (numParts > 1)
        o << "    <part-group type=\"start\" number=\"1\"><group-symbol>" << (q.perManual ? "bracket" : "none")
          << "</group-symbol><group-barline>yes</group-barline></part-group>\n";
    for (int p = 0; p < numParts; ++p)
    {
        // the manuals together under a thin line inside the bracket (Verovio only leaves out
        // empty staves when a score has more than one group sign)
        if (q.perManual && p == 0)
            o << "    <part-group type=\"start\" number=\"2\"><group-symbol>line</group-symbol><group-barline>yes</group-barline></part-group>\n";
        // a pipe organ (MuseScore and other programs pick their organ instrument from this)
        o << "    <score-part id=\"P" << (p + 1) << "\"><part-name>" << esc (q.parts[(size_t) p].name) << "</part-name><part-abbreviation>"
          << esc (q.parts[(size_t) p].abbreviation) << "</part-abbreviation><score-instrument id=\"P" << (p + 1)
          << "-I1\"><instrument-name>Pipe Organ</instrument-name><instrument-sound>keyboard.organ.pipe</instrument-sound></score-instrument>"
          << "<midi-instrument id=\"P" << (p + 1) << "-I1\"><midi-channel>" << juce::jmin (16, p + 1) << "</midi-channel><midi-program>20</midi-program></midi-instrument></score-part>\n";
        if (q.perManual && p + 1 < numParts && q.parts[(size_t) p + 1].pedal)
            o << "    <part-group type=\"stop\" number=\"2\"/>\n";
        if (q.perManual && p + 1 == numParts && ! q.parts[(size_t) p].pedal)
            o << "    <part-group type=\"stop\" number=\"2\"/>\n";
    }
    if (numParts > 1)
        o << "    <part-group type=\"stop\" number=\"1\"/>\n";
    o << "  </part-list>\n";

    // Where a hand changes manual, the manual is named at the staff, as organ editions do:
    // a label goes to every note on a manual that was not sounding on this staff just before
    // (a hand moving over, or a second manual joining in). With two manuals on one staff the
    // upper voice is named above the staff, the lower one below.
    // With colours on, quick back-and-forth is left to the colours: a label then needs the hand
    // to stay a beat on the new manual, and a beat since the last label.
    std::map<std::tuple<int, int, int, int>, bool> labelAt; // (measure, staff, position, division) -> below the staff
    // (each manual on its own staves: the part names say it all)
    if (q.manualsUsed > 1 && ! q.perManual)
        for (int k = 0; k < (int) q.staves.size(); ++k)
        {
            if (q.staves[(size_t) k].pedal)
                continue;
            auto& st = q.staves[(size_t) k];
            struct Ev { int abs, mi, start, end, division; bool lower, tied; };
            std::vector<Ev> all;
            for (int mi = 0; mi < (int) q.measures.size(); ++mi)
            {
                const int ms = q.measures[(size_t) mi].start;
                int voicesWithNotes = 0;
                for (auto& voice : st.voices)
                    for (auto& e : voice[(size_t) mi])
                        if (! e.rest)
                        {
                            ++voicesWithNotes;
                            break;
                        }
                for (int v = 0; v < (int) st.voices.size(); ++v)
                    for (auto& e : st.voices[(size_t) v][(size_t) mi])
                        if (! e.rest && ! e.notes.empty() && e.notes.front().division >= 0)
                            all.push_back ({ ms + e.start, mi, e.start, ms + e.start + e.dur, e.notes.front().division,
                                             voicesWithNotes > 1 && v % 2 == 1, e.notes.front().tieStop });
            }
            std::stable_sort (all.begin(), all.end(), [] (const Ev& x, const Ev& y) { return x.abs < y.abs; });
            if (! opt.colourManuals)
            {
                for (size_t i = 0; i < all.size(); ++i)
                {
                    auto& e = all[i];
                    // a manual this staff has played within the last beat (or holds now) needs no
                    // name; one that comes back after a while, or is new, gets it
                    if (e.tied)
                        continue;
                    bool known = false;
                    size_t from = i;
                    while (from > 0 && all[from - 1].abs > e.abs - 16 * U)
                        --from;
                    for (size_t j = from; j < all.size() && all[j].abs <= e.abs && ! known; ++j)
                        known = j != i && all[j].division == e.division && (all[j].abs < e.abs || j < i) && all[j].end >= e.abs - U;
                    if (! known)
                        labelAt[{ e.mi, k, e.start, e.division }] = e.lower;
                }
                continue;
            }
            int current = -1;
            int lastLabel = -(1 << 30);
            for (size_t i = 0; i < all.size(); ++i)
            {
                auto& e = all[i];
                if (e.division == current)
                    continue;
                bool oldSounding = false;
                for (size_t j = 0; j < i; ++j)
                    oldSounding |= all[j].division == current && all[j].end > e.abs;
                current = e.division;
                int runEnd = e.end;
                for (size_t j = i + 1; j < all.size() && all[j].division == e.division; ++j)
                    runEnd = juce::jmax (runEnd, all[j].end);
                if (oldSounding || runEnd - e.abs < U || e.abs - lastLabel < U)
                    continue;
                labelAt[{ e.mi, k, e.start, e.division }] = false;
                lastLabel = e.abs;
            }
        }
    std::set<std::tuple<int, int, int, int>> labelled;
    for (int part = 0; part < numParts; ++part)
    {
        std::vector<int> staffIdx;
        for (int s = 0; s < (int) q.staves.size(); ++s)
            if (q.staves[(size_t) s].part == part)
                staffIdx.push_back (s);

        o << "  <part id=\"P" << (part + 1) << "\">\n";
        for (int mi = 0; mi < (int) q.measures.size(); ++mi)
        {
            auto& m = q.measures[(size_t) mi];
            o << "    <measure number=\"" << (mi + 1) << "\"" << (mi > 0 && m.length != m.num * 4 * U / m.den ? " implicit=\"yes\"" : "") << ">\n";
            bool clefChange = false;
            for (int k = 0; k < (int) staffIdx.size(); ++k)
            {
                auto& cb = q.staves[(size_t) staffIdx[(size_t) k]].clefBass;
                clefChange |= mi > 0 && mi < (int) cb.size() && cb[(size_t) mi] != cb[(size_t) mi - 1];
            }
            if (mi == 0 || m.timeChange || m.keyChange || clefChange)
            {
                o << "      <attributes>";
                if (mi == 0)
                    o << "<divisions>" << U << "</divisions>";
                if (m.keyChange)
                    o << "<key><fifths>" << m.fifths << "</fifths><mode>" << (m.minor ? "minor" : "major") << "</mode></key>";
                if (m.timeChange)
                    o << "<time><beats>" << m.num << "</beats><beat-type>" << m.den << "</beat-type></time>";
                if (mi == 0)
                {
                    o << "<staves>" << (int) staffIdx.size() << "</staves>";
                    for (int k = 0; k < (int) staffIdx.size(); ++k)
                    {
                        const bool bass = q.staves[(size_t) staffIdx[(size_t) k]].bass;
                        o << "<clef number=\"" << (k + 1) << "\"><sign>" << (bass ? "F" : "G") << "</sign><line>" << (bass ? 4 : 2) << "</line></clef>";
                    }
                }
                else
                    for (int k = 0; k < (int) staffIdx.size(); ++k)
                    {
                        auto& cb = q.staves[(size_t) staffIdx[(size_t) k]].clefBass;
                        if (mi < (int) cb.size() && cb[(size_t) mi] != cb[(size_t) mi - 1])
                            o << "<clef number=\"" << (k + 1) << "\"><sign>" << (cb[(size_t) mi] ? "F" : "G") << "</sign><line>"
                              << (cb[(size_t) mi] ? 4 : 2) << "</line></clef>";
                    }
                o << "</attributes>\n";
            }
            if (mi == 0 && part == 0)
                o << "      <direction placement=\"above\"><direction-type><metronome><beat-unit>quarter</beat-unit><per-minute>"
                  << juce::roundToInt (q.firstTempo) << "</per-minute></metronome></direction-type><sound tempo=\""
                  << juce::String (q.firstTempo, 2) << "\"/></direction>\n";

            // registration, small and in italics above the staff, at its place in the bar
            for (auto& rt : regTexts)
                if (rt.measure == mi && rt.part == part)
                    o << "      <direction placement=\"above\"><direction-type><words font-style=\"italic\">" << esc (rt.text)
                      << "</words></direction-type>" << (rt.pos > 0 ? "<offset>" + juce::String (rt.pos) + "</offset>" : juce::String())
                      << "<staff>1</staff></direction>\n";

            bool firstRun = true;
            for (int k = 0; k < (int) staffIdx.size(); ++k)
            {
                auto& st = q.staves[(size_t) staffIdx[(size_t) k]];
                for (int v = 0; v < st.numVoices; ++v)
                {
                    auto& evs = st.voices[(size_t) v][(size_t) mi];
                    if (evs.empty())
                        continue;
                    if (! firstRun)
                        o << "      <backup><duration>" << m.length << "</duration></backup>\n";
                    firstRun = false;
                    const int voiceNo = k * 4 + v + 1;
                    size_t tempoIdx = 0;
                    int written = 0;
                    // triplet groups: one "3" per beat and staff, with a bracket unless the
                    // group is one beamed run or has hidden rests in it
                    std::map<const QEvent*, juce::String> tupletMark; // event -> "<tuplet .../>"
                    for (size_t i = 0; i < evs.size(); ++i)
                    {
                        size_t j = 0;
                        bool bracket = false;
                        if (! tupletShown (evs, i, j, bracket))
                            continue;
                        const int beatNo = (m.start + evs[i].start) / U;
                        bool markedAbove = false;
                        for (int w = 0; w < v && ! markedAbove; ++w)
                        {
                            auto& other = st.voices[(size_t) w][(size_t) mi];
                            for (size_t i2 = 0; i2 < other.size() && ! markedAbove; ++i2)
                            {
                                size_t j2 = 0;
                                bool b2 = false;
                                markedAbove = (m.start + other[i2].start) / U == beatNo && tupletShown (other, i2, j2, b2);
                            }
                        }
                        if (markedAbove)
                            continue;
                        tupletMark[&evs[i]] = juce::String ("<tuplet type=\"start\" bracket=\"") + (bracket ? "yes" : "no") + "\"/>";
                        tupletMark[&evs[j]] += "<tuplet type=\"stop\"/>";
                    }
                    const juce::String timeMod = "<time-modification><actual-notes>3</actual-notes><normal-notes>2</normal-notes></time-modification>";
                    for (auto& e : evs)
                    {
                        const auto markIt = tupletMark.find (&e);
                        const juce::String mark = markIt != tupletMark.end() ? markIt->second : juce::String();
                        if (part == 0 && k == 0 && v == 0)
                            while (tempoIdx < m.tempos.size() && m.tempos[tempoIdx].first <= e.start)
                            {
                                if (! (mi == 0 && m.tempos[tempoIdx].first == 0))
                                    o << "      <sound tempo=\"" << juce::String (m.tempos[tempoIdx].second, 2) << "\"/>\n";
                                ++tempoIdx;
                            }
                        auto writeCommon = [&] (bool isChordMember) {
                            juce::ignoreUnused (isChordMember);
                            o << "<duration>" << e.dur << "</duration>";
                        };
                        // a hand moves to another manual: say so at the staff, as organ editions do
                        if (! e.rest && ! q.perManual && ! q.parts[(size_t) part].pedal)
                        {
                            std::set<int> divs;
                            for (auto& n : e.notes)
                                if (n.division >= 0)
                                    divs.insert (n.division);
                            for (int dv : divs)
                            {
                                const auto labelIt = labelAt.find ({ mi, k, e.start, dv });
                                if (labelIt == labelAt.end() || ! labelled.insert ({ mi, k, e.start, dv }).second)
                                    continue;
                                auto label = divisionAbbreviation (juce::isPositiveAndBelow (dv, (int) opt.divisionNames.size()) ? opt.divisionNames[(size_t) dv] : juce::String(),
                                                                   dv, false);
                                // the registration printed at this place already names the division
                                bool named = false, crowded = false;
                                for (auto& rt : regTexts)
                                    if (rt.part == part)
                                    {
                                        named |= rt.measure == mi && rt.division == label && std::abs (rt.pos - e.start) < U;
                                        // a registration line above this staff nearby: the label goes under the staff
                                        crowded |= std::abs ((q.measures[(size_t) rt.measure].start + rt.pos) - (m.start + e.start)) < 2 * q.measures[(size_t) mi].length;
                                    }
                                if (named)
                                    continue;
                                const bool below = labelIt->second || k > 0 || crowded;
                                o << "      <direction placement=\"" << (below ? "below" : "above") << "\"><direction-type><words";
                                if (opt.colourManuals)
                                    o << " font-weight=\"bold\" color=\"" << manualColour (dv) << "\"";
                                o << ">" << esc (label) << "</words></direction-type><staff>" << (k + 1) << "</staff></direction>\n";
                            }
                        }
                        if (e.rest)
                        {
                            o << "      <note" << (e.hidden ? " print-object=\"no\"" : "") << "><rest" << (e.wholeMeasureRest ? " measure=\"yes\"" : "") << "/>";
                            writeCommon (false);
                            o << "<voice>" << voiceNo << "</voice>";
                            if (! e.wholeMeasureRest)
                            {
                                o << "<type>" << typeName (e.typeUnits) << "</type>";
                                for (int d = 0; d < e.dots; ++d)
                                    o << "<dot/>";
                                if (e.triplet)
                                    o << timeMod;
                            }
                            o << "<staff>" << (k + 1) << "</staff>";
                            if (mark.isNotEmpty())
                                o << "<notations>" << mark << "</notations>";
                            o << "</note>\n";
                        }
                        else
                        {
                            for (size_t ni = 0; ni < e.notes.size(); ++ni)
                            {
                                auto& n = e.notes[ni];
                                const bool colourIt = opt.colourManuals && q.manualsUsed > 1 && ! q.parts[(size_t) part].pedal && n.division > 0;
                                o << "      <note" << (colourIt ? " color=\"" + manualColour (n.division) + "\"" : juce::String()) << ">"
                                  << (ni > 0 ? "<chord/>" : "") << "<pitch><step>" << juce::String::charToString (stepName[n.step]) << "</step>";
                                if (n.alter != 0)
                                    o << "<alter>" << n.alter << "</alter>";
                                o << "<octave>" << n.octave << "</octave></pitch>";
                                writeCommon (ni > 0);
                                if (n.tieStop) o << "<tie type=\"stop\"/>";
                                if (n.tieStart) o << "<tie type=\"start\"/>";
                                o << "<voice>" << voiceNo << "</voice><type>" << typeName (e.typeUnits) << "</type>";
                                for (int d = 0; d < e.dots; ++d)
                                    o << "<dot/>";
                                if (n.showAccidental != -99)
                                    o << "<accidental>" << (n.showAccidental > 0 ? "sharp" : n.showAccidental < 0 ? "flat" : "natural") << "</accidental>";
                                if (e.triplet)
                                    o << timeMod;
                                if (e.stem != 0)
                                    o << "<stem>" << (e.stem == 1 ? "up" : "down") << "</stem>";
                                o << "<staff>" << (k + 1) << "</staff>";
                                if (ni == 0 && e.beam != 0)
                                    o << "<beam number=\"1\">" << (e.beam == 1 ? "begin" : e.beam == 2 ? "continue" : "end") << "</beam>";
                                const bool markHere = ni == 0 && mark.isNotEmpty();
                                // an ornament belongs to the top note of a chord
                                const bool ornamentHere = n.ornament != 0 && ni + 1 == e.notes.size();
                                if (n.tieStart || n.tieStop || markHere || ornamentHere)
                                {
                                    o << "<notations>";
                                    if (n.tieStop) o << "<tied type=\"stop\"/>";
                                    if (n.tieStart) o << "<tied type=\"start\"/>";
                                    if (markHere) o << mark;
                                    if (ornamentHere)
                                        {
                                        const juce::String place = e.stem == 2 ? " placement=\"below\"" : " placement=\"above\"";
                                        o << "<ornaments>" << (n.ornament == 1 ? "<trill-mark" : n.ornament == 2 ? "<mordent" : "<inverted-mordent") << place << "/></ornaments>";
                                    }
                                    o << "</notations>";
                                }
                                o << "</note>\n";
                            }
                        }
                        written += e.dur;
                    }
                    juce::ignoreUnused (written);
                }
            }
            if (firstRun)
                o << "      <note><rest measure=\"yes\"/><duration>" << m.length << "</duration><voice>1</voice><staff>1</staff></note>\n";
            o << "    </measure>\n";
        }
        o << "  </part>\n";
    }
    o << "</score-partwise>\n";
    return o.toString();
}

//==============================================================================
// ABC and LilyPond are written on the classic three staves
static Options classicLayout (Options opt)
{
    opt.staffPerManual = false;
    return opt;
}

juce::String toABC (const Score& score, const Options& options)
{
    const auto opt = classicLayout (options);
    auto q = build (score, opt);
    const auto regTexts = registrationTexts (score, opt, q);
    juce::MemoryOutputStream o;
    auto& m0 = q.measures.front();
    o << "X:1\n";
    o << "T:" << (q.title.isNotEmpty() ? q.title : juce::String ("Orgelwerk")) << "\n";
    if (q.composer.isNotEmpty())
        o << "C:" << q.composer << "\n";
    o << "M:" << m0.num << "/" << m0.den << "\n";
    o << "L:1/32\n";
    constexpr int perAbc = U / 8; // grid units per ABC length unit (a 32nd)
    o << "Q:1/4=" << juce::roundToInt (q.firstTempo) << "\n";

    juce::StringArray voiceIds;
    juce::String scoreLine = "%%score ";
    for (int s = 0; s < (int) q.staves.size(); ++s)
    {
        auto& st = q.staves[(size_t) s];
        if (s == 0) scoreLine << "{ ";
        scoreLine << "(";
        for (int v = 0; v < st.numVoices; ++v)
        {
            auto id = "S" + juce::String (s + 1) + "V" + juce::String (v + 1);
            voiceIds.add (id);
            scoreLine << id << (v + 1 < st.numVoices ? " " : "");
        }
        scoreLine << ")";
        if (s == 1) scoreLine << " }";
        scoreLine << " ";
    }
    o << scoreLine.trimEnd() << "\n";
    o << "K:" << keyName (m0.fifths, m0.minor, false) << "\n";

    for (int s = 0; s < (int) q.staves.size(); ++s)
    {
        auto& st = q.staves[(size_t) s];
        for (int v = 0; v < st.numVoices; ++v)
        {
            o << "V:S" << (s + 1) << "V" << (v + 1) << " clef=" << (st.bass ? "bass" : "treble");
            if (v == 0)
                o << " name=\"" << (st.pedal ? opt.pedalName : (s == 0 ? opt.manualName : juce::String())) << "\"";
            o << "\n";
            for (int mi = 0; mi < (int) q.measures.size(); ++mi)
            {
                auto& m = q.measures[(size_t) mi];
                if (mi > 0 && m.timeChange)
                    o << "[M:" << m.num << "/" << m.den << "]";
                const bool clefNow = mi > 0 && mi < (int) st.clefBass.size() && st.clefBass[(size_t) mi] != st.clefBass[(size_t) mi - 1];
                if ((mi > 0 && m.keyChange) || clefNow)
                    o << "[K:" << keyName (m.fifths, m.minor, false)
                      << (clefNow ? (st.clefBass[(size_t) mi] ? " clef=bass" : " clef=treble") : "") << "]";
                auto& evs = st.voices[(size_t) v][(size_t) mi];
                if (evs.empty())
                    o << "x" << m.length / perAbc;
                for (size_t i = 0; i < evs.size(); ++i)
                {
                    auto& e = evs[i];
                    // triplets are written with their plain value inside (3:2:n
                    const int abcLen = (e.triplet ? e.dur * 3 / 2 : e.dur) / perAbc;
                    if (e.tupletStart)
                    {
                        int n = 0;
                        for (size_t j = i; j < evs.size(); ++j)
                        {
                            ++n;
                            if (evs[j].tupletStop)
                                break;
                        }
                        o << "(3:2:" << n;
                    }
                    for (auto& t : registrationAt (regTexts, q, s, v, mi, e))
                        o << "\"^" << t.replace ("\"", "'") << "\"";
                    if (e.rest)
                        o << (e.hidden ? "x" : "z") << abcLen;
                    else
                    {
                        int orn = 0;
                        for (auto& n : e.notes)
                            orn = n.ornament != 0 ? n.ornament : orn;
                        if (orn != 0)
                            o << (orn == 1 ? "!trill!" : orn == 2 ? "!mordent!" : "!uppermordent!");
                        if (e.notes.size() > 1)
                            o << "[";
                        for (auto& n : e.notes)
                        {
                            if (n.showAccidental != -99)
                                o << (n.showAccidental > 0 ? "^" : n.showAccidental < 0 ? "_" : "=");
                            juce::String letter = juce::String::charToString (stepName[n.step]);
                            if (n.octave >= 5)
                            {
                                o << letter.toLowerCase();
                                for (int k = 5; k < n.octave; ++k)
                                    o << "'";
                            }
                            else
                            {
                                o << letter;
                                for (int k = n.octave; k < 4; ++k)
                                    o << ",";
                            }
                            if (e.notes.size() > 1 && n.tieStart)
                                o << "-";
                        }
                        if (e.notes.size() > 1)
                            o << "]";
                        o << abcLen;
                        if (e.notes.size() == 1 && e.notes[0].tieStart)
                            o << "-";
                    }
                    const bool joined = e.beam == 1 || e.beam == 2;
                    if (! joined)
                        o << " ";
                }
                if (mi + 1 == (int) q.measures.size())
                    o << "|]\n";
                else
                    o << "|" << ((mi + 1) % 4 == 0 ? "\n" : " ");
            }
        }
    }
    return o.toString();
}

//==============================================================================
juce::String toLilyPond (const Score& score, const Options& options)
{
    const auto opt = classicLayout (options);
    auto q = build (score, opt);
    const auto regTexts = registrationTexts (score, opt, q);
    juce::MemoryOutputStream o;
    o << "\\version \"2.24.0\"\n\\header {\n";
    o << "  title = \"" << q.title.replace ("\"", "'") << "\"\n";
    if (q.composer.isNotEmpty())
        o << "  composer = \"" << q.composer.replace ("\"", "'") << "\"\n";
    o << "  tagline = \"Orgelwerk\"\n}\n\n";

    auto durStr = [] (int typeUnits, int dots) {
        juce::String d (4 * U / typeUnits);
        for (int i = 0; i < dots; ++i)
            d << ".";
        return d;
    };
    auto pitch = [] (const QNote& n) {
        juce::String p = juce::String::charToString (stepName[n.step]).toLowerCase();
        if (n.alter > 0) p << "is";
        if (n.alter < 0) p << (n.step == 2 || n.step == 5 ? "s" : "es");
        if (n.octave >= 3)
            for (int k = 3; k < n.octave; ++k) p << "'";
        else
            for (int k = n.octave; k < 3; ++k) p << ",";
        if (n.showAccidental != -99 && n.alter == 0) p << "!";
        return p;
    };
    auto spacer = [] (int units) {
        // spacer for a whole measure of any length in 32nds
        return "s32*" + juce::String (units * 8 / U);
    };

    juce::StringArray staffNames;
    for (int s = 0; s < (int) q.staves.size(); ++s)
    {
        auto& st = q.staves[(size_t) s];
        auto name = juce::String ("staff") + juce::String::charToString ((juce::juce_wchar) ('A' + s));
        staffNames.add (name);
        o << name << " = {\n  \\clef " << (st.bass ? "bass" : "treble") << "\n";
        o << "  <<\n";
        for (int v = 0; v < st.numVoices; ++v)
        {
            if (v > 0)
                o << "  \\\\\n";
            o << "  {\n    ";
            for (int mi = 0; mi < (int) q.measures.size(); ++mi)
            {
                auto& m = q.measures[(size_t) mi];
                if (mi > 0 && mi < (int) st.clefBass.size() && st.clefBass[(size_t) mi] != st.clefBass[(size_t) mi - 1])
                    o << "\\clef " << (st.clefBass[(size_t) mi] ? "bass" : "treble") << " ";
                if (m.keyChange)
                    o << "\\key " << keyName (m.fifths, m.minor, true) << " ";
                if (m.timeChange)
                    o << "\\time " << m.num << "/" << m.den << " ";
                auto& evs = st.voices[(size_t) v][(size_t) mi];
                if (evs.empty())
                    o << spacer (m.length) << " ";
                for (auto& e : evs)
                {
                    juce::String markup;
                    for (auto& t : registrationAt (regTexts, q, s, v, mi, e))
                        markup << "^\\markup \\italic \"" << t.replace ("\"", "'") << "\" ";
                    if (e.tupletStart)
                        o << "\\tuplet 3/2 { ";
                    const char* closeTuplet = e.tupletStop ? "} " : "";
                    if (e.rest)
                    {
                        if (e.hidden)
                            o << "s" << (e.wholeMeasureRest ? "32*" + juce::String (m.length * 8 / U) : durStr (e.typeUnits, e.dots)) << " ";
                        else if (e.wholeMeasureRest)
                            o << "R32*" << m.length * 8 / U << " ";
                        else
                            o << "r" << durStr (e.typeUnits, e.dots) << " ";
                        o << markup << closeTuplet;
                        continue;
                    }
                    if (e.notes.size() > 1)
                    {
                        o << "<";
                        for (size_t i = 0; i < e.notes.size(); ++i)
                            o << pitch (e.notes[i]) << (i + 1 < e.notes.size() ? " " : "");
                        o << ">";
                    }
                    else
                        o << pitch (e.notes[0]);
                    o << durStr (e.typeUnits, e.dots);
                    {
                        int orn = 0;
                        for (auto& n : e.notes)
                            orn = n.ornament != 0 ? n.ornament : orn;
                        if (orn != 0)
                            o << (orn == 1 ? "\\trill" : orn == 2 ? "\\mordent" : "\\prall");
                    }
                    if (markup.isNotEmpty())
                        o << " " << markup;
                    bool tie = false;
                    for (auto& n : e.notes)
                        tie |= n.tieStart;
                    if (tie)
                        o << "~";
                    if (e.beam == 1) o << "[";
                    if (e.beam == 3) o << "]";
                    o << " " << closeTuplet;
                }
                o << "|\n    ";
            }
            o << "\n  }\n";
        }
        o << "  >>\n}\n\n";
    }

    o << "\\score {\n  <<\n";
    o << "    \\new PianoStaff \\with { instrumentName = \"" << opt.manualName << "\" } <<\n";
    o << "      \\new Staff \\" << staffNames[0] << "\n";
    o << "      \\new Staff \\" << staffNames[1] << "\n";
    o << "    >>\n";
    if (q.hasPedal)
        o << "    \\new Staff \\with { instrumentName = \"" << opt.pedalName << "\" } \\" << staffNames[2] << "\n";
    o << "  >>\n  \\layout { }\n  \\midi { \\tempo 4 = " << juce::roundToInt (q.firstTempo) << " }\n}\n";
    return o.toString();
}
} // namespace orgel::NotationBuilder
