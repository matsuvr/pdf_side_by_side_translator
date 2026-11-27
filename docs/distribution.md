# 配布

各OSで `python scripts/package.py --model-file <モデルへのパス>` を実行します。コミット済みソースから新しいステージングディレクトリへビルドし、同梱モデルのSHA-256を照合します。Windowsの配布単位は自己完結したフォルダー、Macはアプリバンドルとその隣の公式GGUFです。

配布物にはモデルの契約書、日本語の使い方、アプリと依存ライブラリのライセンス、ビルド元コミットを含めます。ZIPを再展開して内容を照合します。Macの既定ビルドはアドホック署名です。配布用の署名・公証には `--windows-cert`、`--mac-identity`、`--notary-profile` が使用できます。詳細は `scripts/package.py --help` を参照してください。
