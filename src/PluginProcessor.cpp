#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <set>

namespace orgel
{
//==============================================================================
MidiInputRouter::MidiInputRouter() = default;
MidiInputRouter::~MidiInputRouter() { stop(); }

void MidiInputRouter::start()
{
    listConnection = std::make_unique<juce::MidiDeviceListConnection> (
        juce::MidiDeviceListConnection::make ([this] { refreshDevices(); }));
    refreshDevices();
}

void MidiInputRouter::stop()
{
    listConnection.reset();
    for (auto& [id, in] : open)
        if (in != nullptr)
            in->stop();
    open.clear();
}

void MidiInputRouter::refreshDevices()
{
    auto available = juce::MidiInput::getAvailableDevices();
    // close removed devices
    for (auto it = open.begin(); it != open.end();)
    {
        bool still = false;
        for (auto& d : available)
            still |= d.identifier == it->first;
        if (! still)
        {
            if (it->second != nullptr)
                it->second->stop();
            it = open.erase (it);
        }
        else
            ++it;
    }
    for (auto& d : available)
    {
        {
            const juce::SpinLock::ScopedLockType sl (tableLock);
            if (! identifiers.contains (d.identifier))
            {
                identifiers.add (d.identifier);
                names.add (d.name);
            }
        }
        if (open.count (d.identifier) == 0)
            if (auto in = juce::MidiInput::openDevice (d.identifier, this))
            {
                in->start();
                open[d.identifier] = std::move (in);
            }
    }
}

juce::String MidiInputRouter::identifierFor (int i) const
{
    const juce::SpinLock::ScopedLockType sl (tableLock);
    return identifiers[i];
}

juce::String MidiInputRouter::nameFor (int i) const
{
    const juce::SpinLock::ScopedLockType sl (tableLock);
    return names[i];
}

juce::Array<juce::MidiDeviceInfo> MidiInputRouter::connectedDevices() const
{
    return juce::MidiInput::getAvailableDevices();
}

void MidiInputRouter::handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& m)
{
    if (m.getRawDataSize() > 3 || m.isSysEx())
        return;
    int index = -1;
    {
        const juce::SpinLock::ScopedLockType sl (tableLock);
        index = identifiers.indexOf (source->getIdentifier());
    }
    if (index < 0)
        return;
    Message msg;
    msg.device = index;
    msg.size = m.getRawDataSize();
    std::memcpy (msg.data, m.getRawData(), (size_t) msg.size);

    const juce::SpinLock::ScopedLockType sl (writeLock); // several devices call in on their own threads
    const auto scope = fifo.write (1);
    if (scope.blockSize1 > 0)
        buffer[(size_t) scope.startIndex1] = msg;
    else if (scope.blockSize2 > 0)
        buffer[(size_t) scope.startIndex2] = msg;
}

//==============================================================================
OrgelProcessor::OrgelProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    standalone = juce::JUCEApplicationBase::isStandaloneApp();
    organ.setSoundFont (SoundFont::loadBuiltIn());
    organ.setDefaultDisposition();
    events.reserve (8192);
    pendingRegistrationsAudio.reserve (256);
    if (standalone)
    {
        router.start();
        // a score given on the command line (or via "Open with") is loaded at start
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            for (auto& arg : app->getCommandLineParameterArray())
            {
                juce::File f (arg.unquoted());
                if (f.existsAsFile())
                {
                    juce::MessageManager::callAsync ([this, f, token = std::weak_ptr<int> (alive)] {
                        if (! token.expired())
                            loadFileAsync (f);
                    });
                    break;
                }
            }
    }
    startTimerHz (30);
}

OrgelProcessor::~OrgelProcessor()
{
    alive.reset();
    stopTimer();
    pool.removeAllJobs (true, 30000);
    router.stop();
}

bool OrgelProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void OrgelProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    organ.prepare (sampleRate, samplesPerBlock);
}

void OrgelProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    events.clear();
    pendingRegistrationsAudio.clear();

    const juce::ScopedLock sl (organ.getLock());
    const int numDivisions = organ.getNumDivisions();

    auto route = [&] (const juce::MidiMessage& m, int sample, bool fromHost, int deviceIndex) {
        const int ch = m.getChannel();
        juce::String deviceId = fromHost ? juce::String (MidiAssign::hostDevice) : router.identifierFor (deviceIndex);

        if (m.isNoteOn() && learnDivision.load() >= 0)
        {
            learnResult.division = learnDivision.exchange (-1);
            learnResult.channel = ch;
            learnResult.device = deviceId;
            learnResult.deviceName = fromHost ? juce::String ("DAW / Host") : router.nameFor (deviceIndex);
            learnResult.ready = true;
        }
        for (int d = 0; d < numDivisions; ++d)
        {
            auto& a = organ.getDivision (d).midi;
            const bool deviceOk = a.device.isEmpty() || a.device == deviceId;
            const bool channelOk = a.channel == 0 || a.channel == ch;
            if (! deviceOk || ! channelOk)
                continue;
            if (m.isNoteOn())
                events.push_back ({ sample, d, m.getNoteNumber(), true, srcMidi });
            else if (m.isNoteOff())
                events.push_back ({ sample, d, m.getNoteNumber(), false, srcMidi });
            else if (m.isController() && (m.getControllerNumber() == 11 || m.getControllerNumber() == 7))
                organ.setDivisionExpression (d, 0.15f + 0.85f * m.getControllerValue() / 127.0f); // swell pedal
            else if (m.isAllNotesOff() || m.isAllSoundOff())
                for (int k = 0; k < 128; ++k)
                    events.push_back ({ sample, d, k, false, srcMidi });
        }
    };

    // In the standalone app every device is opened by the router; the wrapper's MIDI
    // would only duplicate it. In a DAW, MIDI comes from the host.
    if (! standalone)
        for (const auto meta : midi)
            route (meta.getMessage(), meta.samplePosition, true, -1);
    router.drain ([&] (const MidiInputRouter::Message& msg) {
        route (juce::MidiMessage (msg.data, msg.size), 0, false, msg.device);
    });
    midi.clear();

    // follow the DAW transport if asked to
    if (hostSync && ! standalone)
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
            {
                const bool hostPlaying = pos->getIsPlaying();
                if (hostPlaying && player.hasScore())
                {
                    const double t = pos->getTimeInSeconds().orFallback (0.0);
                    if (! player.isPlaying() || std::abs (player.getPosition() - t) > 0.25)
                    {
                        player.seek (t);
                        player.play();
                    }
                }
                else if (wasHostPlaying && ! hostPlaying)
                {
                    player.pause();
                    player.needsRelease = true;
                }
                wasHostPlaying = hostPlaying;
            }

    if (player.needsRelease.exchange (false))
        organ.releaseSource (srcPlayer);
    player.process (n, currentSampleRate, events, pendingRegistrationsAudio);
    for (int r : pendingRegistrationsAudio)
    {
        if (r <= -2)
        {
            // a registration mark: switch exactly here, on the audio thread
            const int m = -r - 2;
            if (m < (int) markCombos.size())
                organ.applyCombinationLocked (markCombos[(size_t) m]);
            continue;
        }
        const auto scope = regFifo.write (1);
        if (scope.blockSize1 > 0)
            regBuffer[(size_t) scope.startIndex1] = r;
    }

    std::stable_sort (events.begin(), events.end(), [] (const KeyEvent& a, const KeyEvent& b) { return a.sample < b.sample; });
    organ.renderLocked (buffer, events);
}

//==============================================================================
void OrgelProcessor::timerCallback()
{
    // manuals added or removed: the parts of the piece need to find their divisions again
    if (organ.getNumDivisions() != knownDivisions)
    {
        const bool first = knownDivisions < 0;
        knownDivisions = organ.getNumDivisions();
        auto p = getPiece();
        if (! first && p != nullptr && p->score != nullptr)
        {
            auto map = ScoreIO::autoAssign (*p->score, organ.getNumManuals(), organ.pedalIndex());
            {
                const juce::ScopedLock sl (organ.getLock());
                organ.releaseSource (srcPlayer);
                partDivision = map;
                player.setPartDivision (map);
            }
            reengrave();
            sendChangeMessage();
        }
    }

    if (learnResult.ready.exchange (false))
    {
        MidiAssign a;
        a.device = learnResult.device;
        a.deviceName = learnResult.deviceName;
        a.channel = learnResult.channel;
        organ.setMidiAssign (learnResult.division, a);
    }

    // registration changes written in the score (program changes during the piece)
    const auto scope = regFifo.read (regFifo.getNumReady());
    auto apply = [this] (int r) {
        auto p = getPiece();
        if (p == nullptr || p->score == nullptr || r >= (int) p->score->registrationChanges.size())
            return;
        auto& rc = p->score->registrationChanges[(size_t) r];
        if (applyFileRegistration && rc.part < (int) partDivision.size() && partDivision[(size_t) rc.part] >= 0)
            organ.applyRegistration (partDivision[(size_t) rc.part], rc.registration);
    };
    for (int i = 0; i < scope.blockSize1; ++i) apply (regBuffer[(size_t) (scope.startIndex1 + i)]);
    for (int i = 0; i < scope.blockSize2; ++i) apply (regBuffer[(size_t) (scope.startIndex2 + i)]);
}

bool OrgelProcessor::loadSoundFont (const juce::File& f, juce::String& error)
{
    auto sf = SoundFont::loadFromFile (f, error);
    if (sf == nullptr)
        return false;
    organ.setSoundFont (sf);
    return true;
}

void OrgelProcessor::useBuiltInSoundFont()
{
    organ.setSoundFont (SoundFont::loadBuiltIn());
}

void OrgelProcessor::storeCombination (int i)
{
    if (juce::isPositiveAndBelow (i, numCombinations))
        combinations[i] = organ.captureCombination();
    sendChangeMessage();
}

void OrgelProcessor::recallCombination (int i)
{
    if (juce::isPositiveAndBelow (i, numCombinations) && combinations[i].isNotEmpty())
        organ.recallCombination (combinations[i]);
}

//==============================================================================
void OrgelProcessor::loadFileAsync (const juce::File& f)
{
    if (loading.exchange (true))
        return;
    sendChangeMessage();
    auto sf = organ.getSoundFont();
    const int manuals = organ.getNumManuals(), ped = organ.pedalIndex();
    std::weak_ptr<int> token = alive;
    pool.addJob ([this, f, sf, manuals, ped, token] {
        juce::String error;
        std::shared_ptr<Piece> p (Formats::load (f, sf.get(), manuals, ped, error).release());
        juce::MessageManager::callAsync ([this, p, error, token] {
            if (token.expired())
                return;
            {
                const juce::ScopedLock sl (messageLock);
                loadMessage = p == nullptr ? error : p->warning;
            }
            if (p != nullptr)
                setPiece (p);
            loading = false;
            if (restorePending)
            {
                restorePending = false;
                applyFileRegistration = restoreApply;
                for (int i = 0; i < restorePartMap.size() && i < (int) partDivision.size(); ++i)
                    if (restorePartMap[i].getIntValue() < organ.getNumDivisions())
                        setPartDivision (i, restorePartMap[i].getIntValue());
            }
            sendChangeMessage();
        });
    });
}

juce::String OrgelProcessor::takeLoadMessage()
{
    const juce::ScopedLock sl (messageLock);
    auto m = loadMessage;
    loadMessage = {};
    return m;
}

std::shared_ptr<Piece> OrgelProcessor::getPiece() const
{
    const juce::ScopedLock sl (pieceLock);
    return piece;
}

void OrgelProcessor::setPiece (std::shared_ptr<Piece> p)
{
    auto map = ScoreIO::autoAssign (*p->score, organ.getNumManuals(), organ.pedalIndex());
    {
        const juce::ScopedLock sl (organ.getLock());
        organ.releaseSource (srcPlayer);
        player.setScore (p->score, map);
    }
    {
        const juce::ScopedLock sl (pieceLock);
        const bool sameFile = piece != nullptr && piece->source == p->source;
        piece = p;
        partDivision = map;
        if (! sameFile && ! restorePending)
            marks.clear();
    }
    compileMarks();
    if (applyFileRegistration)
        applyPieceRegistration();
    sendChangeMessage();
}

void OrgelProcessor::applyPieceRegistration()
{
    auto p = getPiece();
    if (p == nullptr)
        return;
    std::set<int> touched;
    for (int part = 0; part < (int) p->score->parts.size(); ++part)
    {
        const int d = part < (int) partDivision.size() ? partDivision[(size_t) part] : -1;
        auto& reg = p->score->parts[(size_t) part].registration;
        if (d < 0 || reg.empty())
            continue;
        // two parts on one division: combine their stops
        Registration combined = reg;
        if (touched.count (d))
            for (int s = 0; s < (int) organ.getDivision (d).stops.size(); ++s)
                if (organ.getDivision (d).stops[(size_t) s].on)
                    combined.push_back ({ organ.getDivision (d).stops[(size_t) s].bank, organ.getDivision (d).stops[(size_t) s].program });
        organ.applyRegistration (d, combined);
        touched.insert (d);
    }
}

void OrgelProcessor::setPartDivision (int part, int division)
{
    {
        const juce::ScopedLock sl (organ.getLock());
        if (part < 0 || part >= (int) partDivision.size())
            return;
        organ.releaseSource (srcPlayer);
        partDivision[(size_t) part] = division;
        player.setPartDivision (partDivision);
    }
    reengrave();
    sendChangeMessage();
}

std::vector<bool> OrgelProcessor::partIsPedal() const
{
    std::vector<bool> r;
    for (int d : partDivision)
        r.push_back (d >= 0 && d == organ.pedalIndex());
    return r;
}

void OrgelProcessor::reengrave()
{
    auto p = getPiece();
    if (p == nullptr || ! p->notationFromScore)
        return;
    auto pedal = partIsPedal();
    auto map = partDivision;
    std::weak_ptr<int> token = alive;
    pool.addJob ([this, p, pedal, map, token] {
        juce::String error;
        auto n = Formats::engrave (*p->score, pedal, map, error);
        if (n == nullptr)
            return;
        juce::MessageManager::callAsync ([this, p, n, token] {
            if (token.expired())
                return;
            const juce::ScopedLock sl (pieceLock);
            if (piece != p)
                return;
            auto copy = std::make_shared<Piece> (*p);
            copy->notation = n;
            piece = copy;
            sendChangeMessage();
        });
    });
}

std::vector<OrgelProcessor::RegistrationMark> OrgelProcessor::getMarks() const
{
    const juce::ScopedLock sl (pieceLock);
    return marks;
}

void OrgelProcessor::compileMarks()
{
    auto p = getPiece();
    std::vector<OrganEngine::Combination> combos;
    std::vector<double> times;
    {
        const juce::ScopedLock sl (pieceLock);
        for (auto& m : marks)
        {
            combos.push_back (organ.parseCombination (m.combination));
            times.push_back (p != nullptr ? p->score->beatToSeconds (m.beat) : 0.0);
        }
    }
    const juce::ScopedLock sl (organ.getLock());
    markCombos = std::move (combos);
    player.setMarkTimes (std::move (times));
}

void OrgelProcessor::addMarkHere()
{
    if (getPiece() == nullptr)
        return;
    const double beat = getCurrentBeat();
    {
        const juce::ScopedLock sl (pieceLock);
        // replace a mark at (almost) the same spot
        marks.erase (std::remove_if (marks.begin(), marks.end(), [beat] (auto& m) { return std::abs (m.beat - beat) < 0.05; }), marks.end());
        marks.push_back ({ beat, organ.captureCombination() });
        std::sort (marks.begin(), marks.end(), [] (auto& a, auto& b) { return a.beat < b.beat; });
    }
    compileMarks();
    sendChangeMessage();
}

void OrgelProcessor::updateMark (int index)
{
    {
        const juce::ScopedLock sl (pieceLock);
        if (! juce::isPositiveAndBelow (index, (int) marks.size()))
            return;
        marks[(size_t) index].combination = organ.captureCombination();
    }
    compileMarks();
    sendChangeMessage();
}

void OrgelProcessor::removeMark (int index)
{
    {
        const juce::ScopedLock sl (pieceLock);
        if (! juce::isPositiveAndBelow (index, (int) marks.size()))
            return;
        marks.erase (marks.begin() + index);
    }
    compileMarks();
    sendChangeMessage();
}

void OrgelProcessor::clearMarks()
{
    {
        const juce::ScopedLock sl (pieceLock);
        marks.clear();
    }
    compileMarks();
    sendChangeMessage();
}

// Starting in the middle of the piece: use the registration of the last mark before here.
void OrgelProcessor::applyMarkAtPosition()
{
    const double beat = getCurrentBeat();
    juce::String combo;
    {
        const juce::ScopedLock sl (pieceLock);
        for (auto& m : marks)
            if (m.beat <= beat + 1.0e-6)
                combo = m.combination;
    }
    if (combo.isNotEmpty())
        organ.recallCombination (combo);
}

void OrgelProcessor::play()
{
    applyMarkAtPosition();
    const juce::ScopedLock sl (organ.getLock());
    if (player.hasScore())
        player.play();
}

void OrgelProcessor::pause()
{
    const juce::ScopedLock sl (organ.getLock());
    player.pause();
    player.needsRelease = true;
}

void OrgelProcessor::stop()
{
    const juce::ScopedLock sl (organ.getLock());
    player.pause();
    player.seek (0.0);
}

void OrgelProcessor::seek (double seconds)
{
    {
        const juce::ScopedLock sl (organ.getLock());
        player.seek (seconds);
    }
    applyMarkAtPosition();
}

double OrgelProcessor::getCurrentBeat() const
{
    auto p = getPiece();
    if (p == nullptr)
        return 0;
    return p->score->secondsToBeat (player.getPosition());
}

Formats::ExportContext OrgelProcessor::makeExportContext() const
{
    Formats::ExportContext ctx;
    ctx.partDivision = partDivision;
    ctx.partIsPedal = partIsPedal();
    for (int d : partDivision)
        ctx.partNames.push_back (d >= 0 && d < organ.getNumDivisions() ? organ.getDivision (d).name : juce::String());
    ctx.liveOrgan = const_cast<OrganEngine*> (&organ);
    ctx.sampleRate = getSampleRateForExport();
    ctx.speed = player.speed.load();
    if (auto p = getPiece())
        for (auto& m : getMarks())
            ctx.registrationMarks.push_back ({ p->score->beatToSeconds (m.beat), m.combination });
    return ctx;
}

//==============================================================================
void OrgelProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::XmlElement root ("Orgelwerk");
    root.addChildElement (organ.toXml().release());
    auto sf = organ.getSoundFont();
    if (sf != nullptr && sf->getFile().existsAsFile())
        root.setAttribute ("soundfont", sf->getFile().getFullPathName());
    if (auto p = getPiece())
        root.setAttribute ("file", p->source.getFullPathName());
    juce::StringArray map;
    for (int d : partDivision)
        map.add (juce::String (d));
    root.setAttribute ("partDivision", map.joinIntoString (","));
    root.setAttribute ("applyRegistration", applyFileRegistration);
    root.setAttribute ("hostSync", hostSync);
    root.setAttribute ("speed", (double) player.speed.load());
    for (int i = 0; i < numCombinations; ++i)
        root.setAttribute ("combination" + juce::String (i), combinations[i]);
    auto* marksXml = root.createNewChildElement ("Marks");
    for (auto& m : getMarks())
    {
        auto* e = marksXml->createNewChildElement ("Mark");
        e->setAttribute ("beat", m.beat);
        e->setAttribute ("combination", m.combination);
    }
    copyXmlToBinary (root, destData);
}

void OrgelProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName ("Orgelwerk"))
        return;
    auto sfPath = xml->getStringAttribute ("soundfont");
    if (sfPath.isNotEmpty())
    {
        juce::String err;
        loadSoundFont (juce::File (sfPath), err);
    }
    if (auto* o = xml->getChildByName ("Organ"))
        organ.fromXml (*o);
    applyFileRegistration = xml->getBoolAttribute ("applyRegistration", true);
    hostSync = xml->getBoolAttribute ("hostSync", false);
    player.speed = (float) xml->getDoubleAttribute ("speed", 1.0);
    for (int i = 0; i < numCombinations; ++i)
        combinations[i] = xml->getStringAttribute ("combination" + juce::String (i));

    {
        const juce::ScopedLock sl (pieceLock);
        marks.clear();
        if (auto* mx = xml->getChildByName ("Marks"))
            for (auto* e : mx->getChildWithTagNameIterator ("Mark"))
                marks.push_back ({ e->getDoubleAttribute ("beat"), e->getStringAttribute ("combination") });
    }
    // reload the piece but keep the registration and part mapping the user saved
    juce::File f (xml->getStringAttribute ("file"));
    if (f.existsAsFile())
    {
        restoreApply = applyFileRegistration;
        restorePartMap = juce::StringArray::fromTokens (xml->getStringAttribute ("partDivision"), ",", "");
        restorePending = true;
        applyFileRegistration = false;
        loadFileAsync (f);
    }
    sendChangeMessage();
}
} // namespace orgel

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new orgel::OrgelProcessor();
}

juce::AudioProcessorEditor* orgel::OrgelProcessor::createEditor()
{
    return new OrgelEditor (*this);
}
