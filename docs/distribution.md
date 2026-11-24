# 配布物と検証

メモリ内で動作する読み取り専用ビューワーは **0.1.2** からです。0.1.1 以前の ZIP には、当時の永続保存・書き出し機能が含まれます。旧版を使用した場合の保存先は [セッションデータの仕様](session-data.md) に記載しています。

## パッケージの作成

対象 OS でビルドと CTest を実行し、`scripts/package.py` で ZIP を生成します。コマンドは [開発手順](development.md) にあります。パッケージ処理はコミット済みの作業ツリー、バイナリ内のビルド識別情報、固定した PDFium リビジョン、ライセンス通知、同梱 GGUF の SHA-256 を照合します。

ZIP の最上位にはアプリ、公式 GGUF、`README.md`、モデルの `PLaMo-Community-ja.txt`、アプリの `LICENSE`、依存ライブラリ・フォントの `THIRD_PARTY_NOTICES.txt` を配置します。ライセンス文書もパッケージのハッシュ照合の対象です。

GitHub のリリースページには、閲覧専用ビューアーとしての使い方、対象 OS、同梱ファイル、利用条件を記載します。ビルド・検証の詳細は開発資料で管理します。

`dist/<target>/<identity>/package-manifest.json` には、版・コミット・ZIP 名・SHA-256・署名状態・依存監査の結果が入ります。配布する ZIP は、この manifest の版とコミットを確認して選びます。同じ階層に `BUILD-INFO.json`（ビルド識別情報）、`DEPENDENCIES.json`（PDFium の依存解決結果）、`SBOM.cdx.json`（ビルド時の依存一覧）を保存します。これらは配布者向けの監査資料で、利用者向け ZIP や macOS の `.app` には含めません。実行時のビルド識別情報は実行ファイルに埋め込み済みで、外部 JSON を読みません。ZIP 再展開後に実行ファイルと同梱物を再検証します。

Windows では PE の通常・遅延インポート、CFG の関数表、CET 互換フラグ、静的 CRT のコンパイル設定を検査します。配布用 Release は Windows 上で Authenticode 署名と RFC 3161 タイムスタンプを付けてから ZIP 化します。Windows SDK の `signtool.exe` を PATH に追加し、署名端末の `Cert:\CurrentUser\My` に、対応する秘密鍵を使えるコード署名証明書を用意します。個人名義の証明書は署名から証明書の Subject（発行先）を閲覧できるため、初回公開前に表示される発行先を確認してください。秘密鍵やトークンの PIN はリポジトリやコマンドに置きません。

Visual Studio の開発者 PowerShell で、依存を bootstrap 済みのコミット済みソースから実行します。

```powershell
Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Select-Object Subject, Thumbprint, NotAfter

.\scripts\release_windows.ps1 -ModelFile 'C:\path\to\plamo-3-translate-0.8b-Q4_K_M.gguf' `
    -CertificateThumbprint '<証明書の Thumbprint>' `
    -TimestampUrl '<発行元が指定する RFC 3161 タイムスタンプ URL>'
```

この手順はローカルで configure → ビルド → CTest → 署名 → ZIP 作成まで実行します。ビルド・試験済みの場合は `python scripts/package.py --target windows-x64-release --model-file ... --windows-cert ... --windows-timestamp-url ...` でパッケージ処理から実行できます。署名はビルド済み EXE のコピーに対して行い、ファイルとタイムスタンプの両方に SHA-256 を使用します。タイムスタンプ取得には署名時の通信が必要です。署名したコピーと実際の ZIP から再展開した EXE を `signtool verify /pa /tw` で検証します。署名、タイムスタンプ、ZIP 再展開後の検証のいずれかが失敗すると、新しい配布 ZIP を確定しません。

配布候補は `dist/windows-x64-release/<identity>-signed-rfc3161/` に保存します。既存の候補を再利用する際も ZIP 内の EXE とタイムスタンプを検証します。ローカル試験用に未署名の Release ZIP が必要な場合は `--allow-unsigned-windows-release` を明示します。その ZIP 名には `-unsigned` が付きます。Debug は署名設定なしで未署名のまま作成できます。ここで扱う署名は EXE の発行元と改変有無の確認です。セキュリティ製品での隔離・誤検知は、対象ファイルのハッシュと検知名を添えて製品ベンダーへ別途照会します。

Windows Release では ggml CPU の AVX2 オブジェクトとベースラインオブジェクトが外部関数シンボルを共有していないことも検査します。比較関数の型名を分けて、標準ライブラリが生成するソート関数にも個別の名前を持たせています。

macOS のパッケージ処理は Hardened Runtime を指定して署名します。既定は ad-hoc 署名です。Developer ID 署名には `--mac-identity`、公証には追加で `--notary-profile` を指定します。公証を選んだ場合は Apple の受理とチケットの添付を確認し、再展開後もチケットを検証します。

macOS の同梱GGUF探索は、App Translocationによる実行場所ではなく、元の `.app` の外側の同じフォルダーを使います。Security frameworkの元パス照会を動的に解決し、読取り専用で参照します。この照会は公開SDKヘッダーにないため、利用できない場合は通常の実行パスへ戻します。検疫属性の変更、アプリやGGUFのコピー、再起動は行いません。

## 閲覧動作の受け入れ確認

- ZIP を展開したディレクトリから起動し、隣接する GGUF の読込、PDF の表示、ページ移動、現在ページの翻訳を確認します。
- macOSではブラウザからダウンロード・展開したアプリのApp Translocation起動でも同じGGUFを読み込むことを確認します。実行中の移動先パスと元の配布フォルダーを `macos_companion_probe <実行ファイル> <元の配布フォルダー>` に渡すと、実際のOS照会とGGUF探索を検査できます。
- 原文・訳文の選択とコピー、読み取り専用本文に対する入力・貼り付け・切り取り・削除・Undo の拒否を確認します。
- PDF と GGUF の内容・更新時刻、および実行前後のアプリ管理ファイルを比較します。再起動時には設定・読書位置・訳文を引き継ぎません。
- 検証した OS、CPU、ビルド識別情報を記録します。[過去の版の検証記録](releases/v0.1.1.md) と対象版の記録を区別します。
