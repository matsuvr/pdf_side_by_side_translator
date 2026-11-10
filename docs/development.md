# 開発

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
他に `windows-x64-debug`、`macos-arm64-debug` があります。
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
- `src/translation/`: page attempt、再開・ページ単位の翻訳
- `src/storage/`: I/O専用worker、state/cache、同一open objectのhash、書き出し
- `src/platform/`: native path・font・IME・text viewportの小さなOS差分
- `tests/`: 所有権・競合・cache・Unicode・大判PDF・実モデルの試験
