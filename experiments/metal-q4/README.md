# Apple Silicon向けPLaMo 3 Translate 0.8Bの4bit推論

翻訳エンジン、量子化器、GPUカーネルをRustで実装した。GPUカーネルはCubeCLからビルド時にMetalライブラリへ変換し、アプリに埋め込む。対象はmacOS 14以降、Apple GPU family 7以降。Metalデバイスを初期化できない場合は、ビューアーに翻訳エラーを返す。[CubeCL](https://github.com/tracel-ai/cubecl)

元のBF16重みを行方向に32個ずつまとめ、符号付き4bit整数とFP16スケール1個に変換する。スケールはグループ内の二乗誤差を反復して小さくする。埋め込み、全16層の投影、107,520語の出力層に同じ形式を適用した。スケール込みで行列の重みあたり4.5ビット。正規化係数はFP32、KVキャッシュはFP16で保持する。[変換処理](src/convert.rs)

入力処理では複数トークンの重みを共有し、32トークン×64出力のタイルをMetalのSIMDグループ行列演算で処理する。逐次生成では1つのSIMDグループが1出力行を担当し、レジスター上で4bitの重みを展開して内積を取る。1入力チャンクまたは1生成トークンに必要な全層の演算を1つのMetalコマンドバッファーにまとめる。[GPUカーネル](src/kernels.rs)・[推論ループ](src/engine.rs)

## 実測

MacBook Air、Apple M4（8コアGPU）、16 GB、macOS 27.2.1、Xcode 27.2。対照はベースコミット `fb71566` のMac版で使うllama.cppのMetal推論とQ4_K_Mモデル。モデルのファイルサイズは573,126,848バイトから504,729,523バイトへ11.9%減った。モデルの読み込み時間は各方式12回の中央値で1.409秒から1.001秒になった。[モデルのハッシュと構成](../../deps/models.json)・[llama.cppの固定リビジョン](https://github.com/ggml-org/llama.cpp/commit/8d60873dfb8619f2b5ed31adfbab07cccb40db5c)

| 入力ページ | 入力トークン | 生成トークン（従来 / Rust Q4） | 翻訳時間（従来 / Rust Q4） | 短縮率 | 最初の候補を取得（従来 / Rust Q4） |
|---|---:|---:|---:|---:|---:|
| P01-simple、1ページ目 | 61 | 43 / 43 | 0.366 / 0.348秒 | 5.0% | 0.046 / 0.054秒 |
| Human Development Report 2025、10ページ目 | 275 | 279 / 280 | 2.600 / 2.300秒 | 11.5% | 0.202 / 0.271秒 |
| 同報告書、7ページ目 | 696 | 745 / 735 | 7.702 / 6.936秒 | 9.9% | 0.599 / 0.965秒 |

各ページ・各方式を4プロセスで測定した。方式の順番を交互にし、毎回モデルを開き、同じ原文を2回翻訳した。表は2回目の中央値。翻訳時間はトークン化から訳文の完成までで、モデル読込、PDF抽出、描画を除く。両方式とも同じPoppler 26.08.0の抽出結果を与えた。長文ページの候補取得後の生成速度は104.9から123.2トークン/秒になった。暖機後も反復間の変動があり、長文のRust版は6.20～8.34秒だった。[全24プロセスの測定値](measurements.csv)・[報告書の英語版](https://hdr.undp.org/content/human-development-report-2025)

短いP01の訳文は両方式で一致した。報告書の2ページは訳文が異なる。従来版の最終正規化後の隠れ状態を固定して全語彙を評価すると、出力層の第1候補はP01で44/44、報告書10ページ目で275/280、7ページ目で734/746が一致した。これは出力層単独の検査で、翻訳品質の点数には換算できない。

中国語（簡体字）・韓国語・フランス語・ドイツ語・スペイン語・ロシア語の各1文をPDF経由で訳し、原文の抽出一致と「駅の窓口または事務所が午前9時に開く」という内容を確認した。多言語の長文での訳質と、M1実機での速度は未評価。[使用した原文](../../tests/fixtures/plamo3-language-samples.tsv)

GPUの行列積をスカラー参照値と照合し、入力タイルの端数、因果マスク、2,048トークンのスライディングウィンドウ境界をテストした。ファイル書き込みを禁止した環境でもAppKitの実画面で2ページを翻訳し、訳文の選択と読み取り専用表示を確認した。

## ビルドと追試

リポジトリのルートで実行する。Apple Silicon、XcodeとMetal Toolchainが必要。PFNの固定リビジョンから `model.safetensors` と `tokenizer.jsonl` を開発用ディレクトリに用意する。[PFNの固定リビジョン](https://huggingface.co/pfnet/plamo-3-translate-0.8b/tree/c5a33c561bc6a4fbac38c3460bbb384e15c0b231)

```sh
export MACOSX_DEPLOYMENT_TARGET=14.0
manifest=experiments/metal-q4/Cargo.toml
probe=experiments/metal-q4/target/release/plamo-metal-q4
q4=.work/plamo3-translate-0.8b-metal-q4.safetensors
cargo build --locked --release --manifest-path "$manifest"
"$probe" convert .work/bf16 "$q4"
python3 scripts/build.py --model-file "$q4"
cargo test --locked --release --manifest-path "$manifest" --lib
```

比較用プローブには開発用のllama.cpp、CMake、Popplerが必要。GGUFにはSHA-256が `70891ec70dc7ab4205745a38307867c2881b99239f20327927633abfe4bc9ec6` の旧版同梱ファイルを指定する。英語報告書のPDFはSHA-256が `8c836ddac50ab3694fd3c13f113d7e15acf46e04d42dcddb2c12d7372f7a6c37` の版を使用した。

```sh
python3 scripts/prepare_llama.py
cargo build --locked --release --manifest-path "$manifest" --features reference
gguf=.work/plamo-3-translate-0.8b-Q4_K_M.gguf
report=.work/hdr2025reporten.pdf
python3 experiments/metal-q4/benchmark.py "$probe" "$q4" "$gguf" \
  --page tests/fixtures/P01-simple.pdf 1 --page "$report" 10 --page "$report" 7
"$probe" compare-head "$q4" "$report" 7 "$gguf"
```
