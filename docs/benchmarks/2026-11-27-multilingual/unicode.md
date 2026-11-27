# ヒンディー語の置換文字と53言語のモデル入力

2026年11月27日、Windows x64 ReleaseのOCR対応ビルドで検証した。モデルに渡す入力の正解は、Preferred Networks（PFN）のPLaMo 3 Translate 0.8B公式Pythonトークナイザーと翻訳テンプレートで生成した。元のGGUFを素のllama.cppで動かす構成、パッチ版llama.cpp、Rust S4の3構成を、それぞれ独立に照合した。

## PFN公式実装と推論入力の一致

公式の `Plamo3Tokenizer.apply_chat_template(..., tokenize=True, add_generation_prompt=True)` に、ビューアーと同じ原文と翻訳オプションを渡した。原文言語は省略し、通常の日本語は `target_lang="ja", form="plain"`、やさしい日本語は `target_lang="ja(easy)"` とした。Transformersは4.57.1。GGUF用の固定版 `8b4d4285` とRustモデルの変換元 `c5a33c56` の間で、語彙、Python実装、テンプレート、設定2ファイルのSHA-256がすべて一致した。[PFN公式テンプレート](https://huggingface.co/pfnet/plamo-3-translate-0.8b/blob/8b4d42855622093e610ba9b90c2d2b59f8d5c15d/chat_template.jinja)／[PFN公式トークナイザー](https://huggingface.co/pfnet/plamo-3-translate-0.8b/blob/8b4d42855622093e610ba9b90c2d2b59f8d5c15d/tokenization_plamo.py)

修正前のGGUF版は、本文に `a<|plamo:tag|>b` とあると、予約トークンを削って残りを翻訳していた。公式は `<|plamo:` を含む本文全体を拒否する。また、公式が受理する `a:plamo|>b` の本文IDは `[97, 258, 98]` だが、GGUF版は中央のIDも除いていた。この2例の受理・拒否と、受理時の入力IDを公式に揃えた。

| 修正後の照合 | 元のGGUF＋素のllama.cpp | 元のGGUF＋パッチ版 | Rust S4 |
|---|---:|---:|---:|
| 公式が受理した入力。BOS・翻訳指示・本文・末尾指示を含む全トークンIDが順序まで一致 | 1,676/1,676 | 1,676/1,676 | 1,676/1,676 |
| 公式が拒否した入力。同じ本文を推論前に拒否 | 128/128 | 128/128 | 128/128 |

元のGGUFのトークナイザー単体にも、公式テンプレートが出力したUTF-8プロンプト全文を直接渡した。プロンプトに先頭トークン（BOS）が含まれるため二重に追加せず、素のllama.cppの `llama_tokenize` が返した全1,676件のID列がPFNと一致した。これでビューアー独自の前処理による差を切り分けた。

さらに、速度測定で使った10言語の実PDFを各3構成で開き、生成関数に渡す直前の全入力IDを開発用プローブで取得した。ページごとの原文を公式テンプレートに通したID列と30回すべて一致し、全文の翻訳が完了した。ガイドブックのヒンディー語は2,214トークン、英語は556トークンだった。

対象は53言語の例文とそのPDF抽出文、空白・改行・結合文字を含む文、単独コードポイント、語彙内の予約表記、埋め込みNUL、速度測定に使用した10言語の実文書。`A\0B🙂` ではNULより後ろの文字も入力に残った。PDFから抽出したUTF-8をバイト数付きでトークナイザーに渡し、生成には32ビット整数のIDとその個数を渡す。入力に使う値の受け渡しにはOSのコードページやUTF-16の文字数を使っていない。

## 公式実装と入力を揃えた後の速度

3構成とも既定のCPU設定で10言語の各ページを3回ずつ翻訳した。各試行で、原文のUTF-8とモデルに渡した全トークンIDをPFN公式の検証データと照合した。[修正後の言語別タイムと全90試行の記録](speed-after-reference.md)

## ヒンディー語の原文を修正した時点の速度

基準は素のllama.cppと元のGGUFを同じビューアーのCPU経路で使う構成。3構成とも原文言語は自動、出力は通常の日本語、生成はgreedy。使用したCore i9-13900では、アプリの既定スレッド数は8。各ページで3構成に渡る原文のUTF-8と入力トークン数を照合した。

| 実装 | 10ページの翻訳時間合計 | 素の版に対する速度 | 10ページの起動から終了までの合計 | ヒンディー語1ページの翻訳時間 |
|---|---:|---:|---:|---:|
| Rust＋独自量子化 S4 v7 | 41.67秒 | 1.73倍 | 47.88秒 | 4.88秒 |
| パッチ版llama.cpp＋元のGGUF | 51.76秒 | 1.39倍 | 65.36秒 | 6.56秒 |
| 素のllama.cpp＋元のGGUF | 72.20秒 | 1.00倍 | 84.59秒 | 9.47秒 |

10ページ合計は言語ごとの3回の中央値を足した値。入力の変わったヒンディー語だけを再測定し、他の9言語は前回の実測値を使用した。修正後のヒンディー語は9試行すべて同じ2,258文字・2,214入力トークンで完了した。翻訳時間には入力評価と日本語生成を含む。「起動から終了まで」にはモデル読込、PDF抽出、該当ページのOCRも含む。[10言語別の実測記録](README.md)／[修正後のヒンディー語の9試行](hindi-recovered.csv)

本測定はこのPCの既定値と同じ8スレッドを明示した。追加確認として、スレッド数を含む推論用環境変数を外し、ヒンディー語の同じページを各構成で1回ずつ翻訳した。原文のSHA-256と2,214入力トークンが3構成で一致し、翻訳時間はRust S4が4.62秒、パッチ版が6.04秒、素の版が9.47秒だった。

## ヒンディー語の実文書

| 文書と物理ページ | 埋め込みテキストの置換文字数 | 翻訳原文の取得方法 | 原文のUnicode文字数 | 原文の置換文字数 | 通常の日本語向け入力トークン数 |
|---|---:|---|---:|---:|---:|
| 出入国在留管理庁『生活・就労ガイドブック』16ページ | 59 | ページ画像からヒンディー語OCR | 2,258 | 0 | 2,214 |
| ニューヨーク市 IDNYC申請案内 2ページ | 0 | 埋め込みテキスト | 7,686 | 0 | 7,574 |

ガイドブックは、PDFフォントの `ToUnicode` と埋め込みの代替テキスト `ActualText` の両方に U+FFFD が記録されていた。アプリは、この文字を検出したページで、残っているテキストから読み取り言語を推定し、ページ画像をメモリ上で読み直す。読み取り結果にも置換文字が残った場合は翻訳を保留する。ガイドブックのOCR結果では本文の在留カード発行の案内を読み取れたが、見出し番号や罫線の誤認識が残った。[ガイドブックの該当ページ](https://www.moj.go.jp/isa/content/001450902.pdf#page=16)／[IDNYC申請案内の該当ページ](https://www.nyc.gov/assets/idnyc/downloads/pdf/application-materials/application_hindi.pdf?download=1#page=2)

2文書とも、パッチ版とRust版でページ全文のUTF-8バイト列が一致し、日本語訳が完了した。さらに各ページを「通常の日本語」「やさしい日本語」の両方で元のGGUFと照合し、両実装とも4通りの入力トークン列がすべて一致した。ガイドブックは両方のビューアーでも16ページ目を開き、置換文字を含まない日本語訳が完了表示になることを確認した。

## 53言語の照合

対応言語一覧とテスト用例文の言語タグを照合した。中国語の簡体字・繁体字を別項目として数えている。入力互換性の基準はPFN公式実装。語彙とUTF-8の往復には未改変のllama.cpp `8d60873df` とPLaMo 3 Translate 0.8B Q4_K_M GGUFも使用した。独自量子化モデルはP4X86 S4 tile v7。[対応言語一覧](../../../deps/image_reading_languages.tsv)／[検証に使った全例文](../../../tests/fixtures/plamo3-language-samples.tsv)

| 検査 | 結果 |
|---|---|
| GGUFとS4の語彙データ | 全107,520 IDで文字列・スコア（float32）・トークン種別が一致。1バイトごとのトークン256個も一致 |
| 各言語の例文、結合文字等を加えた例文、各言語のPDF抽出文 | 53言語 × 3件。元のGGUFでトークン化・復号し、159件すべてが元のUTF-8バイト列と一致 |
| アプリが推論に渡す完全なプロンプト | 上の159件 × 日本語の2文体。全318件で、パッチ版・Rust版とも基準の全トークンIDと順序が一致 |
| Rustのトークン出力 | 53言語 × 2件。実モデルのトークンから出したバイト列と復号文字列が全106件で原文に一致 |
| A4で1言語1枚に印刷した53ページ | パッチ版・Rust版のPDF抽出文が全ページでバイト単位に一致。置換文字は全ページで0個 |
| Unicodeコードポイントを1文字ずつ入力 | 53言語の検証文に現れた522種類と、デーヴァナーガリー基本ブロック（U+0900–U+097F）の全128種類を照合。重複を除く610種類 × 2文体の全1,220件で、公式テンプレートの受理・拒否および受理時の全入力IDが3構成で一致 |

混合例文では元の文をUnicode NFDに分解し、デーヴァナーガリー数字、結合アクセント、ゼロ幅接合子を含む絵文字、補助面の漢字を加えた。PDF化前の例文とアプリの抽出結果は41言語で完全一致した。残りの12言語（ar、fa、gu、he、km、ml、my、pa、sl、te、th、ur）には、右から左に読む文の逆順、空白の欠落、文字の重複や余分な文字など、両実装に共通の抽出差があった。53言語の一致数はモデル入力の互換性を表す。翻訳の意味やOCRの全文精度は採点していない。

### 再検証

プロジェクトルートで、`deps/tokenizer-test-tools.txt` のPython環境を使う。`--official-tokenizer` の5ファイルは実行前に固定ハッシュを検証する。`--reference` はGGUFのUTF-8往復試験用。`--upstream-probe` は素のllama.cppと製品の入力処理をリンクした開発用実行ファイル。`--corpus` の各行は「ページ名、タブ、原文UTF-8の16進表現」。`--pdf` の各ページは例文の言語順に並べる。新しく印刷するには `--chrome` にChromeの実行ファイルを渡す。[照合スクリプト](../../../scripts/check_multilingual_tokenizer.py)

```powershell
$build = 'build/windows-x64-release'
$rust = ".work/long-page/$build"
python -X utf8 scripts/check_multilingual_tokenizer.py `
  --official-tokenizer .work/unicode-verification/pfn-reference `
  --reference .work/unicode-verification/pristine-reference.exe `
  --upstream-probe .work/unicode-verification/pfn-vanilla-probe.exe `
  --gguf-probe "$build/inference_probe.exe" --rust-probe "$rust/inference_probe.exe" `
  --gguf .work/release-model/plamo-3-translate-0.8b-Q4_K_M.gguf `
  --rust-model "$rust/plamo3-translate-0.8b-x86-s4tile-v7.p4x86" `
  --pdf .work/unicode-verification/53.pdf --codepoints `
  --corpus .work/unicode-verification/pfn-real-pages.tsv
```
