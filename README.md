# Offline PDF Translator

ローカルのPDFを左側に、日本語訳を右側に表示するデスクトップアプリです。
PDFの描画・文字抽出・翻訳を同じコンピューター上で処理します。
推論サーバー、Python、ブラウザー、オンラインアカウントを実行時に必要としません。

![製品icon](resources/app-icon.png)

Built with PLaMo

## 使い方

1. リリースZIPを展開し、アプリを起動します。
2. PDFを開くか、ウィンドウへドロップします。
3. 同梱の `PLaMo-Community-ja.txt` で利用条件を確認し、メニューバーの「モデル設定」から「モデルを選択…」を開き、同じフォルダーの `plamo-3-translate-0.8b-Q4_K_M.gguf` を選びます。
4. 現在ページの訳文が表示されます。「全ページを翻訳」で他のページも処理できます。

バージョン0.1.0のリリースZIPには、アプリ、Q4_K_MのGGUF、`README.md`、`PLaMo-Community-ja.txt` を同梱します。これらを同じフォルダーに展開し、翻訳したいPDFを開いて使います。
起動時の右枠には「Built with PLaMo」とモデル提供元を表示します。「PLaMoコミュニティライセンス契約」は、EXEまたは.appと同じフォルダーに配布する `PLaMo-Community-ja.txt` をアプリ内で開きます。

Windows x64版はEXEと同梱ファイルを同じフォルダーに置いて使います。対応するCPUではAVX2・FMA・F16Cを使い、
モデル準備完了時に `CPU (AVX2)` と表示します。従来のCPUでは同じEXEが `CPU (portable)` を選びます。
対応モデルは **PLaMo 3 Translate 0.8B、英語→日本語** です。常体と敬体を選べます。
基準にした公式GGUFは次の通りです。

| 項目 | 固定値 |
| --- | --- |
| 配布元 | [pfnet/plamo-3-translate-0.8b-GGUF](https://huggingface.co/pfnet/plamo-3-translate-0.8b-GGUF) |
| revision | `1cf96ae02133958a20074bfdfab76517bb737d00` |
| ファイル | `plamo-3-translate-0.8b-Q4_K_M.gguf` |
| SHA-256 | `70891ec70dc7ab4205745a38307867c2881b99239f20327927633abfe4bc9ec6` |

モデルの利用には、アプリのMIT Licenseとは別に[PLaMoコミュニティライセンス契約](https://huggingface.co/pfnet/plamo-3-translate-0.8b-GGUF/blob/1cf96ae02133958a20074bfdfab76517bb737d00/LICENSE/ja)が適用されます。
商業目的の条件や、生成物の提供・公開・学習への利用条件も確認してください。
契約本文は同梱の[PLaMoコミュニティライセンス契約](PLaMo-Community-ja.txt)で読めます。アプリの「ライセンス」にも収録しています。
アプリは利用者の代わりに登録・契約同意をしません。

## 読書と翻訳

原文の左右にある矢印で前後のページへ移動します。「原文」の横には現在ページの入力欄と総ページ数があり、左のPDF表示枠の上にある倍率欄でPDFの大きさを変えられます。訳文は同じページに追従し、スクロール・訳文字サイズは独立しています。
原文の選択・コピーとPDF内検索が使えます。抽出した読順を確認・修正する原文編集もできます。
原文編集はPDF原本やその検索結果を変更しません。文字情報のないPDFへのOCRは行いません。

現在ページの自動翻訳は初期状態でONです。翻訳中でもウィンドウを閉じて終了できます。
見出し・本文・表・数式を読順に並べ、1ページの全文を1回でモデルに渡します。原文を編集したページでは編集後の全文を使います。
ページ全文がモデルの入力容量を超えた場合は、そのページに理由を表示します。
「翻訳」メニューの「このページを最初から再翻訳」で新しい試行を始められます。
全ページ翻訳は現在ページを優先し、完成・テキストなし・部分結果・失敗を区別します。

| 操作 | ショートカット |
| --- | --- |
| PDFを開く | Primary+O |
| ページ番号へ | Primary+G |
| PDF内検索 | Primary+F |
| 全ページを翻訳 | Primary+Shift+Enter |
| モデル設定 | Primary+, |
| ペイン間移動 | F6 / Shift+F6 |

PrimaryはMacではCommand、WindowsではCtrlです。IME変換やダイアログの入力を優先します。
詳しい操作方法とライセンスは、アプリ内でオフラインのまま読めます。
UTF-8書き出しはモデル名・文体・ページ番号を記録し、部分結果を明示します。

## 保存とプライバシー

設定・読書位置・原文編集・翻訳キャッシュは、新しい製品名のユーザーデータ領域に保存します。
以前のアプリの設定や資産は移行しません。PDF原本やGGUFは書き換えません。

| OS | 通常の状態ディレクトリ |
| --- | --- |
| macOS | `~/Library/Application Support/Offline PDF Translator/` |
| Windows | `%LOCALAPPDATA%\Offline PDF Translator\` |

`state.sqlite3` は設定・原文編集・読書位置、`Cache/cache.sqlite3` は再生成可能な翻訳結果です。
キャッシュ削除は原文編集を消しません。保存できない場合や同じ保存先を別の起動が使用中の場合は、メモリ内で処理を続け、未保存と表示します。
「状態データの退避コピーを保存」で、メモリ内の編集も別のSQLiteファイルへ救出できます。
復元手順は[保存・リソースの記録](docs/resources-and-diagnostics.md)を参照してください。
検証用の独立した状態領域や退避データの復元先を使う場合は、実行ファイルに
`--data-dir /absolute/path/to/state` を指定できます。通常起動の保存先は上表のままです。

本文・訳文・診断情報を自動送信する機能はありません。診断情報は内容を表示してからコピーできます。
通常ログには本文、訳文、パスワード、ファイルpathを入れず、最大256 KiB×3世代で保持します。

## 対象と検証状況

| 構成 | 実装方針 / 記録 |
| --- | --- |
| Windows x64 | 静的CRT、CPU推論。Releaseビルド・24件の自動試験・実GGUF翻訳・製品GUIを実機で確認 |
| macOS arm64 | 自己完結した.app、Metal自動選択とCPU経路。開発Mac上で実モデル・GUI・自動試験を検証 |
| macOS x86_64 | 自己完結した.app、CPU推論。クロスビルド、24件、実モデルCPU、製品GUI・長文表示をRosettaで検証。実Intel機では未検証 |

Macのdeployment targetは14.0です。実行確認したOSは開発Macの27.2で、macOS 14実機の確認を意味しません。
Developer ID署名・公証済みの一般配布版は作成していません。package scriptのMac出力はad-hoc署名の開発成果物です。
Gatekeeperの初回起動、開発環境のないクリーンな実機、Windowsの英語環境・DPI・IME、複数モニターのscale変更は別途確認が必要です。
現在の証拠と限界は[実装状況](docs/implementation-status.md)と各検証記録に分けて記載しています。
[配布検証](docs/distribution.md)では、別配置・新規状態・ネットワークと開発workspaceの読取を禁止した条件での製品実行も記録しています。

## 開発者向けビルド

Pythonは依存取得・ビルド補助にだけ使います。製品へ同梱しません。
Python 3.11以降、Git、CMake、NinjaとOSの開発SDKを用意します。
Macで検証した環境はXcode 27.2 / SDK 27.2です。
WindowsはVisual StudioのC++ Build ToolsとWindows SDKを用意し、対応する開発者用shellで実行します。
PDFiumの固定版が要求するSDK/toolchain条件に従います。

```sh
python3 -m venv .tools/venv
# Mac: source .tools/venv/bin/activate
# Windows PowerShell: .tools\venv\Scripts\Activate.ps1
python -m pip install -r deps/build-tools.txt
python scripts/bootstrap.py --target macos-arm64-release
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release
ctest --preset macos-arm64-release
python scripts/package.py --target macos-arm64-release --model-file /absolute/path/to/plamo-3-translate-0.8b-Q4_K_M.gguf
```

Windowsではtargetを `windows-x64-release` に変更します。
他に `windows-x64-debug`、`macos-arm64-debug`、`macos-x64-release` があります。
Mac x64はApple Siliconからクロスビルドできますが、Rosettaの試験は実Intel機の代わりにはなりません。
Debugには対応するDebug依存を別に構築します。

bootstrapは公式の固定ソースを `.work/` へ取得し、PDFiumをGN/Ninjaで構築します。
wxWidgets・llama.cpp・SQLiteとアプリはCMakeで静的に構築します。
通常のCMake configure/buildはネットへ依存を取りに行きません。PDFiumと同じ固定LLVM compiler/linkerを使います。
presetは開発機固有の `CPATH` / `LIBRARY_PATH` 等を子ビルドから除外します。
依存revision、モデルfixture、フォントhashは[dependency lock](deps/third_party.lock.json)にあります。
上流ソースの独自patchはありません。

packageは、コミット済みのcleanなソースと一致するbuild identityを持つ実行ファイルだけを受け付けます。
Releaseのpackageは、指定したGGUFを固定した公式SHA-256と照合し、アプリ・GGUF・README・PLaMoライセンスを含むZIPを作ります。
最終Mach-O / PEの通常・delay importを検査し、Macは署名後のZIPを再展開して再検査します。
生成物とhash・依存検査・ソース識別情報は `dist/<target>/` 以下へ保存します。
製品の起動にREADME、Python、ビルド用manifestや個別ライブラリの配布は不要です。

## 試験と構造

```sh
# 実ネイティブ長文試験プログラムも構築する場合
cmake --preset macos-arm64-release -DOPT_BUILD_GUI_PROBE=ON
cmake --build --preset macos-arm64-release
build/macos-arm64-release/native_text_test.app/Contents/MacOS/native_text_test
```

このネイティブ試験は、フォーカスを持てるデスクトップsessionで実行します。
ヘッドレスCTestと、別途ローカルGGUFを指定する実モデル試験を分けています。
fixtureはこのプロジェクトのために新規作成したPDFです。フォント内蔵のUnicode fixtureの再生成だけは
`deps/fixture-test-tools.txt` の開発用依存を使います。
公式tokenizer比較fixtureの生成手順と固定版は[翻訳の記録](docs/translation.md)を参照してください。

- `src/pdf/`: PDFium専用worker、段階的tile描画、layoutと原文mapping
- `src/inference/`: llama専用worker、PFML3、実token長に基づく分割
- `src/translation/`: page attempt、再開・再分割・lazy batch
- `src/storage/`: I/O専用worker、state/cache、同一open objectのhash、書き出し
- `src/platform/`: native path・font・IME・text viewportの小さなOS差分
- `tests/`: 所有権・競合・cache・Unicode・大判PDF・実モデルの試験

## ライセンス

このプロジェクトの新規コード・文書・fixture・iconは[MIT License](LICENSE)です。
静的に組み込む依存、フォント、別途選択するモデルにはそれぞれの条件が適用されます。
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)は固定ソースから生成した完全な通知文です。
`python scripts/collect_notices.py --check` で入力と通知文のhashを照合できます。
`python scripts/audit_publication.py` は削除済み内容を含む公開branchの履歴と現ソースを確認し、検出値を表示せずに問題の場所だけを報告します。
依存やモデルの利用条件をアプリのMIT Licenseに読み替えるものではありません。
