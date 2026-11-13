# Offline PDF Translator

PDFの原文を左に、日本語訳を右に並べて読む、閲覧専用のオフラインPDFビューアーです。
読んでいるページを、同梱のPLaMo 3 Translate 0.8Bで英語から日本語に翻訳します。
原文・訳文の本文は読み取り専用で、必要な箇所の選択とコピーに対応しています。

Built with PLaMo

## 使い方

1. [リリースページ](https://github.com/matsuvr/pdf_side_by_side_translator/releases/latest)から、使うOSに対応した **0.1.2 以降**のアプリ用ZIPをダウンロードし、フォルダーごと展開します。ZIPにはアプリ、翻訳用GGUF、ライセンス文書が入っています。
2. Windowsでは `offline-pdf-translator.exe`、Macでは `Offline PDF Translator.app` を起動します。同じフォルダーの `plamo-3-translate-0.8b-Q4_K_M.gguf` を自動で読み込みます。
3. PDFを開きます。左に原文、右に現在のページの日本語訳が表示されます。

同梱GGUFが見つからない場合だけ、起動時にファイル選択画面が開きます。
選択をキャンセルしてもPDFを閲覧でき、後から「モデル設定」でGGUFを選べます。

原文の左右の矢印でページを移動します。ページ番号を入力すると、そのページへ移動します。
PDFの倍率は原文枠の上部で、訳文の文字サイズは「表示」メニューで調整します。
翻訳の文体は、標準の日本語とやさしい日本語から選べます。
本文は閲覧・選択・コピー用です。検索語、ページ番号、倍率、モデルの場所は、閲覧操作のために入力できます。

## 文書データの扱い

原文、訳文、読書位置、モデルの選択、診断情報は、アプリの起動中だけメモリ内で管理し、終了時に破棄します。
入力PDF・GGUFは読み取り専用で開きます。次回起動時は、新しい閲覧セッションで同梱GGUFを読み込みます。

表示用の訳文・描画キャッシュは、メモリ予算の範囲で保持します。
非常に複雑なページでは自動文字抽出を止めますが、PDFは引き続き閲覧できます。
0.1.1以前で保存された文書データは、利用者が手動で管理します。
データの扱いと旧版の保存先は、セッションデータの仕様に記載しています。[セッションデータの仕様](https://github.com/matsuvr/pdf_side_by_side_translator/blob/main/docs/session-data.md)

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

対応モデルはPLaMo 3 Translate 0.8B（英語→日本語）です。
リリースZIPには、次のファイルを同梱しています。

- アプリ本体
- `plamo-3-translate-0.8b-Q4_K_M.gguf` — 公式の翻訳モデル
- `PLaMo-Community-ja.txt` — モデルの利用条件
- `LICENSE` — アプリのMITライセンス
- `THIRD_PARTY_NOTICES.txt` — 第三者ライブラリとフォントのライセンス
- `README.md` — 使い方

GGUFはWindowsの `.exe` と同じ階層、Macの `.app` の外側の同じ階層に置きます。
モデルの利用条件は、このPLaMoコミュニティライセンス契約をご確認ください。

- [公式モデル配布元](https://huggingface.co/pfnet/plamo-3-translate-0.8b-GGUF)
- [アプリのライセンス（MIT）](LICENSE)
- [第三者ライブラリ・フォントのライセンス](THIRD_PARTY_NOTICES.txt)
