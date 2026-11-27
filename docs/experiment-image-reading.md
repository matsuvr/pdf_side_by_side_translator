> 旧版の実装・検証記録です。現行版の構成は[設計](../DESIGN.md)、ビルド方法は[開発手順](development.md)を参照してください。

# ページを画像から読み取る

WindowsとMacの通常ビルドでOCRを利用できます。リポジトリの[ビルド手順](development.md)に従ってください。既存の環境でOCR用の依存を追加して試す場合は、該当するOSのpresetを使います。

```sh
python scripts/bootstrap_image_reading.py
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release
ctest --preset macos-arm64-release -R page_image_reading
```

Windowsで実行する場合は `macos-arm64-release` を `windows-x64-release` に置き換えます。Tesseract、Leptonica、CLD2と53言語のデータは実行ファイルに組み込まれ、ネット接続なしで読み取れます。日本語への翻訳には同梱のGGUFを使います。

## 試し方

1. PDFを開き、右ペイン上部の「画像から読み取り (OCR)」をオンにします。表示中のページを原則220dpiで読み取り、右側に読み取り言語と結果を表示します。自動翻訳がオンでモデルが準備済みなら、その文字から日本語に翻訳します。
2. ページをめくると、新しいページで言語を判別します。読み取り言語が違っていたら、言語名をクリックし、正しい言語を選んでください。候補には判別器が挙げた言語と同じ文字体系の言語が並びます。「その他の言語」から残りの対応言語も選べます。
3. 選んだ言語で現在ページを読み直し、同じPDFの後続ページにもその言語を使います。ページごとの判別を再開するには、言語名をクリックして「自動判別に戻す」を選びます。
4. OCRをオフにすると埋め込みテキストを使います。埋め込みテキストに置換文字（�、U+FFFD）があるページは、自動で画像から読み取って翻訳します。残っている文字から読み取り言語を推定し、判別できない場合はページ画像から判定します。別のPDFを開くと手動の指定は解除されます。元PDFの検索・範囲選択には埋め込みテキストを使います。

## 言語の自動判別と限界

まずページ画像の文字体系を判別します。同じ文字体系に候補言語が複数ある場合は、その文字体系向けのモデルで認識した文をCLD2に渡し、十分な手掛かりがあるときだけ言語専用モデルで読み直します。言語が確定すると、右ペインにその言語名を表示します。自動判定に失敗した場合は翻訳を保留します。「言語を選択」から言語を選ぶと、その言語で画像を読み直して翻訳します。OCRの対象はPLaMo 3 Translateと同じ53言語で、中国語の簡体字・繁体字は別項目です。

全モデルを一括ロードしません。手動は指定言語のモデル（中国語は英語も）、自動は判別用モデルと該当する認識モデルだけを順次使います。実行ファイルには読み取り用データとして約150 MiBが加わります。自動で全文を最大2回認識するため、処理速度を重視する場合は手動で指定してください。

OCRは同じ実行ファイルの子プロセスで動きます。ページ画像と結果は、Windowsでは匿名の共有メモリ、Macでは匿名のローカルソケットで受け渡します。ビューアーはページ切替・終了時、またはOCR開始から20秒で子プロセスを停止できます。Tesseractの文字領域は渡された画像から決定します。

固定した上流ソースは[取得スクリプト](../scripts/bootstrap_image_reading.py)、言語対応表は[一覧](../deps/image_reading_languages.tsv)、モデルのSHA-256は[ロック](../deps/image_reading_models.tsv)に記載しています。自動判別に使う共通文字体系モデルのみ、サイズ削減のため上流の単語辞書を除去しました。手動選択用モデルは無加工です。ライセンスはアプリ内の第三者通知と `licenses/` を参照してください。

## WindowsでのASan検証

```powershell
cmake --preset windows-x64-release -B build/windows-x64-ocr-asan -DOPT_PAGE_IMAGE_READING=ON -DOPT_OCR_ASAN=ON
cmake --build build/windows-x64-ocr-asan --target image_reading_test
ctest --test-dir build/windows-x64-ocr-asan -R '^page_image_reading$' --output-on-failure
```

この構成はOCR本体、Tesseract、Leptonica、CLD2、PDFから画像を受け渡すコードをASanで計測します。リンク時には `/OPT:REF,NOICF` を使います。ICFを有効にすると、異なるサイズの文字列定数が同じアドレスに統合され、ASanのグローバル変数登録と境界情報が衝突することを、PDFを開く前の起動試験で確認しています。
