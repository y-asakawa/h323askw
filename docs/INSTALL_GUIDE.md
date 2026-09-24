# H323ASKW インストールマニュアル

このドキュメントでは、H323ASKWのインストール方法と初期設定について説明します。

---

## 目次

1. [システム要件](#システム要件)
2. [インストール方法](#インストール方法)
3. [初回起動時の設定](#初回起動時の設定)
4. [基本的な使い方](#基本的な使い方)
5. [コマンドラインオプション](#コマンドラインオプション)
6. [設定ファイル](#設定ファイル)
7. [アンインストール](#アンインストール)
8. [トラブルシューティング](#トラブルシューティング)
9. [ソースからビルドする場合の注意事項](#ソースからビルドする場合の注意事項)

---

## システム要件

### 対応OS

- macOS 11.0 (Big Sur) 以降
- Apple Silicon (M1/M2/M3/M4) Mac

> **注意**: 現在はApple Silicon専用です。Intel Macでは動作しません。

### 追加ソフトウェア

**不要です。** 必要なライブラリは全てApp Bundle内に含まれています。

> ℹ️ Homebrew、FFmpeg、OpenSSL などを別途インストールする必要はありません。

### ハードウェア要件

| 項目 | 最小要件 | 推奨 |
|------|----------|------|
| メモリ | 4GB | 8GB以上 |
| ストレージ | 200MB | 500MB |
| カメラ | 内蔵/外付けWebカメラ | - |
| マイク | 内蔵/外付けマイク | - |
| ネットワーク | 1Mbps以上 | 10Mbps以上 |

### 対応コーデック

| 種別 | コーデック |
|------|-----------|
| 映像 | H.264, H.263, H.261 |
| 音声 | G.711 (μ-law/A-law), G.722 |

---

## インストール方法

### 方法1: DMGファイルからインストール

1. **DMGファイルを開く**
   
   `H323ASKW-1.0.0.dmg` をダブルクリックしてマウントします。

2. **アプリケーションフォルダにコピー**
   
   開いたウィンドウで `H323ASKW.app` を `Applications` フォルダにドラッグ&ドロップします。

3. **DMGをアンマウント**
   
   Finderのサイドバーで H323ASKW ボリュームの取り出しボタンをクリックします。

### 方法2: App Bundleを直接コピー

1. `H323ASKW.app` を任意の場所（`/Applications` 推奨）にコピーします。

2. 完了です。

---

## 初回起動時の設定

### Gatekeeperの警告について

署名されていないアプリの場合、以下の警告が表示されることがあります。

```
"H323ASKW"は、開発元を検証できないため開けません。
```

**対処方法:**

1. **右クリックから開く（推奨）**
   
   - `H323ASKW.app` を右クリック（またはControl+クリック）
   - 「開く」を選択
   - 確認ダイアログで「開く」をクリック

2. **システム設定から許可**
   
   - 「システム設定」→「プライバシーとセキュリティ」を開く
   - 「セキュリティ」セクションで「このまま開く」をクリック

3. **コマンドラインで検疫属性を削除**
   
   ```bash
   xattr -cr /Applications/H323ASKW.app
   ```

### カメラ・マイクの許可

初回起動時に、カメラとマイクへのアクセス許可を求めるダイアログが表示されます。

1. 「OK」または「許可」をクリックして許可します。
2. 許可しないとビデオ通話機能が使用できません。

後から変更する場合:
- 「システム設定」→「プライバシーとセキュリティ」→「カメラ」/「マイク」
- H323ASKW のチェックを ON にする

---

## 基本的な使い方

### ターミナルからの起動

```bash
# 基本的な起動
/Applications/H323ASKW.app/Contents/MacOS/h323askw [オプション]

# または、アプリディレクトリから
cd /Applications/H323ASKW.app/Contents/MacOS
./h323askw [オプション]
```

### 発信（Make Call）

```bash
# H.323アドレスに発信
./h323askw -n 192.168.1.100

# ゲートキーパー経由で発信
./h323askw -g gatekeeper.example.com -n endpoint_alias
```

### 着信待ち（Wait for Call）

```bash
# 着信を待機
./h323askw -l

# 特定のポートで待機
./h323askw -l -p 1720
```

### Finderからの起動

1. `/Applications/H323ASKW.app` をダブルクリック
2. ターミナルウィンドウが開き、デフォルト設定で起動します

---

## コマンドラインオプション

### 接続オプション

| オプション | 説明 | 例 |
|-----------|------|-----|
| `-n <address>` | 発信先アドレス | `-n 192.168.1.100` |
| `-g <gatekeeper>` | ゲートキーパーアドレス | `-g gk.example.com` |
| `-l` | 着信待機モード | `-l` |
| `-p <port>` | リスニングポート | `-p 1720` |

### メディアオプション

| オプション | 説明 | 例 |
|-----------|------|-----|
| `--video` | ビデオ有効 | `--video` |
| `--no-video` | ビデオ無効 | `--no-video` |
| `-s <device>` | 音声デバイス | `-s "Default Audio Input"` |
| `-v <device>` | ビデオデバイス | `-v "Default Video Input"` |

### 表示オプション

| オプション | 説明 | 例 |
|-----------|------|-----|
| `-t` | トレースレベル | `-t 3` |
| `-o <file>` | トレース出力先 | `-o debug.log` |
| `--help` | ヘルプ表示 | `--help` |

### 使用例

```bash
# ビデオ通話で発信、トレースレベル3
./h323askw -n 192.168.1.100 --video -t 3

# 着信待機、ログ出力
./h323askw -l -t 4 -o /tmp/h323askw.log

# ゲートキーパー経由でエイリアス指定発信
./h323askw -g 10.0.0.1 -n user@example.com --video
```

---

## 設定ファイル

### ユーザー設定の保存場所

アプリケーションの設定は以下の場所に保存されます:

| 設定項目 | 保存場所 |
|----------|----------|
| アドレス履歴 | `~/Library/Preferences/com.H323ASKW.VideoClient.plist` |

#### アドレス履歴について

- **保存場所**: `~/Library/Preferences/com.H323ASKW.VideoClient.plist`
- **保存タイミング**: 接続時およびアプリ終了時に自動保存
- **最大件数**: 20件（古い履歴は自動削除）
- **形式**: macOS標準のplist形式

履歴をクリアしたい場合:
```bash
rm ~/Library/Preferences/com.H323ASKW.VideoClient.plist
```

### プラグインパス

プラグインは以下の場所から読み込まれます:

```
H323ASKW.app/Contents/Resources/plugins/
├── video/
│   ├── H.264/
│   │   └── h264_video_pwplugin.dylib
│   ├── H.263-ffmpeg/
│   │   └── h263-ffmpeg_video_pwplugin.dylib
│   └── H.261-vic/
│       └── h261-vic_video_pwplugin.dylib
├── vidinput/
│   └── vidinput_macos_pwplugin.dylib
├── sound/
│   └── portaudio_pwplugin.dylib
└── audio/
    ├── g722_audio_pwplugin.dylib
    └── g7221_audio_pwplugin.dylib
```

### 環境変数

必要に応じて以下の環境変数を設定できます:

| 変数名 | 説明 |
|--------|------|
| `PTLIB_PLUGIN_DIR` | PTLibプラグインのパス |
| `H323_PLUGIN_DIR` | H.323プラグインのパス |
| `PTRACE_LEVEL` | デフォルトトレースレベル |

```bash
export PTRACE_LEVEL=4
./h323askw -l
```

---

## アンインストール

### 手順

1. **アプリケーションを削除**
   
   ```bash
   rm -rf /Applications/H323ASKW.app
   ```
   
   または、Finderで `H323ASKW.app` をゴミ箱にドラッグ

2. **設定ファイルを削除（オプション）**
   
   ```bash
   # アドレス履歴
   rm -f ~/Library/Preferences/com.H323ASKW.VideoClient.plist
   ```

3. **ゴミ箱を空にする**

---

## トラブルシューティング

### アプリが起動しない

#### 「Library not loaded」エラー

```
dyld: Library not loaded: @executable_path/../Frameworks/libh323.dylib
```

**対処**: App Bundleが破損している可能性があります。再インストールしてください。

#### 「This app is damaged」エラー

```
"H323ASKW.app"は壊れているため開けません。
```

**対処**: 検疫属性を削除します。
```bash
xattr -cr /Applications/H323ASKW.app
```

### カメラ/マイクが使えない

1. **権限を確認**
   - 「システム設定」→「プライバシーとセキュリティ」→「カメラ」/「マイク」
   - H323ASKW が許可されているか確認

2. **アプリを再起動**
   - 権限を変更した後は再起動が必要です

3. **ターミナルから起動してログを確認**
   ```bash
   ./h323askw -l -t 4 2>&1 | tee debug.log
   ```

### 通話が確立しない

#### ファイアウォールの確認

H.323では以下のポートを使用します:

| ポート | プロトコル | 用途 |
|--------|-----------|------|
| 1720 | TCP | H.225 (通話制御) |
| 1719 | UDP | RAS (ゲートキーパー通信) |
| 動的 | UDP | RTP (音声/映像データ) |

**macOSファイアウォールで許可:**

```bash
# 着信接続を許可（初回起動時にダイアログが出ます）
# または「システム設定」→「ファイアウォール」→「オプション」で設定
```

#### NAT/ルーター設定

NAT環境では、ポートフォワーディングが必要な場合があります:
- TCP 1720 → H323ASKWを実行するMacのIPアドレス

### 映像が表示されない

1. **コーデックの互換性を確認**
   - 相手端末が H.264、H.263、または H.261 に対応しているか確認
   
2. **トレースログで確認**
   ```bash
   ./h323askw -n 192.168.1.100 --video -t 5 -o video_debug.log
   ```
   
3. **コーデックフォールバック**
   - H.264で問題がある場合、H.263 → H.261 の順で自動的にフォールバックします

### ビデオ送信が2フレーム目以降で停止する（Buffer too small エラー）

H.323 Plus ライブラリに既知のバグがあります。以下のエラーが出る場合：

```
Buffer too small (2000 bytes), required: 1382400 bytes
Failed to read data from video grabber
```

**原因**: `h323pluginmgr.cxx` の `bytesPerFrame = outputDataSize;` という行が、
生フレームサイズ（1,382,400バイト）をMTUサイズ（2,000バイト）で誤って上書きしています。

**修正方法**:

1. `h323plus/src/h323pluginmgr.cxx` を編集
2. 1858行目付近と1960行目付近の以下の行をコメントアウト:
   ```cpp
   // bytesPerFrame = outputDataSize;  // BUG FIX
   ```
3. H.323 Plus を再ビルド: `cd h323plus && make`

詳細は [READMEのH323Plus映像パッチ手順](../README.md#required-h323plus-video-patch) を参照してください。

### 音声が聞こえない/届かない

1. **音声デバイスの確認**
   ```bash
   # 利用可能なデバイスを表示
   ./h323askw --list-audio-devices
   ```

2. **システムの音量設定を確認**
   - 入出力デバイスが正しく設定されているか確認

3. **ミュート状態の確認**
   - 通話中にマイクミュートになっていないか確認

### ログの取得方法

問題報告時は以下のログを取得してください:

```bash
# 詳細ログを取得
./h323askw [通常のオプション] -t 6 -o ~/Desktop/h323askw_debug.log 2>&1

# ログファイルの場所
~/Desktop/h323askw_debug.log
```

---

## サポート

### ログファイルの場所

- アプリケーションログ: 起動時に `-o` オプションで指定した場所
- システムログ: `Console.app` で「h323askw」を検索

### よくある質問

**Q: Intel Macで使えますか？**

A: 現在はApple Silicon専用です。Intel Mac対応版は別途ビルドが必要です。

**Q: WindowsやLinuxで使えますか？**

A: このApp BundleはmacOS専用です。他のプラットフォームでは、ソースからビルドする必要があります。

**Q: 複数の通話を同時にできますか？**

A: いいえ、1対1の通話のみ対応しています。

---

## 更新履歴

| バージョン | 日付 | 内容 |
|-----------|------|------|
| 1.0.0 | 2025-12-05 | 初版リリース。H.264/H.263/H.261ビデオコーデック対応。Qt6 UI搭載。 |

---

## ライセンス

CallGen323由来のソースファイルにはMozilla Public License Version 1.0
(MPL 1.0)が適用されます。H323ASKW独自のソースファイルやビルドスクリプトも、
別記のない限りMPL 1.0で配布します。ライセンス全文、派生元、変更履歴は
[`LICENSE.md`](../LICENSE.md)、[`NOTICE.md`](../NOTICE.md)、
[`CHANGES.md`](../CHANGES.md)を参照してください。

H323Plus、PTLib、Qt、FFmpegなどの第三者コンポーネントには、それぞれの
ライセンスが適用されます。詳細は
[`THIRD-PARTY-NOTICES.md`](../THIRD-PARTY-NOTICES.md)を参照してください。

---

## ソースからビルドする場合の注意事項

### H.323 Plus ライブラリの必須パッチ

ソースからビルドする場合、**H.323 Plus ライブラリに既知のバグがあるため、必ず以下のパッチを適用してください**。
このパッチを適用しないと、ビデオ通話が正常に動作しません。

#### バグの概要

`h323pluginmgr.cxx` の `bytesPerFrame = outputDataSize;` という行が、
ビデオフレームバッファサイズを誤ってMTUサイズ（2000バイト）で上書きし、
2フレーム目以降のカメラ読み込みが失敗します。

#### パッチ適用手順

1. **ファイルを編集**: `h323plus/src/h323pluginmgr.cxx`

2. **1858行目付近を修正**:
   ```cpp
   dst.SetMinSize(outputDataSize);
   // bytesPerFrame = outputDataSize;  // BUG FIX: This was incorrectly overwriting the raw frame size with MTU size
   ```

3. **1960行目付近を修正**:
   ```cpp
   bufferRTP.SetMinSize(outputDataSize);
   // bytesPerFrame = outputDataSize;  // BUG FIX: This was incorrectly overwriting the raw frame size with MTU size
   ```

4. **H.323 Plus を再ビルド**:
   ```bash
   cd h323plus
   make clean
   make
   ```

#### 詳細ドキュメント

パッチの詳細、症状の説明、検証方法は
[READMEのH323Plus映像パッチ手順](../README.md#required-h323plus-video-patch) を参照してください。

#### パッチ未適用時の症状

- "Buffer too small (2000 bytes), required: 1382400 bytes" エラー
- 相手側で映像が固まる、または表示されない
- H.264エンコーダーが全てI-Frameを出力する
- カメラFPSが著しく低下する
