# H323ASKW macOS App Bundle ビルドガイド

このドキュメントでは、H323ASKWを自分のMacで検証するためにmacOS App Bundleへ
パッケージングする方法を説明します。開発者はDMG、App Bundle、実行ファイルを
公開・配布しません。依存ライブラリを組み込んだBundleの再配布には、別途
ライセンス確認が必要です。

---

## 目次

1. [概要](#概要)
2. [前提条件](#前提条件)
3. [App Bundleとは](#app-bundleとは)
4. [依存関係の調査方法](#依存関係の調査方法)
5. [ビルドスクリプトの使い方](#ビルドスクリプトの使い方)
6. [スクリプトの詳細解説](#スクリプトの詳細解説)
7. [トラブルシューティング](#トラブルシューティング)
8. [公開方針](#公開方針)

---

## 概要

H323ASKWは複数のダイナミックライブラリ（dylib）に依存しています。
ビルドしたままの状態では、ライブラリへのパスが絶対パスでハードコードされているため、
他のMacではそのまま動作しません。

このガイドでは、`install_name_tool`を使ってライブラリパスを相対パスに書き換え、
ローカル検証用のApp Bundleを作成する方法を説明します。

---

## 前提条件

### 必要なソフトウェア

- macOS 11.0 (Big Sur) 以降
- Xcode Command Line Tools
- Homebrew

アプリ本体のクリーンビルド手順は[README](../README.md#building-from-source)と
[Build CI](../.github/workflows/build.yml)を参照してください。Bundle作成は別工程で、
ビデオ・音声プラグインと依存ライブラリの準備が追加で必要です。

`create_app_bundle.sh`は、既定で公開H323Plusソースの
`h323plus/plugins/video/`からH.264などのプラグインを探します。
H.264プラグインはH323Plusのconfigure後に
`make -C h323plus/plugins/video/H.264`などで別途ビルドしてください。
このプラグインの現行Homebrew環境でのビルド・実行互換性は、Build CIの
検証対象外です。独自のプラグインツリーを使う場合だけ、
`H323PLUS_PLUGINS_DIR`に`H.264/`を含む親ディレクトリを指定します。
公開予定の`y-asakawa/h323plus-plugins`は、現時点では非公開であり、
公開版の本体ビルドに必須ではありません。
一方、Bundle作成で必要な`vidinput_macos_pwplugin.dylib`と
`portaudio_pwplugin.dylib`のソースは、現時点の公開PTLibにはありません。
これらを別途入手・ビルドできない場合、同等のカメラ・音声機能付きBundleは
第三者が再現できません。スクリプトは不足を事前に検出して停止します。
検証で外部絶対パスなどの依存問題が見つかった場合もBundle作成は失敗します。

### 必要なファイル

| 種別 | ファイル | 場所 |
|------|----------|------|
| 実行ファイル | `h323askw` | `h323askw/obj_Darwin_aarch64/` |
| H323Plus | `libh323_Darwin_aarch64_.1.28.0.dylib` | `h323plus/lib/` |
| PTLib | `libpt.2.10.9.dylib` | `ptlib/lib_Darwin_aarch64/` |
| OpenSSL | `libssl.3.dylib`, `libcrypto.3.dylib` | `/opt/homebrew/opt/openssl@3/lib/` |
| SDL2 | `libSDL2-2.0.0.dylib` | `/opt/homebrew/opt/sdl2/lib/` |
| FFmpeg | `libavformat`, `libavcodec`, `libavutil`, `libswresample`, `libswscale` の各バージョン付きdylib | `/opt/homebrew/opt/ffmpeg/lib/` |
| x264 | `libx264.165.dylib` | `/opt/homebrew/opt/x264/lib/` |
| PortAudio | `libportaudio.2.dylib` | `/opt/homebrew/opt/portaudio/lib/` |
| H.264 Plugin | `h264_video_pwplugin.dylib` | `h323plus/plugins/video/H.264/` |
| H.263 Plugin | `h263-1998_video_pwplugin.dylib` | `h323plus/plugins/video/H.263-1998/` |
| Video Input | `vidinput_macos_pwplugin.dylib` | `ptlib/plugins/vidinput_macos/` |
| Sound (PortAudio) | `portaudio_pwplugin.dylib` | `ptlib/lib_Darwin_aarch64/device/sound/` |

---

## App Bundleとは

macOSのアプリケーションは、実際には特定の構造を持つディレクトリです。
Finderでは1つのアプリとして表示されますが、内部は以下の構造になっています。

```
H323ASKW.app/
└── Contents/
    ├── Info.plist              # アプリのメタデータ
    ├── MacOS/
    │   ├── h323askw          # 起動スクリプト
    │   └── h323askw_bin      # 実行バイナリ
    ├── Frameworks/
    │   ├── libh323.dylib       # 動的ライブラリ
    │   ├── libpt.dylib
    │   ├── libssl.3.dylib
    │   ├── libcrypto.3.dylib
    │   ├── libSDL2-2.0.0.dylib
    │   ├── libavformat.<major>.dylib
    │   ├── libavcodec.<major>.dylib
    │   ├── libavutil.<major>.dylib
    │   ├── libswresample.<major>.dylib
    │   ├── libswscale.<major>.dylib
    │   ├── libx264.165.dylib
    │   └── libportaudio.2.dylib
    └── Resources/
        └── plugins/
            ├── video/
            │   ├── h264_video_pwplugin.dylib
            │   └── h263-1998_video_pwplugin.dylib
            ├── vidinput/
            │   └── vidinput_macos_pwplugin.dylib
            ├── sound/
            │   └── portaudio_pwplugin.dylib
            └── audio/
                ├── g722_audio_pwplugin.dylib
                └── g7221_audio_pwplugin.dylib
```

### 主要なディレクトリの役割

| ディレクトリ | 役割 |
|-------------|------|
| `Contents/MacOS/` | 実行ファイルを配置 |
| `Contents/Frameworks/` | 動的ライブラリ（.dylib）を配置 |
| `Contents/Resources/` | リソースファイル（画像、プラグインなど）を配置 |

---

## 依存関係の調査方法

### 実行ファイルの依存ライブラリを確認

```bash
otool -L obj_Darwin_aarch64/h323askw
```

出力例:
```
obj_Darwin_aarch64/h323askw:
    /path/to/h323plus/lib/libh323_Darwin_aarch64_.1.28.0.dylib
    /path/to/ptlib/lib_Darwin_aarch64/libpt.2.10.9.dylib
    /opt/homebrew/opt/openssl@3/lib/libssl.3.dylib
    /opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib
    /opt/homebrew/opt/sdl2/lib/libSDL2-2.0.0.dylib
    /usr/lib/libSystem.B.dylib
    ...
```

**問題点**: `/Users/example/...` や `/opt/homebrew/...` のような絶対パスは、
他のMacには存在しないため、そのままでは動作しません。

### ライブラリ自体の依存関係を確認

```bash
# H323Plusライブラリの依存関係
otool -L /path/to/h323plus/lib/libh323_Darwin_aarch64_.1.28.0.dylib

# PTLibの依存関係
otool -L /path/to/ptlib/lib_Darwin_aarch64/libpt.2.10.9.dylib
```

### ライブラリのInstall Nameを確認

```bash
otool -D /path/to/h323plus/lib/libh323_Darwin_aarch64_.1.28.0.dylib
```

出力:
```
/path/to/h323plus/lib/libh323_Darwin_aarch64_.1.28.0.dylib
```

このInstall Nameも書き換える必要があります。

### アーキテクチャの確認

```bash
file obj_Darwin_aarch64/h323askw
```

出力:
```
Mach-O 64-bit executable arm64
```

**注意**: ARM64（Apple Silicon）用にビルドされたバイナリは、Intel Mac (x86_64) では動作しません。

---

## ビルドスクリプトの使い方

### 1. スクリプトの実行

```bash
cd /path/to/h323askw
./create_app_bundle.sh
```

既存の`dist/`を上書きせずに試す場合は、`OUTPUT_DIR`で出力先を指定できます。

```bash
OUTPUT_DIR=/tmp/h323askw-bundle-test ./create_app_bundle.sh
```

ローカル検証用のDMGも必要な場合だけ、`--create-dmg`を指定します。

```bash
./create_app_bundle.sh --create-dmg
```

### 2. 実行結果

通常は以下のApp Bundleだけが生成されます:

```
dist/
└── H323ASKW.app/          # ローカル検証用App Bundle
```

`--create-dmg`を指定した場合のみ、`dist/H323ASKW-<version>.dmg`も生成されます。

### 3. 動作確認

```bash
# App Bundleを直接実行
./dist/H323ASKW.app/Contents/MacOS/h323askw --help

# または Finder からダブルクリック
open dist/H323ASKW.app
```

---

## スクリプトの詳細解説

### 処理フロー

```
1. check_prerequisites()     ファイル存在確認
         ↓
2. create_bundle_structure() ディレクトリ構造作成
         ↓
3. create_info_plist()       メタデータ生成
         ↓
4. copy_executable()         実行ファイルコピー
         ↓
5. copy_libraries()          ライブラリコピー
         ↓
6. copy_plugins()            プラグインコピー
         ↓
7. fix_library_paths()       ★パス書き換え（核心部分）
         ↓
8. create_launcher_script()  起動スクリプト作成
         ↓
9. verify_bundle()           検証
         ↓
10. show_bundle_size()       サイズ表示
         ↓
11. create_dmg()             --create-dmg指定時のみ
```

---

### install_name_tool の使い方

macOSでは、ダイナミックライブラリへの参照パスを変更するために `install_name_tool` を使用します。

#### ライブラリのID（自身の名前）を変更

```bash
install_name_tool -id <新しいID> <ライブラリファイル>
```

例:
```bash
install_name_tool -id "@executable_path/../Frameworks/libh323.dylib" \
    H323ASKW.app/Contents/Frameworks/libh323.dylib
```

#### 依存ライブラリのパスを変更

```bash
install_name_tool -change <古いパス> <新しいパス> <対象ファイル>
```

例:
```bash
install_name_tool -change \
    "/path/to/h323plus/lib/libh323_Darwin_aarch64_.1.28.0.dylib" \
    "@executable_path/../Frameworks/libh323.dylib" \
    H323ASKW.app/Contents/MacOS/h323askw
```

---

### 特殊なパス変数

| 変数 | 意味 |
|------|------|
| `@executable_path` | 実行ファイルの場所を基準とした相対パス |
| `@loader_path` | ロードするライブラリの場所を基準とした相対パス |
| `@rpath` | 実行時に設定されるランタイムパス |

本スクリプトでは `@executable_path` を使用しています。

#### パス解決の例

実行ファイルが `Contents/MacOS/h323askw` の場合:

```
@executable_path/../Frameworks/libh323.dylib
    ↓ 解決
Contents/MacOS/../Frameworks/libh323.dylib
    ↓ 正規化
Contents/Frameworks/libh323.dylib
```

---

### 依存関係の連鎖

ライブラリは他のライブラリに依存していることがあります。
全ての依存関係を追跡して書き換える必要があります。

```
h323askw
    ├── libh323.dylib
    │       ├── libpt.dylib
    │       ├── libssl.3.dylib
    │       │       └── libcrypto.3.dylib
    │       └── libcrypto.3.dylib
    ├── libpt.dylib
    │       ├── libssl.3.dylib
    │       └── libcrypto.3.dylib
    ├── libssl.3.dylib
    │       └── libcrypto.3.dylib
    ├── libcrypto.3.dylib
    └── libSDL2-2.0.0.dylib

h264_video_pwplugin.dylib
    ├── libavcodec.62.dylib
    │       ├── libavutil.60.dylib
    │       └── libswresample.6.dylib
    │               └── libavutil.60.dylib
    ├── libavutil.60.dylib
    ├── libswresample.6.dylib
    ├── libswscale.9.dylib
    │       └── libavutil.60.dylib
    └── libx264.165.dylib
```

---

### 起動スクリプトの役割

ptlib/h323plusはプラグインを動的にロードするため、環境変数でプラグインの場所を教える必要があります。

```bash
#!/bin/bash
APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RESOURCES_DIR="${APP_DIR}/Resources"

# プラグインパスを環境変数に設定
export PTLIB_PLUGIN_DIR="${RESOURCES_DIR}/plugins/vidinput"
export PWLIB_PLUGIN_DIR="${RESOURCES_DIR}/plugins/vidinput"
export H323_PLUGIN_DIR="${RESOURCES_DIR}/plugins"

# 本体を実行
exec "${APP_DIR}/MacOS/h323askw_bin" "$@"
```

---

### Info.plist の主要項目

```xml
<!-- アプリの識別子（一意である必要がある） -->
<key>CFBundleIdentifier</key>
<string>com.h323askw.app</string>

<!-- 実行ファイル名 -->
<key>CFBundleExecutable</key>
<string>h323askw</string>

<!-- 最低動作OS -->
<key>LSMinimumSystemVersion</key>
<string>11.0</string>

<!-- マイク使用許可のダイアログメッセージ -->
<key>NSMicrophoneUsageDescription</key>
<string>H323ASKW needs access to your microphone for voice calls.</string>

<!-- カメラ使用許可のダイアログメッセージ -->
<key>NSCameraUsageDescription</key>
<string>H323ASKW needs access to your camera for video calls.</string>

<!-- 対応アーキテクチャ -->
<key>LSArchitecturePriority</key>
<array>
    <string>arm64</string>
</array>
```

---

## トラブルシューティング

### 「Library not loaded」エラー

```
dyld: Library not loaded: /Users/example/...
  Referenced from: /path/to/h323askw
  Reason: image not found
```

**原因**: ライブラリパスの書き換えが不完全

**対処**:
1. `otool -L` で問題のあるファイルを確認
2. 絶対パスが残っていないか確認
3. 不足している `install_name_tool -change` を追加

### 検証コマンド

```bash
# 実行ファイルの依存関係を確認
otool -L dist/H323ASKW.app/Contents/MacOS/h323askw_bin

# 絶対パスが残っていないか確認
otool -L dist/H323ASKW.app/Contents/MacOS/h323askw_bin | grep -E "/Users/|/opt/homebrew/"

# 全ライブラリを確認
for lib in dist/H323ASKW.app/Contents/Frameworks/*.dylib; do
    echo "=== $lib ==="
    otool -L "$lib" | grep -E "/Users/|/opt/homebrew/"
done
```

### プラグインが見つからない

**原因**: 環境変数が正しく設定されていない

**対処**: 起動スクリプトで以下の環境変数を確認
- `PTLIB_PLUGIN_DIR`
- `PWLIB_PLUGIN_DIR`
- `H323_PLUGIN_DIR`

### コード署名エラー

ローカル検証用にアドホック署名する場合:

```bash
codesign --force --deep --sign - dist/H323ASKW.app
```

---

## 公開方針

このリポジトリで公開するのはソースコードのみです。スクリプトは明示的に
指定した場合のみDMGを作成できますが、ローカル検証用であり、開発者は
公開しません。
この方針は、各ライセンスが第三者に認める権利を制限するものではありません。
BundleやDMGを再配布する人は、完成した成果物について
[`DEPENDENCY-LICENSE-REVIEW.md`](DEPENDENCY-LICENSE-REVIEW.md)に記載した
依存関係とライセンス条件を個別に確認する必要があります。

---

## 参考資料

- [Apple Developer: Bundle Programming Guide](https://developer.apple.com/library/archive/documentation/CoreFoundation/Conceptual/CFBundles/)
- [Apple Developer: Code Signing Guide](https://developer.apple.com/library/archive/documentation/Security/Conceptual/CodeSigningGuide/)
- [man install_name_tool](https://www.unix.com/man-page/osx/1/install_name_tool/)
- [man otool](https://www.unix.com/man-page/osx/1/otool/)

---

## 更新履歴

| 日付 | 内容 |
|------|------|
| 2025-12-01 | 初版作成 |
