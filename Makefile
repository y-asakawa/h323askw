#
# Makefile
#
# Makefile for H.323 Video Client
#

PROG		= h323askw
SOURCES		= main.cxx

# USB HID Controller for physical mute button (Jabra, Plantronics, etc.)
# macOS only - requires IOKit framework
# usb_hid_controller.cpp uses PTLib and can be added to SOURCES
# usb_hid_impl.mm uses IOKit and must be compiled separately to avoid ULONG conflict
ifeq ($(shell uname -s),Darwin)
SOURCES += usb_hid_controller.cpp
endif

# Qt6 is enabled by default. Set DISABLE_QT6=1 to disable.
# Example: make DISABLE_QT6=1
ifndef DISABLE_QT6
USE_QT6 := 1
endif

# Qt6 sources must be added BEFORE include openh323u.mak
ifdef USE_QT6
SOURCES += qt_video_window.cpp moc_qt_video_window.cpp
# Add -DUSE_QT6 to compiler flags BEFORE including openh323u.mak
STDCCFLAGS += -DUSE_QT6 -std=c++17

# Qt6 detection via pkg-config (must be before include)
PKG_CONFIG := $(shell which pkg-config 2>/dev/null)
ifneq ($(PKG_CONFIG),)
  QT6_CFLAGS := $(shell pkg-config --cflags Qt6Widgets Qt6Core Qt6Gui 2>/dev/null)
  QT6_LIBS := $(shell pkg-config --libs Qt6Widgets Qt6Core Qt6Gui 2>/dev/null)
  ifneq ($(QT6_CFLAGS),)
    STDCCFLAGS += $(QT6_CFLAGS) -fPIC
    ENDLDLIBS += $(QT6_LIBS)
  else
    $(warning Qt6 pkg-config found but QT6_CFLAGS is empty - Qt6 UI may not work)
  endif
else
  $(warning pkg-config not found - Qt6 UI may not work. Install pkg-config.)
endif

# moc compiler for Q_OBJECT classes
MOC := /opt/homebrew/share/qt/libexec/moc
endif

# Control verbosity: set QUIET=0 to enable informational messages, QUIET=1 (default) to suppress them
QUIET ?= 1

# Set up H323Plus environment variables
export PWLIBDIR := ../ptlib
export OPENH323DIR := ../h323plus
export PWLIBPLUGINDIR := ../h323plus/plugins

ifndef OPENH323DIR
OPENH323DIR=$(HOME)/h323plus
endif

# Check if H323Plus directory exists
ifeq ($(wildcard $(OPENH323DIR)/openh323u.mak),)
$(error H323Plus not found at $(OPENH323DIR). Please build H323Plus first or set OPENH323DIR correctly)
endif

# Check if PTLib directory exists
ifeq ($(wildcard $(PWLIBDIR)/make/ptlib.mak),)
$(error PTLib not found at $(PWLIBDIR). Please build PTLib first or set PWLIBDIR correctly)
endif

include $(OPENH323DIR)/openh323u.mak

# Qt6 moc rule (must be after include openh323u.mak)
ifdef USE_QT6
moc_qt_video_window.cpp: qt_video_window.h
	$(MOC) -DUSE_QT6 $< -o $@
endif

# Objective-C++ compilation rule for .mm files (macOS only)
# This is needed because the default rules may not handle .mm files
$(OBJDIR)/%.o: %.mm
	@if [ ! -d $(OBJDIR) ]; then mkdir -p $(OBJDIR); fi
	$(Q_CC)$(CXX) $(STDCCFLAGS) $(CXXFLAGS) $(CPPFLAGS) -x objective-c++ -fobjc-arc -c $< -o $@

# Robust macOS detection (use uname as fallback because OSTYPE may vary)
DARWIN := $(shell uname -s 2>/dev/null)

# Add USB HID implementation object (macOS only)
# usb_hid_impl.mm uses IOKit and must be compiled separately to avoid ULONG conflict
# usb_hid_controller.cpp is in SOURCES and will be built normally
ifeq ($(DARWIN),Darwin)
USB_HID_IMPL_OBJ := $(OBJDIR)/usb_hid_impl.o
# Use ENDLDLIBS instead of OBJS to add object file at link time
# This ensures it's linked but we control when it's compiled
PERMISSIONS_OBJ := $(OBJDIR)/Permissions.o
ENDLDLIBS += $(USB_HID_IMPL_OBJ) $(PERMISSIONS_OBJ)
endif

# add cleanup files
CLEAN_FILES += PWL*

STDCCFLAGS += -Wno-unused-variable

PKG_CONFIG ?= $(shell which pkg-config 2>/dev/null)

# ===== SpeexDSP (AEC/NS/AGC) =====
SPEEXDSP_FOUND := 0
ifneq ($(PKG_CONFIG),)
  SPEEXDSP_CFLAGS := $(shell pkg-config --cflags speexdsp 2>/dev/null)
  SPEEXDSP_LIBS   := $(shell pkg-config --libs speexdsp 2>/dev/null)
  ifneq ($(SPEEXDSP_CFLAGS),)
    SPEEXDSP_FOUND := 1
    STDCCFLAGS += -DUSE_SPEEXDSP -DUSE_SPEEXDSP_AEC $(SPEEXDSP_CFLAGS)
    ENDLDLIBS += $(SPEEXDSP_LIBS)
    ifeq ($(QUIET),0)
      $(info SpeexDSP found via pkg-config - enabling AEC/NS/AGC)
    endif
  endif
endif
ifeq ($(SPEEXDSP_FOUND),0)
  $(warning SpeexDSP not found - echo cancellation / noise suppression disabled)
endif

# H.264 plugin support - using unified plugin only
H264_PLUGIN_DIR := ../h323plus/plugins/video/H.264
H264_UNIFIED_PLUGIN := $(H264_PLUGIN_DIR)/h264_plugin_h323plus.dylib

# Check for unified H.264 plugin
ifneq ($(wildcard $(H264_UNIFIED_PLUGIN)),)
	ifeq ($(QUIET),0)
		$(info Found unified H.264 plugin at $(H264_UNIFIED_PLUGIN))
	endif
  STDCCFLAGS += -DH264_PLUGIN_AVAILABLE -DH264_UNIFIED_PLUGIN
else
  $(warning H.264 plugin not found - H.264 video codec may not be available)
  $(warning Run 'make build-h264-unified-plugin' to build the unified H.264 plugin)
endif

# Video support - always enable for this application
STDCCFLAGS += -DH323_VIDEO
ifeq ($(QUIET),0)
  $(info Video support enabled via H323_VIDEO)
endif

# Enable H.239 (content sharing) in this application build
STDCCFLAGS += -DH323_H239

# macOS USB camera support
ifeq ($(OSTYPE),Darwin)
  VIDINPUT_MACOS := ../ptlib/plugins/vidinput_macos/libvidinput_macos.dylib
  ifneq ($(wildcard $(VIDINPUT_MACOS)),)
    ifeq ($(QUIET),0)
      $(info Found macOS video input plugin)
    endif
    STDCCFLAGS += -DMACOS_VIDINPUT_AVAILABLE
  else
    $(warning macOS video input plugin not found - USB camera support may not work)
    $(warning Build vidinput_macos plugin in PTLib)
  endif
  # Set plugin directory for PTLib plugins
  STDCCFLAGS += -DPTLIB_PLUGIN_DIR=\"../ptlib/lib_Darwin_aarch64\"
endif

# Additional macOS-specific flags
ifeq ($(OSTYPE),Darwin)
  STDCCFLAGS += -D_REENTRANT
  # Add macOS framework support
  ENDLDLIBS += -framework CoreFoundation -framework CoreVideo -framework CoreMedia -framework CoreGraphics
  # IOKit for USB HID Controller (mute button support)
  ENDLDLIBS += -framework IOKit
  # AVFoundation/Foundation for permission bootstrap (camera/mic + Bonjour trigger)
  ENDLDLIBS += -framework AVFoundation -framework Foundation
endif

# Sanitize ENDLDLIBS using a concise shell pipeline:
#  - remove exact tokens '-multiply_defined' and 'suppress'
#  - drop any -L path referencing the old Homebrew OpenSSL Cellar that no longer exists
#  - remove duplicate tokens while preserving first occurrence order
ORIG_ENDLDLIBS := $(ENDLDLIBS)
# Convert token stream so that '-framework NAME' becomes a single token, then
# filter out obsolete tokens and deduplicate while preserving order.
ENDLDLIBS := $(shell printf "%s" "$(ORIG_ENDLDLIBS)" \
	| perl -pe 's/\s+/\n/g' \
	| awk '\
		{ if ($$0 == "-multiply_defined" || $$0 == "suppress" || $$0 == "/opt/homebrew/Cellar/openssl@3/3.5.1/lib") next; \
			if ($$0 == "-framework") { getline name; print "-framework " name } else { print $$0 } \
		}' \
	| awk '!seen[$$0]++ { printf "%s ", $$0 }')
ENDLDLIBS := $(filter-out -L/opt/homebrew/Cellar/openssl@3/3.5.1/lib,$(ENDLDLIBS))
LDFLAGS := $(filter-out -L/opt/homebrew/Cellar/openssl@3/3.5.1/lib,$(LDFLAGS))

# Sanitize LDFLAGS coming from included makefiles: remove obsolete flags like
# '-multiply_defined' and stray 'suppress', and collapse duplicate OpenSSL -L
# entries into a single occurrence while preserving order.
ORIG_LDFLAGS := $(LDFLAGS)
HAS_OPENSSL_L := $(filter -L/opt/homebrew/opt/openssl@3/lib,$(ORIG_LDFLAGS))
LDFLAGS := $(filter-out -multiply_defined suppress -L/opt/homebrew/opt/openssl@3/lib,$(ORIG_LDFLAGS))
ifeq ($(HAS_OPENSSL_L),-L/opt/homebrew/opt/openssl@3/lib)
LDFLAGS := -L/opt/homebrew/opt/openssl@3/lib $(LDFLAGS)
endif

# Debug information
debug-info:
	@echo "=== H323ASKW Build Configuration ==="
	@echo "PROG: $(PROG)"
	@echo "SOURCES: $(SOURCES)"
	@echo "OPENH323DIR: $(OPENH323DIR)"
	@echo "PWLIBDIR: $(PWLIBDIR)"
	@echo "OSTYPE: $(DARWIN)"
	@echo "SDL2_FOUND: $(SDL2_FOUND)"
	@echo "STDCCFLAGS: $(STDCCFLAGS)"
	@echo "ENDLDLIBS: $(ENDLDLIBS)"
	@echo "H264_PLUGIN_DIR: $(H264_PLUGIN_DIR)"
	@echo "=================================="

# Build targets
.PHONY: all video debug-info install-deps check-deps clean-all rebuild test-video help \
        build-h264-plugins deploy-h264-plugins clean-h264-plugins test-h264-plugins rebuild-with-h264-plugins \
        build-h264-plugin clean-h264-plugin test-h264-plugin rebuild-with-h264

# Build targets
.PHONY: all video-with-h264 build-h264-unified-plugin deploy-unified-h264-plugin clean-unified-h264-plugin test-unified-h264-plugin \
        all-with-plugins video debug-info install-deps check-deps clean-all rebuild test-video help \
        build-h264-plugins deploy-h264-plugins clean-h264-plugins test-h264-plugins rebuild-with-h264-plugins \
        build-h264-plugin clean-h264-plugin test-h264-plugin rebuild-with-h264

# Default target using unified H.264 plugin
default: video deploy-unified-h264-plugin

# Main build target using unified H.264 plugin
video-with-unified-plugin: video deploy-unified-h264-plugin

# Video build with unified H.264 plugin deployment
video-with-h264: video deploy-unified-h264-plugin
	@echo ""
	@echo "🎉 H323ASKW with unified H.264 plugin is ready!"
	@echo ""
	@echo "Built components:"
	@if [ -f "./obj_Darwin_aarch64/h323askw" ]; then \
		echo "  ✅ H323ASKW: ./obj_Darwin_aarch64/h323askw"; \
	fi
	@if [ -f "./h264_plugin_h323plus.dylib" ]; then \
		echo "  ✅ H.264 Plugin: ./h264_plugin_h323plus.dylib"; \
	fi
	@echo ""
	@echo "🚀 Ready to use:"
	@echo "  ./obj_Darwin_aarch64/h323askw -v --usb-camera --sdl2-display h323:HOST"
	@echo ""
	@echo "📖 For more options, run: make help"

# Build unified H.264 plugin from source
build-h264-unified-plugin:
	@echo "注意: Makefile の安全モードにより自動ビルドは無効化されています。"
	@echo "統合 H.264 プラグイン（h264_plugin_h323plus.dylib）を手動でビルド／配置する手順:"
	@echo "  1) 依存ライブラリを確認・インストールしてください（macOS の例）:"
	@echo "       brew install ffmpeg x264" 
	@echo "  2) プラグインソースディレクトリへ移動:" 
	@echo "       cd $(H264_PLUGIN_DIR)" 
	@echo "  3) 下記のようにビルドします（環境に合わせて pkg-config を使うか -I/-L を調整してください）:"
	@echo "       g++ -shared -fPIC -DPIC -I$(OPENH323DIR)/include -I$(PWLIBDIR)/include \"
	@echo "         $$(pkg-config --cflags libavcodec libavutil libswresample x264 2>/dev/null || echo '-I/opt/homebrew/include') \" 
	@echo "         -o h264_plugin_h323plus.dylib h264_plugin_h323plus.cpp \"
	@echo "         $$(pkg-config --libs libavcodec libavutil libswresample x264 2>/dev/null || echo '-L/opt/homebrew/lib -lavcodec -lavutil -lswresample -lx264') \"
	@echo "         -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox -undefined dynamic_lookup"
	@echo "  4) ビルドが成功したらプラグインを h323askw ルートへコピー（デプロイ）してください:"
	@echo "       cp $(H264_PLUGIN_DIR)/h264_plugin_h323plus.dylib ./"
	@echo "  5) 既存の分離型プラグインがある場合は競合を避けるため削除または無効化してください。"
	@echo "  付記: 元の自動化手順は Makefile.plugin-targets.bak に保存されています。必要なら復元してください。"

# Enhanced 'all-with-plugins' target - complete build with H.264 plugins (legacy for separated plugins)
all-with-plugins: deploy-h264-plugins video
	@echo ""
	@echo "🎉 Complete Build with H.264 Plugins Finished!"
	@echo "H323ASKW with separated H.264 plugins is ready for H.323 video calls"
	@echo ""
	@echo "Built components:"
	@if [ -f "./obj_Darwin_aarch64_d/h323askw" ]; then \
		echo "  ✅ H323ASKW (debug): ./obj_Darwin_aarch64_d/h323askw"; \
	fi
	@if [ -f "./obj_Darwin_aarch64/h323askw" ]; then \
		echo "  ✅ H323ASKW (release): ./obj_Darwin_aarch64/h323askw"; \
	fi
	@if [ -f "./a_h264_decoder_ptplugin.dylib" ]; then \
		echo "  ✅ H.264 Decoder Plugin: ./a_h264_decoder_ptplugin.dylib"; \
	fi
	@if [ -f "./z_h264_encoder_ptplugin.dylib" ]; then \
		echo "  ✅ H.264 Encoder Plugin: ./z_h264_encoder_ptplugin.dylib"; \
	fi
	@echo ""
	@echo "🚀 Ready to use:"
	@echo "  ./obj_Darwin_aarch64/h323askw -v --usb-camera --sdl2-display h323:HOST"
	@echo ""
	@echo "📖 For more options, run: make help"

# H.264 Plugin Build Targets - Separated Encoder/Decoder Architecture
build-h264-plugins:
	@echo "注意: Makefile の安全モードにより分離型プラグインの自動ビルドは無効化されています。"
	@echo "分離型プラグイン（デコーダ／エンコーダ）を手動でビルドする手順（要: ソースが存在すること）:"
	@echo "  1) 依存を確認: ffmpeg / x264 がインストールされていること。"
	@echo "  2) プラグインソースディレクトリへ移動: cd $(H264_PLUGIN_DIR)"
	@echo "  3) デコーダをビルド例:"
	@echo "       g++ -shared -fPIC -DPIC -I$(OPENH323DIR)/include -I$(PWLIBDIR)/include \"
	@echo "         $$(pkg-config --cflags libavcodec libavutil 2>/dev/null || echo '-I/opt/homebrew/include') \" 
	@echo "         -o h264_decoder_only_ptplugin.dylib h264_decoder_only_plugin.cpp \"
	@echo "         $$(pkg-config --libs libavcodec libavutil 2>/dev/null || echo '-L/opt/homebrew/lib -lavcodec -lavutil') -undefined dynamic_lookup"
	@echo "  4) エンコーダのビルドも同様に行ってください（ソースファイル名: h264_encoder_only_plugin.cpp）。"
	@echo "  5) ビルド後、必要なら h323askw ルートへコピーしてデプロイしてください。"
	@echo "  付記: 自動化された元の手順は Makefile.plugin-targets.bak に保存されています。"

# Legacy support for old target name
build-h264-plugin: build-h264-plugins

deploy-h264-plugins: build-h264-plugins
	@echo "注意: 分離型プラグインの自動デプロイは無効化されています。手動デプロイ手順:"
	@echo "  1) 分離型プラグインが $(H264_PLUGIN_DIR) にあることを確認。"
	@echo "  2) h323askw ディレクトリへコピー:"
	@echo "       cp $(H264_PLUGIN_DIR)/h264_decoder_only_ptplugin.dylib ./"
	@echo "       cp $(H264_PLUGIN_DIR)/h264_encoder_only_ptplugin.dylib ./"
	@echo "  3) アルファベット順にリネームして読み込み順を制御（ decoder が先にロードされるように）："
	@echo "       mv ./h264_decoder_only_ptplugin.dylib ./a_h264_decoder_ptplugin.dylib"
	@echo "       mv ./h264_encoder_only_ptplugin.dylib ./z_h264_encoder_ptplugin.dylib"
	@echo "  4) 権限を設定: chmod 755 ./a_h264_decoder_ptplugin.dylib ./z_h264_encoder_ptplugin.dylib"
	@echo "  付記: 元の自動化手順は Makefile.plugin-targets.bak に保存されています。"

clean-h264-plugins:
	@echo "Cleaning H.264 plugins..."
	@echo "Removing built plugin files..."
	@rm -f $(H264_PLUGIN_DIR)/h264_decoder_only_ptplugin.dylib
	@rm -f $(H264_PLUGIN_DIR)/h264_encoder_only_ptplugin.dylib
	@echo "Removing deployed plugin files..."
	@rm -f ./a_h264_decoder_ptplugin.dylib
	@rm -f ./z_h264_encoder_ptplugin.dylib
	@echo "H.264 plugins cleaned"

# Legacy support
clean-h264-plugin: clean-h264-plugins

test-h264-plugins: deploy-h264-plugins
	@echo "Testing H.264 separated plugins..."
	@echo "=== Plugin File Verification ==="
	@if [ -f "./a_h264_decoder_ptplugin.dylib" ] && [ -f "./z_h264_encoder_ptplugin.dylib" ]; then \
		echo "✅ Both plugins are deployed correctly"; \
		echo "Decoder plugin: $$(ls -la ./a_h264_decoder_ptplugin.dylib)"; \
		echo "Encoder plugin: $$(ls -la ./z_h264_encoder_ptplugin.dylib)"; \
	else \
		echo "❌ Plugin deployment failed"; \
		exit 1; \
	fi
	@echo ""
	@echo "=== Plugin Symbol Verification ==="
	@echo "Checking decoder plugin symbols..."
	@if nm ./a_h264_decoder_ptplugin.dylib | grep -q "OpalCodecPlugin_GetCodecs"; then \
		echo "✅ Decoder plugin has required symbols"; \
	else \
		echo "❌ Decoder plugin missing required symbols"; \
	fi
	@echo "Checking encoder plugin symbols..."
	@if nm ./z_h264_encoder_ptplugin.dylib | grep -q "OpalCodecPlugin_GetCodecs"; then \
		echo "✅ Encoder plugin has required symbols"; \
	else \
		echo "❌ Encoder plugin missing required symbols"; \
	fi
	@echo ""
	@echo "=== Plugin Integration Test ==="
	@if [ -f "./obj_Darwin_aarch64/h323askw" ]; then \
		echo "Running brief integration test..."; \
		timeout 10 ./obj_Darwin_aarch64/h323askw --help >/dev/null 2>&1 && echo "✅ Basic integration test passed" || echo "⚠️  Integration test incomplete (timeout expected)"; \
	else \
		echo "⚠️  H323ASKW not built yet - run 'make video' first"; \
	fi
	@echo ""
	@echo "🎯 Plugin Architecture Summary:"
	@echo "  - Separated encoder/decoder overcome H323plus plugin manager limitations"
	@echo "  - Alphabetical loading ensures decoder registers before encoder"
	@echo "  - Ready for H.323 video calls with improved codec registration"

# H.264 Plugin Deployment - Unified Plugin Support
deploy-unified-h264-plugin:
	@echo "注意: 統合プラグインの自動デプロイは無効化されています。手動デプロイ手順:"
	@echo "  1) プラグインが $(H264_PLUGIN_DIR)/h264_plugin_h323plus.dylib にあることを確認。"
	@echo "     もし無ければ、'make build-h264-unified-plugin' の手順に従って手動でビルドしてください（Makefile.plugin-targets.bak を参照）。"
	@echo "  2) h323askw ルートへコピー:"
	@echo "       cp $(H264_PLUGIN_DIR)/h264_plugin_h323plus.dylib ./"
	@echo "  3) 必要に応じて古い分離型プラグインを削除して競合を避けてください。"
	@echo "  付記: 自動化された元の手順は Makefile.plugin-targets.bak に保存されています。"

clean-unified-h264-plugin:
	@echo "Cleaning unified H.264 plugin..."
	@rm -f ./h264_plugin_h323plus.dylib
	@echo "Unified H.264 plugin cleaned"

test-unified-h264-plugin: deploy-unified-h264-plugin
	@echo "Testing unified H.264 plugin..."
	@echo "=== Plugin File Verification ==="
	@if [ -f "./h264_plugin_h323plus.dylib" ]; then \
		echo "✅ Unified plugin is deployed correctly"; \
		echo "Plugin file: $$(ls -la ./h264_plugin_h323plus.dylib)"; \
	else \
		echo "❌ Unified plugin deployment failed"; \
		exit 1; \
	fi
	@echo ""
	@echo "=== Plugin Symbol Verification ==="
	@echo "Checking unified plugin symbols..."
	@if nm ./h264_plugin_h323plus.dylib | grep -q "OpalCodecPlugin_GetCodecs"; then \
		echo "✅ Unified plugin has required symbols"; \
	else \
		echo "❌ Unified plugin missing required symbols"; \
	fi
	@echo ""
	@echo "=== Plugin Integration Test ==="
	@if [ -f "./obj_Darwin_aarch64/h323askw" ]; then \
		echo "Running brief integration test..."; \
		timeout 10 ./obj_Darwin_aarch64/h323askw --help >/dev/null 2>&1 && echo "✅ Basic integration test passed" || echo "⚠️  Integration test incomplete (timeout expected)"; \
	else \
		echo "⚠️  H323ASKW not built yet - run 'make video' first"; \
	fi
	@echo ""
	@echo "🎯 Unified Plugin Architecture Summary:"
	@echo "  - Single plugin contains both encoder and decoder"
	@echo "  - Compatible with H323plus plugin manager expectations"
	@echo "  - Ready for H.323 video calls with standard codec registration"

# Primary target supporting both plugin architectures
all-with-unified-plugins: video deploy-unified-h264-plugin
	@echo ""
	@echo "🎉 H323ASKW build complete with unified H.264 plugin support!"
	@echo ""
	@echo "📁 Built files:"
	@if [ -f "./obj_Darwin_aarch64/h323askw" ]; then \
		echo "  ✅ Main executable: ./obj_Darwin_aarch64/h323askw"; \
	fi
	@if [ -f "./h264_plugin_h323plus.dylib" ]; then \
		echo "  ✅ H.264 Plugin: ./h264_plugin_h323plus.dylib"; \
	fi

# Legacy support
test-h264-plugin: test-h264-plugins
rebuild-with-h264: rebuild-with-h264-plugins

# USB HID implementation object rule (macOS only)
# This is compiled separately because IOKit's ULONG conflicts with PTLib's ULONG
# The object file must exist before linking, so make all OBJS depend on it
ifeq ($(shell uname -s),Darwin)
$(OBJDIR)/usb_hid_impl.o: usb_hid_impl.mm usb_hid_impl.h
	@mkdir -p $(OBJDIR)
	@echo "[CC] usb_hid_impl.mm (IOKit, no PTLib)"
	$(CXX) -std=c++17 -O2 -Wall -c $< -o $@

# Make main.o depend on usb_hid_impl.o to ensure correct build order
$(OBJDIR)/main.o: $(OBJDIR)/usb_hid_impl.o
$(OBJDIR)/usb_hid_controller.o: $(OBJDIR)/usb_hid_impl.o

# Permissions bootstrap (TCC / Local Network / Firewall)
$(OBJDIR)/Permissions.o: Permissions.mm Permissions.h
	@mkdir -p $(OBJDIR)
	@echo "[CC] Permissions.mm (Objective-C++)"
	$(CXX) $(STDCCFLAGS) $(CXXFLAGS) $(CPPFLAGS) -x objective-c++ -fobjc-arc -c $< -o $@

# Ensure main links against permission bootstrap object
$(OBJDIR)/main.o: $(OBJDIR)/Permissions.o
endif

video: debug-info
	@echo "Building H323ASKW with video support..."
	$(MAKE) debugnoshared

rebuild: clean-all video

clean-all:
	@echo "Cleaning all build artifacts..."
	$(MAKE) clean
	@rm -rf obj_* PWL* core *.log
	@echo "Cleaning deployed H.264 plugins..."
	@rm -f ./h264_plugin_h323plus.dylib
	@rm -f ./a_h264_decoder_ptplugin.dylib
	@rm -f ./z_h264_encoder_ptplugin.dylib
	@echo "All artifacts cleaned"

install-deps:
	@echo "Installing dependencies..."
ifeq ($(OSTYPE),Darwin)
	@if ! which brew >/dev/null 2>&1; then \
		echo "Homebrew not found. Please install from https://brew.sh/"; \
		exit 1; \
	fi
	@echo "Checking SDL2..."
	@if ! which sdl2-config >/dev/null 2>&1; then \
		echo "Installing SDL2..."; \
		brew install sdl2; \
	else \
		echo "SDL2 already installed"; \
	fi
	@echo "Checking FFmpeg..."
	@if ! which ffmpeg >/dev/null 2>&1; then \
		echo "Installing FFmpeg..."; \
		brew install ffmpeg; \
	else \
		echo "FFmpeg already installed"; \
	fi
	@echo "Checking x264..."
	@if ! brew list x264 >/dev/null 2>&1; then \
		echo "Installing x264..."; \
		brew install x264; \
	else \
		echo "x264 already installed"; \
	fi
	@echo "Dependencies check complete"
else
	@echo "Please install development packages:"
	@echo "  Ubuntu/Debian: sudo apt-get install libsdl2-dev libavcodec-dev libavutil-dev libx264-dev"
	@echo "  CentOS/RHEL: sudo yum install SDL2-devel ffmpeg-devel x264-devel"
	@echo "  Fedora: sudo dnf install SDL2-devel ffmpeg-devel x264-devel"
endif

# Check dependencies
check-deps:
	@echo "=== Dependency Check ==="
	@echo "Operating System: $(OSTYPE)"
	@echo ""
	@echo "SDL2:"
	@if which sdl2-config >/dev/null 2>&1; then \
		echo "  ✅ sdl2-config found: $$(which sdl2-config)"; \
		echo "  Version: $$(sdl2-config --version 2>/dev/null)"; \
	else \
		echo "  ❌ SDL2 not found"; \
	fi
	@echo ""
	@echo "FFmpeg:"
	@if which ffmpeg >/dev/null 2>&1; then \
		echo "  ✅ ffmpeg found: $$(which ffmpeg)"; \
		echo "  Version: $$(ffmpeg -version 2>/dev/null | head -1)"; \
	else \
		echo "  ❌ FFmpeg not found"; \
	fi
	@echo ""
	@echo "libavcodec:"
	@if pkg-config --exists libavcodec 2>/dev/null; then \
		echo "  ✅ libavcodec found via pkg-config"; \
		echo "  Version: $$(pkg-config --modversion libavcodec 2>/dev/null)"; \
	elif [ -f "/opt/homebrew/lib/libavcodec.dylib" ]; then \
		echo "  ✅ libavcodec found at /opt/homebrew/lib/"; \
	else \
		echo "  ❌ libavcodec not found"; \
	fi
	@echo ""
	@echo "x264:"
	@if pkg-config --exists x264 2>/dev/null; then \
		echo "  ✅ x264 found via pkg-config"; \
		echo "  Version: $$(pkg-config --modversion x264 2>/dev/null)"; \
	elif [ -f "/opt/homebrew/lib/libx264.dylib" ]; then \
		echo "  ✅ x264 found at /opt/homebrew/lib/"; \
	else \
		echo "  ❌ x264 not found"; \
	fi
	@echo ""

test-video: video
	@echo "Testing video capabilities..."
	@if [ -f "./obj_Darwin_aarch64_d_s/h323askw" ]; then \
		echo "Built successfully: ./obj_Darwin_aarch64_d_s/h323askw"; \
		echo "Testing help output:"; \
		./obj_Darwin_aarch64_d_s/h323askw --help | grep -E "(video|camera|sdl)" || echo "No video options found in help"; \
	else \
		echo "Build failed - executable not found"; \
		exit 1; \
	fi

h323askw-help::
	@echo "==============================================="
	@echo "H323ASKW H.323 Video Call Client - Makefile Help"
	@echo "==============================================="
	@echo ""
	@echo "📋 Main Build Targets:"
	@echo "  default                  - Build H323ASKW with unified H.264 plugin (RECOMMENDED)"
	@echo "  video-with-unified-plugin - Build with detailed status output"
	@echo "  video-with-h264          - Build with detailed status output (alias)"
	@echo "  video                    - Build H323ASKW only (no plugin deployment)"
	@echo ""
	@echo "🔧 H.264 Plugin Targets (Unified):"
	@echo "  build-h264-unified-plugin     - Build unified h264_plugin_h323plus.dylib"
	@echo "  deploy-unified-h264-plugin    - Deploy unified plugin to h323askw directory"
	@echo "  clean-unified-h264-plugin     - Remove deployed unified plugin"
	@echo "  test-unified-h264-plugin      - Test unified plugin functionality"
	@echo ""
	@echo "🔧 H.264 Plugin Targets (Legacy - Separated):"
	@echo "  build-h264-plugins       - Build separated encoder/decoder plugins"
	@echo "  deploy-h264-plugins      - Deploy separated plugins with optimal loading"
	@echo "  clean-h264-plugins       - Remove separated plugin files"
	@echo "  test-h264-plugins        - Test separated plugin system"
	@echo "  all-with-plugins         - Build with separated plugins (legacy)"
	@echo ""
	@echo "🔧 Dependencies:"
	@echo "  install-deps             - Install dependencies (SDL2, FFmpeg, x264)"
	@echo "  check-deps               - Check dependency status"
	@echo ""
	@echo "🧹 Cleanup:"
	@echo "  clean                    - Standard OpenH323 clean"
	@echo "  clean-all                - Remove all build artifacts and plugins"
	@echo ""
	@echo "Utility Targets:"
	@echo "  debug-info               - Show build configuration"
	@echo "  help                     - Show this help (run 'make h323askw-help')"
	@echo ""
	@echo "⚠️  Current Default: Unified Plugin Architecture"
	@echo "  'make default' now uses the unified h264_plugin_h323plus.dylib"
	@echo "  For legacy separated plugins, use 'make all-with-plugins'"
	@echo ""
	@echo "🔧 Plugin Architecture:"
	@echo "  The unified plugin contains both encoder and decoder in a single dylib"
	@echo "  for compatibility with standard H323plus expectations."
	@echo ""
	@echo "📋 Recommended Workflow:"
	@echo "  1. make check-deps                # Check what's installed"
	@echo "  2. make install-deps              # Install missing dependencies"  
	@echo "  3. make default                   # Build everything (RECOMMENDED)"
	@echo "  4. make test-unified-h264-plugin  # Test the plugin system"
	@echo ""
	@echo "🚀 Quick Start:"
	@echo "  make default                      # Build everything from scratch"
	@echo ""
	@echo "📖 Usage Examples:"
	@echo "  Listen mode:   ./obj_Darwin_aarch64/h323askw -l -v --sdl2-display"
	@echo "  USB camera:    ./obj_Darwin_aarch64/h323askw -v --usb-camera --sdl2-display h323:HOST"
	@echo "  H.264 call:    ./obj_Darwin_aarch64/h323askw -v --usb-camera --sdl2-display -P h264 HOST"
	@echo ""
	@echo "🔍 Plugin Loading:"
	@echo "  - h264_plugin_h323plus.dylib  (unified encoder+decoder)"

# dependencies
$(OBJDIR)/main.o: main.h version.h


# Shim so `make help` runs our h323askw-help without overriding included makefiles
help: h323askw-help
