# 配布物と検証

メモリ内で動作する読み取り専用ビューワーは **0.1.2** からです。0.1.1 以前の ZIP には、当時の永続保存・書き出し機能が含まれます。旧版を使用した場合の保存先は [セッションデータの仕様](session-data.md) に記載しています。

## パッケージの作成

対象 OS でビルドと CTest を実行し、`scripts/package.py` で ZIP を生成します。コマンドは [開発手順](development.md) にあります。パッケージ処理はコミット済みの作業ツリー、バイナリ内のビルド識別情報、固定した PDFium リビジョン、ライセンス通知、同梱 GGUF の SHA-256 を照合します。

ZIP の最上位にはアプリ、公式 GGUF、`README.md`、モデルの `PLaMo-Community-ja.txt`、アプリの `LICENSE`、依存ライブラリ・フォントの `THIRD_PARTY_NOTICES.txt` を配置します。ライセンス文書もパッケージのハッシュ照合の対象です。

GitHub のリリースページには、閲覧専用ビューアーとしての使い方、対象 OS、同梱ファイル、利用条件を記載します。ビルド・検証の詳細は開発資料で管理します。

`dist/<target>/<identity>/package-manifest.json` には、版・コミット・ZIP 名・SHA-256・署名状態・依存監査の結果が入ります。配布する ZIP は、この manifest の版とコミットを確認して選びます。同じ階層に `BUILD-INFO.json`（ビルド識別情報）、`DEPENDENCIES.json`（PDFium の依存解決結果）、`SBOM.cdx.json`（ビルド時の依存一覧）、`BUILD-VERIFICATION.json`（有効なビルド設定の検証結果）を保存します。これらは配布者向けの監査資料で、利用者向け ZIP や macOS の `.app` には含めません。実行時のビルド識別情報は実行ファイルに埋め込み済みで、外部 JSON を読みません。ZIP 再展開後に実行ファイルと同梱物を再検証します。

Windows では PE の通常・遅延インポート、CFG の関数表、CET 互換フラグ、静的 CRT のコンパイル設定を検査します。`--windows-cert` に署名証明書の拇印を指定すると、パッケージ内の実行ファイルに Authenticode 署名し、再展開後に検証します。証明書を指定しない開発ビルドは未署名です。

### ビルド設定と配布物の独立検証

パッケージ作成時に CMakeCache.txt、compile_commands.json、PDFium の解決済み GN 引数を確認します。PDFium の V8/XFA 無効、SQLite の拡張ロード無効・一時処理のメモリ限定、通信機能に関わる依存のビルド設定が開発方針と一致することを検証します。PDFium のリンク用アーカイブは生成時に SHA-256 を記録し、パッケージ作成時に再照合します。旧形式の PDFium マニフェストでビルド済みの場合は、`scripts/build_pdfium.py` を再実行してからアプリを再ビルドしてください。

`package-manifest.json` は ZIP、実行ファイル、同梱物、各監査 JSON の SHA-256 とサイズを記録します。別途入手した信頼できるマニフェスト SHA-256 と発行者証明書の拇印を用いると、Windows SDK の SignTool がある Windows 上で、配布物を実行せずに独立検証できます。

```powershell
python scripts/verify_package.py dist/windows-x64-release/<identity> `
  --expected-manifest-sha256 <trusted-manifest-sha256> `
  --expected-thumbprint <trusted-publisher-thumbprint>
```

発行者の認証情報とマニフェストのハッシュは、ZIP と同じ未認証のファイル一式とは独立した信頼できる経路で確認してください。検証では ZIP 内の全ファイルの内容を照合し、実行ファイルを一時フォルダーに取り出して Authenticode の信頼性と署名者証明書を確認します。一時ファイルは開発・検証用で、ビューアーの利用時には生成しません。署名の信頼性確認には Windows の証明書失効確認ポリシーが適用されます。検証資料はビルド設定・配布物の整合性を示すものであり、実行時の挙動やビルド環境そのものの信頼性は、別途実機試験・ビルド環境の管理が必要です。

Windows Release では ggml CPU の AVX2 オブジェクトとベースラインオブジェクトが外部関数シンボルを共有していないことも検査します。比較関数の型名を分けて、標準ライブラリが生成するソート関数にも個別の名前を持たせています。

macOS のパッケージ処理は Hardened Runtime を指定して署名します。既定は ad-hoc 署名です。Developer ID 署名には `--mac-identity`、公証には追加で `--notary-profile` を指定します。公証を選んだ場合は Apple の受理とチケットの添付を確認し、再展開後もチケットを検証します。

macOS の同梱GGUF探索は、App Translocationによる実行場所ではなく、元の `.app` の外側の同じフォルダーを使います。Security frameworkの元パス照会を動的に解決し、読取り専用で参照します。この照会は公開SDKヘッダーにないため、利用できない場合は通常の実行パスへ戻します。検疫属性の変更、アプリやGGUFのコピー、再起動は行いません。

## 閲覧動作の受け入れ確認

- ZIP を展開したディレクトリから起動し、隣接する GGUF の読込、PDF の表示、ページ移動、現在ページの翻訳を確認します。
- macOSではブラウザからダウンロード・展開したアプリのApp Translocation起動でも同じGGUFを読み込むことを確認します。実行中の移動先パスと元の配布フォルダーを `macos_companion_probe <実行ファイル> <元の配布フォルダー>` に渡すと、実際のOS照会とGGUF探索を検査できます。
- 原文・訳文の選択とコピー、読み取り専用本文に対する入力・貼り付け・切り取り・削除・Undo の拒否を確認します。
- PDF と GGUF の内容・更新時刻、および実行前後のアプリ管理ファイルを比較します。再起動時には設定・読書位置・訳文を引き継ぎません。
- 検証した OS、CPU、ビルド識別情報を記録します。[過去の版の検証記録](releases/v0.1.1.md) と対象版の記録を区別します。
