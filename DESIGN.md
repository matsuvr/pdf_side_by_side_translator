# Offline PDF Translator — 製品仕様

対象バージョン: 0.1.1

## 体験

英文PDFの原文を左に、日本語訳を右に表示する。
利用者が現在読んでいるページの全文をローカルモデルで翻訳し、ページ移動に追従する。
画面の中心は原文と訳文の二つの枠とする。

## 対象環境

| OS | 構成 |
| --- | --- |
| Windows x64 | CPU推論、静的CRT |
| macOS 14以降・Apple Silicon（M1以降） | arm64アプリ、Metal推論 |

製品名は **Offline PDF Translator**、Macのアプリ名は `Offline PDF Translator.app`、Windowsの実行ファイル名は `offline-pdf-translator.exe` とする。
製品名・バージョン・bundle IDは `cmake/Product.cmake` にまとめる。

## ページと翻訳

PDFを開くと現在ページを表示し、見出し・本文・表・数式を読順に並べてページ全文を一つの入力として翻訳する。
ページを移動すると進行中の翻訳を取り消し、移動先のページを処理する。
全ページ翻訳はv0.1.1で削除し、処理対象を現在ページに限定する。
入力容量や生成上限に達した結果は、状態を分けて表示する。

モデル設定はGGUFの選択とライセンス表示で構成する。
MacではMetal、WindowsではCPUを自動選択する。Metalの読込失敗時は内部で一度CPUへ再構築する。
PLaMo 3 Translate 0.8Bの標準文体は `lang=ja, form=plain`、やさしい文体は `lang=ja-easy` を使う。
旧敬体設定はやさしい文体へ移行し、旧キャッシュとの混同を避ける。

## 実装と保存

C++20、wxWidgets、PDFium、llama.cpp、SQLiteを使う。
GUI、PDFium、推論、I/Oの所有権を分け、非同期結果には文書・ページ・モデル・設定・試行の識別子を付ける。
翻訳キューは現在ページの一つの試行を保持する。ページ移動・モデル変更・文体変更では古い試行を無効化する。
翻訳キャッシュはモデル・文体・原文・推論設定を含む識別子で管理する。
状態とキャッシュはOSのユーザーデータ領域へ保存する。

## 配布

リリースZIPにはアプリ、固定した公式Q4_K_M GGUF、README、PLaMoコミュニティライセンス契約を同梱する。
パッケージはコミット済みソースと一致するビルド識別情報を持つ成果物から生成する。
署名後のアプリとZIP再展開後のアプリのハッシュを照合する。
開発手順は [docs/development.md](docs/development.md)、検証記録は [docs/implementation-status.md](docs/implementation-status.md) にまとめる。
初期設計は [docs/history/design-v0.1.0.md](docs/history/design-v0.1.0.md) に保存する。
