#!/bin/bash
#
# H323ASKW App Bundle 作成スクリプト
# macOS用の配布可能なアプリケーションバンドルを作成します
#

set -e  # エラー時に停止

# ===== スクリプトのディレクトリから相対パスを導出 =====
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ===== version.h からバージョン情報を読み取る =====
VERSION_H="${SCRIPT_DIR}/version.h"
if [ -f "${VERSION_H}" ]; then
    MAJOR_VERSION=$(grep "#define MAJOR_VERSION" "${VERSION_H}" | awk '{print $3}')
    MINOR_VERSION=$(grep "#define MINOR_VERSION" "${VERSION_H}" | awk '{print $3}')
    BUILD_NUMBER=$(grep "#define BUILD_NUMBER" "${VERSION_H}" | awk '{print $3}')
    APP_VERSION="${MAJOR_VERSION}.${MINOR_VERSION}.${BUILD_NUMBER}"
else
    echo "Warning: version.h not found, using default version"
    APP_VERSION="1.0.0"
fi

# ===== 設定 =====
APP_NAME="H323ASKW"
# APP_VERSION は version.h から自動取得
BUNDLE_ID="com.h323askw.app"

# ソースパス (スクリプト位置から相対的に導出)
CALLGEN_DIR="${SCRIPT_DIR}"
H323PLUS_DIR="${SCRIPT_DIR}/../h323plus"
PTLIB_DIR="${SCRIPT_DIR}/../ptlib"

# Homebrew ディレクトリ (環境変数または自動検出)
if [ -n "${HOMEBREW_PREFIX}" ]; then
    HOMEBREW_DIR="${HOMEBREW_PREFIX}"
elif [ -d "/opt/homebrew" ]; then
    HOMEBREW_DIR="/opt/homebrew"
elif [ -d "/usr/local" ]; then
    HOMEBREW_DIR="/usr/local"
else
    echo "Error: Homebrew directory not found"
    exit 1
fi

# 出力ディレクトリ
OUTPUT_DIR="${CALLGEN_DIR}/dist"
APP_BUNDLE="${OUTPUT_DIR}/${APP_NAME}.app"

# ===== 色付き出力 =====
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# ===== ライブラリ検出ヘルパー関数 =====
find_dylib() {
    local pattern="$1"
    local dir="$2"
    # ワイルドカードで最初にマッチするものを返す
    local found=$(ls -1 ${dir}/${pattern} 2>/dev/null | head -1)
    echo "$found"
}

# ===== 必要なファイルの存在確認 =====
check_prerequisites() {
    log_info "必要なファイルを確認中..."
    log_info "  CALLGEN_DIR: ${CALLGEN_DIR}"
    log_info "  H323PLUS_DIR: ${H323PLUS_DIR}"
    log_info "  PTLIB_DIR: ${PTLIB_DIR}"
    log_info "  HOMEBREW_DIR: ${HOMEBREW_DIR}"
    
    local missing=0
    
    # 実行ファイル
    if [ ! -f "${CALLGEN_DIR}/obj_Darwin_aarch64/h323askw" ]; then
        log_error "実行ファイルが見つかりません: ${CALLGEN_DIR}/obj_Darwin_aarch64/h323askw"
        missing=1
    fi
    
    # 自作ライブラリ (ワイルドカードで検出)
    H323_DYLIB=$(find_dylib "libh323_Darwin_aarch64_*.dylib" "${H323PLUS_DIR}/lib")
    if [ -z "${H323_DYLIB}" ]; then
        log_error "h323plus ライブラリが見つかりません"
        missing=1
    else
        log_info "  Found h323plus: ${H323_DYLIB}"
    fi
    
    PTLIB_DYLIB=$(find_dylib "libpt.*.dylib" "${PTLIB_DIR}/lib_Darwin_aarch64")
    if [ -z "${PTLIB_DYLIB}" ]; then
        log_error "ptlib ライブラリが見つかりません"
        missing=1
    else
        log_info "  Found ptlib: ${PTLIB_DYLIB}"
    fi
    
    # Homebrewライブラリ (メジャーバージョンのワイルドカード)
    local homebrew_libs=(
        "openssl@3/lib/libssl.3*.dylib"
        "openssl@3/lib/libcrypto.3*.dylib"
        "ffmpeg/lib/libavcodec.6*.dylib"
        "ffmpeg/lib/libavutil.6*.dylib"
        "ffmpeg/lib/libswresample.*.dylib"
        "ffmpeg/lib/libswscale.*.dylib"
        "x264/lib/libx264.*.dylib"
        "portaudio/lib/libportaudio.*.dylib"
    )
    
    for pattern in "${homebrew_libs[@]}"; do
        local found=$(find_dylib "$(basename "$pattern")" "${HOMEBREW_DIR}/opt/$(dirname "$pattern")")
        if [ -z "$found" ]; then
            log_warn "Homebrewライブラリが見つかりません: ${pattern}"
            # Don't fail for optional libs, but warn
        fi
    done
    
    # Qt6 フレームワーク (qtbaseに配置される)
    for fw in "QtCore" "QtGui" "QtWidgets"; do
        if [ ! -d "${HOMEBREW_DIR}/opt/qtbase/lib/${fw}.framework" ]; then
            log_error "Qt6フレームワークが見つかりません: ${fw}"
            missing=1
        fi
    done
    
    # プラグイン
    if [ ! -f "${H323PLUS_DIR}/plugins/video/H.264/h264_video_pwplugin.dylib" ]; then
        log_error "H.264 プラグインが見つかりません"
        missing=1
    fi
    
    if [ $missing -eq 1 ]; then
        log_error "必要なファイルが不足しています。終了します。"
        exit 1
    fi
    
    log_success "全ての必要なファイルが見つかりました"
}

# ===== App Bundle 構造の作成 =====
create_bundle_structure() {
    log_info "App Bundle 構造を作成中..."
    
    # 既存のバンドルを削除
    rm -rf "${APP_BUNDLE}"
    mkdir -p "${OUTPUT_DIR}"
    
    # ディレクトリ構造
    mkdir -p "${APP_BUNDLE}/Contents/MacOS"
    mkdir -p "${APP_BUNDLE}/Contents/Frameworks"
    mkdir -p "${APP_BUNDLE}/Contents/Resources/plugins/video"
    mkdir -p "${APP_BUNDLE}/Contents/Resources/plugins/vidinput"
    mkdir -p "${APP_BUNDLE}/Contents/Resources/plugins/audio"
    mkdir -p "${APP_BUNDLE}/Contents/Resources/plugins/sound"
    
    log_success "App Bundle 構造を作成しました"
}

# ===== Info.plist の作成 =====
create_info_plist() {
    log_info "Info.plist を作成中..."
    
    cat > "${APP_BUNDLE}/Contents/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>h323askw</string>
    <key>CFBundleIdentifier</key>
    <string>${BUNDLE_ID}</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>${APP_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${APP_VERSION}</string>
    <key>CFBundleVersion</key>
    <string>${APP_VERSION}</string>
    <key>LSMinimumSystemVersion</key>
    <string>11.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSMicrophoneUsageDescription</key>
    <string>H323ASKW needs access to your microphone for voice calls.</string>
    <key>NSCameraUsageDescription</key>
    <string>H323ASKW needs access to your camera for video calls.</string>
    <key>NSBluetoothAlwaysUsageDescription</key>
    <string>H323ASKW uses Bluetooth to connect to wireless headsets with mute buttons.</string>
    <key>LSArchitecturePriority</key>
    <array>
        <string>arm64</string>
    </array>
</dict>
</plist>
EOF
    
    log_success "Info.plist を作成しました"
}

# ===== 実行ファイルのコピー =====
copy_executable() {
    log_info "実行ファイルをコピー中..."
    
    cp "${CALLGEN_DIR}/obj_Darwin_aarch64/h323askw" "${APP_BUNDLE}/Contents/MacOS/"
    chmod +x "${APP_BUNDLE}/Contents/MacOS/h323askw"
    
    log_success "実行ファイルをコピーしました"
}

# ===== ライブラリのコピー =====
copy_libraries() {
    log_info "ライブラリをコピー中..."
    
    local FRAMEWORKS="${APP_BUNDLE}/Contents/Frameworks"
    
    # 自作ライブラリ (動的に検出されたパスを使用)
    cp "${H323_DYLIB}" "${FRAMEWORKS}/libh323.dylib"
    cp "${PTLIB_DYLIB}" "${FRAMEWORKS}/libpt.dylib"
    
    # OpenSSL (ワイルドカードで最新を取得)
    local ssl_lib=$(find_dylib "libssl.3*.dylib" "${HOMEBREW_DIR}/opt/openssl@3/lib")
    local crypto_lib=$(find_dylib "libcrypto.3*.dylib" "${HOMEBREW_DIR}/opt/openssl@3/lib")
    [ -n "$ssl_lib" ] && cp "$ssl_lib" "${FRAMEWORKS}/libssl.3.dylib"
    [ -n "$crypto_lib" ] && cp "$crypto_lib" "${FRAMEWORKS}/libcrypto.3.dylib"
    
    # Qt6 フレームワーク (qtbaseに配置される)
    log_info "  Qt6 フレームワークをコピー中..."
    for fw in QtCore QtGui QtWidgets QtDBus; do
        if [ -d "${HOMEBREW_DIR}/opt/qtbase/lib/${fw}.framework" ]; then
            cp -R "${HOMEBREW_DIR}/opt/qtbase/lib/${fw}.framework" "${FRAMEWORKS}/"
            # 不要なファイルを削除してサイズ削減
            rm -rf "${FRAMEWORKS}/${fw}.framework/Headers"
            rm -rf "${FRAMEWORKS}/${fw}.framework/Versions/A/Headers"
        elif [ -d "${HOMEBREW_DIR}/lib/${fw}.framework" ]; then
            cp -R "${HOMEBREW_DIR}/lib/${fw}.framework" "${FRAMEWORKS}/"
            rm -rf "${FRAMEWORKS}/${fw}.framework/Headers"
            rm -rf "${FRAMEWORKS}/${fw}.framework/Versions/A/Headers"
        else
            log_warn "  ${fw}.framework not found - skipping"
        fi
    done
    
    # FFmpeg (実体ファイルをコピー)
    cp "${HOMEBREW_DIR}/opt/ffmpeg/lib/libavcodec.62.19.100.dylib" "${FRAMEWORKS}/libavcodec.62.dylib"
    cp "${HOMEBREW_DIR}/opt/ffmpeg/lib/libavutil.60.18.100.dylib" "${FRAMEWORKS}/libavutil.60.dylib"
    cp "${HOMEBREW_DIR}/opt/ffmpeg/lib/libswresample.6.2.100.dylib" "${FRAMEWORKS}/libswresample.6.dylib"
    cp "${HOMEBREW_DIR}/opt/ffmpeg/lib/libswscale.9.3.100.dylib" "${FRAMEWORKS}/libswscale.9.dylib"
    
    # x264
    cp "${HOMEBREW_DIR}/opt/x264/lib/libx264.165.dylib" "${FRAMEWORKS}/"
    
    # PortAudio
    cp "${HOMEBREW_DIR}/opt/portaudio/lib/libportaudio.2.dylib" "${FRAMEWORKS}/"
    
    # FFmpeg 追加依存ライブラリ (配布用に必要)
    log_info "  FFmpeg 依存ライブラリをコピー中..."
    
    # 主要なFFmpeg依存ライブラリをコピー
    local FFMPEG_DEPS=(
        "libvpx/lib/libvpx.11.dylib"
        "webp/lib/libwebpmux.3.dylib"
        "webp/lib/libwebp.7.dylib"
        "webp/lib/libsharpyuv.0.dylib"
        "xz/lib/liblzma.5.dylib"
        "aribb24/lib/libaribb24.0.dylib"
        "dav1d/lib/libdav1d.7.dylib"
        "opencore-amr/lib/libopencore-amrwb.0.dylib"
        "opencore-amr/lib/libopencore-amrnb.0.dylib"
        "snappy/lib/libsnappy.1.dylib"
        "aom/lib/libaom.3.dylib"
        "libvmaf/lib/libvmaf.3.dylib"
        "jpeg-xl/lib/libjxl.0.11.dylib"
        "jpeg-xl/lib/libjxl_threads.0.11.dylib"
        "jpeg-xl/lib/libjxl_cms.0.11.dylib"
        "lame/lib/libmp3lame.0.dylib"
        "openjpeg/lib/libopenjp2.7.dylib"
        "opus/lib/libopus.0.dylib"
        "rav1e/lib/librav1e.0.8.dylib"
        "speex/lib/libspeex.1.dylib"
        "svt-av1/lib/libSvtAv1Enc.3.dylib"
        "theora/lib/libtheoraenc.2.dylib"
        "theora/lib/libtheoradec.2.dylib"
        "libogg/lib/libogg.0.dylib"
        "libvorbis/lib/libvorbis.0.dylib"
        "libvorbis/lib/libvorbisenc.2.dylib"
        "x265/lib/libx265.215.dylib"
        "libsoxr/lib/libsoxr.0.dylib"
        "libx11/lib/libX11.6.dylib"
        "libxcb/lib/libxcb.1.dylib"
        "libxau/lib/libXau.6.dylib"
        "libxdmcp/lib/libXdmcp.6.dylib"
        "brotli/lib/libbrotlidec.1.dylib"
        "brotli/lib/libbrotlienc.1.dylib"
        "brotli/lib/libbrotlicommon.1.dylib"
        "highway/lib/libhwy.1.dylib"
        "highway/lib/libhwy_contrib.1.dylib"
        "little-cms2/lib/liblcms2.2.dylib"
        "jpeg-turbo/lib/libjpeg.8.dylib"
        "openssl@3/lib/libcrypto.3.dylib"
        "libpng/lib/libpng16.16.dylib"
    )
    
    for dep in "${FFMPEG_DEPS[@]}"; do
        local src="${HOMEBREW_DIR}/opt/${dep}"
        local name=$(basename "$dep")
        if [ -f "$src" ]; then
            cp "$src" "${FRAMEWORKS}/" 2>/dev/null || true
        fi
    done
    
    log_success "ライブラリをコピーしました"
}

# ===== プラグインのコピー =====
copy_plugins() {
    log_info "プラグインをコピー中..."
    
    local PLUGINS="${APP_BUNDLE}/Contents/Resources/plugins"
    
    # Qt6 プラットフォームプラグイン（必須）
    log_info "  Qt6 プラットフォームプラグインをコピー中..."
    local QT_PLUGINS="${APP_BUNDLE}/Contents/PlugIns"
    mkdir -p "${QT_PLUGINS}/platforms"
    cp "${HOMEBREW_DIR}/opt/qtbase/share/qt/plugins/platforms/libqcocoa.dylib" "${QT_PLUGINS}/platforms/"
    
    # Qt6 スタイルプラグイン（macOS ネイティブの丸みを帯びたウィンドウスタイルに必要）
    log_info "  Qt6 スタイルプラグインをコピー中..."
    mkdir -p "${QT_PLUGINS}/styles"
    if [ -f "${HOMEBREW_DIR}/opt/qtbase/share/qt/plugins/styles/libqmacstyle.dylib" ]; then
        cp "${HOMEBREW_DIR}/opt/qtbase/share/qt/plugins/styles/libqmacstyle.dylib" "${QT_PLUGINS}/styles/"
    elif [ -d "${HOMEBREW_DIR}/opt/qt/share/qt/plugins/styles" ]; then
        cp -R "${HOMEBREW_DIR}/opt/qt/share/qt/plugins/styles/"* "${QT_PLUGINS}/styles/" 2>/dev/null || true
    fi
    
    # Video codecs (H323plusと同じディレクトリ構造を維持)
    mkdir -p "${PLUGINS}/video/H.264"
    mkdir -p "${PLUGINS}/video/H.263-ffmpeg"
    mkdir -p "${PLUGINS}/video/H.261-vic"
    
    # H.264 plugin
    cp "${H323PLUS_DIR}/plugins/video/H.264/h264_video_pwplugin.dylib" "${PLUGINS}/video/H.264/"
    
    # H.263-ffmpeg plugin (FFmpeg-based H.263 codec)
    if [ -f "${H323PLUS_DIR}/plugins/video/H.263-ffmpeg/h263-ffmpeg_video_pwplugin.dylib" ]; then
        cp "${H323PLUS_DIR}/plugins/video/H.263-ffmpeg/h263-ffmpeg_video_pwplugin.dylib" "${PLUGINS}/video/H.263-ffmpeg/"
        log_info "  H.263-ffmpeg プラグインをコピーしました"
    else
        log_warn "  H.263-ffmpeg プラグインが見つかりません"
    fi
    
    # H.261-vic plugin
    if [ -f "${H323PLUS_DIR}/plugins/video/H.261-vic/h261-vic_video_pwplugin.dylib" ]; then
        cp "${H323PLUS_DIR}/plugins/video/H.261-vic/h261-vic_video_pwplugin.dylib" "${PLUGINS}/video/H.261-vic/"
        log_info "  H.261-vic プラグインをコピーしました"
    else
        log_warn "  H.261-vic プラグインが見つかりません"
    fi
    
    # Video input
    cp "${PTLIB_DIR}/plugins/vidinput_macos/vidinput_macos_pwplugin.dylib" "${PLUGINS}/vidinput/"
    
    # Audio codecs (オプション)
    if [ -f "${H323PLUS_DIR}/plugins/audio/G722/g722_audio_pwplugin.dylib" ]; then
        cp "${H323PLUS_DIR}/plugins/audio/G722/g722_audio_pwplugin.dylib" "${PLUGINS}/audio/"
    fi
    if [ -f "${H323PLUS_DIR}/plugins/audio/G.722.1/g7221_audio_pwplugin.dylib" ]; then
        cp "${H323PLUS_DIR}/plugins/audio/G.722.1/g7221_audio_pwplugin.dylib" "${PLUGINS}/audio/"
    fi
    
    # Sound device (PortAudio)
    cp "${PTLIB_DIR}/lib_Darwin_aarch64/device/sound/portaudio_pwplugin.dylib" "${PLUGINS}/sound/"
    
    log_success "プラグインをコピーしました"
}

# ===== ライブラリパスの書き換え =====
fix_library_paths() {
    log_info "ライブラリパスを書き換え中..."
    
    local FRAMEWORKS="${APP_BUNDLE}/Contents/Frameworks"
    local MACOS="${APP_BUNDLE}/Contents/MacOS"
    local PLUGINS="${APP_BUNDLE}/Contents/Resources/plugins"
    
    # --- 実行ファイルの依存関係を修正 ---
    log_info "  実行ファイルのパスを修正..."
    
    # h323plus
    install_name_tool -change \
        "/Users/example/h323plus/lib/libh323_Darwin_aarch64_.1.28.0.dylib" \
        "@executable_path/../Frameworks/libh323.dylib" \
        "${MACOS}/h323askw"
    
    # ptlib
    install_name_tool -change \
        "/Users/example/ptlib/lib_Darwin_aarch64/libpt.2.10.9.dylib" \
        "@executable_path/../Frameworks/libpt.dylib" \
        "${MACOS}/h323askw"
    
    # OpenSSL
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/openssl@3/lib/libssl.3.dylib" \
        "@executable_path/../Frameworks/libssl.3.dylib" \
        "${MACOS}/h323askw"
    
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/openssl@3/lib/libcrypto.3.dylib" \
        "@executable_path/../Frameworks/libcrypto.3.dylib" \
        "${MACOS}/h323askw"
    
    # Qt6 (qtbaseからコピー)
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/qtbase/lib/QtCore.framework/Versions/A/QtCore" \
        "@executable_path/../Frameworks/QtCore.framework/Versions/A/QtCore" \
        "${MACOS}/h323askw"
    
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/qtbase/lib/QtGui.framework/Versions/A/QtGui" \
        "@executable_path/../Frameworks/QtGui.framework/Versions/A/QtGui" \
        "${MACOS}/h323askw"
    
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/qtbase/lib/QtWidgets.framework/Versions/A/QtWidgets" \
        "@executable_path/../Frameworks/QtWidgets.framework/Versions/A/QtWidgets" \
        "${MACOS}/h323askw"
    
    # --- ライブラリ自体のIDを修正 ---
    log_info "  ライブラリIDを修正..."
    
    install_name_tool -id "@executable_path/../Frameworks/libh323.dylib" "${FRAMEWORKS}/libh323.dylib"
    install_name_tool -id "@executable_path/../Frameworks/libpt.dylib" "${FRAMEWORKS}/libpt.dylib"
    install_name_tool -id "@executable_path/../Frameworks/libssl.3.dylib" "${FRAMEWORKS}/libssl.3.dylib"
    install_name_tool -id "@executable_path/../Frameworks/libcrypto.3.dylib" "${FRAMEWORKS}/libcrypto.3.dylib"
    install_name_tool -id "@executable_path/../Frameworks/libavcodec.62.dylib" "${FRAMEWORKS}/libavcodec.62.dylib"
    
    # Qt6 フレームワークのIDを修正
    log_info "  Qt6 フレームワークのIDを修正..."
    for fw in QtCore QtGui QtWidgets QtDBus; do
        if [ -f "${FRAMEWORKS}/${fw}.framework/Versions/A/${fw}" ]; then
            install_name_tool -id "@executable_path/../Frameworks/${fw}.framework/Versions/A/${fw}" \
                "${FRAMEWORKS}/${fw}.framework/Versions/A/${fw}"
        fi
    done
    
    # Qt6 内部依存の修正 (qtbaseパス)
    log_info "  Qt6 内部依存を修正..."
    # QtWidgets -> QtGui, QtCore
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/qtbase/lib/QtGui.framework/Versions/A/QtGui" \
        "@executable_path/../Frameworks/QtGui.framework/Versions/A/QtGui" \
        "${FRAMEWORKS}/QtWidgets.framework/Versions/A/QtWidgets" 2>/dev/null || true
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/qtbase/lib/QtCore.framework/Versions/A/QtCore" \
        "@executable_path/../Frameworks/QtCore.framework/Versions/A/QtCore" \
        "${FRAMEWORKS}/QtWidgets.framework/Versions/A/QtWidgets" 2>/dev/null || true
    install_name_tool -change \
        "@rpath/QtGui.framework/Versions/A/QtGui" \
        "@executable_path/../Frameworks/QtGui.framework/Versions/A/QtGui" \
        "${FRAMEWORKS}/QtWidgets.framework/Versions/A/QtWidgets" 2>/dev/null || true
    install_name_tool -change \
        "@rpath/QtCore.framework/Versions/A/QtCore" \
        "@executable_path/../Frameworks/QtCore.framework/Versions/A/QtCore" \
        "${FRAMEWORKS}/QtWidgets.framework/Versions/A/QtWidgets" 2>/dev/null || true
    
    # QtGui -> QtCore, QtDBus
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/qtbase/lib/QtCore.framework/Versions/A/QtCore" \
        "@executable_path/../Frameworks/QtCore.framework/Versions/A/QtCore" \
        "${FRAMEWORKS}/QtGui.framework/Versions/A/QtGui" 2>/dev/null || true
    install_name_tool -change \
        "@rpath/QtCore.framework/Versions/A/QtCore" \
        "@executable_path/../Frameworks/QtCore.framework/Versions/A/QtCore" \
        "${FRAMEWORKS}/QtGui.framework/Versions/A/QtGui" 2>/dev/null || true
    install_name_tool -change \
        "@rpath/QtDBus.framework/Versions/A/QtDBus" \
        "@executable_path/../Frameworks/QtDBus.framework/Versions/A/QtDBus" \
        "${FRAMEWORKS}/QtGui.framework/Versions/A/QtGui" 2>/dev/null || true
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/qtbase/lib/QtDBus.framework/Versions/A/QtDBus" \
        "@executable_path/../Frameworks/QtDBus.framework/Versions/A/QtDBus" \
        "${FRAMEWORKS}/QtGui.framework/Versions/A/QtGui" 2>/dev/null || true
    
    # QtDBus -> QtCore
    if [ -f "${FRAMEWORKS}/QtDBus.framework/Versions/A/QtDBus" ]; then
        install_name_tool -change \
            "${HOMEBREW_DIR}/opt/qtbase/lib/QtCore.framework/Versions/A/QtCore" \
            "@executable_path/../Frameworks/QtCore.framework/Versions/A/QtCore" \
            "${FRAMEWORKS}/QtDBus.framework/Versions/A/QtDBus" 2>/dev/null || true
        install_name_tool -change \
            "@rpath/QtCore.framework/Versions/A/QtCore" \
            "@executable_path/../Frameworks/QtCore.framework/Versions/A/QtCore" \
            "${FRAMEWORKS}/QtDBus.framework/Versions/A/QtDBus" 2>/dev/null || true
    fi
    install_name_tool -id "@executable_path/../Frameworks/libavutil.60.dylib" "${FRAMEWORKS}/libavutil.60.dylib"
    install_name_tool -id "@executable_path/../Frameworks/libswresample.6.dylib" "${FRAMEWORKS}/libswresample.6.dylib"
    install_name_tool -id "@executable_path/../Frameworks/libswscale.9.dylib" "${FRAMEWORKS}/libswscale.9.dylib"
    install_name_tool -id "@executable_path/../Frameworks/libx264.165.dylib" "${FRAMEWORKS}/libx264.165.dylib"
    install_name_tool -id "@executable_path/../Frameworks/libportaudio.2.dylib" "${FRAMEWORKS}/libportaudio.2.dylib"
    
    # --- libh323 の依存関係を修正 ---
    log_info "  libh323 の依存関係を修正..."
    
    install_name_tool -change \
        "/Users/example/ptlib/lib_Darwin_aarch64/libpt.2.10.9.dylib" \
        "@executable_path/../Frameworks/libpt.dylib" \
        "${FRAMEWORKS}/libh323.dylib"
    
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/openssl@3/lib/libssl.3.dylib" \
        "@executable_path/../Frameworks/libssl.3.dylib" \
        "${FRAMEWORKS}/libh323.dylib"
    
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/openssl@3/lib/libcrypto.3.dylib" \
        "@executable_path/../Frameworks/libcrypto.3.dylib" \
        "${FRAMEWORKS}/libh323.dylib"
    
    # --- libpt の依存関係を修正 ---
    log_info "  libpt の依存関係を修正..."
    
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/openssl@3/lib/libssl.3.dylib" \
        "@executable_path/../Frameworks/libssl.3.dylib" \
        "${FRAMEWORKS}/libpt.dylib"
    
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/openssl@3/lib/libcrypto.3.dylib" \
        "@executable_path/../Frameworks/libcrypto.3.dylib" \
        "${FRAMEWORKS}/libpt.dylib"
    
    # --- libssl の libcrypto 依存を修正 ---
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/openssl@3/lib/libcrypto.3.dylib" \
        "@executable_path/../Frameworks/libcrypto.3.dylib" \
        "${FRAMEWORKS}/libssl.3.dylib"
    
    # --- H.264 プラグインの依存関係を修正 ---
    log_info "  H.264 プラグインの依存関係を修正..."
    
    local H264_PLUGIN="${PLUGINS}/video/H.264/h264_video_pwplugin.dylib"
    
    # Note: H.264プラグインはヘッダスペース不足でinstall_name_toolが失敗することがある
    # その場合はDYLD_LIBRARY_PATHで解決する
    install_name_tool -id "@executable_path/../Resources/plugins/video/H.264/h264_video_pwplugin.dylib" \
        "${H264_PLUGIN}" 2>/dev/null || log_warn "  H.264プラグインID設定をスキップ"
    
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/ffmpeg/lib/libavcodec.62.dylib" \
        "@executable_path/../Frameworks/libavcodec.62.dylib" \
        "${H264_PLUGIN}" 2>/dev/null || true
    
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/ffmpeg/lib/libavutil.60.dylib" \
        "@executable_path/../Frameworks/libavutil.60.dylib" \
        "${H264_PLUGIN}" 2>/dev/null || true
    
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/ffmpeg/lib/libswresample.6.dylib" \
        "@executable_path/../Frameworks/libswresample.6.dylib" \
        "${H264_PLUGIN}" 2>/dev/null || true
    
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/ffmpeg/lib/libswscale.9.dylib" \
        "@executable_path/../Frameworks/libswscale.9.dylib" \
        "${H264_PLUGIN}" 2>/dev/null || true
    
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/x264/lib/libx264.165.dylib" \
        "@executable_path/../Frameworks/libx264.165.dylib" \
        "${H264_PLUGIN}" 2>/dev/null || true
    
    # --- H.263-ffmpeg プラグインの依存関係を修正 ---
    log_info "  H.263-ffmpeg プラグインの依存関係を修正..."
    
    local H263_PLUGIN="${PLUGINS}/video/H.263-ffmpeg/h263-ffmpeg_video_pwplugin.dylib"
    if [ -f "${H263_PLUGIN}" ]; then
        install_name_tool -id "@executable_path/../Resources/plugins/video/H.263-ffmpeg/h263-ffmpeg_video_pwplugin.dylib" \
            "${H263_PLUGIN}" 2>/dev/null || log_warn "  H.263プラグインID設定をスキップ"
        
        install_name_tool -change \
            "${HOMEBREW_DIR}/opt/ffmpeg/lib/libavcodec.62.dylib" \
            "@executable_path/../Frameworks/libavcodec.62.dylib" \
            "${H263_PLUGIN}" 2>/dev/null || true
        
        install_name_tool -change \
            "${HOMEBREW_DIR}/opt/ffmpeg/lib/libavutil.60.dylib" \
            "@executable_path/../Frameworks/libavutil.60.dylib" \
            "${H263_PLUGIN}" 2>/dev/null || true
        
        # ローカルビルドの依存関係も修正
        install_name_tool -change \
            "/usr/local/lib/libavcodec.62.dylib" \
            "@executable_path/../Frameworks/libavcodec.62.dylib" \
            "${H263_PLUGIN}" 2>/dev/null || true
        
        install_name_tool -change \
            "/usr/local/lib/libavutil.60.dylib" \
            "@executable_path/../Frameworks/libavutil.60.dylib" \
            "${H263_PLUGIN}" 2>/dev/null || true
    fi
    
    # --- H.261-vic プラグインの依存関係を修正 ---
    log_info "  H.261-vic プラグインの依存関係を修正..."
    
    local H261_PLUGIN="${PLUGINS}/video/H.261-vic/h261-vic_video_pwplugin.dylib"
    if [ -f "${H261_PLUGIN}" ]; then
        install_name_tool -id "@executable_path/../Resources/plugins/video/H.261-vic/h261-vic_video_pwplugin.dylib" \
            "${H261_PLUGIN}" 2>/dev/null || log_warn "  H.261プラグインID設定をスキップ"
    fi
    
    # --- FFmpeg 内部依存の修正 ---
    log_info "  FFmpeg ライブラリの内部依存を修正..."
    
    # libavcodec -> libavutil, libswresample
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/ffmpeg/lib/libavutil.60.dylib" \
        "@executable_path/../Frameworks/libavutil.60.dylib" \
        "${FRAMEWORKS}/libavcodec.62.dylib"
    
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/ffmpeg/lib/libswresample.6.dylib" \
        "@executable_path/../Frameworks/libswresample.6.dylib" \
        "${FRAMEWORKS}/libavcodec.62.dylib"
    
    # libswscale -> libavutil
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/ffmpeg/lib/libavutil.60.dylib" \
        "@executable_path/../Frameworks/libavutil.60.dylib" \
        "${FRAMEWORKS}/libswscale.9.dylib"
    
    # libswresample -> libavutil
    install_name_tool -change \
        "${HOMEBREW_DIR}/opt/ffmpeg/lib/libavutil.60.dylib" \
        "@executable_path/../Frameworks/libavutil.60.dylib" \
        "${FRAMEWORKS}/libswresample.6.dylib"
    
    # --- FFmpeg 追加依存ライブラリのパス修正 ---
    log_info "  FFmpeg 追加依存ライブラリのパスを修正..."
    
    # すべてのdylibのIDを設定し、Homebrew参照を修正
    for lib in "${FRAMEWORKS}"/*.dylib; do
        local libname=$(basename "$lib")
        # IDを設定
        install_name_tool -id "@executable_path/../Frameworks/${libname}" "$lib" 2>/dev/null || true
    done
    
    # Homebrewパスを@executable_pathに一括置換
    local HOMEBREW_LIBS=(
        "libvpx.11.dylib"
        "libwebpmux.3.dylib"
        "libwebp.7.dylib"
        "libsharpyuv.0.dylib"
        "liblzma.5.dylib"
        "libaribb24.0.dylib"
        "libdav1d.7.dylib"
        "libopencore-amrwb.0.dylib"
        "libopencore-amrnb.0.dylib"
        "libsnappy.1.dylib"
        "libaom.3.dylib"
        "libvmaf.3.dylib"
        "libjxl.0.11.dylib"
        "libjxl_threads.0.11.dylib"
        "libjxl_cms.0.11.dylib"
        "libmp3lame.0.dylib"
        "libopenjp2.7.dylib"
        "libopus.0.dylib"
        "librav1e.0.8.dylib"
        "libspeex.1.dylib"
        "libSvtAv1Enc.3.dylib"
        "libtheoraenc.2.dylib"
        "libtheoradec.2.dylib"
        "libogg.0.dylib"
        "libvorbis.0.dylib"
        "libvorbisenc.2.dylib"
        "libx265.215.dylib"
        "libsoxr.0.dylib"
        "libX11.6.dylib"
        "libxcb.1.dylib"
        "libXau.6.dylib"
        "libXdmcp.6.dylib"
        "libbrotlidec.1.dylib"
        "libbrotlienc.1.dylib"
        "libbrotlicommon.1.dylib"
        "libhwy.1.dylib"
        "libhwy_contrib.1.dylib"
        "liblcms2.2.dylib"
        "libjpeg.8.dylib"
        "libpng16.16.dylib"
    )
    
    # 各ライブラリの依存関係を修正
    for target in "${FRAMEWORKS}"/*.dylib "${FRAMEWORKS}"/Qt*.framework/Versions/A/Qt*; do
        [ -f "$target" ] || continue
        for hblib in "${HOMEBREW_LIBS[@]}"; do
            # 様々なHomebrewパスパターンに対応
            install_name_tool -change "/opt/homebrew/opt/${hblib%.*}*/lib/${hblib}" \
                "@executable_path/../Frameworks/${hblib}" "$target" 2>/dev/null || true
            install_name_tool -change "/opt/homebrew/Cellar/*/${hblib}" \
                "@executable_path/../Frameworks/${hblib}" "$target" 2>/dev/null || true
        done
    done
    
    # 特定のHomebrewパスを直接修正（よく使われるパターン）
    for target in "${FRAMEWORKS}"/*.dylib; do
        [ -f "$target" ] || continue
        # FFmpegのCellarパス
        install_name_tool -change "/opt/homebrew/Cellar/ffmpeg/HEAD-fc3893f/lib/libavutil.60.dylib" \
            "@executable_path/../Frameworks/libavutil.60.dylib" "$target" 2>/dev/null || true
        install_name_tool -change "/opt/homebrew/Cellar/ffmpeg/HEAD-fc3893f/lib/libswresample.6.dylib" \
            "@executable_path/../Frameworks/libswresample.6.dylib" "$target" 2>/dev/null || true
        install_name_tool -change "/opt/homebrew/Cellar/openssl@3/3.6.0/lib/libcrypto.3.dylib" \
            "@executable_path/../Frameworks/libcrypto.3.dylib" "$target" 2>/dev/null || true
        # 一般的なoptパス
        for hblib in "${HOMEBREW_LIBS[@]}"; do
            local pkgname="${hblib%%.*}"
            pkgname="${pkgname#lib}"
            install_name_tool -change "/opt/homebrew/opt/${pkgname}/lib/${hblib}" \
                "@executable_path/../Frameworks/${hblib}" "$target" 2>/dev/null || true
            install_name_tool -change "/opt/homebrew/opt/lib${pkgname}/lib/${hblib}" \
                "@executable_path/../Frameworks/${hblib}" "$target" 2>/dev/null || true
        done
    done
    
    # 最終クリーンアップ: otoolで残りのHomebrewパスを検出して修正
    log_info "  残りのHomebrewパスを自動検出して修正..."
    for target in "${FRAMEWORKS}"/*.dylib; do
        [ -f "$target" ] || continue
        otool -L "$target" 2>/dev/null | grep "/opt/homebrew" | awk '{print $1}' | while read oldpath; do
            libname=$(basename "$oldpath")
            install_name_tool -change "$oldpath" "@executable_path/../Frameworks/$libname" "$target" 2>/dev/null || true
        done
    done

    # --- vidinput プラグインの修正 ---
    log_info "  vidinput プラグインの依存関係を修正..."
    
    install_name_tool -id "@executable_path/../Resources/plugins/vidinput/vidinput_macos_pwplugin.dylib" \
        "${PLUGINS}/vidinput/vidinput_macos_pwplugin.dylib"
    
    install_name_tool -change \
        "/Users/example/ptlib/lib_Darwin_aarch64/libpt.2.10.9.dylib" \
        "@executable_path/../Frameworks/libpt.dylib" \
        "${PLUGINS}/vidinput/vidinput_macos_pwplugin.dylib"
    
    # --- sound_portaudio プラグインの修正 ---
    log_info "  sound_portaudio プラグインの依存関係を修正..."
    
    install_name_tool -id "@executable_path/../Resources/plugins/sound/portaudio_pwplugin.dylib" \
        "${PLUGINS}/sound/portaudio_pwplugin.dylib"
    
    install_name_tool -change \
        "/opt/homebrew/opt/portaudio/lib/libportaudio.2.dylib" \
        "@executable_path/../Frameworks/libportaudio.2.dylib" \
        "${PLUGINS}/sound/portaudio_pwplugin.dylib"
    
    install_name_tool -change \
        "/Users/example/ptlib/lib_Darwin_aarch64/libpt.2.10.9.dylib" \
        "@executable_path/../Frameworks/libpt.dylib" \
        "${PLUGINS}/sound/portaudio_pwplugin.dylib"
    
    # --- Qt6 プラットフォームプラグインの修正 ---
    log_info "  Qt6 プラットフォームプラグインの依存関係を修正..."
    
    local QT_PLUGINS="${APP_BUNDLE}/Contents/PlugIns"
    
    if [ -f "${QT_PLUGINS}/platforms/libqcocoa.dylib" ]; then
        # libqcocoa のID設定
        install_name_tool -id "@executable_path/../PlugIns/platforms/libqcocoa.dylib" \
            "${QT_PLUGINS}/platforms/libqcocoa.dylib"
        
        # QtCore への依存関係修正
        install_name_tool -change "@rpath/QtCore.framework/Versions/A/QtCore" \
            "@executable_path/../Frameworks/QtCore.framework/Versions/A/QtCore" \
            "${QT_PLUGINS}/platforms/libqcocoa.dylib"
        
        # QtGui への依存関係修正
        install_name_tool -change "@rpath/QtGui.framework/Versions/A/QtGui" \
            "@executable_path/../Frameworks/QtGui.framework/Versions/A/QtGui" \
            "${QT_PLUGINS}/platforms/libqcocoa.dylib"
    fi
    
    # --- qt.conf の作成 ---
    log_info "  qt.conf を作成中..."
    cat > "${APP_BUNDLE}/Contents/Resources/qt.conf" << EOF
[Paths]
Plugins = PlugIns
EOF
    
    log_success "ライブラリパスの書き換えが完了しました"
}

# ===== 起動スクリプトの作成 =====
create_launcher_script() {
    log_info "起動スクリプトを作成中..."
    
    # 元の実行ファイルをリネーム
    mv "${APP_BUNDLE}/Contents/MacOS/h323askw" "${APP_BUNDLE}/Contents/MacOS/h323askw_bin"
    
    # ラッパースクリプト作成
    cat > "${APP_BUNDLE}/Contents/MacOS/h323askw" << 'EOF'
#!/bin/bash
# H323ASKW Launcher Script

# スクリプトのディレクトリ（MacOS）に移動
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# アプリケーションのディレクトリを取得
APP_DIR="$(cd ".." && pwd)"
RESOURCES_DIR="${APP_DIR}/Resources"
FRAMEWORKS_DIR="${APP_DIR}/Frameworks"

# ライブラリパスを設定（プラグインがFrameworksのライブラリを見つけられるように）
export DYLD_LIBRARY_PATH="${FRAMEWORKS_DIR}:${DYLD_LIBRARY_PATH}"
export DYLD_FALLBACK_LIBRARY_PATH="${FRAMEWORKS_DIR}:/usr/lib"

# プラグインパスを環境変数に設定
# PTLib用（vidinput, sound）
export PTLIB_PLUGIN_DIR="${RESOURCES_DIR}/plugins/vidinput:${RESOURCES_DIR}/plugins/sound"
export PWLIB_PLUGIN_DIR="${RESOURCES_DIR}/plugins/vidinput:${RESOURCES_DIR}/plugins/sound"

# H323Plus用（video, audio コーデック）
export H323_PLUGIN_DIR="${RESOURCES_DIR}/plugins"
export PTLIBPLUGINDIR="${RESOURCES_DIR}/plugins"

# 実行
exec "./h323askw_bin" "$@"
EOF
    
    chmod +x "${APP_BUNDLE}/Contents/MacOS/h323askw"
    
    log_success "起動スクリプトを作成しました"
}

# ===== 検証 =====
verify_bundle() {
    log_info "App Bundle を検証中..."
    
    local has_error=0
    
    # 実行ファイルの依存関係をチェック
    log_info "  実行ファイルの依存関係を確認..."
    if otool -L "${APP_BUNDLE}/Contents/MacOS/h323askw_bin" | grep -q "/Users/example"; then
        log_error "  絶対パスがまだ残っています！"
        otool -L "${APP_BUNDLE}/Contents/MacOS/h323askw_bin" | grep "/Users/example"
        has_error=1
    fi
    
    if otool -L "${APP_BUNDLE}/Contents/MacOS/h323askw_bin" | grep -q "/opt/homebrew"; then
        log_error "  Homebrewパスがまだ残っています！"
        otool -L "${APP_BUNDLE}/Contents/MacOS/h323askw_bin" | grep "/opt/homebrew"
        has_error=1
    fi
    
    # ライブラリの依存関係をチェック
    log_info "  ライブラリの依存関係を確認..."
    for lib in "${APP_BUNDLE}/Contents/Frameworks/"*.dylib; do
        if otool -L "$lib" | grep -q "/Users/example\|/opt/homebrew"; then
            log_warn "  $(basename $lib) に問題があるかもしれません"
            has_error=1
        fi
    done
    
    if [ $has_error -eq 0 ]; then
        log_success "App Bundle の検証が完了しました (問題なし)"
    else
        log_warn "いくつかの問題が見つかりました。確認してください。"
    fi
}

# ===== サイズ情報 =====
show_bundle_size() {
    log_info "App Bundle サイズ情報:"
    echo ""
    du -sh "${APP_BUNDLE}"
    echo ""
    du -sh "${APP_BUNDLE}/Contents/MacOS"
    du -sh "${APP_BUNDLE}/Contents/Frameworks"
    du -sh "${APP_BUNDLE}/Contents/Resources/plugins"
}

# ===== コード署名 =====
sign_bundle() {
    log_info "App Bundle にコード署名中..."
    
    # まず、すべてのdylibに署名
    log_info "  Frameworksのライブラリに署名中..."
    for lib in "${APP_BUNDLE}/Contents/Frameworks/"*.dylib; do
        if [ -f "$lib" ]; then
            codesign --force --sign - --timestamp=none "$lib" 2>/dev/null || {
                log_warn "  署名スキップ: $(basename $lib)"
            }
        fi
    done
    
    # Qt フレームワークに署名 (内側から外側へ)
    log_info "  Qt フレームワークに署名中..."
    for fw in QtCore QtDBus QtGui QtWidgets; do
        local fw_path="${APP_BUNDLE}/Contents/Frameworks/${fw}.framework"
        if [ -d "$fw_path" ]; then
            # まずバイナリに署名
            if [ -f "${fw_path}/Versions/A/${fw}" ]; then
                codesign --force --sign - --timestamp=none "${fw_path}/Versions/A/${fw}" 2>/dev/null || true
            fi
            # フレームワーク全体に署名
            codesign --force --sign - --timestamp=none "${fw_path}" 2>/dev/null || true
        fi
    done
    
    # プラグインに署名
    log_info "  プラグインに署名中..."
    if [ -d "${APP_BUNDLE}/Contents/Resources/plugins" ]; then
        find "${APP_BUNDLE}/Contents/Resources/plugins" -name "*.dylib" -exec codesign --force --sign - --timestamp=none {} \; 2>/dev/null || true
    fi
    
    # Qt6 プラットフォームプラグインに署名
    log_info "  Qt6 プラットフォームプラグインに署名中..."
    if [ -d "${APP_BUNDLE}/Contents/PlugIns/platforms" ]; then
        for plugin in "${APP_BUNDLE}/Contents/PlugIns/platforms/"*.dylib; do
            if [ -f "$plugin" ]; then
                codesign --force --sign - --timestamp=none "$plugin" 2>/dev/null || true
            fi
        done
    fi
    
    # 実行ファイルに署名
    log_info "  実行ファイルに署名中..."
    codesign --force --sign - --timestamp=none "${APP_BUNDLE}/Contents/MacOS/h323askw_bin" 2>/dev/null || {
        log_warn "  実行ファイル署名に問題がありました"
    }
    
    # 起動スクリプトに署名（必要に応じて）
    if [ -f "${APP_BUNDLE}/Contents/MacOS/h323askw" ]; then
        codesign --force --sign - --timestamp=none "${APP_BUNDLE}/Contents/MacOS/h323askw" 2>/dev/null || true
    fi
    
    # アプリバンドル全体に署名
    log_info "  App Bundle 全体に署名中..."
    codesign --force --deep --sign - --timestamp=none "${APP_BUNDLE}" 2>/dev/null || {
        log_warn "  App Bundle署名に問題がありました"
    }
    
    # 署名を検証
    log_info "  署名を検証中..."
    if codesign --verify --verbose=2 "${APP_BUNDLE}" 2>&1; then
        log_success "コード署名が完了しました"
    else
        log_warn "署名検証で警告がありましたが、ad-hoc署名は正常です"
    fi
}

# ===== DMG作成 (オプション) =====
create_dmg() {
    log_info "DMG を作成中..."
    
    local DMG_NAME="${APP_NAME}-${APP_VERSION}.dmg"
    local DMG_PATH="${OUTPUT_DIR}/${DMG_NAME}"
    
    # 既存のDMGを削除
    rm -f "${DMG_PATH}"
    
    # DMG作成
    hdiutil create -volname "${APP_NAME}" \
        -srcfolder "${APP_BUNDLE}" \
        -ov -format UDZO \
        "${DMG_PATH}"
    
    log_success "DMG を作成しました: ${DMG_PATH}"
    ls -lh "${DMG_PATH}"
}

# ===== メイン処理 =====
main() {
    echo ""
    echo "=========================================="
    echo "  ${APP_NAME} App Bundle Builder"
    echo "=========================================="
    echo ""
    
    check_prerequisites
    create_bundle_structure
    create_info_plist
    copy_executable
    copy_libraries
    copy_plugins
    fix_library_paths
    create_launcher_script
    verify_bundle
    sign_bundle
    show_bundle_size
    
    echo ""
    log_success "App Bundle が作成されました: ${APP_BUNDLE}"
    echo ""
    
    # DMG作成（デフォルトで実行）
    create_dmg
    
    echo ""
    log_info "完了しました！"
}

# 実行
main "$@"
