#include "OrganEngine.h"
#include "tsf.h"

namespace orgel
{
juce::String romanNumeral (int n)
{
    static const char* r[] = { "0", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X" };
    return n >= 0 && n <= 10 ? juce::String (r[n]) : juce::String (n);
}

StopFamily classifyStop (const juce::String& n)
{
    auto has = [&n] (std::initializer_list<const char*> words) {
        for (auto* w : words)
            if (n.containsIgnoreCase (w))
                return true;
        return false;
    };
    if (has ({ "Glocken", "Carillon", "Bells", "Zimbelstern", "Nachtigal", "Twinkly" }))
        return StopFamily::Percussion;
    if (has ({ "Tromp", "Trumpet", "Posaune", "Bombarde", "Hautbois", "Krummhorn", "Cromorne", "Regal", "Vox", "Voix Humaine",
               "Fagotto", "Clairon", "Schalmei", "Cor Anglais", "Cornemuse", "reeds", "Clarines", "Anches", "Piffaro", "Musette" }))
        return StopFamily::Reed;
    if (has ({ "Mixtur", "Cymbal", "Fourniture", "Plein Jeu", "Cornet", "Sesquialtera", "Nazard", "Tierce", "Larigot", "Quint",
               "Ripieno", "Terz", "Zimbel", "Septi", "Nonade", "Septade", "Nasardos", "Neuvi", "Tutti", "Grand Jeu", "Pienino", "Rip." }))
        return StopFamily::Mixture;
    if (has ({ "Gamba", "Viol", "Salicional", "Celeste", "String", "Quintad" }))
        return StopFamily::String;
    if (has ({ "Fl", "Bourdon", "Gedackt", "Gedeckt", "Rohr", "Subbass", "Stopped", "Gemshorn", "Spitz", "Holz", "Goblet", "Block" }))
        return StopFamily::Flute;
    return StopFamily::Principal;
}

//==============================================================================
OrganEngine::OrganEngine() = default;

OrganEngine::~OrganEngine() = default;

void OrganEngine::setSoundFont (std::shared_ptr<SoundFont> sf)
{
    {
        const juce::ScopedLock sl (lock);
        soundFont = std::move (sf);
        for (int d = 0; d < (int) divisions.size(); ++d)
        {
            auto& div = divisions[(size_t) d];
            div.synth.reset();
            div.usedChannels.reset();
            for (auto& st : div.stops)
            {
                st.sounding.reset();
                st.channel = st.channel2 = -1;
                assignChannel (d, st);
            }
            applyPlacement (d);
            reconcileDivisionStops (d);
        }
    }
    sendChangeMessage();
}

std::shared_ptr<tsf> OrganEngine::makeSynth() const
{
    tsf* t = soundFont ? soundFont->createInstance() : nullptr;
    if (t == nullptr)
        return {};
    tsf_set_output (t, TSF_STEREO_INTERLEAVED, (int) sampleRate, -15.0f);
    // plenty of voices: running out means voices get cut off, which clicks
    tsf_set_max_voices (t, 1024);
    tsf_channel_set_volume (t, 255, 1.0f); // allocates all 256 channels up front
    return std::shared_ptr<tsf> (t, [] (tsf* p) { tsf_close (p); });
}

void OrganEngine::ensureSynth (Division& div)
{
    if (div.synth == nullptr)
    {
        div.synth = makeSynth();
        div.usedChannels.reset();
        div.appliedGain = -1.0f;
    }
    prepareDivisionDsp (div);
}

void OrganEngine::prepareDivisionDsp (Division& div)
{
    const size_t frames = (size_t) (sampleRate * 0.012) + 2;
    if (div.delayLine.size() != frames * 2)
    {
        div.delayLine.assign (frames * 2, 0.0f);
        div.delayPos = 0;
    }
}

// Where the pipes of a division stand, seen from the nave: the pedal in the side towers,
// the Hauptwerk in the middle, the Rueckpositiv close in front, the swell box high at the back.
OrganEngine::Placement OrganEngine::placementFor (int d) const
{
    if (! spatial)
        return { 0.5f, 0.5f, 0.4f };
    auto& div = divisions[(size_t) d];
    if (div.pedal)
        return { 0.10f, 0.90f, 0.55f };
    switch (d)
    {
        case 0: return { 0.36f, 0.64f, 0.45f }; // Hauptwerk
        case 1: return { 0.40f, 0.60f, 0.12f }; // Positiv (Rueckpositiv)
        case 2: return { 0.43f, 0.57f, 0.85f }; // Schwellwerk
        case 3: return { 0.26f, 0.74f, 0.65f }; // Solowerk
        default: return { 0.30f, 0.70f, 0.95f }; // Fernwerk and beyond
    }
}

void OrganEngine::applyPlacement (int d)
{
    auto& div = divisions[(size_t) d];
    if (div.synth == nullptr)
        return;
    const auto p = placementFor (d);
    for (auto& st : div.stops)
    {
        if (st.channel >= 0) tsf_channel_set_pan (div.synth.get(), st.channel, p.panC);
        if (st.channel2 >= 0) tsf_channel_set_pan (div.synth.get(), st.channel2, p.panCis);
    }
}

void OrganEngine::setSpatial (bool on)
{
    {
        const juce::ScopedLock sl (lock);
        spatial = on;
        for (int d = 0; d < (int) divisions.size(); ++d)
            applyPlacement (d);
    }
    sendChangeMessage();
}

void OrganEngine::prepare (double sr, int maxBlockSize)
{
    const juce::ScopedLock sl (lock);
    sampleRate = sr;
    interleaved.assign ((size_t) juce::jmax (512, maxBlockSize) * 2, 0.0f);
    for (auto& div : divisions)
    {
        if (div.synth != nullptr)
            tsf_set_output (div.synth.get(), TSF_STEREO_INTERLEAVED, (int) sr, -15.0f);
        div.delayLine.clear();
        prepareDivisionDsp (div);
    }
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (512, maxBlockSize), 2 };
    convolution.prepare (spec);
    limiter.prepare (spec);
    limiter.setThreshold (-1.0f);
    limiter.setRelease (120.0f);
    wet.setSize (2, juce::jmax (512, maxBlockSize));
    send.setSize (2, juce::jmax (512, maxBlockSize));
    reverbSmoothed.reset (sr, 0.05);
    masterSmoothed.reset (sr, 0.05);
    reverbSmoothed.setCurrentAndTargetValue (reverb);
    masterSmoothed.setCurrentAndTargetValue (master);
    dspPrepared = true;
    buildImpulseResponse();
}

// A synthetic church: diffuse stereo tail of about 3.5 s that gets darker as it decays,
// with a few early reflections. Generated here so no impulse-response file is needed.
void OrganEngine::buildImpulseResponse()
{
    const double sr = sampleRate;
    const double rt60 = 3.6;
    const int length = (int) (sr * 4.2);
    juce::AudioBuffer<float> ir (2, length);
    ir.clear();
    juce::Random rnd (4711);
    const int preDelay = (int) (0.022 * sr);
    for (int ch = 0; ch < 2; ++ch)
    {
        auto* d = ir.getWritePointer (ch);
        float lp = 0.0f;
        for (int i = preDelay; i < length; ++i)
        {
            const double t = (i - preDelay) / sr;
            const float env = (float) std::exp (-6.907755 * t / rt60);
            // the tail loses treble over time: lowpass coefficient falls from ~0.9 to ~0.15
            const float coeff = (float) juce::jmap (std::exp (-t / 0.9), 0.0, 1.0, 0.15, 0.9);
            const float noise = rnd.nextFloat() * 2.0f - 1.0f;
            lp += coeff * (noise - lp);
            // soft onset so the tail builds up like in a large room
            const float build = (float) juce::jmin (1.0, t / 0.08);
            d[i] = lp * env * build;
        }
        // early reflections
        const double taps[] = { 0.011, 0.019, 0.027, 0.034, 0.046, 0.058, 0.071 };
        for (int k = 0; k < 7; ++k)
        {
            const int pos = preDelay + (int) ((taps[k] + (ch ? 0.0031 * k : 0.0)) * sr);
            if (pos < length)
                d[pos] += (0.5f - 0.05f * k) * (k % 2 ? -1.0f : 1.0f);
        }
    }
    convolution.loadImpulseResponse (std::move (ir), sr, juce::dsp::Convolution::Stereo::yes,
                                     juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::yes);
}

bool OrganEngine::isReverbReady() const
{
    return const_cast<juce::dsp::Convolution&> (convolution).getCurrentIRSize() > 0;
}

// Volume, swell and tremulant reach the voices in small smooth steps, never as jumps.
void OrganEngine::updateGains (int numSamples)
{
    tremPhase += juce::MathConstants<double>::twoPi * 5.5 * numSamples / sampleRate;
    if (tremPhase > juce::MathConstants<double>::twoPi)
        tremPhase -= juce::MathConstants<double>::twoPi;
    const float lfo = (float) std::sin (tremPhase);
    for (auto& div : divisions)
    {
        if (div.synth == nullptr)
            continue;
        const float target = div.volume * div.expression * (div.tremulant ? 1.0f + 0.18f * lfo : 1.0f);
        const float g = div.appliedGain < 0.0f ? target : div.appliedGain + (target - div.appliedGain) * 0.3f;
        if (std::abs (g - div.appliedGain) > 1.0e-4f)
        {
            for (auto& st : div.stops)
                for (int ch : { st.channel, st.channel2 })
                    if (ch >= 0)
                        tsf_channel_set_volume (div.synth.get(), ch, g);
            div.appliedGain = g;
        }
        if (div.tremulant)
            for (auto& st : div.stops)
                for (int ch : { st.channel, st.channel2 })
                    if (ch >= 0)
                        tsf_channel_set_pitchwheel (div.synth.get(), ch, 8192 + (int) (lfo * 300.0f));
    }
}

void OrganEngine::renderLocked (juce::AudioBuffer<float>& out, const std::vector<KeyEvent>& events)
{
    const int numSamples = out.getNumSamples();
    out.clear();
    if (numSamples == 0 || soundFont == nullptr)
        return;
    const bool stereo = out.getNumChannels() > 1;
    const bool useSend = stereo && dspPrepared && send.getNumSamples() >= numSamples;
    if (useSend)
        send.clear (0, numSamples);

    // every division renders into the mix from its own place: delay and treble loss with
    // distance, more of it into the reverb the further back it stands
    auto renderRange = [&] (int start, int end) {
        const int maxFrames = (int) interleaved.size() / 2;
        while (start < end)
        {
            const int n = juce::jmin (end - start, juce::jmin (maxFrames, 64));
            updateGains (n);
            for (int d = 0; d < (int) divisions.size(); ++d)
            {
                auto& div = divisions[(size_t) d];
                if (div.synth == nullptr || div.delayLine.empty())
                    continue;
                tsf_render_float (div.synth.get(), interleaved.data(), n, 0);
                const auto pl = placementFor (d);
                const float dryGain = 1.0f - 0.45f * pl.depth;
                const float sendGain = 0.35f + 0.9f * pl.depth;
                const float fc = 18000.0f - 11000.0f * pl.depth;
                const float coeff = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * fc / (float) sampleRate);
                const int frames = (int) div.delayLine.size() / 2;
                const int delay = juce::jlimit (0, frames - 1, (int) (pl.depth * 0.008 * sampleRate));
                auto* l = out.getWritePointer (0, start);
                auto* r = stereo ? out.getWritePointer (1, start) : nullptr;
                auto* sl = useSend ? send.getWritePointer (0, start) : nullptr;
                auto* sr = useSend ? send.getWritePointer (1, start) : nullptr;
                for (int i = 0; i < n; ++i)
                {
                    float x[2] = { interleaved[(size_t) i * 2], interleaved[(size_t) i * 2 + 1] };
                    for (int c = 0; c < 2; ++c)
                    {
                        div.lowpass[c] += coeff * (x[c] - div.lowpass[c]);
                        div.delayLine[(size_t) (div.delayPos * 2 + c)] = div.lowpass[c];
                    }
                    const int readPos = (div.delayPos - delay + frames) % frames;
                    const float yl = div.delayLine[(size_t) readPos * 2], yr = div.delayLine[(size_t) readPos * 2 + 1];
                    div.delayPos = (div.delayPos + 1) % frames;
                    if (r != nullptr)
                    {
                        l[i] += yl * dryGain;
                        r[i] += yr * dryGain;
                    }
                    else
                        l[i] += 0.5f * (yl + yr) * dryGain;
                    if (sl != nullptr)
                    {
                        sl[i] += yl * sendGain;
                        sr[i] += yr * sendGain;
                    }
                }
            }
            start += n;
        }
    };

    int pos = 0;
    for (auto& e : events)
    {
        const int at = juce::jlimit (0, numSamples, e.sample);
        if (at > pos)
        {
            renderRange (pos, at);
            pos = at;
        }
        if (e.division < 0 || e.division >= (int) divisions.size() || e.key < 0 || e.key > 127)
            continue;
        if (e.on)
            keyDownLocked (e.division, e.key, e.source);
        else
            keyUpLocked (e.division, e.key, e.source);
    }
    renderRange (pos, numSamples);

    reverbSmoothed.setTargetValue (reverb);
    masterSmoothed.setTargetValue (master);

    // church reverb (convolution) fed by the send bus
    if (useSend && wet.getNumSamples() >= numSamples)
    {
        for (int ch = 0; ch < 2; ++ch)
            wet.copyFrom (ch, 0, send, ch, 0, numSamples);
        juce::dsp::AudioBlock<float> wetBlock (wet.getArrayOfWritePointers(), 2, (size_t) numSamples);
        convolution.process (juce::dsp::ProcessContextReplacing<float> (wetBlock));
        for (int i = 0; i < numSamples; ++i)
        {
            const float rv = reverbSmoothed.getNextValue();
            const float dry = 1.0f - 0.3f * rv, wetGain = 0.75f * rv;
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* o = out.getWritePointer (ch);
                o[i] = o[i] * dry + wet.getReadPointer (ch)[i] * wetGain;
            }
        }
    }

    // master volume, then a limiter so a full tutti never clips
    for (int i = 0; i < numSamples; ++i)
    {
        const float m = masterSmoothed.getNextValue();
        for (int ch = 0; ch < out.getNumChannels(); ++ch)
            out.getWritePointer (ch)[i] *= m;
    }
    if (dspPrepared && out.getNumChannels() <= 2)
    {
        juce::dsp::AudioBlock<float> block (out.getArrayOfWritePointers(), (size_t) out.getNumChannels(), (size_t) numSamples);
        limiter.process (juce::dsp::ProcessContextReplacing<float> (block));
    }
}

//==============================================================================
static const std::vector<std::pair<int, int>> pedalStops = {
    { 1, 60 }, { 0, 110 }, { 0, 111 }, { 1, 62 }, { 1, 61 }, { 0, 112 }, { 1, 46 }, { 0, 120 },
    { 0, 115 }, { 0, 102 }, { 0, 101 }, { 0, 116 }, { 1, 58 }
};
static const std::vector<std::pair<int, int>> greatStops = {
    { 0, 15 }, { 0, 0 }, { 0, 16 }, { 0, 19 }, { 0, 1 }, { 0, 17 }, { 0, 11 }, { 0, 2 },
    { 0, 12 }, { 0, 33 }, { 0, 34 }, { 0, 35 }, { 0, 98 }, { 0, 25 }, { 0, 78 }
};
static const std::vector<std::pair<int, int>> positiveStops = {
    { 0, 45 }, { 0, 51 }, { 0, 53 }, { 0, 65 }, { 0, 46 }, { 0, 54 }, { 0, 56 }, { 0, 55 },
    { 0, 13 }, { 0, 57 }, { 0, 27 }, { 0, 89 }, { 0, 71 }, { 0, 77 }
};
static const std::vector<std::pair<int, int>> swellStops = {
    { 1, 5 }, { 0, 64 }, { 1, 0 }, { 1, 55 }, { 0, 93 }, { 0, 66 }, { 1, 19 }, { 0, 103 },
    { 1, 69 }, { 1, 53 }, { 0, 96 }, { 1, 54 }, { 0, 74 }
};
static const std::vector<std::pair<int, int>> soloStops = {
    { 1, 52 }, { 1, 51 }, { 0, 73 }, { 0, 75 }, { 1, 74 }, { 1, 75 }, { 0, 124 }, { 0, 126 }, { 0, 127 }
};

static juce::String defaultManualName (int n)
{
    switch (n)
    {
        case 1: return "Hauptwerk";
        case 2: return "Positiv";
        case 3: return "Schwellwerk";
        case 4: return "Solowerk";
        case 5: return "Fernwerk";
        default: return "Manual " + romanNumeral (n);
    }
}

static const std::vector<std::pair<int, int>>& defaultStopsFor (int manualNumber)
{
    static const std::vector<std::pair<int, int>> none;
    switch (manualNumber)
    {
        case 1: return greatStops;
        case 2: return positiveStops;
        case 3: return swellStops;
        case 4: return soloStops;
        default: return none;
    }
}

void OrganEngine::setDefaultDisposition()
{
    {
        const juce::ScopedLock sl (lock);
        silenceAll();
        divisions.clear();
        couplers.clear();

        for (int m = 1; m <= 3; ++m)
        {
            Division d;
            d.name = defaultManualName (m);
            d.midi.channel = m;
            divisions.push_back (d);
        }
        Division ped;
        ped.name = "Pedal";
        ped.pedal = true;
        ped.midi.channel = 4;
        divisions.push_back (ped);
        rebuildCouplers();
    }

    for (int m = 1; m <= 3; ++m)
        for (auto& [b, p] : defaultStopsFor (m))
            addStop (m - 1, b, p, {});
    for (auto& [b, p] : pedalStops)
        addStop (pedalIndex(), b, p, {});

    // a sensible starting registration: Montre 8 + Prestant 4, pedal 16'
    setStop (0, 1, true);
    setStop (0, 4, true);
    setStop (pedalIndex(), 1, true);
    setStop (pedalIndex(), 2, true);
    sendChangeMessage();
}

int OrganEngine::getNumManuals() const
{
    int n = 0;
    for (auto& d : divisions)
        n += d.pedal ? 0 : 1;
    return n;
}

int OrganEngine::pedalIndex() const
{
    for (int i = 0; i < (int) divisions.size(); ++i)
        if (divisions[(size_t) i].pedal)
            return i;
    return -1;
}

juce::String OrganEngine::couplerName (const Coupler& c) const
{
    auto label = [this] (int d) {
        return divisions[(size_t) d].pedal ? juce::String ("P") : romanNumeral (d + 1);
    };
    return label (c.from) + "/" + label (c.to);
}

void OrganEngine::rebuildCouplers()
{
    // keep the state of couplers that still exist (matched by name)
    juce::StringArray wasOn;
    for (auto& c : couplers)
        if (c.on && c.from < (int) divisions.size() && c.to < (int) divisions.size())
            wasOn.add (couplerName (c));

    couplers.clear();
    const int ped = pedalIndex();
    const int manuals = (int) divisions.size() - (ped >= 0 ? 1 : 0);
    for (int to = 0; to < manuals; ++to)
        for (int from = to + 1; from < manuals; ++from)
            couplers.push_back ({ from, to, false });
    if (ped >= 0)
        for (int from = 0; from < manuals; ++from)
            couplers.push_back ({ from, ped, false });

    for (auto& c : couplers)
        c.on = wasOn.contains (couplerName (c));
}

int OrganEngine::addManual()
{
    int index;
    {
        const juce::ScopedLock sl (lock);
        if (getNumManuals() >= 10)
            return -1;
        const int number = getNumManuals() + 1;
        Division d;
        d.name = defaultManualName (number);
        d.midi.channel = juce::jmin (16, number <= 3 ? number : number + 1);
        const int ped = pedalIndex();
        index = ped >= 0 ? ped : (int) divisions.size();
        // pedal stays last; note-offs keep their division index valid because we release first
        silenceAll();
        for (auto& c : couplers)
        {
            if (c.from >= index) ++c.from;
            if (c.to >= index) ++c.to;
        }
        divisions.insert (divisions.begin() + index, d);
        rebuildCouplers();
    }
    for (auto& [b, p] : defaultStopsFor (index + 1))
        addStop (index, b, p, {});
    sendChangeMessage();
    return index;
}

void OrganEngine::removeDivision (int d)
{
    {
        const juce::ScopedLock sl (lock);
        if (d < 0 || d >= (int) divisions.size())
            return;
        silenceAll();
        // keep coupler states by remapping before erasing
        std::vector<Coupler> kept;
        for (auto c : couplers)
        {
            if (c.from == d || c.to == d)
                continue;
            if (c.from > d) --c.from;
            if (c.to > d) --c.to;
            kept.push_back (c);
        }
        divisions.erase (divisions.begin() + d);
        couplers = kept;
        rebuildCouplers();
        for (int i = 0; i < (int) divisions.size(); ++i)
            applyPlacement (i); // manuals moved up: they now stand elsewhere
    }
    sendChangeMessage();
}

void OrganEngine::renameDivision (int d, const juce::String& name)
{
    {
        const juce::ScopedLock sl (lock);
        if (d >= 0 && d < (int) divisions.size())
            divisions[(size_t) d].name = name;
    }
    sendChangeMessage();
}

int OrganEngine::addStop (int d, int bank, int program, const juce::String& name, bool on)
{
    int index = -1;
    {
        const juce::ScopedLock sl (lock);
        if (d < 0 || d >= (int) divisions.size())
            return -1;
        Stop st;
        st.bank = bank;
        st.program = program;
        st.name = name.isNotEmpty() ? name : (soundFont ? soundFont->presetName (bank, program) : juce::String (program));
        auto& div = divisions[(size_t) d];
        assignChannel (d, st);
        div.stops.push_back (st);
        index = (int) div.stops.size() - 1;
        if (on)
        {
            div.stops.back().on = true;
            reconcileStop (d, index);
        }
    }
    sendChangeMessage();
    return index;
}

void OrganEngine::removeStop (int d, int s)
{
    {
        const juce::ScopedLock sl (lock);
        if (d < 0 || d >= (int) divisions.size() || s < 0 || s >= (int) divisions[(size_t) d].stops.size())
            return;
        auto& div = divisions[(size_t) d];
        auto& st = div.stops[(size_t) s];
        stopVoice (div, st);
        freeChannel (div, st.channel);
        freeChannel (div, st.channel2);
        divisions[(size_t) d].stops.erase (divisions[(size_t) d].stops.begin() + s);
    }
    sendChangeMessage();
}

void OrganEngine::moveStop (int d, int s, int newIndex)
{
    {
        const juce::ScopedLock sl (lock);
        if (d < 0 || d >= (int) divisions.size())
            return;
        auto& stops = divisions[(size_t) d].stops;
        if (s < 0 || s >= (int) stops.size())
            return;
        newIndex = juce::jlimit (0, (int) stops.size() - 1, newIndex);
        auto st = stops[(size_t) s];
        stops.erase (stops.begin() + s);
        stops.insert (stops.begin() + newIndex, st);
    }
    sendChangeMessage();
}

void OrganEngine::setStop (int d, int s, bool on)
{
    {
        const juce::ScopedLock sl (lock);
        if (d < 0 || d >= (int) divisions.size() || s < 0 || s >= (int) divisions[(size_t) d].stops.size())
            return;
        divisions[(size_t) d].stops[(size_t) s].on = on;
        reconcileStop (d, s);
    }
    sendChangeMessage();
}

void OrganEngine::toggleStop (int d, int s)
{
    bool now = false;
    {
        const juce::ScopedLock sl (lock);
        if (d < 0 || d >= (int) divisions.size() || s < 0 || s >= (int) divisions[(size_t) d].stops.size())
            return;
        now = ! divisions[(size_t) d].stops[(size_t) s].on;
    }
    setStop (d, s, now);
}

void OrganEngine::setCoupler (int c, bool on)
{
    {
        const juce::ScopedLock sl (lock);
        if (c < 0 || c >= (int) couplers.size())
            return;
        couplers[(size_t) c].on = on;
        reconcileDivisionStops (couplers[(size_t) c].from);
    }
    sendChangeMessage();
}

void OrganEngine::setDivisionVolume (int d, float v)
{
    const juce::ScopedLock sl (lock);
    if (d < 0 || d >= (int) divisions.size())
        return;
    divisions[(size_t) d].volume = v;
    applyChannelVolumes (d);
}

void OrganEngine::setDivisionExpression (int d, float v)
{
    const juce::ScopedLock sl (lock);
    if (d < 0 || d >= (int) divisions.size())
        return;
    divisions[(size_t) d].expression = v;
    applyChannelVolumes (d);
}

void OrganEngine::setTremulant (int d, bool on)
{
    {
        const juce::ScopedLock sl (lock);
        if (d < 0 || d >= (int) divisions.size())
            return;
        divisions[(size_t) d].tremulant = on;
        auto& div = divisions[(size_t) d];
        if (! on && div.synth != nullptr)
            for (auto& st : div.stops)
                for (int ch : { st.channel, st.channel2 })
                    if (ch >= 0)
                        tsf_channel_set_pitchwheel (div.synth.get(), ch, 8192);
        applyChannelVolumes (d);
    }
    sendChangeMessage();
}

std::array<uint8_t, 128> OrganEngine::getSoundingCounts() const
{
    const juce::ScopedLock sl (lock);
    std::array<uint8_t, 128> counts {};
    for (auto& d : divisions)
        for (auto& st : d.stops)
            if (st.sounding.any())
                for (int k = 0; k < 128; ++k)
                    if (st.sounding[(size_t) k])
                        counts[(size_t) k] = (uint8_t) juce::jmin (255, counts[(size_t) k] + 1);
    return counts;
}

OrganEngine::Combination OrganEngine::parseCombination (const juce::String& text) const
{
    const juce::ScopedLock sl (lock);
    Combination c;
    auto on = juce::StringArray::fromTokens (text.upToFirstOccurrenceOf (";", false, false), ",", "");
    auto con = juce::StringArray::fromTokens (text.fromFirstOccurrenceOf (";", false, false), ",", "");
    for (int d = 0; d < (int) divisions.size(); ++d)
    {
        c.stops.emplace_back();
        for (int s = 0; s < (int) divisions[(size_t) d].stops.size(); ++s)
            c.stops.back().push_back (on.contains (juce::String (d) + ":" + juce::String (s)));
    }
    for (int i = 0; i < (int) couplers.size(); ++i)
        c.couplers.push_back (con.contains (juce::String (i)));
    return c;
}

void OrganEngine::applyCombinationLocked (const Combination& c)
{
    for (int d = 0; d < (int) divisions.size() && d < (int) c.stops.size(); ++d)
        for (int s = 0; s < (int) divisions[(size_t) d].stops.size() && s < (int) c.stops[(size_t) d].size(); ++s)
            divisions[(size_t) d].stops[(size_t) s].on = c.stops[(size_t) d][(size_t) s];
    for (int i = 0; i < (int) couplers.size() && i < (int) c.couplers.size(); ++i)
        couplers[(size_t) i].on = c.couplers[(size_t) i];
    for (int d = 0; d < (int) divisions.size(); ++d)
        reconcileDivisionStops (d);
}

void OrganEngine::setMidiAssign (int d, const MidiAssign& m)
{
    {
        const juce::ScopedLock sl (lock);
        if (d < 0 || d >= (int) divisions.size())
            return;
        divisions[(size_t) d].midi = m;
    }
    sendChangeMessage();
}

void OrganEngine::clearRegistration()
{
    {
        const juce::ScopedLock sl (lock);
        for (int d = 0; d < (int) divisions.size(); ++d)
        {
            for (auto& st : divisions[(size_t) d].stops)
                st.on = false;
            reconcileDivisionStops (d);
        }
        for (auto& c : couplers)
            c.on = false;
    }
    sendChangeMessage();
}

void OrganEngine::applyRegistration (int d, const std::vector<std::pair<int, int>>& bankPrograms)
{
    if (d < 0 || d >= getNumDivisions())
        return;
    std::vector<int> wanted;
    for (auto [bank, program] : bankPrograms)
    {
        if (soundFont && ! soundFont->resolvePreset (bank, program))
            continue;
        int found = -1;
        {
            const juce::ScopedLock sl (lock);
            auto& stops = divisions[(size_t) d].stops;
            for (int s = 0; s < (int) stops.size(); ++s)
                if (stops[(size_t) s].bank == bank && stops[(size_t) s].program == program)
                    found = s;
        }
        if (found < 0)
            found = addStop (d, bank, program, {});
        if (found >= 0)
            wanted.push_back (found);
    }
    {
        const juce::ScopedLock sl (lock);
        auto& stops = divisions[(size_t) d].stops;
        for (int s = 0; s < (int) stops.size(); ++s)
        {
            stops[(size_t) s].on = std::find (wanted.begin(), wanted.end(), s) != wanted.end();
            reconcileStop (d, s);
        }
    }
    sendChangeMessage();
}

juce::String OrganEngine::captureCombination() const
{
    const juce::ScopedLock sl (lock);
    juce::StringArray parts;
    for (int d = 0; d < (int) divisions.size(); ++d)
        for (int s = 0; s < (int) divisions[(size_t) d].stops.size(); ++s)
            if (divisions[(size_t) d].stops[(size_t) s].on)
                parts.add (juce::String (d) + ":" + juce::String (s));
    juce::StringArray cs;
    for (int c = 0; c < (int) couplers.size(); ++c)
        if (couplers[(size_t) c].on)
            cs.add (juce::String (c));
    return parts.joinIntoString (",") + ";" + cs.joinIntoString (",");
}

void OrganEngine::recallCombination (const juce::String& combination)
{
    {
        const juce::ScopedLock sl (lock);
        auto stopsPart = combination.upToFirstOccurrenceOf (";", false, false);
        auto couplerPart = combination.fromFirstOccurrenceOf (";", false, false);
        juce::StringArray on = juce::StringArray::fromTokens (stopsPart, ",", "");
        juce::StringArray con = juce::StringArray::fromTokens (couplerPart, ",", "");
        for (int d = 0; d < (int) divisions.size(); ++d)
            for (int s = 0; s < (int) divisions[(size_t) d].stops.size(); ++s)
                divisions[(size_t) d].stops[(size_t) s].on = on.contains (juce::String (d) + ":" + juce::String (s));
        for (int c = 0; c < (int) couplers.size(); ++c)
            couplers[(size_t) c].on = con.contains (juce::String (c));
        for (int d = 0; d < (int) divisions.size(); ++d)
            reconcileDivisionStops (d);
    }
    sendChangeMessage();
}

void OrganEngine::setTranspose (int semitones)
{
    {
        const juce::ScopedLock sl (lock);
        // re-sound everything at the new pitch
        for (auto& div : divisions)
            for (auto& st : div.stops)
                stopVoice (div, st);
        transpose = juce::jlimit (-12, 12, semitones);
        for (int d = 0; d < (int) divisions.size(); ++d)
            reconcileDivisionStops (d);
    }
    sendChangeMessage();
}

//==============================================================================
void OrganEngine::keyDown (int d, int key, uint8_t source)
{
    const juce::ScopedLock sl (lock);
    keyDownLocked (d, key, source);
}

void OrganEngine::keyUp (int d, int key, uint8_t source)
{
    const juce::ScopedLock sl (lock);
    keyUpLocked (d, key, source);
}

void OrganEngine::keyDownLocked (int d, int key, uint8_t source)
{
    if (d < 0 || d >= (int) divisions.size() || key < 0 || key > 127)
        return;
    auto& h = divisions[(size_t) d].held[(size_t) key];
    if ((h & source) != 0)
        return;
    h = (uint8_t) (h | source);
    reconcileKey (d, key);
}

void OrganEngine::keyUpLocked (int d, int key, uint8_t source)
{
    if (d < 0 || d >= (int) divisions.size() || key < 0 || key > 127)
        return;
    auto& h = divisions[(size_t) d].held[(size_t) key];
    if ((h & source) == 0)
        return;
    h = (uint8_t) (h & ~source);
    reconcileKey (d, key);
}

void OrganEngine::releaseSource (uint8_t source)
{
    const juce::ScopedLock sl (lock);
    for (int d = 0; d < (int) divisions.size(); ++d)
        for (int k = 0; k < 128; ++k)
            if ((divisions[(size_t) d].held[(size_t) k] & source) != 0)
                keyUpLocked (d, k, source);
}

void OrganEngine::allNotesOff()
{
    const juce::ScopedLock sl (lock);
    silenceAll();
}

void OrganEngine::silenceAll()
{
    for (auto& div : divisions)
    {
        div.held.fill (0);
        for (auto& st : div.stops)
            stopVoice (div, st);
    }
}

std::bitset<128> OrganEngine::getHeldKeys (int d) const
{
    const juce::ScopedLock sl (lock);
    std::bitset<128> b;
    if (d >= 0 && d < (int) divisions.size())
        for (int k = 0; k < 128; ++k)
            b[(size_t) k] = divisions[(size_t) d].held[(size_t) k] != 0;
    return b;
}

bool OrganEngine::keyActive (int d, int key) const
{
    if (divisions[(size_t) d].held[(size_t) key] != 0)
        return true;
    for (auto& c : couplers)
        if (c.on && c.from == d && divisions[(size_t) c.to].held[(size_t) key] != 0)
            return true;
    return false;
}

void OrganEngine::reconcileKey (int target, int key)
{
    auto touch = [this, key] (int d) {
        auto& div = divisions[(size_t) d];
        if (div.synth == nullptr)
            return;
        const bool active = keyActive (d, key);
        const int sk = juce::jlimit (0, 127, key + transpose);
        for (auto& st : div.stops)
        {
            const bool want = st.on && active;
            const int ch = cisSide (sk) ? st.channel2 : st.channel;
            if (want == st.sounding[(size_t) key] || ch < 0)
                continue;
            if (want)
                tsf_channel_note_on (div.synth.get(), ch, sk, 0.8f);
            else
                tsf_channel_note_off (div.synth.get(), ch, sk);
            st.sounding[(size_t) key] = want;
        }
    };
    touch (target);
    for (auto& c : couplers)
        if (c.on && c.to == target)
            touch (c.from);
}

void OrganEngine::reconcileStop (int d, int s)
{
    auto& div = divisions[(size_t) d];
    auto& st = div.stops[(size_t) s];
    if (div.synth == nullptr || st.channel < 0)
        return;
    for (int k = 0; k < 128; ++k)
    {
        const bool want = st.on && keyActive (d, k);
        if (want == st.sounding[(size_t) k])
            continue;
        const int sk = juce::jlimit (0, 127, k + transpose);
        const int ch = cisSide (sk) ? st.channel2 : st.channel;
        if (want)
            tsf_channel_note_on (div.synth.get(), ch, sk, 0.8f);
        else
            tsf_channel_note_off (div.synth.get(), ch, sk);
        st.sounding[(size_t) k] = want;
    }
}

void OrganEngine::reconcileDivisionStops (int d)
{
    if (d < 0 || d >= (int) divisions.size())
        return;
    for (int s = 0; s < (int) divisions[(size_t) d].stops.size(); ++s)
        reconcileStop (d, s);
}

void OrganEngine::stopVoice (Division& div, Stop& st)
{
    if (div.synth != nullptr && st.channel >= 0)
        for (int k = 0; k < 128; ++k)
            if (st.sounding[(size_t) k])
            {
                const int sk = juce::jlimit (0, 127, k + transpose);
                tsf_channel_note_off (div.synth.get(), cisSide (sk) ? st.channel2 : st.channel, sk);
            }
    st.sounding.reset();
}

int OrganEngine::allocateChannel (Division& div)
{
    for (int i = 0; i < 256; ++i)
        if (! div.usedChannels[(size_t) i])
        {
            div.usedChannels[(size_t) i] = true;
            return i;
        }
    return -1;
}

void OrganEngine::freeChannel (Division& div, int ch)
{
    if (ch >= 0 && ch < 256)
        div.usedChannels[(size_t) ch] = false;
}

void OrganEngine::assignChannel (int d, Stop& st)
{
    auto& div = divisions[(size_t) d];
    ensureSynth (div);
    if (div.synth == nullptr)
        return;
    if (st.channel < 0)
        st.channel = allocateChannel (div);
    if (st.channel2 < 0)
        st.channel2 = allocateChannel (div);
    if (st.channel < 0 || st.channel2 < 0)
        return;
    int bank = st.bank;
    if (soundFont)
        soundFont->resolvePreset (bank, st.program);
    const auto pl = placementFor (d);
    for (int ch : { st.channel, st.channel2 })
    {
        tsf_channel_set_bank_preset (div.synth.get(), ch, bank, st.program);
        tsf_channel_set_volume (div.synth.get(), ch, div.volume * div.expression);
        tsf_channel_set_pan (div.synth.get(), ch, ch == st.channel ? pl.panC : pl.panCis);
    }
}

void OrganEngine::applyChannelVolumes (int d)
{
    // picked up smoothly by updateGains() on the audio thread
    divisions[(size_t) d].appliedGain = juce::jmax (0.0f, divisions[(size_t) d].appliedGain);
}

//==============================================================================
std::unique_ptr<juce::XmlElement> OrganEngine::toXml() const
{
    const juce::ScopedLock sl (lock);
    auto xml = std::make_unique<juce::XmlElement> ("Organ");
    xml->setAttribute ("reverb", reverb);
    xml->setAttribute ("master", master);
    xml->setAttribute ("transpose", transpose);
    xml->setAttribute ("spatial", spatial);
    for (auto& d : divisions)
    {
        auto* e = xml->createNewChildElement ("Division");
        e->setAttribute ("name", d.name);
        e->setAttribute ("pedal", d.pedal);
        e->setAttribute ("volume", d.volume);
        e->setAttribute ("tremulant", d.tremulant);
        e->setAttribute ("device", d.midi.device);
        e->setAttribute ("deviceName", d.midi.deviceName);
        e->setAttribute ("channel", d.midi.channel);
        for (auto& st : d.stops)
        {
            auto* s = e->createNewChildElement ("Stop");
            s->setAttribute ("name", st.name);
            s->setAttribute ("bank", st.bank);
            s->setAttribute ("program", st.program);
            s->setAttribute ("on", st.on);
        }
    }
    for (auto& c : couplers)
    {
        auto* e = xml->createNewChildElement ("Coupler");
        e->setAttribute ("from", c.from);
        e->setAttribute ("to", c.to);
        e->setAttribute ("on", c.on);
    }
    return xml;
}

void OrganEngine::fromXml (const juce::XmlElement& xml)
{
    if (! xml.hasTagName ("Organ"))
        return;
    {
        const juce::ScopedLock sl (lock);
        silenceAll();
        divisions.clear();
        couplers.clear();
        reverb = (float) xml.getDoubleAttribute ("reverb", 0.35);
        master = (float) xml.getDoubleAttribute ("master", 0.75);
        transpose = xml.getIntAttribute ("transpose", 0);
        spatial = xml.getBoolAttribute ("spatial", true);

        for (auto* e : xml.getChildWithTagNameIterator ("Division"))
        {
            Division d;
            d.name = e->getStringAttribute ("name");
            d.pedal = e->getBoolAttribute ("pedal");
            d.volume = (float) e->getDoubleAttribute ("volume", 1.0);
            d.tremulant = e->getBoolAttribute ("tremulant");
            d.midi.device = e->getStringAttribute ("device");
            d.midi.deviceName = e->getStringAttribute ("deviceName");
            d.midi.channel = e->getIntAttribute ("channel", 0);
            divisions.push_back (d);
            const int di = (int) divisions.size() - 1;
            for (auto* s : e->getChildWithTagNameIterator ("Stop"))
            {
                Stop st;
                st.name = s->getStringAttribute ("name");
                st.bank = s->getIntAttribute ("bank");
                st.program = s->getIntAttribute ("program");
                st.on = s->getBoolAttribute ("on");
                assignChannel (di, st);
                divisions.back().stops.push_back (st);
            }
        }
        // a valid organ always has a pedal last
        if (pedalIndex() < 0)
        {
            Division ped;
            ped.name = "Pedal";
            ped.pedal = true;
            divisions.push_back (ped);
        }
        rebuildCouplers();
        for (auto* e : xml.getChildWithTagNameIterator ("Coupler"))
            for (auto& c : couplers)
                if (c.from == e->getIntAttribute ("from") && c.to == e->getIntAttribute ("to"))
                    c.on = e->getBoolAttribute ("on");
    }
    sendChangeMessage();
}

void OrganEngine::copySetupFrom (OrganEngine& other)
{
    auto xml = other.toXml();
    setSoundFont (other.getSoundFont());
    fromXml (*xml);
}
} // namespace orgel
