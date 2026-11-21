# Apple Silicon: 0.8B専用MLX / Metalエンジン

`feat/macos-plamo08-mlx` は、Mac版の推論をPLaMo 3 Translate 0.8B専用のC++ / Metal実装に切り替えるブランチです。標準起動では専用カーネルを使います。

## ビルドして起動

Apple SiliconのMac、macOS 14以降、XcodeのMetalコンパイラー、Python 3、CMake 3.25以降、Ninjaを使います。PDFiumとwxWidgetsの準備方法は従来のプロジェクトと同じです。

```sh
git fetch origin
git switch feat/macos-plamo08-mlx
python3 scripts/bootstrap.py --target macos-arm64-release
python3 scripts/prepare_mlx.py --model-only
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release
open "build/macos-arm64-release/Offline PDF Translator.app"
```

既にこのチェックアウトでPDFium等を準備済みなら、`bootstrap.py` の代わりに `python3 scripts/prepare_mlx.py` でMLX関連の依存とモデルを追加できます。その後のCMake構成・ビルドは同じです。

モデルは公式の [`pfnet/plamo-3-translate-0.8b-MLX-4bit`](https://huggingface.co/pfnet/plamo-3-translate-0.8b-MLX-4bit) です。取得スクリプトは固定リビジョンの次の5ファイルを、`.app` の隣の `plamo-3-translate-0.8b-MLX-4bit` フォルダーに置きます。重みは約976 MBです。

- `config.json`
- `model.safetensors`
- `tokenizer.jsonl`
- `tokenizer_config.json`
- `generation_config.json`

別の場所に取得する場合は `--model-dir /path/to/model` を指定します。アプリの「モデル設定」では、この5ファイルを含むフォルダーを選択します。GGUFからの変換は必要ありません。

取得と依存の準備は開発用Pythonスクリプトで行います。アプリはC++でモデルを直接読み込み、Python、mlx-lm、Transformers、ネットワーク接続を使わずに翻訳します。

## 帰宅後の実機検証

まず既存のテストを実行します。

```sh
ctest --preset macos-arm64-release
```

次にモデルを使ったチェックを実行します。`ctest` はモデルを自動取得しないため、これらは明示的に実行します。

```sh
build/macos-arm64-release/plamo08-tokenizer-test \
  build/macos-arm64-release/plamo-3-translate-0.8b-MLX-4bit \
  third_party/plamo08/tests/tokenizer_parity.json

build/macos-arm64-release/plamo08-model-test --model \
  build/macos-arm64-release/plamo-3-translate-0.8b-MLX-4bit

build/macos-arm64-release/inference_probe \
  build/macos-arm64-release/plamo-3-translate-0.8b-MLX-4bit auto
```

`plamo08-model-test` は、同じ重み・同じトークン列について、MLX標準演算で組んだモデルと専用カーネルのlogitsを比較します。英語・日本語の入力、2048トークンのlocal KV cache折り返し、cache reset、キャンセル、出力コールバックによる終了を確認します。`top1` とlogitsの誤差も標準出力に出します。

`inference_probe` はアプリの推論アダプターを通し、通常の日本語／やさしい日本語、入力準備、停止トークン、出力上限、キャンセルを確認します。

アプリではステータス欄が `MLX / Metal (0.8B fused)` になることを確認してPDFを開いてください。翻訳途中でページを移動し、移動先の訳文に前ページの文字列が混ざらず、正常に翻訳されることも確認します。文体を変更すると、それに対応した日本語訳を生成します。

### MLX標準演算との比較

同じアプリ・同じモデルで標準演算の経路を試すには、起動中のアプリを終了してから次を実行します。

```sh
OPT_MLX_BACKEND=mlx \
  "build/macos-arm64-release/Offline PDF Translator.app/Contents/MacOS/Offline PDF Translator"
```

このときの表示は `MLX / Metal (reference)` です。環境変数を付けずに起動すると専用カーネルに戻ります。設定ファイルは作りません。

速度を比較するときは、同じPDF・ページ・文体を使い、各起動で最初の翻訳をウォームアップにしてから別の未翻訳ページを測ってください。「診断情報」の `Last chunk seconds` はアプリで処理した直近チャンクの所要時間です。初回のMetalカーネルコンパイル、入力長、生成された訳文の長さ、ページ分割の影響を受けるので、短い1回の測定だけで速度差は判断できません。

## 実装

- `src/inference/engine_mlx.cpp` が既存の `InferenceBackend` 契約を実装します。逐次表示、ページ切り替え時のキャンセル、UTF-8の途中バイト保持、PFML制御トークンの扱いを引き継ぎます。
- `third_party/plamo08/src/kernels.cpp` に0.8B固定形状のMetalカーネルとC++ディスパッチがあります。hidden 1536、16層、16 query heads / 1 KV head、head dim 128、FFN 6144、4bit / group 64に特化しています。
- PrefillはMLXの量子化行列積とattentionを使い、decodeでは融合した行列・ベクトル積、norm、RoPE、MQA、SwiGLU等を使います。量子化重みを事前にFP16へ展開しません。公式checkpointでBF16のembeddingとlm_headはBF16のまま使います。
- 14層のlocal attentionは2048トークンのring cache、残る2層はfull attentionです。コンテキスト容量はモデルの `max_position_embeddings` を使い、アプリのページ分割に報告します。
- 生成は1トークン先のGPU処理とCPUの表示処理を重ねます。キャンセルはprefillの256トークン単位とdecodeのトークン単位で確認します。実行中のGPU処理が完了してからcacheを再利用します。
- MLX v0.32.4は静的リンクし、内蔵カーネル `mlx.metallib` を `.app/Contents/MacOS/` に同梱します。CMake構成時の依存ダウンロードはありません。
- モデルは複数ファイルなので、読込ごとに独立したメモリ内モデルIDを使います。configやtokenizerを変更して読み直した際に以前の訳文を流用しません。入力ファイルは読み取り専用で、アプリの状態・訳文はメモリ内だけに保持します。

専用カーネルの元になったコードは [`pfnet/plamo-translate-cli`](https://github.com/pfnet/plamo-translate-cli/tree/82788925ce64b5ec15abb25a650ccf4616eff57a) です。改変内容とライセンスは `third_party/plamo08/THIRD_PARTY_NOTICES.md`、配布用全文はルートの `THIRD_PARTY_NOTICES.txt` にあります。

## ZIPを作る場合

ビルド後、既存のパッケージ手順でモデルフォルダーと `mlx.metallib` を含むZIPを作れます。

```sh
python3 scripts/package.py --target macos-arm64-release \
  --model-dir build/macos-arm64-release/plamo-3-translate-0.8b-MLX-4bit
```

## このブランチでの検証範囲

Linux上で、実際のMLX v0.32.4ヘッダーに対するC++構文、公式tokenizerとの20ケースの一致、量子化行列・ベクトル積とattentionのCPU参照計算、配布構成、既存の関連テストを確認しています。Apple Siliconでのネイティブビルド、Metalカーネル実行、GUI動作、速度測定は実機での確認が必要です。速度向上率はまだ測定していません。
