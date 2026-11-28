# Apple Silicon向けPLaMo 3 Translate 0.8BのネイティブMetal推論

翻訳エンジン、GGUF可逆リパッカー、Metal 3.1専用GPUカーネルを実装した。ビルド時に `src/kernels.metal` を `xcrun metal` / `metallib` で直接Metalライブラリへコンパイルし、アプリに埋め込む（CubeCL不要）。対象はmacOS 14以降、Apple GPU family 7以降（M1以降のApple Silicon専用）。Metalデバイスを初期化できない場合は、ビューアーに翻訳エラーを返す。[GPUカーネル](src/kernels.metal)・[推論ループ](src/engine.rs)・[変換処理](src/convert.rs)

## 量子化形式と高速化手法

- **公式GGUF（`Q4_K_M`）の部分量子化・重み値を完全一致させる可逆リパック**: PFN公式の `plamo-3-translate-0.8b-Q4_K_M.gguf`（644チャンクのimportance matrixで量子化）から全テンソルの量子化型・スケール・量子化整数を**1bitも変えずに**Apple Silicon向けの16バイト整列スーパーブロックへ再配置する。
  - **`Q4K_M`（49テンソル・4.50 bpw）**: `token_embd.weight`、全16層の `attn_output.weight` / `ffn_up.weight`、および `blk.{2,3,5,6,8,9,11,12}.{attn_qkv,ffn_down}.weight`。2個のGGUF `Q4_K` ブロック（512重み）を288バイト（`[hdr0(16B) | hdr1(16B) | linear q4(256B)]`）に配置。`ffn_up` は `gate`/`up` 行を交互配置しSwiGLUを融合。
  - **`Q6K_M`（17テンソル・6.75 bpw）**: `output.weight`（`lm_head`）および `blk.{0,1,4,7,10,13,14,15}.{attn_qkv,ffn_down}.weight`。2個のGGUF `Q6_K` ブロック（512重み）を432バイト（`[d0,d1(4B) | pad(12B) | scales[32](32B) | linear ql(256B) | linear qh(128B)]`）に配置。
  - **FP32正規化係数（97テンソル）**: GGUFのFP32重みをそのまま保持。残差ストリーム・RMSNorm・RoPE・内積/GEMM累算・Softmax・ロジットはFP32、中間活性バッファーとKVキャッシュはFP16で保持。
- **2段階ビットプレーン `lm_head`（1bit選別 → `Q6K_M` 厳密再評価）**: 全107,520語彙に対し、第1段で25.8 MBの1bit符号プレーン（256スレッド/TG・12 KB共有メモリ4bit LUT）により64語ブロックごとに上位4候補（計6,720候補）を抽出し、第2段で公式GGUFと完全同一の `Q6K_M` 重みで厳密内積を再計算する。全語彙フル評価と100%完全一致（`verify-head` で確認）しつつ出力層の計算量を約1/16に削減した。
- **単一Metalバッファー・ゼロコピー読込**: 全テンソルを16バイト境界に整列させ、1つの共有 `MTLBuffer` へ直接読み込みながらAppleのハードウェアSHA-256（`CC_SHA256`）で検証する。
- **入力処理（Prefill）の最適化**: 1チャンク最大1,024トークン、`simdgroup_half8x8 × simdgroup_half8x8 → simdgroup_float8x8`（64×64・32×64タイル、`Q4K_M` / `Q6K_M` 専用カーネル）、交互配置 `gate_up` による融合SwiGLU、MQA（16 Q : 1 KV）向けFlashAttention-2、最終層（Layer 15）末尾トークン限定スキップを実装。
- **逐次生成（Decode）の最適化**: 32レーンゼロシフト `uint2`・64スレッド/TGの `Q4K_M` / `Q6K_M` MatVecと融合SwiGLU、埋め込み＋正規化／Post-Norm＋残差＋Pre-Norm／Q・K正規化＋RoPE＋KV書込のカーネル融合、1パス4重アンロール遅延Softmax MQAアテンション、16トークン単位の一括コマンド発行を実装。

## 実測

MacBook Air、Apple M4（8コアGPU）、16 GB、macOS 27.2.1、Xcode 27.2。対照は旧Mac版で使うllama.cppのMetal推論と公式 `Q4_K_M` GGUF（573,126,848バイト）。本版のモデルサイズは `606,167,971` バイト（公式GGUFの可逆リパック＋25.8 MBの1bit選別インデックス＋tokenizer）。モデルの読み込み時間は各方式12回の中央値で1.405秒から0.321秒（77.1%短縮・4.37倍高速）になった。[モデルのハッシュと構成](../../deps/models.json)・[llama.cppの固定リビジョン](https://github.com/ggml-org/llama.cpp/commit/8d60873dfb8619f2b5ed31adfbab07cccb40db5c)

| 入力ページ | 入力トークン | 生成トークン（従来 / 本版） | 翻訳時間（従来 / 本版） | 短縮率（速度比） | 最初の候補を取得（従来 / 本版） |
|---|---:|---:|---:|---:|---:|
| P01-simple、1ページ目 | 61 | 43 / 43（SHA-256完全一致） | 0.333 / 0.220秒 | 34.2%（1.52倍） | 0.048 / 0.036秒 |
| Human Development Report 2025、10ページ目 | 275 | 279 / 279（SHA-256完全一致） | 2.082 / 1.414秒 | 32.1%（1.47倍） | 0.174 / 0.152秒 |
| 同報告書、7ページ目 | 696 | 745 / 747（類似度0.994） | 5.589 / 3.817秒 | 31.7%（1.46倍） | 0.404 / 0.361秒 |

各ページ・各方式を4プロセスで交互に測定し、毎回モデルを開いて同じ原文を2回翻訳した2回目の中央値を示す（翻訳時間はトークン化から訳文完成までで、モデル読込・PDF抽出・描画を除く）。長文ページの候補取得後の逐次生成速度は143.5から215.9トークン/秒（1.50倍）に向上した。長文4回の測定範囲は3.80〜3.82秒で、全反復で同一ハッシュの決定論的出力を得た。[全24プロセスの測定値](measurements.csv)・[報告書の英語版](https://hdr.undp.org/content/human-development-report-2025)

## 訳質・厳密性検証

- **`verify-head`（2段階 `lm_head` 対 全107,520語彙フル評価）**: P01 p1（43語）・HDR p10（279語）・p7（747語）・p20（927語）・p40（580語）・p100（802語）の計3,378生成トークンすべてで **100%完全一致**。
- **`compare-head`（従来版の最終隠れ状態を固定した全語彙出力層一致率）**: P01 p1 で **44/44（100.0%、RMS 0.00018）**、HDR p10 で **280/280（100.0%、RMS 0.00020）**、HDR p7 で **746/746（100.0%、RMS 0.00019）**（計 **1,070/1,070 = 100.0%**）。
- **全53言語サンプル対比（`compare-tsv`）**: **51/53言語で従来版（llama.cpp）と文字単位の完全一致**。残り2言語も浮動小数点丸め差による1語タイブレークのみ（`ko`: 開く/開かれる、`uz`: 朝食は済ませた/朝食は提供される）。[使用した原文](../../tests/fixtures/plamo3-language-samples.tsv)
- **PDF 18ページ対比（フィクスチャ4ページ＋HDR 2025の14ページ）**: **7/18ページで文字単位の完全一致**（フィクスチャ全4ページおよびHDR p10・p30・800トークン長文p120を含む）、残り11長文ページも文字数比 `0.992〜1.006`・類似度 `0.937〜0.999`・全18ページで数値完全一致・欠落ゼロ。多言語の長文での訳質とM1実機での速度は未評価。
- GPUカーネル単体テスト（`Q4K_M`/`Q6K_M` decode/prefill/融合SwiGLUのスカラー参照照合、因果・2,048スライディング境界）および `pdf-reader` 実モデル統合テスト全12件に合格。

## ビルドと追試

リポジトリのルートで実行する。Apple Silicon、XcodeとMetal Toolchainが必要。公式GGUF（SHA-256 `70891ec70dc7ab4205745a38307867c2881b99239f20327927633abfe4bc9ec6`）と `tokenizer.jsonl`（PFN固定リビジョン）を用意する。[PFNの固定リビジョン](https://huggingface.co/pfnet/plamo-3-translate-0.8b/tree/c5a33c561bc6a4fbac38c3460bbb384e15c0b231)

```sh
export MACOSX_DEPLOYMENT_TARGET=14.0
manifest=experiments/metal-q4/Cargo.toml
probe=experiments/metal-q4/target/release/plamo-metal-q4
gguf=.work/plamo-3-translate-0.8b-Q4_K_M.gguf
q4=.work/plamo3-translate-0.8b-metal-q4.safetensors
cargo build --locked --release --manifest-path "$manifest"
"$probe" convert "$gguf" .work/bf16/tokenizer.jsonl "$q4"
python3 scripts/build.py --model-file "$q4"
cargo test --locked --release --manifest-path "$manifest" --lib
"$probe" verify-head "$q4" tests/fixtures/P01-simple.pdf 1 1
```

比較用プローブには開発用のllama.cpp、CMake、Popplerが必要。英語報告書のPDFはSHA-256が `8c836ddac50ab3694fd3c13f113d7e15acf46e04d42dcddb2c12d7372f7a6c37` の版を使用した。

```sh
python3 scripts/prepare_llama.py
cargo build --locked --release --manifest-path "$manifest" --features reference
report=.work/hdr2025reporten.pdf
python3 experiments/metal-q4/benchmark.py "$probe" "$q4" "$gguf" \
  --page tests/fixtures/P01-simple.pdf 1 --page "$report" 10 --page "$report" 7
"$probe" compare-head "$q4" "$report" 7 "$gguf"
"$probe" compare-tsv "$q4" "$gguf" tests/fixtures/plamo3-language-samples.tsv
```
