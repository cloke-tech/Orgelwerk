#include "NotationBuilder.h"
#include <map>
#include <set>
#include <tuple>
#include "../engine/OrganModel.h"

namespace orgel::NotationBuilder
{
namespace
{
    constexpr int U = 8; // grid units per quarter note (32nd notes)

    struct QNote
    {
        int key = 60, step = 0, alter = 0, octave = 4;
        int showAccidental = -99; // -99 = none, otherwise the alter to print
        bool tieStart = false, tieStop = false;
        int division = -1;        // organ division (manual) that plays it
    };

    struct QEvent
    {
        int start = 0, dur = 0; // relative to measure start
        bool rest = false;
        int typeUnits = 8, dots = 0;
        std::vector<QNote> notes;
        int beam = 0; // 0 none, 1 begin, 2 continue, 3 end
        int stem = 0; // 0 auto, 1 up, 2 down
        bool wholeMeasureRest = false;
        bool hidden = false; // rest that would collide with another voice's notes
    };

    struct QMeasure
    {
        int start = 0, length = 32;
        int num = 4, den = 4;
        int fifths = 0;
        bool minor = false;
        bool timeChange = false, keyChange = false;
        std::vector<std::pair<int, double>> tempos; // (position in measure, bpm)
    };

    struct QStaff
    {
        bool bass = false;
        int part = 0; // 0 = manuals, 1 = pedal
        int numVoices = 1;
        std::vector<std::vector<std::vector<QEvent>>> voices; // [voice][measure] -> events
    };

    struct QScore
    {
        juce::String title, composer;
        std::vector<QMeasure> measures;
        std::vector<QStaff> staves;
        bool hasPedal = false;
        int manualsUsed = 0;
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
    };
    const Value values[] = { { 32, 32, 0 }, { 24, 16, 1 }, { 16, 16, 0 }, { 12, 8, 1 }, { 8, 8, 0 },
                             { 6, 4, 1 }, { 4, 4, 0 }, { 3, 2, 1 }, { 2, 2, 0 }, { 1, 1, 0 } };

    std::vector<Value> decompose (int pos, int len, const QMeasure& m, bool rest = false)
    {
        const bool compound = m.den == 8 && m.num % 3 == 0 && m.num > 3;
        std::vector<Value> out;
        while (len > 0)
        {
            const Value* chosen = &values[9];
            for (auto& v : values)
            {
                if (v.units > len || (rest && v.dots > 0 && ! (m.den == 8 && v.units == 12)))
                    continue;
                int align = v.typeUnits;
                if (compound && v.units == 12)
                    align = 12;
                if (v.units == 32 && m.length != 32)
                    continue;
                if (pos % align == 0)
                {
                    chosen = &v;
                    break;
                }
            }
            out.push_back (*chosen);
            pos += chosen->units;
            len -= chosen->units;
        }
        return out;
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

        int endUnits = 0;
        for (auto& n : score.notes)
            endUnits = juce::jmax (endUnits, (int) std::round ((n.start + n.length) * U));
        endUnits = juce::jmax (endUnits, 1);

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

        // staves: grand staff for manuals, one staff for the pedal
        // Hands: notes that start together are split between right and left hand so that
        // each hand stays near where it was and neither has to stretch further than a tenth.
        std::vector<int> staffOf (score.notes.size(), 0);
        {
            std::map<int, std::vector<int>> onsets; // quantised start -> manual notes
            for (int i = 0; i < (int) score.notes.size(); ++i)
            {
                auto& n = score.notes[(size_t) i];
                const bool pedal = n.part < (int) opt.partIsPedal.size() && opt.partIsPedal[(size_t) n.part];
                if (pedal)
                {
                    staffOf[(size_t) i] = 2;
                    q.hasPedal = true;
                }
                else
                    onsets[(int) std::round (n.start * U)].push_back (i);
            }
            double right = 67, left = 50;
            for (auto& [t, idx] : onsets)
            {
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
                    staffOf[(size_t) idx[(size_t) k]] = k < bestSplit ? 1 : 0;
                    (k < bestSplit ? lm : rm) += key (k);
                }
                if (bestSplit > 0) left = 0.6 * left + 0.4 * (lm / bestSplit);
                if (bestSplit < n) right = 0.6 * right + 0.4 * (rm / (n - bestSplit));
                // hands drift back towards their home range when one is idle
                left = 0.97 * left + 0.03 * 50;
                right = 0.97 * right + 0.03 * 67;
            }
        }
        std::array<std::vector<std::pair<int, int>>, 3> staffNotes; // per staff: (index into score.notes, key)
        for (int i = 0; i < (int) score.notes.size(); ++i)
            staffNotes[(size_t) staffOf[(size_t) i]].push_back ({ i, score.notes[(size_t) i].key });

        {
            std::set<int> manuals;
            for (int p = 0; p < (int) score.parts.size(); ++p)
                if (p < (int) opt.partDivision.size() && opt.partDivision[(size_t) p] >= 0
                    && ! (p < (int) opt.partIsPedal.size() && opt.partIsPedal[(size_t) p]))
                    manuals.insert (opt.partDivision[(size_t) p]);
            q.manualsUsed = (int) manuals.size();
        }
        const int numStaves = q.hasPedal ? 3 : 2;
        for (int s = 0; s < numStaves; ++s)
        {
            QStaff st;
            st.bass = s > 0;
            st.part = s == 2 ? 1 : 0;

            // chords: same start and end
            // quantise, then close small gaps before the next onset of the same part
            // (performed non-legato would otherwise turn into a rest after every note)
            struct QN { int qs, qe, key, part; };
            std::vector<QN> qn;
            for (auto [idx, key] : staffNotes[(size_t) s])
            {
                auto& n = score.notes[(size_t) idx];
                int qs = (int) std::round (n.start * U);
                int qe = (int) std::round ((n.start + n.length) * U);
                qs = juce::jlimit (0, juce::jmax (0, totalUnits - 1), qs);
                // longer notes end on the 16th grid; only short notes keep 32nd precision
                if (qe - qs >= 4)
                    qe = (int) std::round ((n.start + n.length) * U / 2.0) * 2;
                qe = juce::jlimit (qs + 1, totalUnits, qe);
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
                if (gap > 0 && gap <= juce::jmax (2, len) && gap <= U)
                    n.qe = next;
                // legato playing lets a note ring into the next one; cut that small overlap
                // so a running line stays in one voice
                else if (gap < 0 && -gap <= juce::jmax (1, len / 3))
                    n.qe = next;
            }

            std::map<std::pair<int, int>, int> divisionOf; // (start, key) -> division
            for (auto& n : qn)
                divisionOf[{ n.qs, n.key }] = n.part < (int) opt.partDivision.size() ? opt.partDivision[(size_t) n.part] : -1;
            std::map<std::pair<int, int>, std::vector<int>> chordMap;
            for (auto& n : qn)
            {
                const int qs = n.qs, qe = n.qe, key = n.key;
                auto& keys = chordMap[{ qs, qe }];
                if (std::find (keys.begin(), keys.end(), key) == keys.end())
                    keys.push_back (key);
            }
            std::vector<Chord> chords;
            for (auto& [range, keys] : chordMap)
            {
                Chord c { range.first, range.second, keys };
                std::sort (c.keys.begin(), c.keys.end());
                chords.push_back (c);
            }
            std::stable_sort (chords.begin(), chords.end(), [] (const Chord& a, const Chord& b) {
                if (a.qs != b.qs) return a.qs < b.qs;
                return a.keys.back() > b.keys.back();
            });

            // voices: give each chord to a free voice, preferring the one that continues nearby pitches
            struct VoiceState
            {
                int lastEnd = 0, lastPitch = -1, lastStart = -1;
                std::vector<Chord> chords;
                double pitchSum = 0;
                int pitchCount = 0;
            };
            std::vector<VoiceState> vs;
            for (auto& c : chords)
            {
                const int top = c.keys.back();
                int bestV = -1;
                int bestDist = 1 << 30;
                for (int v = 0; v < (int) vs.size(); ++v)
                    if (vs[(size_t) v].lastEnd <= c.qs)
                    {
                        const int dist = vs[(size_t) v].lastPitch < 0 ? 0 : std::abs (vs[(size_t) v].lastPitch - top);
                        if (dist < bestDist)
                        {
                            bestDist = dist;
                            bestV = v;
                        }
                    }
                if (bestV < 0 && vs.size() < 4)
                {
                    vs.push_back ({});
                    bestV = (int) vs.size() - 1;
                }
                if (bestV < 0)
                {
                    // no free voice: join a chord that starts together, else start after the earliest voice ends
                    for (int v = 0; v < (int) vs.size() && bestV < 0; ++v)
                        if (vs[(size_t) v].lastStart == c.qs)
                        {
                            auto& last = vs[(size_t) v].chords.back();
                            for (int k : c.keys)
                                if (std::find (last.keys.begin(), last.keys.end(), k) == last.keys.end())
                                    last.keys.push_back (k);
                            std::sort (last.keys.begin(), last.keys.end());
                            bestV = -2;
                        }
                    if (bestV == -2)
                        continue;
                    int earliest = 0;
                    for (int v = 1; v < (int) vs.size(); ++v)
                        if (vs[(size_t) v].lastEnd < vs[(size_t) earliest].lastEnd)
                            earliest = v;
                    if (vs[(size_t) earliest].lastEnd >= c.qe)
                        continue;
                    c.qs = vs[(size_t) earliest].lastEnd;
                    bestV = earliest;
                }
                auto& v = vs[(size_t) bestV];
                v.chords.push_back (c);
                v.lastEnd = c.qe;
                v.lastStart = c.qs;
                v.lastPitch = top;
                for (int k : c.keys)
                {
                    v.pitchSum += k;
                    v.pitchCount++;
                }
            }
            std::stable_sort (vs.begin(), vs.end(), [] (const VoiceState& a, const VoiceState& b) {
                const double ma = a.pitchCount ? a.pitchSum / a.pitchCount : 0;
                const double mb = b.pitchCount ? b.pitchSum / b.pitchCount : 0;
                return ma > mb;
            });
            if (vs.empty())
                vs.push_back ({});
            if (s == 1 || s == 2)
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
                            for (auto& val : decompose (pos - ms, s0 - pos, m, true))
                            {
                                QEvent r;
                                r.start = pos - ms;
                                r.dur = val.units;
                                r.rest = true;
                                r.typeUnits = val.typeUnits;
                                r.dots = val.dots;
                                evs.push_back (r);
                                pos += val.units;
                            }
                        auto pieces = decompose (s0 - ms, e0 - s0, m);
                        for (size_t p = 0; p < pieces.size(); ++p)
                        {
                            QEvent e;
                            e.start = pos - ms;
                            e.dur = pieces[p].units;
                            e.typeUnits = pieces[p].typeUnits;
                            e.dots = pieces[p].dots;
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
                        for (auto& val : decompose (pos - ms, me - pos, m, true))
                        {
                            QEvent r;
                            r.start = pos - ms;
                            r.dur = val.units;
                            r.rest = true;
                            r.typeUnits = val.typeUnits;
                            r.dots = val.dots;
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
                        r.typeUnits = 32;
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
                const int group = compound ? 12 : (m.den == 8 ? 4 * 3 : 8);

                for (int v = 0; v < (int) st.voices.size(); ++v)
                {
                    auto& evs = st.voices[(size_t) v][(size_t) mi];
                    for (auto& e : evs)
                        e.stem = activeVoices > 1 && ! e.rest ? (v % 2 == 0 ? 1 : 2) : 0;

                    size_t i = 0;
                    while (i < evs.size())
                    {
                        auto beamable = [&] (const QEvent& e) { return ! e.rest && e.typeUnits <= 4; };
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
            q.staves.push_back (std::move (st));
        }
        return q;
    }

    juce::String typeName (int typeUnits)
    {
        switch (typeUnits)
        {
            case 32: return "whole";
            case 16: return "half";
            case 8: return "quarter";
            case 4: return "eighth";
            case 2: return "16th";
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
juce::String toMusicXML (const Score& score, const Options& opt)
{
    auto q = build (score, opt);
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
    if (q.hasPedal)
        o << "    <part-group type=\"start\" number=\"1\"><group-symbol>none</group-symbol><group-barline>yes</group-barline></part-group>\n";
    o << "    <score-part id=\"P1\"><part-name>" << esc (opt.manualName) << "</part-name><part-abbreviation>Man.</part-abbreviation></score-part>\n";
    if (q.hasPedal)
        o << "    <score-part id=\"P2\"><part-name>" << esc (opt.pedalName) << "</part-name><part-abbreviation>Ped.</part-abbreviation></score-part>\n"
          << "    <part-group type=\"stop\" number=\"1\"/>\n";
    o << "  </part-list>\n";

    const int numParts = q.hasPedal ? 2 : 1;
    // Where a hand changes manual: walk each manual staff in time order (all voices together)
    // and mark the places where the manual differs from what that staff played just before,
    // unless the old manual is still sounding there (two manuals at once need no label).
    std::set<std::tuple<int, int, int>> labelAt; // measure, staff, position
    if (q.manualsUsed > 1)
        for (int k = 0; k < (int) q.staves.size(); ++k)
        {
            if (q.staves[(size_t) k].part != 0)
                continue;
            auto& st = q.staves[(size_t) k];
            int current = -1;
            for (int mi = 0; mi < (int) q.measures.size(); ++mi)
            {
                std::vector<const QEvent*> evs;
                for (auto& voice : st.voices)
                    for (auto& e : voice[(size_t) mi])
                        if (! e.rest && ! e.notes.empty() && ! e.notes.front().tieStop)
                            evs.push_back (&e);
                std::stable_sort (evs.begin(), evs.end(), [] (auto* a, auto* b) { return a->start < b->start; });
                for (auto* e : evs)
                {
                    const int dv = e->notes.front().division;
                    if (dv < 0 || dv == current)
                        continue;
                    bool oldStillSounding = false;
                    for (auto* o : evs)
                        oldStillSounding |= o != e && o->notes.front().division == current && o->start < e->start + e->dur
                                            && o->start + o->dur > e->start;
                    if (! oldStillSounding)
                        labelAt.insert ({ mi, k, e->start });
                    current = dv;
                }
            }
        }
    std::set<std::tuple<int, int, int>> labelled;
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
            if (mi == 0 || m.timeChange || m.keyChange)
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
                o << "</attributes>\n";
            }
            if (mi == 0 && part == 0)
                o << "      <direction placement=\"above\"><direction-type><metronome><beat-unit>quarter</beat-unit><per-minute>"
                  << juce::roundToInt (q.firstTempo) << "</per-minute></metronome></direction-type><sound tempo=\""
                  << juce::String (q.firstTempo, 2) << "\"/></direction>\n";

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
                    for (auto& e : evs)
                    {
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
                        if (! e.rest && part == 0 && ! e.notes.empty() && labelAt.count ({ mi, k, e.start })
                            && labelled.insert ({ mi, k, e.start }).second)
                        {
                            const int dv = e.notes.front().division;
                            auto label = dv < (int) opt.divisionLabels.size() ? opt.divisionLabels[(size_t) dv] : "Man. " + romanNumeral (dv + 1);
                            o << "      <direction placement=\"" << (k == 0 ? "above" : "below") << "\"><direction-type><words font-weight=\"bold\" color=\""
                              << manualColour (dv) << "\">" << esc (label) << "</words></direction-type><staff>" << (k + 1) << "</staff></direction>\n";
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
                            }
                            o << "<staff>" << (k + 1) << "</staff></note>\n";
                        }
                        else
                        {
                            for (size_t ni = 0; ni < e.notes.size(); ++ni)
                            {
                                auto& n = e.notes[ni];
                                const bool colourIt = q.manualsUsed > 1 && part == 0 && n.division > 0;
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
                                if (e.stem != 0)
                                    o << "<stem>" << (e.stem == 1 ? "up" : "down") << "</stem>";
                                o << "<staff>" << (k + 1) << "</staff>";
                                if (ni == 0 && e.beam != 0)
                                    o << "<beam number=\"1\">" << (e.beam == 1 ? "begin" : e.beam == 2 ? "continue" : "end") << "</beam>";
                                if (n.tieStart || n.tieStop)
                                {
                                    o << "<notations>";
                                    if (n.tieStop) o << "<tied type=\"stop\"/>";
                                    if (n.tieStart) o << "<tied type=\"start\"/>";
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
juce::String toABC (const Score& score, const Options& opt)
{
    auto q = build (score, opt);
    juce::MemoryOutputStream o;
    auto& m0 = q.measures.front();
    o << "X:1\n";
    o << "T:" << (q.title.isNotEmpty() ? q.title : juce::String ("Orgelwerk")) << "\n";
    if (q.composer.isNotEmpty())
        o << "C:" << q.composer << "\n";
    o << "M:" << m0.num << "/" << m0.den << "\n";
    o << "L:1/32\n";
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
                o << " name=\"" << (st.part == 1 ? opt.pedalName : (s == 0 ? opt.manualName : juce::String())) << "\"";
            o << "\n";
            for (int mi = 0; mi < (int) q.measures.size(); ++mi)
            {
                auto& m = q.measures[(size_t) mi];
                if (mi > 0 && m.timeChange)
                    o << "[M:" << m.num << "/" << m.den << "]";
                if (mi > 0 && m.keyChange)
                    o << "[K:" << keyName (m.fifths, m.minor, false) << "]";
                auto& evs = st.voices[(size_t) v][(size_t) mi];
                if (evs.empty())
                    o << "x" << m.length;
                for (size_t i = 0; i < evs.size(); ++i)
                {
                    auto& e = evs[i];
                    if (e.rest)
                        o << (e.hidden ? "x" : "z") << e.dur;
                    else
                    {
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
                        o << e.dur;
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
juce::String toLilyPond (const Score& score, const Options& opt)
{
    auto q = build (score, opt);
    juce::MemoryOutputStream o;
    o << "\\version \"2.24.0\"\n\\header {\n";
    o << "  title = \"" << q.title.replace ("\"", "'") << "\"\n";
    if (q.composer.isNotEmpty())
        o << "  composer = \"" << q.composer.replace ("\"", "'") << "\"\n";
    o << "  tagline = \"Orgelwerk\"\n}\n\n";

    auto durStr = [] (int typeUnits, int dots) {
        juce::String d (32 / typeUnits);
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
        return "s32*" + juce::String (units);
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
                if (m.keyChange)
                    o << "\\key " << keyName (m.fifths, m.minor, true) << " ";
                if (m.timeChange)
                    o << "\\time " << m.num << "/" << m.den << " ";
                auto& evs = st.voices[(size_t) v][(size_t) mi];
                if (evs.empty())
                    o << spacer (m.length) << " ";
                for (auto& e : evs)
                {
                    if (e.rest)
                    {
                        if (e.hidden)
                            o << "s" << (e.wholeMeasureRest ? "32*" + juce::String (m.length) : durStr (e.typeUnits, e.dots)) << " ";
                        else if (e.wholeMeasureRest)
                            o << "R32*" << m.length << " ";
                        else
                            o << "r" << durStr (e.typeUnits, e.dots) << " ";
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
                    bool tie = false;
                    for (auto& n : e.notes)
                        tie |= n.tieStart;
                    if (tie)
                        o << "~";
                    if (e.beam == 1) o << "[";
                    if (e.beam == 3) o << "]";
                    o << " ";
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
