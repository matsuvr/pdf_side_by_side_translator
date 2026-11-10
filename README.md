# Offline PDF Translator

PDFの原文を左に、日本語訳を右に表示するオフライン翻訳アプリです。
読んでいるページをローカルのPLaMo 3 Translate 0.8Bで翻訳します。

Built with PLaMo

## 使い方

1. [リリースページ](https://github.com/matsuvr/pdf_side_by_side_translator/releases/latest)から、使うOSのZIPをダウンロードして展開します。
2. Windowsでは `offline-pdf-translator.exe`、Macでは `Offline PDF Translator.app` を起動します。
3. 「モデル設定」→「モデルを選択…」で、同梱の `plamo-3-translate-0.8b-Q4_K_M.gguf` を指定します。
4. PDFを開きます。左に原文、右に現在のページの日本語訳が表示されます。

原文の左右の矢印でページを移動します。ページ番号を入力すると、そのページへ移動します。
PDFの倍率は原文枠の上部で、訳文の文字サイズは「表示」メニューで調整します。
原文や訳文の必要な部分を選択してコピーします。

キャッシュは以下のフォルダーに保存されています。

| OS | 保存先 |
| --- | --- |
| Windows | `%LOCALAPPDATA%\Offline PDF Translator\Cache\` |
| macOS | `~/Library/Application Support/Offline PDF Translator/Cache/` |

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
リリースZIPには、公式のQ4_K_M GGUFと `PLaMo-Community-ja.txt` を同梱しています。
モデルの利用条件は、このPLaMoコミュニティライセンス契約をご確認ください。

- [公式モデル配布元](https://huggingface.co/pfnet/plamo-3-translate-0.8b-GGUF)
- [アプリのライセンス（MIT）](LICENSE)
- [第三者ライブラリ・フォントのライセンス](THIRD_PARTY_NOTICES.txt)
