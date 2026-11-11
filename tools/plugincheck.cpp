// Loads the built VST3, plays a chord through it and reports the output level.
#include <juce_audio_processors/juce_audio_processors.h>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 2) { std::cout << "usage: plugincheck <plugin.vst3>\n"; return 1; }
    juce::AudioPluginFormatManager fm;
    fm.addFormat (new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> found;
    juce::VST3PluginFormat vst3;
    vst3.findAllTypesForFile (found, juce::File (argv[1]).getFullPathName());
    if (found.isEmpty()) { std::cout << "no plugin found\n"; return 2; }
    std::cout << "found: " << found[0]->name << " by " << found[0]->manufacturerName << " instrument=" << (int) found[0]->isInstrument << "\n";
    juce::String err;
    auto inst = fm.createPluginInstance (*found[0], 48000, 512, err);
    if (inst == nullptr) { std::cout << "load failed: " << err << "\n"; return 3; }
    inst->prepareToPlay (48000, 512);
    juce::AudioBuffer<float> buf (2, 512);
    double peak = 0;
    for (int block = 0; block < 200; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 5) for (int k : { 48, 52, 55, 60 }) midi.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100), 0);
        if (block == 150) for (int k : { 48, 52, 55, 60 }) midi.addEvent (juce::MidiMessage::noteOff (1, k), 0);
        buf.clear();
        inst->processBlock (buf, midi);
        if (block > 10 && block < 150) peak = juce::jmax (peak, (double) buf.getMagnitude (0, 512));
    }
    juce::MemoryBlock state;
    inst->getStateInformation (state);
    inst->setStateInformation (state.getData(), (int) state.getSize());
    std::cout << "peak while holding chord: " << peak << ", state bytes " << state.getSize() << "\n";
    std::unique_ptr<juce::AudioProcessorEditor> ed (inst->createEditorIfNeeded());
    std::cout << "editor: " << (ed ? juce::String (ed->getWidth()) + "x" + juce::String (ed->getHeight()) : juce::String ("none")) << "\n";
    ed.reset();
    inst->releaseResources();
    return peak > 0.01 ? 0 : 4;
}
