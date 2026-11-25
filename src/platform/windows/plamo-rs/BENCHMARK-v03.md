# v0.3.0 追加高速化：独立チャンク一括実行、固定prefix、追加モデル不要の投機的生成

2026-11-25、公式PLaMo 3 Translate 0.8B Q4_K_M GGUF（v0.2と同一SHA256）、EPYC 9V74、CPU 4スレッド、Rust 1.100.0 release、llama.cpp b12721 (79c69f4b0)。各3回中央値。モデル初期化時間は除外。Rustはライブラリ呼び出し時間、llama.cppはlocalhostへのHTTP要求から完了までの壁時計時間（通信・スケジュールのオーバーヘッドを含む）。共有仮想環境の小規模テスト。

## 同一の独立8件を翻訳

| 方式 | 時間 | Rustバッチ8に対する比率 |
|---|---:|---:|
| Rust逐次（固定prefix warm） | 2.441秒 | 2.37倍 |
| Rustバッチ8（固定prefix cold） | 1.098秒 | 1.06倍 |
| **Rustバッチ8（固定prefix warm）** | **1.032秒** | **1.00** |
| llama.cpp 8並列サーバー、prompt cache無効 | 2.088秒 | 2.02倍 |
| llama.cpp 8並列サーバー、prompt cache有効 | 1.475秒 | 1.43倍 |

各llama.cppスロットは4096トークン（サーバーcontext 32768、8 slots）、Rustは各独立session 4096。入力は英語の技術短文8件、英→日、greedy、最大128新規トークン。Rustバッチは逐次実行と全8件の生成ID列・停止状態が一致（135出力トークン、停止含まず）。約131出力tok/s（入力処理込み）。llama.cppの生成ステップ合計は143〜145（停止含む）であり、出力長や浮動小数演算が異なるため完全同一の作業量ではない。prompt cache有効比較は同じ8入力を反復した結果で、Rustのprefixキャッシュは固定指示のみ。自動分割した訳の品質を検証したものではない。

## 単一の反復文（57入力トークン、Rust 45出力トークン）

| 方式 | 時間 |
|---|---:|
| Rust通常 | 0.818秒 |
| **Rust投機的生成** | **0.587秒** |
| llama.cpp通常 | 0.794秒 |
| llama.cpp ngram-simple (N=3, M=12) | 0.678秒 |

Rustは通常より28.2%短縮、llama.cpp通常より26.1%短縮、llama.cpp ngram-simpleより13.4%短縮。モデル初期化時間は除く。Rust通常と投機的生成の出力列完全一致。24候補のうち21採用、45逐次forward相当から24回の検証パスに削減。繰り返しが少ない文章に同じ倍率は期待できない。長文すべてに対して既存llama.cppの速度・品質を必ず上回るという主張はしない。

## 検証

通常7件、公式GGUF統合4件（従来12ケースの近似候補と全語彙の一致、バッチと逐次のlogit一致、8件のcold/warm共有prefix・バッチ・投機の生成列一致など）が通過。新しい方式は元のGGUF形式を変更せず、別の重みを追加しない。
v0.2の通常比較と品質上の留意点は [BENCHMARK.md](BENCHMARK.md) に残す。N100、Windows/macOSの実機性能、およびユーザーの高速化済みllama.cppパッチとの比較は未実施。

## 再現用データ

`benchmarks/v03/rust-batch-4t.txt`、`rust-spec-4t.jsonl` と対応する `llama-*.jsonl` を収録。READMEのコマンドで再現可能。
