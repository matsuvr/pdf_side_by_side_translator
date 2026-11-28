# Offline PDF Translator

PDFを左に、そのページの日本語訳を右に表示するオフラインの閲覧専用アプリです。PDFを開くと、同梱のPLaMo 3 Translate 0.8Bが自動で翻訳します。次のページへ進むと訳も切り替わります。

Built with PLaMo

## 使い方

1. Windowsでは `offline-pdf-translator.exe`、Macでは `Offline PDF Translator.app` を起動します。
2. 「PDFを開く」で文書を選びます。矢印またはページ番号で移動できます。
3. 原文は初期状態でページ全体を表示します。「−」「＋」、倍率の直接入力、「幅に合わせる」で調整できます。
4. 原文はメニューの「ページの原文をコピー」でコピーできます。右側には生成中の訳文から順に表示され、そのまま選択・コピーできます。「検索」ではPDFの埋め込み本文から一致するページへ移動します。

訳文の書体と日本語のフォント補完はOSの標準に従います。本文を抽出できないページや、抽出時に文字が欠けたページでは、端末の文字認識を自動で試みます。画像から読み取れる言語はOSの対応状況に依存します。翻訳や文字認識ができないときも、元のページを表示します。

## 実行環境とモデル

| | Windows x64 | macOS 14以降・Apple Silicon |
| --- | --- | --- |
| UI | WinUI 3 / C# | SwiftUI / AppKit |
| 翻訳 | Rust製CPU推論 | Apple Silicon向けのRust製Metal推論 |
| 同梱モデル | `plamo3-translate-0.8b-x86-s4tile-v7.p4x86` | `plamo3-translate-0.8b-metal-q4.safetensors` |

モデルはWindowsでは実行ファイル、Macではアプリバンドルの隣に置きます。アプリはモデルを読み取り専用で開きます。PDF・原文・訳文・閲覧位置・キャッシュはセッション中だけ使用します。

このREADMEはリポジトリのネイティブUI版を対象としています。公開済みの旧版の操作は、対応するリリースとマニュアルに記載しています。[リリース](https://github.com/matsuvr/pdf_side_by_side_translator/releases) · [公開版のマニュアル](https://matsuvr.github.io/pdf_side_by_side_translator/manual.html)

[ビルドとテスト](docs/development.md) · [Quintによる形式検証](formal/README.md) · [設計](DESIGN.md) · [ライセンス](LICENSE) · [第三者通知](THIRD_PARTY_NOTICES.txt)
