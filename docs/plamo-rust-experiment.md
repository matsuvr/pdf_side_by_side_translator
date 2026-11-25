# Windows PLaMo x86 S4 v7 実験

Windows x64 では元のPLaMo 3 Translate 0.8B BF16から生成した P4X86 v7 専用ウェイトと
Rust 0.4.0 staticlib を利用する。Macの標準 llama.cpp/Metal は従来の公式GGUFを使用する。
既存の Engine インターフェースを介し、PDF表示、ページ分割、翻訳ストリーム、
原文言語の自動判定、標準/平易日本語、キャンセル、メモリ内キャッシュを維持する。

- Windowsは516,613,376バイトの `plamo3-translate-0.8b-x86-s4tile-v7.p4x86` を使用する。
  固定SHA-256と同一の内容を読み取り専用FileSnapshotから検証し、Rustが所有するメモリに保持する。
  推論中のメモリマップの外部変更による障害を避ける。GGUF版との混同を防ぐ。
- 256ウェイトの8個のサブスケールを整数化し、256ウェイト分を整数累積してからFP32に変換。
  8出力行のSIMD向け事前配置、AVX2/FMA/AVX-VNNI/AVX512-VNNIを自動選択し、
  AVX2非対応CPUにはスカラー演算経路を備える。
- 固定PFML prefixのメモリ内再利用、32-token prefill、出力語彙の動的3072候補選択。
  C ABIは単一入力・ストリーム出力を維持し、投機生成や独立入力バッチは製品経路では使用しない。
- 実行時のファイル書出し・変換・コピー・履歴・外部プロセス・ネット接続は追加しない。
- 開発時のモデル再生成は `scripts/convert_windows_model.py` で行う。
  元のBF16/設定/tokenizer、および出力SHA-256を検証する。実ファイルはGitに含めない。
- [スタンドアロン実験の測定](../src/platform/windows/plamo-rs/BENCHMARK-v7.md)では、
  AVX2強制2/4スレッドの6文翻訳について旧Rust v2より24%/31%短縮、
  生成速度20%/43%向上。実製品の速度はWindows実機で確認する。
- Windows製品の完全コンパイル・リンク、静的CRT、CFG/CET、PE依存監査、ZIP、
  低コア・Eコアのみ・AVX2非対応CPU、長文、モデル再読込と品質はドラフト採用前の確認項目。
  Mac製品のビルド・Metal経路も既存の試験で回帰確認する。

Built with PLaMo. Windows配布物には元BF16版のPLaMo Community License、Rust依存ライセンスおよび
Rust標準ライブラリの公式著作権通知を同梱する。
