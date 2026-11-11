#pragma once
#include "PluginProcessor.h"
#include "ui/Console.h"
#include "ui/MusicDesk.h"
#include <set>

namespace orgel
{
// Little gold diamonds on the time line: where the registration track changes stops.
class MarkerStrip : public juce::Component
{
public:
    MarkerStrip() { setInterceptsMouseClicks (false, false); }
    void setMarks (std::vector<double> t, double len)
    {
        if (t != times || std::abs (len - length) > 1.0e-6)
        {
            times = std::move (t);
            length = len;
            repaint();
        }
    }
    void paint (juce::Graphics& g) override
    {
        if (length <= 0)
            return;
        const float inset = 10.0f; // matches the slider's thumb travel
        for (auto t : times)
        {
            const float x = inset + (float) (t / length) * ((float) getWidth() - 2 * inset);
            juce::Path d;
            d.addTriangle (x - 4, 0, x + 4, 0, x, 6);
            g.setColour (juce::Colour (0xffd4af5a));
            g.fillPath (d);
            g.setColour (juce::Colour (0xffd4af5a).withAlpha (0.5f));
            g.drawVerticalLine ((int) x, 6.0f, (float) getHeight());
        }
    }

private:
    std::vector<double> times;
    double length = 0;
};

class OrgelEditor : public juce::AudioProcessorEditor,
                    public juce::FileDragAndDropTarget,
                    private juce::ChangeListener,
                    public juce::MenuBarModel,
                    private juce::Timer
{
public:
    explicit OrgelEditor (OrgelProcessor&);
    ~OrgelEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int, int) override;
    bool keyPressed (const juce::KeyPress& key) override;
    bool keyStateChanged (bool isKeyDown) override;
    void parentHierarchyChanged() override;

    // menu bar of the standalone app (macOS main menu, or inside the window elsewhere)
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int index, const juce::String& name) override;
    void menuItemSelected (int itemID, int topLevelMenuIndex) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void rebuildConsole();
    juce::String structureSignature() const;
    void drawFacade (juce::Graphics& g, juce::Rectangle<float> area);

    void openFile();
    void showExportMenu();
    void exportAs (const Formats::ExportFormat& fmt);
    void showPartMenu();
    void showSoundFontMenu();
    void showOptionsMenu();
    void togglePlay();
    void showTrackMenu();

    OrgelProcessor& proc;
    juce::TooltipWindow tooltips { this, 600 };

    juce::TextButton openButton { juce::String::fromUTF8 ("Öffnen…") }, exportButton { "Exportieren" }, rewindButton, playButton, stopButton,
        partsButton { "Stimmen" }, addManualButton { "+ Manual" }, soundFontButton { "SoundFont" }, optionsButton { "Optionen" };
    juce::TextButton trackButton { "Registrierspur" };
    MarkerStrip markerStrip;
    std::set<int> computerKeysDown;
    int computerOctave = 60;
    juce::ToggleButton registrationToggle { "Registrierung aus Datei" };
    juce::Slider position { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Slider speed { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Slider reverb { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider volume { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label timeLabel, titleLabel, speedLabel { {}, "Tempo" }, reverbLabel { {}, "Hall" }, volumeLabel { {}, juce::String::fromUTF8 ("Lautstärke") };

    StopJamb leftJamb, rightJamb;
    MusicDesk desk;
    CouplerRail rail;
    juce::OwnedArray<KeyboardView> keyboards;
    juce::OwnedArray<DivisionPlate> plates;
    juce::OwnedArray<juce::Slider> swells;

    struct FacadePipe
    {
        juce::Rectangle<float> body; // in facade coordinates
        float mouthY = 0;
        int note = 60;
        float level = 0, lastTarget = 0;
        double popStart = -1.0e9; // ms, when the pipe last started to speak
        float baseY = 0;
    };
    void drawPipe (juce::Graphics& g, const FacadePipe& p, juce::Point<float> offset, double nowMs);
    std::vector<FacadePipe> pipes;
    void updateFacade();
    void paintFacadeGlow (juce::Graphics& g);
    juce::Rectangle<int> facadeArea, consoleArea;
    juce::Image facadeCache;
    juce::String lastSignature;
    std::shared_ptr<Piece> shownPiece;
    std::unique_ptr<juce::FileChooser> chooser;
    bool draggingPosition = false;
    bool windowSetUp = false;
    void setUpStandaloneWindow();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrgelEditor)
};
} // namespace orgel
