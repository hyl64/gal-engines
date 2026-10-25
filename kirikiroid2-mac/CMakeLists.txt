cmake_minimum_required(VERSION 3.20)
project(krkr2-mac CXX C)

# Kirikiroid2 (zeas2/Kirikiroid2) built natively for macOS arm64.
#
# Upstream is an Android/cocos2d-x application.  This tree brings it up on the
# desktop, and it is one library because the engine's halves reference each other
# (the script manager registers the visual classes, the layers raise script
# errors, the application layer drives both).
#
# What the desktop supplies is the platform: the SDL window and presentation, the
# file storage media, the font hooks, the clipboard, and the software render
# manager's textures.  Everything else is the engine's own code, including its
# software renderer, its bitmap layer, its text drawing and its layer system.

set(CMAKE_CXX_STANDARD 14)   # the sources predate std::binary_function's removal
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_OSX_ARCHITECTURES arm64)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
set(PLATFORM ${CMAKE_CURRENT_SOURCE_DIR}/platform)

# OpenCV has to be 4.x: visual/RenderManager.cpp uses cv::getAffineTransform and
# the three-argument cv::getPerspectiveTransform, which 5.x removed or changed,
# and Homebrew installs both side by side.
set(OPENCV4 /opt/homebrew/opt/opencv@4)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ONIG REQUIRED oniguruma)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(PNG REQUIRED libpng)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(LZ4 REQUIRED liblz4)

add_compile_options(-w)
add_compile_definitions(TJS_TEXT_OUT_CRLF __STDC_CONSTANT_MACROS USE_UNICODE_FSTRING KRKR_DESKTOP_UNPACK)

set(KRKR_INCLUDES
	${CORE}
	${CORE}/tjs2 ${CORE}/base ${CORE}/base/win32
	${CORE}/environ ${CORE}/environ/win32 ${CORE}/environ/ConfigManager
	${CORE}/msg ${CORE}/msg/win32
	${CORE}/utils ${CORE}/utils/win32
	${CORE}/extension
	${CORE}/sound ${CORE}/sound/win32
	${CORE}/visual ${CORE}/visual/win32 ${CORE}/visual/gl ${CORE}/visual/ogl ${CORE}/visual/darwin
	${CORE}/movie
	${PLATFORM} ${PLATFORM}/darwin ${PLATFORM}/compat
	${OPENCV4}/include/opencv4
	${SDL2_INCLUDE_DIRS} ${FREETYPE_INCLUDE_DIRS} ${PNG_INCLUDE_DIRS} ${LZ4_INCLUDE_DIRS} ${ONIG_INCLUDE_DIRS}
)

# ---- TJS2, the script engine -------------------------------------------------
file(GLOB TJS2_SRC ${CORE}/tjs2/*.cpp)
add_library(tjs2 STATIC ${TJS2_SRC}
	${PLATFORM}/darwin_platform.cpp)
target_include_directories(tjs2 PUBLIC ${KRKR_INCLUDES})
target_link_directories(tjs2 PUBLIC ${ONIG_LIBRARY_DIRS})
target_link_libraries(tjs2 PUBLIC ${ONIG_LIBRARIES})

# ---- the engine ---------------------------------------------------------------
file(GLOB BASE_SRC_EXTRA ${CORE}/base/OverlayArchive.cpp)
file(GLOB UTILS_SRC ${CORE}/utils/*.cpp)
file(GLOB UTILS_ENC ${CORE}/utils/encoding/*.c)

add_library(krkr STATIC
	${BASE_SRC_EXTRA}
	# ---- base and storage
	${CORE}/base/BinaryStream.cpp ${CORE}/base/CharacterSet.cpp ${CORE}/base/PluginIntf.cpp
	${CORE}/base/StorageIntf.cpp ${CORE}/base/TARArchive.cpp ${CORE}/base/TextStream.cpp
	${CORE}/base/UtilStreams.cpp ${CORE}/base/XP3Archive.cpp ${CORE}/base/EventIntf.cpp
	${CORE}/base/ScriptMgnIntf.cpp ${CORE}/base/SystemIntf.cpp
	${CORE}/base/win32/SystemImpl.cpp ${CORE}/base/win32/PluginImpl.cpp
	${CORE}/base/win32/EventImpl.cpp ${CORE}/base/win32/NativeEventQueue.cpp
	${CORE}/visual/win32/BitmapInfomation.cpp ${CORE}/visual/win32/BitmapBitsAlloc.cpp
	${CORE}/msg/MsgIntf.cpp
	${UTILS_SRC} ${UTILS_ENC} ${CORE}/utils/md5.c
	${CORE}/utils/win32/ThreadImpl.cpp
	# TimerImpl.cpp is the Windows TJS Timer class; darwin/Timer_Darwin.cpp is the one
	# this back end registers, and two definitions of it would fight over the name
	${CORE}/utils/win32/TVPTimer.cpp ${CORE}/utils/win32/DebugImpl.cpp
	# ---- the software renderer and the pixel pipeline
	${CORE}/visual/tvpgl.cpp ${CORE}/visual/gl/blend_function.cpp ${CORE}/visual/argb.cpp
	${CORE}/visual/RenderManager.cpp ${CORE}/visual/ComplexRect.cpp
	# ---- images and fonts
	${CORE}/visual/LoadPNG.cpp ${CORE}/visual/LoadJPEG.cpp ${CORE}/visual/LoadTLG.cpp ${CORE}/visual/GraphicsLoaderIntf.cpp
	${CORE}/visual/GraphicsLoadThread.cpp
	${CORE}/visual/FontSystem.cpp ${CORE}/visual/FreeType.cpp ${CORE}/visual/FreeTypeFontRasterizer.cpp
	${CORE}/visual/PrerenderedFont.cpp ${CORE}/visual/CharacterData.cpp
	# ---- bitmaps, layers, windows
	${CORE}/visual/BitmapIntf.cpp ${CORE}/visual/LayerBitmapIntf.cpp ${CORE}/visual/LayerIntf.cpp
	${CORE}/visual/LayerManager.cpp ${CORE}/visual/WindowIntf.cpp ${CORE}/visual/TransIntf.cpp
	${CORE}/visual/RectItf.cpp
	${CORE}/visual/win32/DrawDevice.cpp ${CORE}/visual/win32/BasicDrawDevice.cpp
	${CORE}/visual/win32/LayerImpl.cpp ${CORE}/visual/win32/GraphicsLoaderImpl.cpp
	${CORE}/visual/win32/WindowImpl.cpp
	# ---- sound classes the script layer registers
	${CORE}/sound/PhaseVocoderFilter.cpp ${CORE}/sound/PhaseVocoderDSP.cpp
	${CORE}/sound/MathAlgorithms.cpp ${CORE}/sound/WaveSegmentQueue.cpp
	${CORE}/sound/WaveFormatConverter.cpp
	${CORE}/utils/RealFFT_Default.cpp
	# ---- the application layer
	${CORE}/environ/Application.cpp
	# ---- what the desktop supplies
	${PLATFORM}/darwin_host.cpp ${PLATFORM}/darwin/Platform.cpp ${PLATFORM}/darwin/darwin_font.cpp
	${PLATFORM}/darwin/darwin_config.cpp ${PLATFORM}/darwin/darwin_visual_services.cpp
	${CORE}/visual/darwin/LayerBitmapImpl.cpp ${CORE}/visual/darwin/TextDraw_Darwin.cpp ${CORE}/visual/darwin/MenuItem_Darwin.cpp ${CORE}/visual/darwin/Timer_Darwin.cpp ${CORE}/visual/darwin/SoundBuffer_Darwin.cpp ${CORE}/visual/darwin/VideoOverlay_Darwin.cpp
	${CORE}/visual/darwin/FontRasterizers_Darwin.cpp ${CORE}/visual/darwin/ResampleImage_Soft.cpp
	${CORE}/visual/darwin/DrawDevice_Darwin.cpp ${CORE}/visual/darwin/DrawDeviceClass_Darwin.cpp
	${PLATFORM}/compat/renderer/CCTexture2D.cpp
)
target_include_directories(krkr PUBLIC ${KRKR_INCLUDES})
target_link_directories(krkr PUBLIC ${PNG_LIBRARY_DIRS} ${FREETYPE_LIBRARY_DIRS}
	${LZ4_LIBRARY_DIRS} ${SDL2_LIBRARY_DIRS} /opt/homebrew/lib ${OPENCV4}/lib)
target_link_libraries(krkr PUBLIC tjs2 z archive ${PNG_LIBRARIES} ${FREETYPE_LIBRARIES}
	${LZ4_LIBRARIES} ${SDL2_LIBRARIES} swscale avformat avcodec avutil swresample /opt/homebrew/lib/libSDL2_mixer.dylib /opt/homebrew/opt/jpeg-turbo/lib/libturbojpeg.dylib /opt/homebrew/opt/jpeg-turbo/lib/libjpeg.dylib
	${OPENCV4}/lib/libopencv_core.dylib ${OPENCV4}/lib/libopencv_imgproc.dylib)

# ---- the tools, each documented in tools/ ------------------------------------
function(krkr_tool name)
	add_executable(${name} tools/${name}.cpp)
	target_link_libraries(${name} PRIVATE krkr ${SDL2_LIBRARIES})
	target_link_directories(${name} PRIVATE ${SDL2_LIBRARY_DIRS})
	target_include_directories(${name} PRIVATE ${KRKR_INCLUDES})
endfunction()

krkr_tool(tjsc)        # run a .tjs script
krkr_tool(krkrpack)    # list and extract an XP3
krkr_tool(krkrscript)  # a TJS2 host with the storage layer
krkr_tool(krkrblit)    # the software blitters
krkr_tool(krkrview)    # decode a picture out of an XP3
krkr_tool(krkrfont)    # rasterise glyphs
krkr_tool(krkrscene)   # compose background, window and text
krkr_tool(krkrbitmap)  # the bitmap API
krkr_tool(krkrpresent) # engine texture onto a window
krkr_tool(krkrtext)    # text onto a bitmap
krkr_tool(krkrrun)     # run a script against the engine classes
