#include "SoundPanel.h"

namespace orgel
{
std::vector<SoundPanel::Preset> SoundPanel::builtInPresets()
{
    // reverb, then room, distance, bass, sub, width, treble. Larger rooms: more reverb, one sits
    // further away (darker, wider), and the bass carries more. The town church is the default.
    return {
        { tr ("Neutral (ohne Effekt)"), 0.0f, { 2, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f } }, // dry, flat
        { tr ("Kapelle"), 0.25f, { 0, 0.30f, 0.30f, 0.25f, 0.35f, 0.55f } },        // close, clear, small
        { tr ("Dorfkirche"), 0.30f, { 1, 0.40f, 0.38f, 0.30f, 0.42f, 0.48f } },
        { tr ("Stadtkirche"), 0.35f, OrganEngine::Acoustics {} },
        { tr ("Kathedrale"), 0.45f, { 3, 0.62f, 0.55f, 0.45f, 0.65f, 0.34f } },
        { tr ("Dom"), 0.55f, { 4, 0.72f, 0.62f, 0.52f, 0.78f, 0.28f } },
    };
}

std::vector<SoundPanel::Preset> SoundPanel::ownPresets()
{
    std::vector<Preset> list;
    if (auto xml = appSettings().getXmlValue ("ownRooms"))
        for (auto* e : xml->getChildIterator())
        {
            Preset p;
            p.name = e->getStringAttribute ("name");
            p.reverb = (float) e->getDoubleAttribute ("reverb", 0.35);
            auto& a = p.acoustics;
            a.room = juce::jlimit (0, OrganEngine::numRooms() - 1, e->getIntAttribute ("room", a.room));
            a.distance = (float) e->getDoubleAttribute ("distance", a.distance);
            a.bass = (float) e->getDoubleAttribute ("bass", a.bass);
            a.sub = (float) e->getDoubleAttribute ("sub", a.sub);
            a.width = (float) e->getDoubleAttribute ("width", a.width);
            a.treble = (float) e->getDoubleAttribute ("treble", a.treble);
            if (p.name.isNotEmpty())
                list.push_back (p);
        }
    return list;
}

void SoundPanel::storeOwnPresets (const std::vector<Preset>& list)
{
    juce::XmlElement xml ("Rooms");
    for (auto& p : list)
    {
        auto* e = xml.createNewChildElement ("Room");
        e->setAttribute ("name", p.name);
        e->setAttribute ("reverb", p.reverb);
        e->setAttribute ("room", p.acoustics.room);
        e->setAttribute ("distance", p.acoustics.distance);
        e->setAttribute ("bass", p.acoustics.bass);
        e->setAttribute ("sub", p.acoustics.sub);
        e->setAttribute ("width", p.acoustics.width);
        e->setAttribute ("treble", p.acoustics.treble);
    }
    appSettings().setValue ("ownRooms", &xml);
    appSettings().saveIfNeeded();
}

SoundPanel::SoundPanel (OrgelProcessor& p) : proc (p)
{
    title.setText (tr ("Klang und Raum"), juce::dontSendNotification);
    title.setFont (theme::serif (17.0f, true));
    title.setColour (juce::Label::textColourId, theme::gold);
    addAndMakeVisible (title);
    roomLabel.setText (tr ("Raum"), juce::dontSendNotification);
    addAndMakeVisible (roomLabel);
    room.setTextWhenNothingSelected (tr ("Angepasst"));
    room.onChange = [this] {
        const int i = room.getSelectedId() - 1;
        if (i >= 0 && i < (int) presets.size())
            choose (presets[(size_t) i]);
    };
    addAndMakeVisible (room);

    const std::pair<Row*, juce::String> labels[] = {
        { &reverb, tr ("Hall") }, { &distance, tr ("Hörplatz") }, { &bass, tr ("Bass") },
        { &sub, tr ("Tiefbass (Vibration)") }, { &width, tr ("Breite") }, { &treble, tr ("Glanz") },
    };
    for (auto& [row, text] : labels)
    {
        row->label.setText (text, juce::dontSendNotification);
        row->slider.setRange (0.0, 1.0, 0.01);
        row->slider.onValueChange = [this] { apply(); };
        addAndMakeVisible (row->label);
        addAndMakeVisible (row->slider);
    }
    distance.slider.setTooltip (tr ("Links am Spieltisch, rechts hinten im Kirchenschiff"));
    sub.slider.setTooltip (tr ("Hebt die tiefsten Pedaltöne an (16′, 32′) und gibt ihnen Obertöne, damit man sie auch über kleine Lautsprecher spürt"));
    bass.slider.setTooltip (tr ("Grundton und Wärme; der Bass hallt länger nach"));

    reset.setButtonText (tr ("Standard"));
    reset.setTooltip (tr ("Stadtkirche, wie beim ersten Start"));
    reset.onClick = [this] { choose (builtInPresets()[3]); };
    ownName.setTextToShowWhenEmpty (tr ("Name für eigenen Raum"), theme::text.withAlpha (0.45f));
    ownName.setColour (juce::TextEditor::backgroundColourId, theme::woodDark.darker (0.4f));
    ownName.setColour (juce::TextEditor::textColourId, theme::text);
    ownName.onReturnKey = [this] { saveOwn(); };
    ownName.onTextChange = [this] { save.setEnabled (ownName.getText().trim().isNotEmpty()); };
    save.setButtonText (tr ("Speichern"));
    save.setTooltip (tr ("Die jetzige Einstellung als eigenen Raum merken"));
    save.onClick = [this] { saveOwn(); };
    save.setEnabled (false);
    remove.setButtonText (tr ("Löschen"));
    remove.setTooltip (tr ("Diesen eigenen Raum entfernen"));
    remove.onClick = [this] { deleteOwn(); };
    for (auto* c : std::initializer_list<juce::Component*> { &reset, &ownName, &save, &remove })
        addAndMakeVisible (c);
    for (auto* l : { &roomLabel, &reverb.label, &distance.label, &bass.label, &sub.label, &width.label, &treble.label })
        l->setColour (juce::Label::textColourId, theme::text);
    fillRooms();
    load();
}

void SoundPanel::fillRooms()
{
    presets = builtInPresets();
    numBuiltIn = (int) presets.size();
    auto own = ownPresets();
    presets.insert (presets.end(), own.begin(), own.end());
    room.clear (juce::dontSendNotification);
    for (int i = 0; i < (int) presets.size(); ++i)
    {
        if (i == 1 || (i == numBuiltIn && ! own.empty()))
            room.addSeparator();
        room.addItem (presets[(size_t) i].name, i + 1);
    }
}

int SoundPanel::matchingItem() const
{
    const auto a = proc.organ.getAcoustics();
    auto near = [] (float x, float y) { return std::abs (x - y) < 0.005f; };
    // own rooms first: one may have saved a copy of a built-in one under its own name
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < (int) presets.size(); ++i)
        {
            if ((i >= numBuiltIn) != (pass == 0))
                continue;
            auto& p = presets[(size_t) i];
            auto& b = p.acoustics;
            if (near (p.reverb, proc.organ.reverb) && near (a.distance, b.distance) && near (a.bass, b.bass) && near (a.sub, b.sub)
                && near (a.width, b.width) && near (a.treble, b.treble) && (a.room == b.room || p.reverb == 0.0f))
                return i + 1;
        }
    return 0;
}

void SoundPanel::load()
{
    const auto a = proc.organ.getAcoustics();
    room.setSelectedId (matchingItem(), juce::dontSendNotification);
    reverb.slider.setValue (proc.organ.reverb, juce::dontSendNotification);
    distance.slider.setValue (a.distance, juce::dontSendNotification);
    bass.slider.setValue (a.bass, juce::dontSendNotification);
    sub.slider.setValue (a.sub, juce::dontSendNotification);
    width.slider.setValue (a.width, juce::dontSendNotification);
    treble.slider.setValue (a.treble, juce::dontSendNotification);
    remove.setVisible (room.getSelectedId() > numBuiltIn);
    resized();
}

void SoundPanel::choose (const Preset& p)
{
    proc.organ.reverb = p.reverb;
    proc.organ.setAcoustics (p.acoustics);
    load();
    if (onChange)
        onChange();
}

void SoundPanel::apply()
{
    auto a = proc.organ.getAcoustics(); // keeps the room (the size of the reverb)
    a.distance = (float) distance.slider.getValue();
    a.bass = (float) bass.slider.getValue();
    a.sub = (float) sub.slider.getValue();
    a.width = (float) width.slider.getValue();
    a.treble = (float) treble.slider.getValue();
    proc.organ.reverb = (float) reverb.slider.getValue();
    proc.organ.setAcoustics (a);
    room.setSelectedId (matchingItem(), juce::dontSendNotification);
    remove.setVisible (room.getSelectedId() > numBuiltIn);
    if (onChange)
        onChange();
}

void SoundPanel::saveOwn()
{
    const auto name = ownName.getText().trim();
    if (name.isEmpty())
        return;
    auto own = ownPresets();
    Preset p { name, proc.organ.reverb, proc.organ.getAcoustics() };
    bool replaced = false;
    for (auto& o : own)
        if (o.name == name)
        {
            o = p;
            replaced = true;
        }
    if (! replaced)
        own.push_back (p);
    storeOwnPresets (own);
    ownName.clear();
    save.setEnabled (false);
    fillRooms();
    load();
}

void SoundPanel::deleteOwn()
{
    const int i = room.getSelectedId() - 1 - numBuiltIn;
    auto own = ownPresets();
    if (i < 0 || i >= (int) own.size())
        return;
    own.erase (own.begin() + i);
    storeOwnPresets (own);
    fillRooms();
    load(); // the settings stay; the room shows as changed
}

void SoundPanel::paint (juce::Graphics& g)
{
    theme::fillWood (g, getLocalBounds().toFloat(), theme::woodDark);
}

void SoundPanel::resized()
{
    auto r = getLocalBounds().reduced (12);
    title.setBounds (r.removeFromTop (26));
    r.removeFromTop (6);
    auto line = r.removeFromTop (26);
    roomLabel.setBounds (line.removeFromLeft (150));
    if (remove.isVisible())
    {
        remove.setBounds (line.removeFromRight (70));
        line.removeFromRight (6);
    }
    room.setBounds (line);
    r.removeFromTop (8);
    for (auto* row : rows)
    {
        auto l = r.removeFromTop (28);
        row->label.setBounds (l.removeFromLeft (150));
        row->slider.setBounds (l);
        r.removeFromTop (2);
    }
    r.removeFromTop (8);
    auto bottom = r.removeFromTop (26);
    reset.setBounds (bottom.removeFromLeft (90));
    bottom.removeFromLeft (14);
    save.setBounds (bottom.removeFromRight (90));
    bottom.removeFromRight (6);
    ownName.setBounds (bottom);
}
} // namespace orgel
