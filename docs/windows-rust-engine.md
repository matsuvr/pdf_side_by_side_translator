> 旧版の実装・検証記録です。現行版の構成は[設計](../DESIGN.md)、ビルド方法は[開発手順](development.md)を参照してください。

# Windows版の標準推論エンジン

Windows x64 では元のPLaMo 3 Translate 0.8B BF16から生成した P4X86 v7 専用ウェイトと
Rust 0.4.0 staticlib を利用する。Macの標準 llama.cpp/Metal は従来の公式GGUFを使用する。
既存の Engine インターフェースを介し、PDF表示、ページ単位の翻訳、翻訳ストリーム、
原文言語の自動判定、標準/平易日本語、キャンセル、メモリ内キャッシュを維持する。

- Windowsは516,613,376バイトの `plamo3-translate-0.8b-x86-s4tile-v7.p4x86` を使用する。
  固定SHA-256と同一の内容を読み取り専用FileSnapshotから検証し、Rustが所有するメモリに保持する。
  推論中のメモリマップの外部変更による障害を避ける。GGUF版との混同を防ぐ。
- 256ウェイトの8個のサブスケールを整数化し、256ウェイト分を整数累積してからFP32に変換。
  8出力行のSIMD向け事前配置、AVX2/FMA/AVX-VNNI/AVX512-VNNIを自動選択し、
  AVX2非対応CPUにはスカラー演算経路を備える。
- ページ間で共通するPFMLプロンプトの先頭部分はメモリ内で再利用する。
  C ABIは単一入力・ストリーム出力を維持し、投機生成や独立入力バッチは製品経路では使用しない。
- 実行時のファイル書出し・変換・コピー・履歴・外部プロセス・ネット接続は追加しない。
- 開発時のモデル再生成は `scripts/convert_windows_model.py` で行う。
  元のBF16/設定/tokenizer、および出力SHA-256を検証する。実ファイルはGitに含めない。

## 採用時の検証

PFN公式トークナイザーを基準に、対応53言語のテキストと翻訳プロンプトの入力トークン列を照合した。[文字入力の検証](benchmarks/2026-11-27-multilingual/unicode.md)

10言語の行政案内を各1ページ翻訳し、各言語3回の中央値を合計した。既定の8スレッドではRust版が41.78秒、パッチ版llama.cppが51.87秒、素のllama.cppが72.34秒だった。物理3コア・3スレッドではそれぞれ62.93秒、81.59秒、117.73秒だった。[既定設定の測定](benchmarks/2026-11-27-multilingual/speed-after-reference.md)・[3コアの測定](benchmarks/2026-11-27-multilingual/speed-three-cores.md)

Built with PLaMo. Windows配布物には元BF16版のPLaMo Community License、Rust依存ライセンスおよび
Rust標準ライブラリの公式著作権通知を同梱する。
