# ネイティブ版のビルドとテスト

開発時に依存パッケージを取得します。実行時のPDF処理・OCR・推論はオフラインです。ビルドは各OSでリポジトリのルートから行います。Python 3.9以降、Git、rustupが必要です。Rustは `rust-toolchain.toml` に固定しています。

## Windows x64

Visual StudioのMSVC x64ビルドツール、Windows SDK、.NET 8以降のSDKを使用します。WinUIと.NETランタイムはビルド時に配布フォルダーへ配置します。

```powershell
python scripts/build.py --model-file G:\Models\plamo3-translate-0.8b-x86-s4tile-v7.p4x86
.\build\native\windows-x64\offline-pdf-translator.exe
```

`--model-file` は開発時に検証済みモデルを配置するための引数です。省略するとアプリのみをビルドします。元のBF16から変換する場合は `scripts/convert_windows_model.py --help` を参照してください。

```powershell
$env:PDF_READER_TEST_MODEL = (Resolve-Path build/native/windows-x64/*.p4x86).Path
$env:OPT_P4X86_TEST_MODEL = $env:PDF_READER_TEST_MODEL
cargo test --locked --release --workspace -- --include-ignored
powershell -NoProfile -File tests/native/WindowsViewerTest.ps1 -Exe build/native/windows-x64/offline-pdf-translator.exe
```

## macOS / Apple Silicon

XcodeコマンドラインツールとCMakeが必要です。ビルドスクリプトが固定リビジョンのllama.cppを `.work` に取得し、Metalライブラリを埋め込みます。Rust側のC APIは同じチェックアウトのヘッダーから生成します。

```sh
python3 scripts/build.py --model-file "$HOME/Models/plamo-3-translate-0.8b-Q4_K_M.gguf"
open "build/native/macos-arm64/Offline PDF Translator.app"
export MACOSX_DEPLOYMENT_TARGET=14.0
export PDF_READER_TEST_MODEL="$PWD/build/native/macos-arm64/plamo-3-translate-0.8b-Q4_K_M.gguf"
cargo test --locked --release -p pdf-reader -- --include-ignored
```

SwiftUIのウィンドウから、訳文の表示・選択・標準フォント・ページ移動・終了処理を確認するテストは次のコマンドで実行します。テスト実行ファイルもモデルの隣に置きます。

```sh
out="$PWD/build/native/macos-arm64"
lib="$out/Offline PDF Translator.app/Contents/Frameworks"
swiftc -swift-version 5 -parse-as-library -DVIEWER_UI_TEST -target arm64-apple-macosx14.0 \
  -I crates/reader/include -L "$lib" -lpdf_reader -Xlinker -rpath -Xlinker "$lib" \
  apps/macos/Viewer.swift tests/native/MacViewerTest.swift -o "$out/mac-viewer-test"
"$out/mac-viewer-test" "$PWD/tests/fixtures/P01-simple.pdf"
```

モデル不要の回帰テストには `cargo test --locked --release -p pdf-reader` を使います。Macの初回は先に `python3 scripts/prepare_llama.py` を実行してください。ビルド、fixture生成、署名、ZIP作成はすべて開発マシンで行います。
