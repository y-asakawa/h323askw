#!/bin/bash
#
# H323ASKW App Bundle 作成スクリプト
# macOS用のローカル検証向けアプリケーションバンドルを作成します
# Copyright (c) 2025-2026 Yoshifumi Asakawa
# SPDX-License-Identifier: MPL-1.0
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
H323PLUS_DIR="${H323PLUS_DIR:-${SCRIPT_DIR}/../h323plus}"
H323PLUS_PLUGINS_DIR="${H323PLUS_PLUGINS_DIR:-${H323PLUS_DIR}/plugins/video}"
PTLIB_DIR="${PTLIB_DIR:-${SCRIPT_DIR}/../ptlib}"

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
OUTPUT_DIR="${OUTPUT_DIR:-${CALLGEN_DIR}/dist}"
APP_BUNDLE="${OUTPUT_DIR}/${APP_NAME}.app"
APP_RESOURCES="${APP_BUNDLE}/Contents/Resources"
APP_ICON_NAME="${APP_NAME}.icns"
ICON_ASSETS_DIR="${SCRIPT_DIR}/H323CRT_transparent_iconset_and_png"
ICONSET_PATH="${ICON_ASSETS_DIR}/H323CRT.iconset"
ICON_PNG_PATH="${ICON_ASSETS_DIR}/H323CRT_app_icon_1024.png"

# 動的に検出したHomebrewライブラリのパスとファイル名（fix_library_pathsでも使う）
OPENSSL_SSL_LIB=""
OPENSSL_SSL_NAME=""
OPENSSL_CRYPTO_LIB=""
OPENSSL_CRYPTO_NAME=""
AVCODEC_LIB=""
AVCODEC_NAME=""
AVFORMAT_LIB=""
AVFORMAT_NAME=""
AVUTIL_LIB=""
AVUTIL_NAME=""
SWRESAMPLE_LIB=""
SWRESAMPLE_NAME=""
SWSCALE_LIB=""
SWSCALE_NAME=""
X264_LIB=""
X264_NAME=""
PORTAUDIO_LIB=""
PORTAUDIO_NAME=""
SPEEXDSP_LIB=""
SPEEXDSP_NAME=""

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

# 指定ディレクトリ内でベース名をもとに最新バージョンのdylibを拾う
find_latest_dylib() {
    local dir="$1"
    local base="$2"
    local found=$(ls -1 "${dir}/${base}."*.dylib 2>/dev/null | sort -V | tail -1)
    echo "$found"
}

# フルパスのdylibから「libxxxx.<major>.dylib」の形の名前を返す
major_dylib_basename() {
    local path="$1"
    local base=$(basename "$path")
    # 例: libogg.0.8.6.dylib → libogg.0.dylib, libogg.0.8.dylib, libogg.0.8.6.dylib
    local out=()
    # 例: libogg.0.8.6.dylib → libogg.0.dylib, libogg.0.8.dylib, libogg.0.8.6.dylib
    if [[ "$base" =~ ^([^.]+)\.([0-9]+)\.([0-9]+)\.([0-9]+)\.dylib$ ]]; then
        out+=("${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.dylib")
        out+=("${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.${BASH_REMATCH[3]}.dylib")
        out+=("${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.${BASH_REMATCH[3]}.${BASH_REMATCH[4]}.dylib")
    elif [[ "$base" =~ ^([^.]+)\.([0-9]+)\.([0-9]+)\.dylib$ ]]; then
        out+=("${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.dylib")
        out+=("${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.${BASH_REMATCH[3]}.dylib")
    elif [[ "$base" =~ ^([^.]+)\.([0-9]+)\.dylib$ ]]; then
        out+=("${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.dylib")
    else
        out+=("$base")
    fi
    for o in "${out[@]}"; do
        echo "$o"
    done
}

# major_dylib_basename() の先頭要素のみを返す
primary_dylib_basename() {
    local path="$1"
    major_dylib_basename "$path" | head -1
}

# otoolで見える既存パスをパターン検索し、見つかれば置換
change_dep_if_present() {
    local binary="$1"
    local pattern="$2"
    local newpath="$3"
    local oldpath
    oldpath=$(otool -L "$binary" 2>/dev/null | awk 'NR>1 {print $1}' | grep -E "$pattern" | head -1 || true)
    if [ -n "$oldpath" ]; then
        install_name_tool -change "$oldpath" "$newpath" "$binary" 2>/dev/null || true
    fi
}

# まず希望パスへ置換し、失敗時は短いパスへフォールバックする
change_dep_with_fallback() {
    local binary="$1"
    local pattern="$2"
    local preferred="$3"
    local fallback="$4"
    local oldpath
    local oldbase
    local shorttarget

    oldpath=$(otool -L "$binary" 2>/dev/null | awk 'NR>1 {print $1}' | grep -E "$pattern" | head -1 || true)
    [ -n "$oldpath" ] || return 0

    if install_name_tool -change "$oldpath" "$preferred" "$binary" 2>/dev/null; then
        return 0
    fi

    [ -n "$fallback" ] || return 1

    if install_name_tool -change "$oldpath" "$fallback" "$binary" 2>/dev/null; then
        return 0
    fi

    oldbase=$(basename "$oldpath")
    if install_name_tool -change "$oldbase" "$fallback" "$binary" 2>/dev/null; then
        return 0
    fi

    shorttarget=$(basename "$fallback")
    if [ -n "$shorttarget" ] && [ "$shorttarget" != "$fallback" ]; then
        if install_name_tool -change "$oldpath" "$shorttarget" "$binary" 2>/dev/null; then
            return 0
        fi
        if install_name_tool -change "$oldbase" "$shorttarget" "$binary" 2>/dev/null; then
            return 0
        fi
    fi

    shorttarget=$(basename "$preferred")
    if [ -n "$shorttarget" ] && [ "$shorttarget" != "$preferred" ]; then
        if install_name_tool -change "$oldpath" "$shorttarget" "$binary" 2>/dev/null; then
            return 0
        fi
        if install_name_tool -change "$oldbase" "$shorttarget" "$binary" 2>/dev/null; then
            return 0
        fi
    fi

    return 1
}

# ===== 必要なファイルの存在確認 =====
build_executable() {
    log_info "App Bundle 用の最適化共有バイナリをビルド中..."
    if ! make -C "${CALLGEN_DIR}" optshared; then
        log_error "実行ファイルのビルドに失敗しました (make optshared)"
        return 1
    fi
}

check_prerequisites() {
    log_info "必要なファイルを確認中..."
    log_info "  CALLGEN_DIR: ${CALLGEN_DIR}"
    log_info "  H323PLUS_DIR: ${H323PLUS_DIR}"
    log_info "  H323PLUS_PLUGINS_DIR: ${H323PLUS_PLUGINS_DIR}"
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
        "speexdsp/lib/libspeexdsp.*.dylib"
    )
    
    for pattern in "${homebrew_libs[@]}"; do
        local found=$(find_dylib "$(basename "$pattern")" "${HOMEBREW_DIR}/opt/$(dirname "$pattern")")
        if [ -z "$found" ]; then
            log_warn "Homebrewライブラリが見つかりません: ${pattern}"
            # Don't fail for optional libs, but warn
        fi
    done
    
    # Qt6 フレームワーク (Homebrew lib ディレクトリに配置される)
    for fw in "QtCore" "QtGui" "QtWidgets" "QtDBus"; do
        if [ ! -d "${HOMEBREW_DIR}/lib/${fw}.framework" ] && [ ! -d "${HOMEBREW_DIR}/opt/qt6/lib/${fw}.framework" ]; then
            log_error "Qt6フレームワークが見つかりません: ${fw}"
            log_error "  確認パス1: ${HOMEBREW_DIR}/lib/${fw}.framework"
            log_error "  確認パス2: ${HOMEBREW_DIR}/opt/qt6/lib/${fw}.framework"
            missing=1
        fi
    done
    
    # プラグイン（h323plus-pluginsディレクトリから）
    if [ ! -f "${H323PLUS_PLUGINS_DIR}/H.264/h264_video_pwplugin.dylib" ]; then
        log_error "H.264 プラグインが見つかりません: ${H323PLUS_PLUGINS_DIR}/H.264/h264_video_pwplugin.dylib"
        missing=1
    else
        log_info "  Found H.264 plugin: ${H323PLUS_PLUGINS_DIR}/H.264/h264_video_pwplugin.dylib"
    fi

    # These macOS PTLib plugins are not part of the upstream PTLib checkout.
    if [ ! -f "${PTLIB_DIR}/plugins/vidinput_macos/vidinput_macos_pwplugin.dylib" ]; then
        log_error "macOS カメラプラグインが見つかりません: ${PTLIB_DIR}/plugins/vidinput_macos/vidinput_macos_pwplugin.dylib"
        missing=1
    fi
    if [ ! -f "${PTLIB_DIR}/lib_Darwin_aarch64/device/sound/portaudio_pwplugin.dylib" ]; then
        log_error "PortAudio プラグインが見つかりません: ${PTLIB_DIR}/lib_Darwin_aarch64/device/sound/portaudio_pwplugin.dylib"
        missing=1
    fi
    
    # アイコン素材
    if [ ! -d "${ICONSET_PATH}" ] && [ ! -f "${ICON_PNG_PATH}" ]; then
        log_error "アプリアイコン素材が見つかりません: ${ICON_ASSETS_DIR}"
        missing=1
    fi
    if ! command -v iconutil >/dev/null 2>&1 && ! command -v sips >/dev/null 2>&1; then
        log_error "iconutil または sips が見つかりません（アイコン生成に必要）"
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
    mkdir -p "${APP_RESOURCES}"
    mkdir -p "${APP_RESOURCES}/plugins/video"
    mkdir -p "${APP_RESOURCES}/plugins/vidinput"
    mkdir -p "${APP_RESOURCES}/plugins/audio"
    mkdir -p "${APP_RESOURCES}/plugins/sound"
    
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
    <key>CFBundleIconFile</key>
    <string>${APP_ICON_NAME}</string>
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
    <key>NSLocalNetworkUsageDescription</key>
    <string>H323ASKW needs access to the local network to connect to H.323 endpoints on your LAN.</string>
    <key>NSBonjourServices</key>
    <array>
        <string>_perm._tcp</string>
    </array>
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

# ===== アイコンの準備 =====
prepare_app_icon() {
    log_info "アプリアイコンを準備中..."
    
    local icon_dest="${APP_RESOURCES}/${APP_ICON_NAME}"
    mkdir -p "${APP_RESOURCES}"
    
    if [ -d "${ICONSET_PATH}" ] && command -v iconutil >/dev/null 2>&1; then
        log_info "  iconset から icns を生成: ${ICONSET_PATH}"
        if iconutil -c icns "${ICONSET_PATH}" -o "${icon_dest}"; then
            log_success "アプリアイコンを作成しました: ${icon_dest}"
            return
        fi
        log_warn "  iconutil での生成に失敗しました。代替手段を試みます。"
    fi
    
    if [ -f "${ICON_PNG_PATH}" ] && command -v sips >/dev/null 2>&1; then
        log_info "  PNG から icns を生成: ${ICON_PNG_PATH}"
        if sips -s format icns "${ICON_PNG_PATH}" --out "${icon_dest}" >/dev/null; then
            log_success "アプリアイコンを作成しました: ${icon_dest}"
            return
        fi
        log_warn "  sips でのアイコン生成に失敗しました。"
    fi
    
    log_warn "アイコン生成に失敗しました。既存のアイコンがない場合、Finder でアイコンが表示されません。"
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
    
    # OpenSSL (最新を取得)
    OPENSSL_SSL_LIB=$(find_latest_dylib "${HOMEBREW_DIR}/opt/openssl@3/lib" "libssl")
    OPENSSL_CRYPTO_LIB=$(find_latest_dylib "${HOMEBREW_DIR}/opt/openssl@3/lib" "libcrypto")
    if [ -n "$OPENSSL_SSL_LIB" ]; then
        OPENSSL_SSL_NAME=$(primary_dylib_basename "$OPENSSL_SSL_LIB")
        cp "$OPENSSL_SSL_LIB" "${FRAMEWORKS}/${OPENSSL_SSL_NAME}"
    fi
    if [ -n "$OPENSSL_CRYPTO_LIB" ]; then
        OPENSSL_CRYPTO_NAME=$(primary_dylib_basename "$OPENSSL_CRYPTO_LIB")
        cp "$OPENSSL_CRYPTO_LIB" "${FRAMEWORKS}/${OPENSSL_CRYPTO_NAME}"
    fi
    
    # Qt6 フレームワーク (Homebrew lib ディレクトリに配置される)
    log_info "  Qt6 フレームワークをコピー中..."
    for fw in QtCore QtGui QtWidgets QtDBus; do
        local fw_found=false
        # 優先順位1: /opt/homebrew/lib/
        if [ -d "${HOMEBREW_DIR}/lib/${fw}.framework" ]; then
            log_info "    ${fw}.framework をコピー (from ${HOMEBREW_DIR}/lib/)..."
            ditto "${HOMEBREW_DIR}/lib/${fw}.framework" "${FRAMEWORKS}/${fw}.framework"
            fw_found=true
        # 優先順位2: /opt/homebrew/opt/qt6/lib/
        elif [ -d "${HOMEBREW_DIR}/opt/qt6/lib/${fw}.framework" ]; then
            log_info "    ${fw}.framework をコピー (from ${HOMEBREW_DIR}/opt/qt6/lib/)..."
            ditto "${HOMEBREW_DIR}/opt/qt6/lib/${fw}.framework" "${FRAMEWORKS}/${fw}.framework"
            fw_found=true
        # 優先順位3: /opt/homebrew/opt/qtbase/lib/ (古い配置)
        elif [ -d "${HOMEBREW_DIR}/opt/qtbase/lib/${fw}.framework" ]; then
            log_info "    ${fw}.framework をコピー (from ${HOMEBREW_DIR}/opt/qtbase/lib/)..."
            ditto "${HOMEBREW_DIR}/opt/qtbase/lib/${fw}.framework" "${FRAMEWORKS}/${fw}.framework"
            fw_found=true
        fi
        
        if [ "$fw_found" = true ]; then
            # 不要なファイルを削除してサイズ削減
            rm -rf "${FRAMEWORKS}/${fw}.framework/Headers"
            rm -rf "${FRAMEWORKS}/${fw}.framework/Versions/A/Headers"
            rm -rf "${FRAMEWORKS}/${fw}.framework/Versions/Current/Headers"
        else
            log_warn "  ${fw}.framework not found - skipping"
        fi
    done

    # Qt Framework が参照する Homebrew dylib を再帰的に収集
    # (配布先での ICU/GLib 等の欠落を防ぐ)
    log_info "  Qt6 追加依存ライブラリを収集中..."
    for pass in 1 2 3 4; do
        local copied_this_pass=0

        # Qt binaries + 既にコピー済みdylib を走査対象にする
        for target in "${FRAMEWORKS}"/Qt*.framework/Versions/A/Qt* "${FRAMEWORKS}"/*.dylib; do
            [ -f "$target" ] || continue

            while IFS= read -r dep; do
                [ -n "$dep" ] || continue
                case "$dep" in
                    ${HOMEBREW_DIR}/*)
                        if [[ "$dep" == *.dylib ]]; then
                            local dep_base
                            dep_base="$(basename "$dep")"
                            if [ ! -f "${FRAMEWORKS}/${dep_base}" ] && [ -f "$dep" ]; then
                                cp "$dep" "${FRAMEWORKS}/${dep_base}"
                                copied_this_pass=1
                                log_info "    copied Qt dep: ${dep_base}"
                            fi
                        fi
                        ;;
                esac
            done < <(otool -L "$target" 2>/dev/null | awk 'NR>1 {print $1}')
        done

        if [ "$copied_this_pass" -eq 0 ]; then
            break
        fi
    done
    
    # FFmpeg (実体ファイルをコピー・バージョン自動検出)
    AVCODEC_LIB=$(find_latest_dylib "${HOMEBREW_DIR}/opt/ffmpeg/lib" "libavcodec")
    AVFORMAT_LIB=$(find_latest_dylib "${HOMEBREW_DIR}/opt/ffmpeg/lib" "libavformat")
    AVUTIL_LIB=$(find_latest_dylib "${HOMEBREW_DIR}/opt/ffmpeg/lib" "libavutil")
    SWRESAMPLE_LIB=$(find_latest_dylib "${HOMEBREW_DIR}/opt/ffmpeg/lib" "libswresample")
    SWSCALE_LIB=$(find_latest_dylib "${HOMEBREW_DIR}/opt/ffmpeg/lib" "libswscale")
    if [ -n "$AVCODEC_LIB" ]; then
        AVCODEC_NAME=$(primary_dylib_basename "$AVCODEC_LIB")
        cp "$AVCODEC_LIB" "${FRAMEWORKS}/${AVCODEC_NAME}"
    fi
    if [ -n "$AVFORMAT_LIB" ]; then
        AVFORMAT_NAME=$(primary_dylib_basename "$AVFORMAT_LIB")
        cp "$AVFORMAT_LIB" "${FRAMEWORKS}/${AVFORMAT_NAME}"
    fi
    if [ -n "$AVUTIL_LIB" ]; then
        AVUTIL_NAME=$(primary_dylib_basename "$AVUTIL_LIB")
        cp "$AVUTIL_LIB" "${FRAMEWORKS}/${AVUTIL_NAME}"
    fi
    if [ -n "$SWRESAMPLE_LIB" ]; then
        SWRESAMPLE_NAME=$(primary_dylib_basename "$SWRESAMPLE_LIB")
        cp "$SWRESAMPLE_LIB" "${FRAMEWORKS}/${SWRESAMPLE_NAME}"
    fi
    if [ -n "$SWSCALE_LIB" ]; then
        SWSCALE_NAME=$(primary_dylib_basename "$SWSCALE_LIB")
        cp "$SWSCALE_LIB" "${FRAMEWORKS}/${SWSCALE_NAME}"
    fi
    
    # SpeexDSP (AEC/NS用)
    SPEEXDSP_LIB=$(find_latest_dylib "${HOMEBREW_DIR}/opt/speexdsp/lib" "libspeexdsp")
    if [ -n "$SPEEXDSP_LIB" ]; then
        SPEEXDSP_NAME=$(primary_dylib_basename "$SPEEXDSP_LIB")
        cp "$SPEEXDSP_LIB" "${FRAMEWORKS}/${SPEEXDSP_NAME}"
    else
        log_warn "SpeexDSP (libspeexdsp) が見つかりませんでした。AEC/NS を使う場合は Homebrew でインストールしてください。"
    fi
    
    # x264
    X264_LIB=$(find_latest_dylib "${HOMEBREW_DIR}/opt/x264/lib" "libx264")
    if [ -n "$X264_LIB" ]; then
        X264_NAME=$(primary_dylib_basename "$X264_LIB")
        cp "$X264_LIB" "${FRAMEWORKS}/${X264_NAME}"
        # Provide an @executable_path alias so plugins can resolve x264 without absolute Homebrew paths
        ln -sf "../Frameworks/${X264_NAME}" "${APP_BUNDLE}/Contents/MacOS/${X264_NAME}"
    fi
    
    # PortAudio
    PORTAUDIO_LIB=$(find_latest_dylib "${HOMEBREW_DIR}/opt/portaudio/lib" "libportaudio")
    if [ -n "$PORTAUDIO_LIB" ]; then
        PORTAUDIO_NAME=$(primary_dylib_basename "$PORTAUDIO_LIB")
        cp "$PORTAUDIO_LIB" "${FRAMEWORKS}/${PORTAUDIO_NAME}"
    fi
    
    # FFmpeg 追加依存ライブラリ (配布用に必要)
    log_info "  FFmpeg 依存ライブラリをコピー中..."
    
    # 主要なFFmpeg依存ライブラリをコピー（バージョン自動検出）
    local FFMPEG_DEPS=(
        "libvpx:libvpx"
        "webp:libwebpmux"
        "webp:libwebp"
        "webp:libsharpyuv"
        "xz:liblzma"
        "aribb24:libaribb24"
        "dav1d:libdav1d"
        "opencore-amr:libopencore-amrwb"
        "opencore-amr:libopencore-amrnb"
        "snappy:libsnappy"
        "aom:libaom"
        "libvmaf:libvmaf"
        "jpeg-xl:libjxl"
        "jpeg-xl:libjxl_threads"
        "jpeg-xl:libjxl_cms"
        "lame:libmp3lame"
        "mpg123:libmpg123"
        "openjpeg:libopenjp2"
        "opus:libopus"
        "rav1e:librav1e"
        "speex:libspeex"
        "svt-av1:libSvtAv1Enc"
        "theora:libtheoraenc"
        "theora:libtheoradec"
        "libogg:libogg"
        "libvorbis:libvorbis"
        "libvorbis:libvorbisenc"
        "x265:libx265"
        "libsoxr:libsoxr"
        "libx11:libX11"
        "libxcb:libxcb"
        "libxau:libXau"
        "libxdmcp:libXdmcp"
        "brotli:libbrotlidec"
        "brotli:libbrotlienc"
        "brotli:libbrotlicommon"
        "highway:libhwy"
        "highway:libhwy_contrib"
        "little-cms2:liblcms2"
        "jpeg-turbo:libjpeg"
        "openssl@3:libcrypto"
        "libpng:libpng16"
    )
    
    for dep in "${FFMPEG_DEPS[@]}"; do
        local pkg="${dep%%:*}"
        local base="${dep#*:}"
        local src=$(find_latest_dylib "${HOMEBREW_DIR}/opt/${pkg}/lib" "${base}")
        if [ -n "$src" ] && [ -f "$src" ]; then
            for name in $(major_dylib_basename "$src"); do
                cp "$src" "${FRAMEWORKS}/${name}" 2>/dev/null || true
            done
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
    
    # 複数のパスを優先順位付けて確認
    local QCOCOA_FOUND=false
    for qt_plugin_path in \
        "${HOMEBREW_DIR}/share/qt/plugins/platforms/libqcocoa.dylib" \
        "${HOMEBREW_DIR}/opt/qt6/share/qt/plugins/platforms/libqcocoa.dylib" \
        "${HOMEBREW_DIR}/opt/qt/share/qt/plugins/platforms/libqcocoa.dylib" \
        "${HOMEBREW_DIR}/opt/qtbase/share/qt/plugins/platforms/libqcocoa.dylib" \
        "${HOMEBREW_DIR}/lib/qt6/plugins/platforms/libqcocoa.dylib"
    do
        if [ -f "$qt_plugin_path" ]; then
            log_info "    libqcocoa.dylib をコピー (from $(dirname $qt_plugin_path))..."
            cp "$qt_plugin_path" "${QT_PLUGINS}/platforms/"
            QCOCOA_FOUND=true
            break
        fi
    done
    
    if [ "$QCOCOA_FOUND" = false ]; then
        log_error "  Qt6 プラットフォームプラグイン libqcocoa.dylib が見つかりません"
        log_error "  確認パス: ${HOMEBREW_DIR}/share/qt/plugins/platforms/"
        exit 1
    fi
    
    # Qt6 スタイルプラグイン（macOS ネイティブの丸みを帯びたウィンドウスタイルに必要）
    log_info "  Qt6 スタイルプラグインをコピー中..."
    mkdir -p "${QT_PLUGINS}/styles"
    for qt_plugin_path in \
        "${HOMEBREW_DIR}/share/qt/plugins/styles/libqmacstyle.dylib" \
        "${HOMEBREW_DIR}/opt/qt6/share/qt/plugins/styles/libqmacstyle.dylib" \
        "${HOMEBREW_DIR}/opt/qt/share/qt/plugins/styles/libqmacstyle.dylib" \
        "${HOMEBREW_DIR}/opt/qtbase/share/qt/plugins/styles/libqmacstyle.dylib"
    do
        if [ -f "$qt_plugin_path" ]; then
            log_info "    libqmacstyle.dylib をコピー (from $(dirname $qt_plugin_path))..."
            cp "$qt_plugin_path" "${QT_PLUGINS}/styles/"
            break
        fi
    done
    
    # Video codecs (h323plus-pluginsディレクトリ構造に合わせる)
    mkdir -p "${PLUGINS}/video/H.264"
    mkdir -p "${PLUGINS}/video/H.263-ffmpeg"
    mkdir -p "${PLUGINS}/video/H.261-vic"
    
    # H.264 plugin (h323plus-plugins/H.264/から)
    log_info "  H.264 プラグインをコピー中: ${H323PLUS_PLUGINS_DIR}/H.264/h264_video_pwplugin.dylib"
    if [ -f "${H323PLUS_PLUGINS_DIR}/H.264/h264_video_pwplugin.dylib" ]; then
        cp "${H323PLUS_PLUGINS_DIR}/H.264/h264_video_pwplugin.dylib" "${PLUGINS}/video/H.264/"
        log_info "  ✅ H.264 プラグインをコピーしました"
    else
        log_error "  H.264 プラグインが見つかりません"
        exit 1
    fi
    
    # H.263-ffmpeg plugin (FFmpeg-based H.263 codec)
    if [ -f "${H323PLUS_PLUGINS_DIR}/H.263-ffmpeg/h263-ffmpeg_video_pwplugin.dylib" ]; then
        cp "${H323PLUS_PLUGINS_DIR}/H.263-ffmpeg/h263-ffmpeg_video_pwplugin.dylib" "${PLUGINS}/video/H.263-ffmpeg/"
        log_info "  ✅ H.263-ffmpeg プラグインをコピーしました"
    else
        log_warn "  H.263-ffmpeg プラグインが見つかりません: ${H323PLUS_PLUGINS_DIR}/H.263-ffmpeg/"
    fi
    
    # H.261-vic plugin
    if [ -f "${H323PLUS_PLUGINS_DIR}/H.261-vic/h261-vic_video_pwplugin.dylib" ]; then
        cp "${H323PLUS_PLUGINS_DIR}/H.261-vic/h261-vic_video_pwplugin.dylib" "${PLUGINS}/video/H.261-vic/"
        log_info "  ✅ H.261-vic プラグインをコピーしました"
    else
        log_warn "  H.261-vic プラグインが見つかりません: ${H323PLUS_PLUGINS_DIR}/H.261-vic/"
    fi
    
    # Video input
    cp "${PTLIB_DIR}/plugins/vidinput_macos/vidinput_macos_pwplugin.dylib" "${PLUGINS}/vidinput/"
    
    # Audio codecs
    if [ -f "${H323PLUS_DIR}/plugins/audio/G722/g722_audio_pwplugin.dylib" ]; then
        cp "${H323PLUS_DIR}/plugins/audio/G722/g722_audio_pwplugin.dylib" "${PLUGINS}/audio/"
        log_info "  ✅ G.722 audio plugin を同梱しました"
    else
        log_warn "  G.722 audio plugin が見つかりません: ${H323PLUS_DIR}/plugins/audio/G722/g722_audio_pwplugin.dylib"
    fi
    if [ -f "${H323PLUS_DIR}/plugins/audio/G.722.1/g7221_audio_pwplugin.dylib" ]; then
        cp "${H323PLUS_DIR}/plugins/audio/G.722.1/g7221_audio_pwplugin.dylib" "${PLUGINS}/audio/"
    fi
    if [ -f "${H323PLUS_DIR}/plugins/audio/G.722.2/g7222_audio_pwplugin.dylib" ]; then
        cp "${H323PLUS_DIR}/plugins/audio/G.722.2/g7222_audio_pwplugin.dylib" "${PLUGINS}/audio/"
    fi
    
    # Sound device (PortAudio)
    cp "${PTLIB_DIR}/lib_Darwin_aarch64/device/sound/portaudio_pwplugin.dylib" "${PLUGINS}/sound/"
    # --- sound_portaudio プラグインの修正 ---
    log_info "  sound_portaudio プラグインの依存関係を修正..."
    install_name_tool -id "@executable_path/../Resources/plugins/sound/portaudio_pwplugin.dylib" \
        "${PLUGINS}/sound/portaudio_pwplugin.dylib"
    change_dep_if_present "${PLUGINS}/sound/portaudio_pwplugin.dylib" "libportaudio.*\\.dylib" "@executable_path/../Frameworks/${PORTAUDIO_NAME}"
    change_dep_if_present "${PLUGINS}/sound/portaudio_pwplugin.dylib" "libpt.*\\.dylib" "@executable_path/../Frameworks/libpt.dylib"
    if [ -n "$SPEEXDSP_NAME" ]; then
        change_dep_if_present "${PLUGINS}/sound/portaudio_pwplugin.dylib" "libspeexdsp.*\\.dylib" "@executable_path/../Frameworks/${SPEEXDSP_NAME}"
    fi
    
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
    change_dep_if_present "${MACOS}/h323askw" "libh323.*\\.dylib" "@executable_path/../Frameworks/libh323.dylib"
    
    # ptlib
    change_dep_if_present "${MACOS}/h323askw" "libpt.*\\.dylib" "@executable_path/../Frameworks/libpt.dylib"
    
    # OpenSSL
    if [ -n "$OPENSSL_SSL_NAME" ]; then
        change_dep_if_present "${MACOS}/h323askw" "libssl.*\\.dylib" "@executable_path/../Frameworks/${OPENSSL_SSL_NAME}"
    fi
    if [ -n "$OPENSSL_CRYPTO_NAME" ]; then
        change_dep_if_present "${MACOS}/h323askw" "libcrypto.*\\.dylib" "@executable_path/../Frameworks/${OPENSSL_CRYPTO_NAME}"
    fi
    # SpeexDSP
    if [ -n "$SPEEXDSP_NAME" ]; then
        change_dep_if_present "${MACOS}/h323askw" "libspeexdsp.*\\.dylib" "@executable_path/../Frameworks/${SPEEXDSP_NAME}"
    fi
    
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
    [ -n "$OPENSSL_SSL_NAME" ] && install_name_tool -id "@executable_path/../Frameworks/${OPENSSL_SSL_NAME}" "${FRAMEWORKS}/${OPENSSL_SSL_NAME}"
    [ -n "$OPENSSL_CRYPTO_NAME" ] && install_name_tool -id "@executable_path/../Frameworks/${OPENSSL_CRYPTO_NAME}" "${FRAMEWORKS}/${OPENSSL_CRYPTO_NAME}"
    [ -n "$AVCODEC_NAME" ] && install_name_tool -id "@executable_path/../Frameworks/${AVCODEC_NAME}" "${FRAMEWORKS}/${AVCODEC_NAME}"
    [ -n "$AVFORMAT_NAME" ] && install_name_tool -id "@executable_path/../Frameworks/${AVFORMAT_NAME}" "${FRAMEWORKS}/${AVFORMAT_NAME}"
    [ -n "$SPEEXDSP_NAME" ] && install_name_tool -id "@executable_path/../Frameworks/${SPEEXDSP_NAME}" "${FRAMEWORKS}/${SPEEXDSP_NAME}"
    
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
    [ -n "$AVUTIL_NAME" ] && install_name_tool -id "@executable_path/../Frameworks/${AVUTIL_NAME}" "${FRAMEWORKS}/${AVUTIL_NAME}"
    [ -n "$SWRESAMPLE_NAME" ] && install_name_tool -id "@executable_path/../Frameworks/${SWRESAMPLE_NAME}" "${FRAMEWORKS}/${SWRESAMPLE_NAME}"
    [ -n "$SWSCALE_NAME" ] && install_name_tool -id "@executable_path/../Frameworks/${SWSCALE_NAME}" "${FRAMEWORKS}/${SWSCALE_NAME}"
    [ -n "$X264_NAME" ] && install_name_tool -id "@executable_path/../Frameworks/${X264_NAME}" "${FRAMEWORKS}/${X264_NAME}"
    [ -n "$PORTAUDIO_NAME" ] && install_name_tool -id "@executable_path/../Frameworks/${PORTAUDIO_NAME}" "${FRAMEWORKS}/${PORTAUDIO_NAME}"
    
    # --- libh323 の依存関係を修正 ---
    log_info "  libh323 の依存関係を修正..."
    
    change_dep_if_present "${FRAMEWORKS}/libh323.dylib" "libpt.*\\.dylib" "@executable_path/../Frameworks/libpt.dylib"
    if [ -n "$OPENSSL_SSL_NAME" ]; then
        change_dep_if_present "${FRAMEWORKS}/libh323.dylib" "libssl.*\\.dylib" "@executable_path/../Frameworks/${OPENSSL_SSL_NAME}"
    fi
    if [ -n "$OPENSSL_CRYPTO_NAME" ]; then
        change_dep_if_present "${FRAMEWORKS}/libh323.dylib" "libcrypto.*\\.dylib" "@executable_path/../Frameworks/${OPENSSL_CRYPTO_NAME}"
    fi
    
    # --- libpt の依存関係を修正 ---
    log_info "  libpt の依存関係を修正..."
    
    if [ -n "$OPENSSL_SSL_NAME" ]; then
        change_dep_if_present "${FRAMEWORKS}/libpt.dylib" "libssl.*\\.dylib" "@executable_path/../Frameworks/${OPENSSL_SSL_NAME}"
    fi
    if [ -n "$OPENSSL_CRYPTO_NAME" ]; then
        change_dep_if_present "${FRAMEWORKS}/libpt.dylib" "libcrypto.*\\.dylib" "@executable_path/../Frameworks/${OPENSSL_CRYPTO_NAME}"
    fi
    
    # --- libssl の libcrypto 依存を修正 ---
    if [ -n "$OPENSSL_CRYPTO_NAME" ] && [ -n "$OPENSSL_SSL_NAME" ]; then
        change_dep_if_present "${FRAMEWORKS}/${OPENSSL_SSL_NAME}" "libcrypto.*\\.dylib" "@executable_path/../Frameworks/${OPENSSL_CRYPTO_NAME}"
    fi
    
    # --- H.264 プラグインの依存関係を修正 ---
    log_info "  H.264 プラグインの依存関係を修正..."
    
    local H264_PLUGIN="${PLUGINS}/video/H.264/h264_video_pwplugin.dylib"
    
    # Note: H.264プラグインはヘッダスペース不足でinstall_name_toolが失敗することがある
    # その場合はDYLD_LIBRARY_PATHで解決する
    install_name_tool -id "@executable_path/../Resources/plugins/video/H.264/h264_video_pwplugin.dylib" \
        "${H264_PLUGIN}" 2>/dev/null || log_warn "  H.264プラグインID設定をスキップ"
    
    # @loader_path を使って Frameworks へ解決（rpath追加不要でヘッダ不足に強い）
    [ -n "$AVCODEC_NAME" ] && change_dep_with_fallback "${H264_PLUGIN}" "libavcodec.*\\.dylib" "@loader_path/../../../../Frameworks/${AVCODEC_NAME}" "@executable_path/../Frameworks/${AVCODEC_NAME}" || log_warn "  H.264: libavcodec の依存書き換えに失敗"
    [ -n "$AVUTIL_NAME" ] && change_dep_with_fallback "${H264_PLUGIN}" "libavutil.*\\.dylib" "@loader_path/../../../../Frameworks/${AVUTIL_NAME}" "@executable_path/../Frameworks/${AVUTIL_NAME}" || log_warn "  H.264: libavutil の依存書き換えに失敗"
    [ -n "$SWRESAMPLE_NAME" ] && change_dep_with_fallback "${H264_PLUGIN}" "libswresample.*\\.dylib" "@loader_path/../../../../Frameworks/${SWRESAMPLE_NAME}" "@executable_path/../Frameworks/${SWRESAMPLE_NAME}" || log_warn "  H.264: libswresample の依存書き換えに失敗"
    [ -n "$SWSCALE_NAME" ] && change_dep_with_fallback "${H264_PLUGIN}" "libswscale.*\\.dylib" "@loader_path/../../../../Frameworks/${SWSCALE_NAME}" "@executable_path/../Frameworks/${SWSCALE_NAME}" || log_warn "  H.264: libswscale の依存書き換えに失敗"
    # h264プラグインはheaderpad不足で長い置換先にできないことがあるため、
    # swscaleは短い名前参照を許容（ランチャーのDYLD_LIBRARY_PATHで解決）
    [ -n "$SWSCALE_NAME" ] && change_dep_if_present "${H264_PLUGIN}" "libswscale.*\\.dylib" "${SWSCALE_NAME}"
    # fallbackで短いファイル名参照に落ちるケースを正規化
    [ -n "$AVCODEC_NAME" ] && change_dep_if_present "${H264_PLUGIN}" "^libavcodec.*\\.dylib$" "@executable_path/../Frameworks/${AVCODEC_NAME}"
    [ -n "$AVUTIL_NAME" ] && change_dep_if_present "${H264_PLUGIN}" "^libavutil.*\\.dylib$" "@executable_path/../Frameworks/${AVUTIL_NAME}"
    [ -n "$SWRESAMPLE_NAME" ] && change_dep_if_present "${H264_PLUGIN}" "^libswresample.*\\.dylib$" "@executable_path/../Frameworks/${SWRESAMPLE_NAME}"
    [ -n "$SWSCALE_NAME" ] && change_dep_if_present "${H264_PLUGIN}" "^libswscale.*\\.dylib$" "@executable_path/../Frameworks/${SWSCALE_NAME}"
    [ -n "$X264_NAME" ] && change_dep_if_present "${H264_PLUGIN}" "libx264.*\.dylib" "@executable_path/${X264_NAME}"

    # libvorbisenc.2.dylibのlibogg依存をlibogg.0.dylibに強制書き換え
    local VORBISENC_DYLIB="${FRAMEWORKS}/libvorbisenc.2.dylib"
    local OGG_MAJOR="@executable_path/../Frameworks/libogg.0.dylib"
    for oggver in $(ls "${FRAMEWORKS}"/libogg.*.dylib | grep -v libogg.0.dylib); do
        change_dep_if_present "$VORBISENC_DYLIB" "$(basename $oggver)" "$OGG_MAJOR"
    done
    
    # --- H.263-ffmpeg プラグインの依存関係を修正 ---
    log_info "  H.263-ffmpeg プラグインの依存関係を修正..."
    
    local H263_PLUGIN="${PLUGINS}/video/H.263-ffmpeg/h263-ffmpeg_video_pwplugin.dylib"
    if [ -f "${H263_PLUGIN}" ]; then
        install_name_tool -id "@executable_path/../Resources/plugins/video/H.263-ffmpeg/h263-ffmpeg_video_pwplugin.dylib" \
            "${H263_PLUGIN}" 2>/dev/null || log_warn "  H.263プラグインID設定をスキップ"
        
        [ -n "$AVFORMAT_NAME" ] && change_dep_with_fallback "${H263_PLUGIN}" "libavformat.*\\.dylib" "@loader_path/../../../../Frameworks/${AVFORMAT_NAME}" "@executable_path/../Frameworks/${AVFORMAT_NAME}" || log_warn "  H.263: libavformat の依存書き換えに失敗"
        [ -n "$AVCODEC_NAME" ] && change_dep_with_fallback "${H263_PLUGIN}" "libavcodec.*\\.dylib" "@loader_path/../../../../Frameworks/${AVCODEC_NAME}" "@executable_path/../Frameworks/${AVCODEC_NAME}" || log_warn "  H.263: libavcodec の依存書き換えに失敗"
        [ -n "$AVUTIL_NAME" ] && change_dep_with_fallback "${H263_PLUGIN}" "libavutil.*\\.dylib" "@loader_path/../../../../Frameworks/${AVUTIL_NAME}" "@executable_path/../Frameworks/${AVUTIL_NAME}" || log_warn "  H.263: libavutil の依存書き換えに失敗"
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
    if [ -n "$AVCODEC_NAME" ]; then
        [ -n "$AVUTIL_NAME" ] && change_dep_if_present "${FRAMEWORKS}/${AVCODEC_NAME}" "libavutil.*\\.dylib" "@executable_path/../Frameworks/${AVUTIL_NAME}"
        [ -n "$SWRESAMPLE_NAME" ] && change_dep_if_present "${FRAMEWORKS}/${AVCODEC_NAME}" "libswresample.*\\.dylib" "@executable_path/../Frameworks/${SWRESAMPLE_NAME}"
    fi
    
    # libswscale -> libavutil
    if [ -n "$SWSCALE_NAME" ] && [ -n "$AVUTIL_NAME" ]; then
        change_dep_if_present "${FRAMEWORKS}/${SWSCALE_NAME}" "libavutil.*\\.dylib" "@executable_path/../Frameworks/${AVUTIL_NAME}"
    fi
    
    # libswresample -> libavutil
    if [ -n "$SWRESAMPLE_NAME" ] && [ -n "$AVUTIL_NAME" ]; then
        change_dep_if_present "${FRAMEWORKS}/${SWRESAMPLE_NAME}" "libavutil.*\\.dylib" "@executable_path/../Frameworks/${AVUTIL_NAME}"
    fi
    
    # --- FFmpeg 追加依存ライブラリのパス修正 ---
    log_info "  FFmpeg 追加依存ライブラリのパスを修正..."
    
    # すべてのdylibのIDを設定し、Homebrew参照を@executable_pathへ修正
    for lib in "${FRAMEWORKS}"/*.dylib; do
        local libname=$(basename "$lib")
        install_name_tool -id "@executable_path/../Frameworks/${libname}" "$lib" 2>/dev/null || true
    done
    
    for target in "${FRAMEWORKS}"/*.dylib "${FRAMEWORKS}"/Qt*.framework/Versions/A/Qt*; do
        [ -f "$target" ] || continue
        otool -L "$target" 2>/dev/null | awk 'NR>1 {print $1}' | grep "/opt/homebrew" | while read oldpath; do
            local libname=$(basename "$oldpath")
            if [ -f "${FRAMEWORKS}/${libname}" ]; then
                install_name_tool -change "$oldpath" "@executable_path/../Frameworks/${libname}" "$target" 2>/dev/null || true
            fi
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
    
    change_dep_if_present "${PLUGINS}/vidinput/vidinput_macos_pwplugin.dylib" "libpt.*\\.dylib" "@executable_path/../Frameworks/libpt.dylib"
    
    # --- sound_portaudio プラグインの修正 ---
    log_info "  sound_portaudio プラグインの依存関係を修正..."
    
    install_name_tool -id "@executable_path/../Resources/plugins/sound/portaudio_pwplugin.dylib" \
        "${PLUGINS}/sound/portaudio_pwplugin.dylib"
    
    if [ -n "$PORTAUDIO_NAME" ]; then
        change_dep_if_present "${PLUGINS}/sound/portaudio_pwplugin.dylib" "libportaudio.*\\.dylib" "@executable_path/../Frameworks/${PORTAUDIO_NAME}"
    fi
    change_dep_if_present "${PLUGINS}/sound/portaudio_pwplugin.dylib" "libpt.*\\.dylib" "@executable_path/../Frameworks/libpt.dylib"
    
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

    # --- Qt6 スタイルプラグインの修正 ---
    if [ -f "${QT_PLUGINS}/styles/libqmacstyle.dylib" ]; then
        install_name_tool -id "@executable_path/../PlugIns/styles/libqmacstyle.dylib" \
            "${QT_PLUGINS}/styles/libqmacstyle.dylib" 2>/dev/null || true
        install_name_tool -change "@rpath/QtWidgets.framework/Versions/A/QtWidgets" \
            "@executable_path/../Frameworks/QtWidgets.framework/Versions/A/QtWidgets" \
            "${QT_PLUGINS}/styles/libqmacstyle.dylib" 2>/dev/null || true
        install_name_tool -change "@rpath/QtGui.framework/Versions/A/QtGui" \
            "@executable_path/../Frameworks/QtGui.framework/Versions/A/QtGui" \
            "${QT_PLUGINS}/styles/libqmacstyle.dylib" 2>/dev/null || true
        install_name_tool -change "@rpath/QtCore.framework/Versions/A/QtCore" \
            "@executable_path/../Frameworks/QtCore.framework/Versions/A/QtCore" \
            "${QT_PLUGINS}/styles/libqmacstyle.dylib" 2>/dev/null || true
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

# Finder 起動時は PATH が最小化されるため Homebrew の ffmpeg を明示的に探索可能にする
export PATH="${SCRIPT_DIR}:/opt/homebrew/bin:/usr/local/bin:${PATH}"

# プラグインパスを環境変数に設定
PLUGIN_ROOT="${RESOURCES_DIR}/plugins"
PLUGIN_PATHS="${PLUGIN_ROOT}:${PLUGIN_ROOT}/vidinput:${PLUGIN_ROOT}/sound:${PLUGIN_ROOT}/video:${PLUGIN_ROOT}/audio:${PLUGIN_ROOT}/video/H.264:${PLUGIN_ROOT}/video/H.263-ffmpeg:${PLUGIN_ROOT}/video/H.261-vic"

# PTLib系（新旧両方の変数名を設定）
export PTLIBPLUGINDIR="${PLUGIN_PATHS}"
export PWLIBPLUGINDIR="${PLUGIN_PATHS}"
export PTLIB_PLUGIN_DIR="${PLUGIN_PATHS}"
export PWLIB_PLUGIN_DIR="${PLUGIN_PATHS}"

# H323Plus用（video, audio コーデック）
export H323_PLUGIN_DIR="${PLUGIN_ROOT}"

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

    check_dep_integrity() {
        local target="$1"
        local dep=""
        local dep_lines=""

        dep_lines="$(otool -L "$target" 2>/dev/null | tail -n +2)"
        # dylib/framework binary では先頭に install name (self id) が来るので除外
        if otool -D "$target" >/dev/null 2>&1; then
            dep_lines="$(printf "%s\n" "$dep_lines" | tail -n +2)"
        fi

        while IFS= read -r dep; do
            [ -n "$dep" ] || continue

            local resolved=""
            case "$dep" in
                /System/*|/usr/lib/*)
                    continue
                    ;;
                /*)
                    log_warn "  $(basename "$target") に外部絶対パス依存があります: ${dep}"
                    has_error=1
                    continue
                    ;;
                @executable_path/*)
                    resolved="${APP_BUNDLE}/Contents/MacOS/${dep#@executable_path/}"
                    ;;
                @loader_path/*)
                    local target_dir
                    target_dir="$(dirname "$target")"
                    resolved="${target_dir}/${dep#@loader_path/}"
                    ;;
                @rpath/*|@*)
                    continue
                    ;;
                *.dylib)
                    # 一部プラグインはheaderpad制約で短い名前参照のみ可能
                    # (ランチャーのDYLD_LIBRARY_PATHでFrameworksから解決)
                    if [ -e "${APP_BUNDLE}/Contents/Frameworks/${dep}" ]; then
                        continue
                    fi
                    log_warn "  $(basename "$target") に裸の dylib 参照があります: ${dep}"
                    has_error=1
                    continue
                    ;;
                *)
                    continue
                    ;;
            esac

            if [ -n "$resolved" ] && [ ! -e "$resolved" ]; then
                log_warn "  依存解決不可: $(basename "$target") -> ${dep} (${resolved})"
                has_error=1
            fi
        done < <(printf "%s\n" "$dep_lines" | awk '{print $1}')
    }
    
    # 実行ファイルの依存関係をチェック (最初の行はファイル名なのでスキップ)
    log_info "  実行ファイルの依存関係を確認..."
    if otool -L "${APP_BUNDLE}/Contents/MacOS/h323askw_bin" | tail -n +2 | grep -q "/Users/"; then
        log_error "  絶対パスがまだ残っています！"
        otool -L "${APP_BUNDLE}/Contents/MacOS/h323askw_bin" | tail -n +2 | grep "/Users/"
        has_error=1
    fi
    
    if otool -L "${APP_BUNDLE}/Contents/MacOS/h323askw_bin" | tail -n +2 | grep -q "/opt/homebrew"; then
        log_error "  Homebrewパスがまだ残っています！"
        otool -L "${APP_BUNDLE}/Contents/MacOS/h323askw_bin" | tail -n +2 | grep "/opt/homebrew"
        has_error=1
    fi
    
    # ライブラリの依存関係をチェック (最初の行はファイル名なのでスキップ)
    log_info "  ライブラリの依存関係を確認..."
    for lib in "${APP_BUNDLE}/Contents/Frameworks/"*.dylib; do
        if otool -L "$lib" | tail -n +2 | grep -q "/Users/\|/opt/homebrew"; then
            log_warn "  $(basename $lib) に問題があるかもしれません"
            has_error=1
        fi
    done

    # プラグイン依存もチェック (Resources/plugins と Qt PlugIns)
    log_info "  プラグインの依存関係を確認..."
    while IFS= read -r -d '' plugin; do
        if otool -L "$plugin" | tail -n +2 | grep -q "/Users/\|/opt/homebrew"; then
            log_warn "  $(basename "$plugin") に外部パス依存があります"
            otool -L "$plugin" | tail -n +2 | grep "/Users/\|/opt/homebrew" || true
            has_error=1
        fi
    done < <(find "${APP_BUNDLE}/Contents/Resources/plugins" "${APP_BUNDLE}/Contents/PlugIns" -name "*.dylib" -print0 2>/dev/null || true)

    # 裸dylib参照/解決不能参照を追加チェック
    log_info "  dylib 解決整合性を確認..."
    check_dep_integrity "${APP_BUNDLE}/Contents/MacOS/h323askw_bin"
    for lib in "${APP_BUNDLE}/Contents/Frameworks/"*.dylib; do
        [ -f "$lib" ] || continue
        check_dep_integrity "$lib"
    done
    for fwbin in "${APP_BUNDLE}/Contents/Frameworks/Qt"*.framework/Versions/A/Qt*; do
        [ -f "$fwbin" ] || continue
        check_dep_integrity "$fwbin"
    done
    while IFS= read -r -d '' plugin; do
        check_dep_integrity "$plugin"
    done < <(find "${APP_BUNDLE}/Contents/Resources/plugins" "${APP_BUNDLE}/Contents/PlugIns" -name "*.dylib" -print0 2>/dev/null || true)
    
    if [ $has_error -eq 0 ]; then
        log_success "App Bundle の検証が完了しました (問題なし)"
    else
        log_error "App Bundle の依存関係に問題があります。"
        return 1
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
    local has_sign_error=0
    
    # まず、すべてのdylibに署名
    log_info "  Frameworksのライブラリに署名中..."
    for lib in "${APP_BUNDLE}/Contents/Frameworks/"*.dylib; do
        if [ -f "$lib" ]; then
            codesign --force --sign - --timestamp=none "$lib" 2>/dev/null || {
                log_warn "  署名スキップ: $(basename $lib)"
                has_sign_error=1
            }
        fi
    done
    
    # Qt フレームワークに署名 (内側から外側へ)
    log_info "  Qt フレームワークに署名中..."
    for fw in QtCore QtDBus QtGui QtWidgets; do
        local fw_path="${APP_BUNDLE}/Contents/Frameworks/${fw}.framework"
        if [ -d "$fw_path" ]; then
            # フレームワーク全体に署名（内部バイナリを含む）
            if ! codesign --force --sign - --timestamp=none "${fw_path}" 2>/dev/null; then
                log_warn "  Qt framework 署名失敗: ${fw}.framework"
                has_sign_error=1
            fi
        fi
    done
    
    # プラグインに署名
    log_info "  プラグインに署名中..."
    if [ -d "${APP_BUNDLE}/Contents/Resources/plugins" ]; then
        while IFS= read -r -d '' dylib; do
            if ! codesign --force --sign - --timestamp=none "$dylib" 2>/dev/null; then
                log_warn "  プラグイン署名失敗: $(basename "$dylib")"
                has_sign_error=1
            fi
        done < <(find "${APP_BUNDLE}/Contents/Resources/plugins" -name "*.dylib" -print0)
    fi
    
    # Qt6 プラットフォームプラグインに署名
    log_info "  Qt6 プラットフォームプラグインに署名中..."
    if [ -d "${APP_BUNDLE}/Contents/PlugIns/platforms" ]; then
        for plugin in "${APP_BUNDLE}/Contents/PlugIns/platforms/"*.dylib; do
            if [ -f "$plugin" ]; then
                if ! codesign --force --sign - --timestamp=none "$plugin" 2>/dev/null; then
                    log_warn "  Qt platform plugin 署名失敗: $(basename "$plugin")"
                    has_sign_error=1
                fi
            fi
        done
    fi
    
    # 実行ファイルに署名
    log_info "  実行ファイルに署名中..."
    codesign --force --sign - --timestamp=none "${APP_BUNDLE}/Contents/MacOS/h323askw_bin" 2>/dev/null || {
        log_warn "  実行ファイル署名に問題がありました"
        has_sign_error=1
    }
    
    # 起動スクリプトに署名（必要に応じて）
    if [ -f "${APP_BUNDLE}/Contents/MacOS/h323askw" ]; then
        if ! codesign --force --sign - --timestamp=none "${APP_BUNDLE}/Contents/MacOS/h323askw" 2>/dev/null; then
            log_warn "  起動スクリプト署名に問題がありました"
            has_sign_error=1
        fi
    fi
    
    # アプリバンドル全体に署名
    log_info "  App Bundle 全体に署名中..."
    codesign --force --deep --sign - --timestamp=none "${APP_BUNDLE}" 2>/dev/null || {
        log_warn "  App Bundle署名に問題がありました"
        has_sign_error=1
    }
    
    # 署名を検証
    log_info "  署名を検証中..."
    if codesign --verify --deep --strict --verbose=2 "${APP_BUNDLE}" 2>&1; then
        log_success "コード署名が完了しました (strict verify OK)"
    else
        log_error "署名検証に失敗しました"
        has_sign_error=1
    fi

    # Gatekeeper 判定（ローカル検証用の参考情報）
    if spctl --assess --type execute --verbose=2 "${APP_BUNDLE}" >/dev/null 2>&1; then
        log_success "Gatekeeper 検証: 通過"
    else
        log_warn "Gatekeeper 検証: 拒否されました"
        log_warn "  このビルドは配布用に承認されたものではありません"
    fi

    if [ $has_sign_error -ne 0 ]; then
        log_error "コード署名に失敗したため中断します"
        exit 1
    fi
}

# ===== ローカル検証用DMG作成 (オプション) =====
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
    if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != "--create-dmg" ]; }; then
        echo "Usage: $0 [--create-dmg]" >&2
        exit 2
    fi

    echo ""
    echo "=========================================="
    echo "  ${APP_NAME} App Bundle Builder"
    echo "=========================================="
    echo ""
    
    build_executable
    check_prerequisites
    create_bundle_structure
    create_info_plist
    prepare_app_icon
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
    
    if [ "${1:-}" = "--create-dmg" ]; then
        create_dmg
    fi
    
    echo ""
    log_info "完了しました！"
}

# 実行
main "$@"
