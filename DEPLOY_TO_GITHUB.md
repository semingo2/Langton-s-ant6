# GitHub Pagesへ公開する手順

## 1. リポジトリへプッシュ

プロジェクトをGitHubリポジトリの `main` ブランチにプッシュします。

## 2. Pagesを有効化

リポジトリの **Settings → Pages → Build and deployment → Source** で **GitHub Actions** を選択します。

## 3. デプロイを確認

**Actions** タブの **Build and deploy GitHub Pages** ワークフローが成功したら、`https://<ユーザー名>.github.io/<リポジトリ名>/` を開きます。ワークフローはEmscriptenを使って `main.cpp` をWebAssemblyにビルドし、サイトと一緒にデプロイします。シミュレーションはブラウザー内で実行されます。
