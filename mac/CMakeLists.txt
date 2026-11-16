# The macOS port, kept beside the Windows tree rather than inside it.
#
# The Windows CMakeLists.txt cannot be reused at all: it fails hard on any
# non-win32 generator and on any compiler that is not MSVC, and it names an
# NVIDIA DLSS SDK that does not exist on this machine.
#
# What is built here, in the order it was proven:
#   1. the ffmpeg pipe and the Metal presenter  (phase 1, picture)
#   2. the audio clock and the CoreAudio sink   (phase 2, sound)
#   3. the policy layer, 50 of 56 headers       (phase 3, logic)
#   4. the Qt window                            (phase 4, chrome)
#
# Phase 4 reuses phases 1-3 unchanged. What it adds is drawing and events: the
# geometry and the colours still come from mac/policy/, which is the point.

cmake_minimum_required(VERSION 3.24)

project(Dlss5VideoPlayerMac LANGUAGES CXX OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "This directory targets macOS.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)

# AUTOMOC for the Q_OBJECT classes. Only the Qt targets turn it on, so the
# headless tests do not pay for a moc pass over nothing.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(dlss5-mac
    src/FfmpegPipeSource.cpp
    src/AudioPlayer.cpp
    src/CoreAudioSink.mm
    src/MetalVideoPresenter.mm
    src/main.mm
)

target_compile_options(dlss5-mac PRIVATE -Wall -Wextra)

# ARC because the Objective-C half is ordinary modern AppKit code; the C++ half
# never sees a retained pointer.
target_compile_options(dlss5-mac PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

# Metal and AppKit by framework rather than by hand-written linker flags, so a
# missing framework is a configure error instead of a link error.
# AudioPlayer.cpp now reads the ported AudioTrackPolicy.h for the default-track
# decision; the policy headers include PlatformCompat.h, which lives in src/.
target_include_directories(dlss5-mac PRIVATE src policy)

target_link_libraries(dlss5-mac PRIVATE
    "-framework AppKit"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Foundation"
    "-framework AudioToolbox"
    "-framework CoreAudio"
)

# The headless half. It shares the source file with the player rather than
# linking a library, because there is exactly one other consumer.
enable_testing()

add_executable(dlss5-pipe-tests
    src/FfmpegPipeSource.cpp
    tests/PipeSourceTests.cpp
)
target_include_directories(dlss5-pipe-tests PRIVATE src)
target_compile_options(dlss5-pipe-tests PRIVATE -Wall -Wextra)

add_test(NAME pipe-source COMMAND dlss5-pipe-tests "${CMAKE_CURRENT_SOURCE_DIR}/../docs/media/amd/amd-live-wipe.mp4")

add_executable(dlss5-metal-tests
    src/FfmpegPipeSource.cpp
    src/MetalVideoPresenter.mm
    tests/MetalRenderTests.mm
)
target_include_directories(dlss5-metal-tests PRIVATE src)
target_compile_options(dlss5-metal-tests PRIVATE -Wall -Wextra
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(dlss5-metal-tests PRIVATE
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Foundation"
)

add_test(NAME metal-render COMMAND dlss5-metal-tests "${CMAKE_CURRENT_SOURCE_DIR}/../docs/media/amd/amd-live-wipe.mp4")

add_executable(dlss5-audio-tests
    src/AudioPlayer.cpp
    src/CoreAudioSink.mm
    tests/AudioClockTests.cpp
)
target_include_directories(dlss5-audio-tests PRIVATE src policy)
target_compile_options(dlss5-audio-tests PRIVATE -Wall -Wextra
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(dlss5-audio-tests PRIVATE
    "-framework AudioToolbox"
    "-framework CoreAudio"
    "-framework Foundation"
)

add_test(NAME audio-clock COMMAND dlss5-audio-tests "${CMAKE_CURRENT_SOURCE_DIR}/../docs/media/amd/amd-live-wipe.mp4")

# The ported policy layer.
#
# `policy/` holds the 50 headers carried over from src/, with one edit each at
# most: the include at the top. `src/` holds the shim they compile against
# (UiTypes.h, UiTheme.h, PlatformCompat.h). Both directories are on the include
# path because a ported file's own #includes are unchanged - it still says
# "MediaSource.h" and means the one beside it.
#
# dlss5-policy-port-tests is a compile test first: its whole value is that it
# includes every ported header and therefore fails to build if one of them
# stops compiling. The checks it runs afterwards are the two facts a compile
# cannot establish (RECT edges, COLORREF order).
add_executable(dlss5-policy-port-tests tests/PolicyPortTests.cpp)
target_include_directories(dlss5-policy-port-tests PRIVATE src policy)
target_compile_options(dlss5-policy-port-tests PRIVATE -Wall -Wextra)
add_test(NAME policy-port COMMAND dlss5-policy-port-tests)

add_executable(dlss5-policy-semantics-tests
    src/UiTheme.cpp
    tests/PolicySemanticsTests.cpp
)
target_include_directories(dlss5-policy-semantics-tests PRIVATE src policy)
target_compile_options(dlss5-policy-semantics-tests PRIVATE -Wall -Wextra)
add_test(NAME policy-semantics COMMAND dlss5-policy-semantics-tests)

# ---- Phase 4: the Qt window ------------------------------------------------
#
# Qt6 Widgets only. No Quick, no QML: the player's chrome is drawn from RECTs
# that the ported layout functions produce, and QPainter draws them. QML would
# mean restating every coordinate in a declarative form and losing the port.
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(dlss5-qt
    src/main_qt.cpp
    src/PlayerWindow.cpp
    src/PlayerWindow.h
    src/AppMenu.cpp
    src/AppMenu.h
    src/CompareCompositor.cpp
    src/CompareCompositor.h
    src/SubtitleTrack.cpp
    src/SubtitleTrack.h
    src/NeuralProcessor.cpp
    src/RenderCache.cpp
    src/RenderCache.h
    src/SegmentPlayback.cpp
    src/SegmentPlayback.h
    src/NeuralProcessor.h
    src/VideoSurface.cpp
    src/VideoSurface.h
    src/TimelineWidget.cpp
    src/TimelineWidget.h
    src/ToolbarWidget.cpp
    src/ToolbarWidget.h
    src/FfmpegPipeSource.cpp
    src/AudioPlayer.cpp
    src/CoreAudioSink.mm
    src/UiTheme.cpp
    policy/UiLayout.cpp
)

target_include_directories(dlss5-qt PRIVATE src policy)
target_compile_options(dlss5-qt PRIVATE -Wall -Wextra)
target_compile_options(dlss5-qt PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

target_link_libraries(dlss5-qt PRIVATE
    Qt6::Widgets
    "-framework AudioToolbox"
    "-framework CoreAudio"
    "-framework Foundation"
)

# The tabler icon font is loaded at runtime from beside the executable, which is
# where a .app bundle would put it too. The macro is the path, defined once here
# rather than hard-coded in the widget.
target_compile_definitions(dlss5-qt PRIVATE
    DISPLAYED_ICON_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../assets/tabler/tabler-icons.ttf"
)

# The toolbar's own render test.
#
# A screenshot proves a window opened; this proves the chrome is right - that
# every pill paints, that a disabled one differs from an enabled one, that a lit
# one is the palette's blue. Those are exactly the things a hand-drawn toolbar
# gets wrong, and none of them are visible in a screenshot at a glance.
add_executable(dlss5-toolbar-tests
    src/ToolbarWidget.cpp
    src/ToolbarWidget.h
    src/UiTheme.cpp
    policy/UiLayout.cpp
    tests/ToolbarRenderTests.cpp
)
target_include_directories(dlss5-toolbar-tests PRIVATE src policy)
target_compile_options(dlss5-toolbar-tests PRIVATE -Wall -Wextra)
target_link_libraries(dlss5-toolbar-tests PRIVATE Qt6::Widgets)
target_compile_definitions(dlss5-toolbar-tests PRIVATE
    DISPLAYED_ICON_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../assets/tabler/tabler-icons.ttf")
add_test(NAME toolbar-render COMMAND dlss5-toolbar-tests)

# The neural integration's own check: that the CLI and the weights are found, that
# the availability logic answers correctly, and that the output path never
# collides with an existing file. It cannot check the model's output - that is the
# CLI's own job - but it is the part the player is responsible for getting wrong.
add_executable(dlss5-neural-tests
    src/NeuralProcessor.cpp
    src/RenderCache.cpp
    src/RenderCache.h
    src/NeuralProcessor.h
    tests/NeuralProcessorTests.cpp
)
target_include_directories(dlss5-neural-tests PRIVATE src policy)
target_compile_options(dlss5-neural-tests PRIVATE -Wall -Wextra)
target_link_libraries(dlss5-neural-tests PRIVATE Qt6::Core Qt6::Widgets)
add_test(NAME neural-integration COMMAND dlss5-neural-tests)




# The coverage arithmetic behind partial rendering. Pure functions, so this one
# needs no window, no video and no weights: it is the part of the cache logic that
# decides what a viewer sees and how long they wait, and it is worth checking on
# its own.
add_executable(dlss5-coverage-tests tests/CoverageTests.cpp)
target_include_directories(dlss5-coverage-tests PRIVATE policy)
target_compile_options(dlss5-coverage-tests PRIVATE -Wall -Wextra)
add_test(NAME coverage COMMAND dlss5-coverage-tests)

# The render cache's on-disk behaviour: which parts of a source have been
# converted, and whether a reopen, a hand-deleted segment and a second source all
# answer correctly.
add_executable(dlss5-cache-tests
    src/RenderCache.cpp
    src/RenderCache.h
    tests/RenderCacheTests.cpp
)
target_include_directories(dlss5-cache-tests PRIVATE src policy)
target_compile_options(dlss5-cache-tests PRIVATE -Wall -Wextra)
target_link_libraries(dlss5-cache-tests PRIVATE Qt6::Core)
add_test(NAME render-cache COMMAND dlss5-cache-tests)


# Pairing the original against the rendered segments. Runs entirely on fake
# decoders: the cases that matter are the seams, and constructing them on purpose
# is the only way to test them.
add_executable(dlss5-segment-tests
    src/SegmentPlayback.cpp
    src/SegmentPlayback.h
    src/FfmpegPipeSource.cpp
    tests/SegmentPlaybackTests.cpp
)
target_include_directories(dlss5-segment-tests PRIVATE src policy)
target_compile_options(dlss5-segment-tests PRIVATE -Wall -Wextra)
target_link_libraries(dlss5-segment-tests PRIVATE Qt6::Core)
add_test(NAME segment-playback COMMAND dlss5-segment-tests)


# The same pairing against real ffmpeg decoders. Registered so the target cannot
# quietly disappear from the build; without a video argument it reports a skip,
# and `ctest -R segment-playback-live` with a video is the way to actually run it.
add_executable(dlss5-segment-live-tests
    src/SegmentPlayback.cpp
    src/SegmentPlayback.h
    src/FfmpegPipeSource.cpp
    tests/SegmentPlaybackLiveTests.cpp
)
target_include_directories(dlss5-segment-live-tests PRIVATE src policy)
target_compile_options(dlss5-segment-live-tests PRIVATE -Wall -Wextra)
target_link_libraries(dlss5-segment-live-tests PRIVATE Qt6::Core)
add_test(NAME segment-playback-live COMMAND dlss5-segment-live-tests)

# Where a live session's next block goes. Pure arithmetic, but it is the two
# failures the session actually had - a render that stopped after one block and
# blocks that skipped the stretch between the render head and the playhead - so
# it gets its own regression suite rather than being checked by eye in the UI.
add_executable(dlss5-session-tests tests/SessionBlockTests.cpp)
target_include_directories(dlss5-session-tests PRIVATE policy)
target_compile_options(dlss5-session-tests PRIVATE -Wall -Wextra)
add_test(NAME session-blocks COMMAND dlss5-session-tests)

# The comparison views: pane arithmetic, the CPU Mix/Difference pass and the
# offscreen PNG a viewer actually saves. QPainter works on a QImage without a
# display server, so all of it is testable with QT_QPA_PLATFORM=offscreen.
add_executable(dlss5-compare-tests
    src/CompareCompositor.cpp
    src/VideoSurface.cpp
    tests/CompareComposeTests.cpp
)
target_include_directories(dlss5-compare-tests PRIVATE src policy)
target_compile_options(dlss5-compare-tests PRIVATE -Wall -Wextra)
target_link_libraries(dlss5-compare-tests PRIVATE Qt6::Widgets)
add_test(NAME compare-views COMMAND dlss5-compare-tests)
set_tests_properties(compare-views PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Sidecar subtitles and the audio-track default/labels that fill the two menus.
add_executable(dlss5-media-list-tests
    src/SubtitleTrack.cpp
    tests/MediaListTests.cpp
)
target_include_directories(dlss5-media-list-tests PRIVATE src policy)
target_compile_options(dlss5-media-list-tests PRIVATE -Wall -Wextra)
target_link_libraries(dlss5-media-list-tests PRIVATE Qt6::Core)
add_test(NAME media-lists COMMAND dlss5-media-list-tests)
