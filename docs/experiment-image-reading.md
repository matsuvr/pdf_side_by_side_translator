# ページを画像から読み取る（Windows 実験版）

通常の Windows 版と同じ開発者用環境で、リポジトリの[ビルド手順](development.md)に従ってください。すでに通常版の依存を構築済みなら、次を追加で実行します。

```powershell
python scripts/bootstrap_image_reading.py
cmake --preset windows-x64-release -DOPT_PAGE_IMAGE_READING=ON
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R page_image_reading
```

同梱配置の GGUF を使用する場合は既存の `scripts/package.py` の Windows 手順を参照してください。単に実行ファイルを試す場合は、対応する GGUF を選択してください。画像の読み取りそのものは翻訳モデルを使いません。Tesseract、Leptonica、CLD2 と53言語のデータは実行ファイルに静的リンク・埋め込みされ、実行時の追加インストールやダウンロードは不要です。依存の取得に通信するのは開発者用 bootstrap だけです。通常ビルドは `-DOPT_PAGE_IMAGE_READING=OFF`、macOS は機能なしです。

## 試し方

1. PDFを開いて、読み取りたいページに移動します。PDFの上にある言語選択は「言語を自動で判別」が初期値です。必要に応じ、[PLaMo 3 Translateの53言語](https://huggingface.co/pfnet/plamo-3-translate-0.8b-GGUF#supported-languages)から明示的に選べます。中国語の簡体字・繁体字は別項目です。
2. **このページを画像分析で読み込む**を押します。現在表示中の1ページを、表示倍率とは別に原則220dpiで読み取ります。大判ページは画像寸法・総画素数の上限に収めます。
3. 右側に画像から読み取った文字が表示されます。既存の自動翻訳がオンでモデルが準備済みなら、その文字から翻訳し、見出しを「画像からの訳文」とします。オフなら読み取り結果をそのまま比較できます。
4. 別のページに移動すると通常の読み方に戻ります。元PDFを変更せず、画像や文字を作業ファイルに保存しません。元PDFの検索・範囲選択にはこの結果を使いません。

ページ切替・別文書の開閉中、古い読み取り結果が右に出ないことも試してください。正式版の採用に向けて、ネット切断、CPUのみ、GGUF使用時のページ送り、処理時間、空白ページと英中の実スキャン・細かい図文字を確認します。縦書き、範囲指定OCR、全ページ一括処理はこの実験版の対象外です。

## 言語の自動判別と限界

まずページ画像の文字体系を判別します。同じ文字体系に候補言語が複数ある場合は、その文字体系向けのモデルで認識した文をCLD2に渡し、十分な手掛かりがあるときだけ言語専用モデルで読み直します。判別が不確かなら特定の言語名は表示せず、言語を選び直すよう案内します。短いラベル、傾いた画像、複数言語の混在、漢字だけのページ、同系統の言語は特に手動指定を推奨します。対応は「モデルが同梱され、手動選択できる」の意味で、53言語すべての実資料で精度を検証したものではありません。

全モデルを一括ロードしません。手動は指定言語のモデル（中国語は英語も）、自動は判別用モデルと該当する認識モデルだけを順次使います。実行ファイルには読み取り用データとして約150 MiBが加わります。自動で全文を最大2回認識するため、処理速度を重視する場合は手動で指定してください。

OCRは同じ実行ファイルの子プロセスで動き、ページ画像と結果を匿名の共有メモリで受け渡します。ビューアーはページ切替・終了時、またはOCR開始から20秒で子プロセスを停止できます。Tesseractの文字領域は渡された画像から決定します。

固定した上流ソースは[取得スクリプト](../scripts/bootstrap_image_reading.py)、言語対応表は[一覧](../deps/image_reading_languages.tsv)、モデルのSHA-256は[ロック](../deps/image_reading_models.tsv)に記載しています。自動判別に使う共通文字体系モデルのみ、サイズ削減のため上流の単語辞書を除去しました。手動選択用モデルは無加工です。ライセンスはアプリ内の第三者通知と `licenses/` を参照してください。

## WindowsでのASan検証

```powershell
cmake --preset windows-x64-release -B build/windows-x64-ocr-asan -DOPT_PAGE_IMAGE_READING=ON -DOPT_OCR_ASAN=ON
cmake --build build/windows-x64-ocr-asan --target image_reading_test
ctest --test-dir build/windows-x64-ocr-asan -R '^page_image_reading$' --output-on-failure
```

この構成はOCR本体、Tesseract、Leptonica、CLD2、PDFから画像を受け渡すコードをASanで計測します。リンク時には `/OPT:REF,NOICF` を使います。ICFを有効にすると、異なるサイズの文字列定数が同じアドレスに統合され、ASanのグローバル変数登録と境界情報が衝突することを、PDFを開く前の起動試験で確認しています。
