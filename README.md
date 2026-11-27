# Offline PDF Translator

PDFの原文を左に、日本語訳を右に並べて読む、閲覧専用のオフラインPDFビューアーです。
読んでいるページを、同梱のPLaMo 3 Translate 0.8Bで日本語に翻訳します。原文の言語は自動で判定し、日本語・英語を含む53言語に対応しています。
原文・訳文の本文は読み取り専用で、必要な箇所の選択とコピーに対応しています。

Built with PLaMo

[製品紹介](https://matsuvr.github.io/pdf_side_by_side_translator/) · [使い方・マニュアル](https://matsuvr.github.io/pdf_side_by_side_translator/manual.html)

## 使い方

1. [リリースページ](https://github.com/matsuvr/pdf_side_by_side_translator/releases)から、使うOSに対応した **0.1.2 以降**のアプリ用ZIPをダウンロードし、フォルダーごと展開します。ZIPにはアプリ、翻訳モデル、ライセンス文書が入っています。
2. Windowsでは `offline-pdf-translator.exe`、Macでは `Offline PDF Translator.app` を起動します。同じフォルダーの同梱モデルを自動で読み込みます（Windows版はP4X86、Mac版は公式GGUF）。
3. PDFを開きます。左に原文、右に現在のページの日本語訳が表示されます。

Macで初めて起動するときは、「システム設定」→「プライバシーとセキュリティ」→「アプリケーションの実行許可」で、`Offline PDF Translator` の「このまま開く」を選び、実行を許可してください。

翻訳モデルは「モデル設定」で選択できます。

原文の左右の矢印でページを移動します。ページ番号を入力すると、そのページへ移動します。
PDFの倍率は原文枠の上部で、訳文の文字サイズは「表示」メニューで調整します。
翻訳の文体は、標準の日本語とやさしい日本語から選べます。

WindowsとMacで、右ペイン上部の「画像から読み取り (OCR)」をオンにすると、ページをめくるたびに画像内の文字をオフラインで読み取ります。読み取り言語をクリックして正しい言語を選ぶと、そのページを読み直します。選んだ言語は同じPDF内で引き継がれます。

## 文書データの扱い

原文、訳文、読書位置、モデルの選択、診断情報は、アプリの起動中はメモリ内で管理し、終了時に破棄します。
入力PDF・翻訳モデルは読み取り専用で開きます。次回起動時は、新しい閲覧セッションで同梱モデルを読み込みます。

非常に複雑なページでは自動文字抽出を止めますが、PDFは引き続き閲覧できます。

## ショートカット

Ctrl（⌘）はWindowsではCtrl、Macでは⌘キーです。

| 操作 | キー |
| --- | --- |
| PDFを開く | Ctrl（⌘）＋O |
| ページ番号へ移動 | Ctrl（⌘）＋G |
| PDF内を検索 | Ctrl（⌘）＋F |
| 選択したテキストをコピー | Ctrl（⌘）＋C |
| モデル設定 | Ctrl（⌘）＋, |
| ペイン間の移動 | F6 / Shift＋F6 |

## モデルとライセンス

対応モデルはPLaMo 3 Translate 0.8Bです。原文の言語は自動で判定し、日本語・英語を含む53言語から日本語へ翻訳します。
Windows x64版は、Rust製の推論エンジンと専用の4ビット量子化モデル（S4 v7）を標準で使用します。Mac版はllama.cppと公式GGUFを使用します。
リリースZIPには、次のファイルを同梱しています。

- アプリ本体
- Windows版: `plamo3-translate-0.8b-x86-s4tile-v7.p4x86` — PLaMo派生x86専用量子化モデル
- Mac版: `plamo-3-translate-0.8b-Q4_K_M.gguf` — 公式の翻訳モデル
- `PLaMo-Community-ja.txt` — モデルの利用条件
- `LICENSE` — アプリのMITライセンス
- `THIRD_PARTY_NOTICES.txt` — 第三者ライブラリとフォントのライセンス
- `README.md` — 使い方
- Windows: `Rust-1.100.0-COPYRIGHT-library.html` — Rust標準ライブラリの著作権通知

翻訳モデルはWindowsの `.exe` と同じ階層、Macの `.app` の外側の同じ階層に置きます。
モデルの利用条件は、このPLaMoコミュニティライセンス契約をご確認ください。

- [元のBF16モデル（Windows派生モデルの出典）](https://huggingface.co/pfnet/plamo-3-translate-0.8b)
- [公式GGUF配布元](https://huggingface.co/pfnet/plamo-3-translate-0.8b-GGUF)
- [アプリのライセンス（MIT）](LICENSE)
- [第三者ライブラリ・フォントのライセンス](THIRD_PARTY_NOTICES.txt)
