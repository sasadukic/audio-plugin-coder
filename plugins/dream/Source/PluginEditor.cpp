#include "PluginEditor.h"
#include "BinaryData.h"

#include <algorithm>
#include <cmath>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace
{
template <size_t N>
juce::String makeJsFloatArray (const std::array<float, N>& values)
{
    juce::String out = "[";
    for (size_t i = 0; i < N; ++i)
    {
        if (i > 0)
            out << ",";

        const float v = values[i];
        out << juce::String (std::isfinite (v) ? v : 0.0f, 6);
    }

    out << "]";
    return out;
}
} // namespace

#if JUCE_WINDOWS
static HWND findNearestCaptionedWindow (HWND start) noexcept
{
    HWND current = start;
    while (current != nullptr && current != GetDesktopWindow())
    {
        const auto style = static_cast<DWORD> (GetWindowLongPtr (current, GWL_STYLE));
        if ((style & WS_CAPTION) != 0)
            return current;

        current = GetParent (current);
    }

    return start;
}
#endif

DreamAudioProcessorEditor::DreamAudioProcessorEditor (DreamAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    webView = std::make_unique<juce::WebBrowserComponent> (createWebOptions (*this));
    addAndMakeVisible (*webView);

    auto startUrl = juce::WebBrowserComponent::getResourceProviderRoot();
    if (! startUrl.endsWithChar ('/'))
        startUrl << '/';
    startUrl << "index.html";
    webView->goToURL (startUrl);

    setSize (980, 620);
    startTimerHz (30);
}

DreamAudioProcessorEditor::~DreamAudioProcessorEditor()
{
   #if JUCE_WINDOWS
    if (fullscreenNativeWindow != nullptr)
    {
        auto* hwnd = static_cast<HWND> (fullscreenNativeWindow);
        SetWindowLongPtr (hwnd, GWL_STYLE, static_cast<LONG_PTR> (savedWindowStyle));
        SetWindowLongPtr (hwnd, GWL_EXSTYLE, static_cast<LONG_PTR> (savedWindowExStyle));
        SetWindowPos (hwnd,
                      nullptr,
                      savedWindowBounds.getX(),
                      savedWindowBounds.getY(),
                      savedWindowBounds.getWidth(),
                      savedWindowBounds.getHeight(),
                      SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        fullscreenNativeWindow = nullptr;
    }
   #else
    if (auto* target = fullscreenTarget.getComponent())
    {
        if (auto* peer = target->getPeer())
            peer->setFullScreen (false);
    }
   #endif

    stopTimer();
}

void DreamAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (9, 12, 19));
}

void DreamAudioProcessorEditor::resized()
{
    webView->setBounds (getLocalBounds());
}

void DreamAudioProcessorEditor::timerCallback()
{
    if (webView == nullptr)
        return;

    const auto spectrum = processorRef.getSpectrumSnapshot();
    const auto oscilloscope = processorRef.getOscilloscopeSnapshot();
    const auto arr = makeJsFloatArray (spectrum);
    const auto oscilloscopeArr = makeJsFloatArray (oscilloscope);

    webView->evaluateJavascript ("if (window.updateSpectrum) window.updateSpectrum(" + arr + ","
                                 + juce::String (processorRef.getCurrentAnalysisSampleRate(), 2) + ","
                                 + oscilloscopeArr + ","
                                 + juce::String (processorRef.getRmsDb(), 2) + ","
                                 + juce::String (processorRef.getLufsIntegrated(), 2) + ");");

    const auto currentRevision = processorRef.getReferenceSpectrumRevision();
    if (currentRevision != lastReferenceRevision)
    {
        lastReferenceRevision = currentRevision;
        const auto reference = processorRef.getReferenceSpectrumSnapshot();
        bool hasReference = currentRevision > 0;

        juce::String referenceArr = "[]";

        if (hasReference)
        {
            referenceArr = makeJsFloatArray (reference);
        }

        webView->evaluateJavascript (
            "if (window.setSmoothPreset) window.setSmoothPreset(" + referenceArr + ","
            + juce::String (hasReference ? "true" : "false") + ");");
    }
}

juce::WebBrowserComponent::Options DreamAudioProcessorEditor::createWebOptions (DreamAudioProcessorEditor& editor)
{
    auto options = juce::WebBrowserComponent::Options{}
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withNativeFunction ("toggleFullscreen",
            [&editor] (const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion done)
            {
                juce::ignoreUnused (args);
                editor.toggleFullscreen();
                done (true);
            })
        .withNativeFunction ("setOscilloscopeLengthMode",
            [&editor] (const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion done)
            {
                int mode = 0;
                if (args.size() > 0)
                {
                    if (args[0].isInt() || args[0].isDouble() || args[0].isBool())
                        mode = static_cast<int> (args[0]);
                }

                editor.processorRef.setOscilloscopeLengthMode (mode);
                done (true);
            })
        .withNativeFunction ("buildSmoothPresetFromFolder",
            [&editor] (const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion done)
            {
                juce::ignoreUnused (args);

                editor.folderChooser = std::make_unique<juce::FileChooser> (
                    "Select a folder with songs",
                    juce::File(),
                    "*");

                constexpr int chooserFlags = juce::FileBrowserComponent::openMode
                    | juce::FileBrowserComponent::canSelectDirectories;

                editor.folderChooser->launchAsync (chooserFlags,
                    [&editor, done] (const juce::FileChooser& chooser)
                    {
                        const auto folder = chooser.getResult();
                        editor.folderChooser.reset();

                        bool success = false;
                        juce::String message;

                        if (folder.isDirectory())
                        {
                            juce::MouseCursor::showWaitCursor();
                            success = editor.processorRef.buildSmoothPresetFromFolder (folder, message);
                            juce::MouseCursor::hideWaitCursor();
                            editor.lastReferenceRevision = (std::numeric_limits<std::uint32_t>::max)();
                        }
                        else
                        {
                            message = "Scan canceled.";
                        }

                        if (editor.webView != nullptr)
                        {
                            juce::String referenceArr = "[]";
                            bool hasReference = false;
                            const auto reference = editor.processorRef.getReferenceSpectrumSnapshot();

                            if (success)
                            {
                                hasReference = true;

                                if (hasReference)
                                {
                                    referenceArr = makeJsFloatArray (reference);
                                }
                            }

                            editor.webView->evaluateJavascript (
                                "if (window.setSmoothPreset) window.setSmoothPreset(" + referenceArr + ","
                                + juce::String (hasReference ? "true" : "false") + ");");

                            editor.webView->evaluateJavascript (
                                "if (window.onSmoothPresetScanFinished) window.onSmoothPresetScanFinished("
                                + juce::String (success ? "true" : "false") + ","
                                + juce::JSON::toString (juce::var (message)) + ");");
                        }

                        done (success);
                    });
            })
        .withNativeFunction ("clearSmoothPreset",
            [&editor] (const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion done)
            {
                juce::ignoreUnused (args);
                editor.processorRef.clearReferenceSpectrum();
                editor.lastReferenceRevision = (std::numeric_limits<std::uint32_t>::max)();
                done (true);
            })
        .withResourceProvider ([&editor] (const juce::String& url) { return editor.getResource (url); });

#if JUCE_WINDOWS
    options = options.withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
        .withWinWebView2Options (
            juce::WebBrowserComponent::Options::WinWebView2{}
                .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("NPS_SPECRUM")));
#endif

    return options;
}

std::optional<juce::WebBrowserComponent::Resource> DreamAudioProcessorEditor::getResource (const juce::String& url)
{
    auto makeResource = [] (const char* data, int size, const char* mime)
    {
        return juce::WebBrowserComponent::Resource {
            std::vector<std::byte> (reinterpret_cast<const std::byte*> (data),
                                    reinterpret_cast<const std::byte*> (data) + size),
            juce::String (mime)
        };
    };

    auto path = url.trim();
    const auto root = juce::WebBrowserComponent::getResourceProviderRoot();
    if (path.startsWithIgnoreCase (root))
        path = path.fromFirstOccurrenceOf (root, false, false);

    if (path.startsWithChar ('/'))
        path = path.substring (1);
    if (path.isEmpty())
        path = "index.html";

    if (path == "index.html")
    {
        return makeResource (dream_BinaryData::index_html,
                             dream_BinaryData::index_htmlSize,
                             "text/html");
    }

    return std::nullopt;
}

void DreamAudioProcessorEditor::toggleFullscreen()
{
   #if JUCE_WINDOWS
    auto* target = getTopLevelComponent();
    if (target == nullptr)
        target = this;

    fullscreenTarget = target;

    auto* peer = fullscreenTarget->getPeer();
    if (peer == nullptr)
        return;

    auto* native = static_cast<HWND> (peer->getNativeHandle());
    if (native == nullptr)
        return;

    if (! fullscreen)
    {
        auto* frameWindow = findNearestCaptionedWindow (native);
        RECT currentRect {};
        if (! GetWindowRect (frameWindow, &currentRect))
            return;

        fullscreenNativeWindow = frameWindow;
        savedWindowStyle = static_cast<std::int64_t> (GetWindowLongPtr (frameWindow, GWL_STYLE));
        savedWindowExStyle = static_cast<std::int64_t> (GetWindowLongPtr (frameWindow, GWL_EXSTYLE));
        savedWindowBounds = juce::Rectangle<int> (currentRect.left,
                                                  currentRect.top,
                                                  currentRect.right - currentRect.left,
                                                  currentRect.bottom - currentRect.top);

        MONITORINFO monitorInfo {};
        monitorInfo.cbSize = sizeof (MONITORINFO);
        if (! GetMonitorInfo (MonitorFromWindow (frameWindow, MONITOR_DEFAULTTONEAREST), &monitorInfo))
            return;

        const auto newStyle = static_cast<LONG_PTR> (savedWindowStyle)
            & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        const auto newExStyle = static_cast<LONG_PTR> (savedWindowExStyle)
            & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);

        SetWindowLongPtr (frameWindow, GWL_STYLE, newStyle);
        SetWindowLongPtr (frameWindow, GWL_EXSTYLE, newExStyle);
        SetWindowPos (frameWindow,
                      HWND_TOP,
                      monitorInfo.rcMonitor.left,
                      monitorInfo.rcMonitor.top,
                      monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                      monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                      SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

        fullscreen = true;
    }
    else
    {
        auto* frameWindow = static_cast<HWND> (fullscreenNativeWindow);
        if (frameWindow != nullptr)
        {
            SetWindowLongPtr (frameWindow, GWL_STYLE, static_cast<LONG_PTR> (savedWindowStyle));
            SetWindowLongPtr (frameWindow, GWL_EXSTYLE, static_cast<LONG_PTR> (savedWindowExStyle));
            SetWindowPos (frameWindow,
                          nullptr,
                          savedWindowBounds.getX(),
                          savedWindowBounds.getY(),
                          savedWindowBounds.getWidth(),
                          savedWindowBounds.getHeight(),
                          SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }

        fullscreenNativeWindow = nullptr;
        fullscreen = false;
    }
   #else
    auto* target = getTopLevelComponent();
    if (target == nullptr)
        target = this;

    fullscreenTarget = target;
    if (auto* peer = fullscreenTarget->getPeer())
    {
        peer->setFullScreen (! fullscreen);
        fullscreen = ! fullscreen;
    }
   #endif

    if (webView != nullptr)
        webView->evaluateJavascript ("window.dispatchEvent(new Event('resize'));");
}
