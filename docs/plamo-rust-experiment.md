# Windows PLaMo Rust 実験
Windows x64 で PLaMo 3 Translate 0.8B Q4_K_M 専用 Rust v0.3.0 を静的リンクするドラフト。macOS の標準 llama.cpp/Metal は維持する。既存の Engine インターフェースを実装するため UI、分割、翻訳 worker、キャッシュは共通。
- Windows の GGUF は deps/third_party.lock.json に記録された公式ファイルの SHA-256 と一致するものを使用。読み取り専用 FileSnapshot から固定長でメモリに読み、ファイル変更を監視。
- 自動検出された CPU 命令セットとスレッド数を利用。固定の PFML prefix / 平易日本語を含む既存 UI 操作を保持。
- 32-token prefill、固定PFML prefixのメモリ内再利用、量子化パッキング、AVX2以降の近似出力候補選別を利用。v0.3 の複数独立入力バッチ・n-gram投機実装はソースにあるが、既存 worker の単一入力・ストリーム出力の経路には未接続。実機性能は測定が必要。
- 入力・訳文・モデルの一時ファイルや履歴を生成しない。
- Windows release/debug のコンパイル、静的リンクおよびパッケージ/実機テストが必要。特に CRT、依存 DLL、Eコアのみ/AVX2非搭載、2スレッド、長文、キャンセル、モデル再読込、品質を確認する。
- Rust 標準ライブラリの配布用通知と実際のリンク閉包の精査は正式リリース前に完了すること。
