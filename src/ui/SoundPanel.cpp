#include "SoundPanel.h"

namespace orgel
{
juce::String SoundPanel::roomName (int r)
{
    switch (r)
    {
        case 0: return tr ("Kapelle");
        case 1: return tr ("Dorfkirche");
        case 2: return tr ("Stadtkirche");
        case 3: return tr ("Kathedrale");
        default: return tr ("Dom");
    }
}

SoundPanel::SoundPanel (OrgelProcessor& p) : proc (p)
{
    title.setText (tr ("Klang und Raum"), juce::dontSendNotification);
    title.setFont (theme::serif (17.0f, true));
    title.setColour (juce::Label::textColourId, theme::gold);
    addAndMakeVisible (title);
    roomLabel.setText (tr ("Raum"), juce::dontSendNotification);
    addAndMakeVisible (roomLabel);
    for (int r = 0; r < OrganEngine::numRooms(); ++r)
        room.addItem (roomName (r), r + 1);
    room.onChange = [this] { apply(); };
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
    reset.onClick = [this] {
        proc.organ.setAcoustics (OrganEngine::Acoustics {});
        proc.organ.reverb = 0.35f;
        load();
        if (onChange)
            onChange();
    };
    addAndMakeVisible (reset);
    for (auto* l : { &roomLabel, &reverb.label, &distance.label, &bass.label, &sub.label, &width.label, &treble.label })
        l->setColour (juce::Label::textColourId, theme::text);
    load();
}

void SoundPanel::load()
{
    const auto a = proc.organ.getAcoustics();
    room.setSelectedId (a.room + 1, juce::dontSendNotification);
    reverb.slider.setValue (proc.organ.reverb, juce::dontSendNotification);
    distance.slider.setValue (a.distance, juce::dontSendNotification);
    bass.slider.setValue (a.bass, juce::dontSendNotification);
    sub.slider.setValue (a.sub, juce::dontSendNotification);
    width.slider.setValue (a.width, juce::dontSendNotification);
    treble.slider.setValue (a.treble, juce::dontSendNotification);
}

void SoundPanel::apply()
{
    OrganEngine::Acoustics a;
    a.room = juce::jmax (0, room.getSelectedId() - 1);
    a.distance = (float) distance.slider.getValue();
    a.bass = (float) bass.slider.getValue();
    a.sub = (float) sub.slider.getValue();
    a.width = (float) width.slider.getValue();
    a.treble = (float) treble.slider.getValue();
    proc.organ.reverb = (float) reverb.slider.getValue();
    proc.organ.setAcoustics (a);
    if (onChange)
        onChange();
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
    room.setBounds (line);
    r.removeFromTop (8);
    for (auto* row : rows)
    {
        auto l = r.removeFromTop (28);
        row->label.setBounds (l.removeFromLeft (150));
        row->slider.setBounds (l);
        r.removeFromTop (2);
    }
    r.removeFromTop (6);
    reset.setBounds (r.removeFromTop (26).removeFromRight (110));
}
} // namespace orgel
