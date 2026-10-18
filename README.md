# 2匹のラングトンのアリ — C++解析＋Web UI

2匹のアリを同時に動かし、104歩周期のハイウェイと相対状態ハッシュによるループを解析するアプリです。シミュレーション／解析はC++17、画面はHTML・CSS・JavaScriptです。

## 機能

- 蟻1（赤）・蟻2（青）のシミュレーション
- 実行／一時停止、1歩・100歩・1,000歩進める操作、速度変更
- マスの大きさ変更、蟻どうしの間隔・向き変更、盤面スクロール／ドラッグ
- 各蟻について104歩周期の反復を20周期分確認するハイウェイ検出
- ハイウェイ開始推定歩数・検出確認歩数の表示
- 黒マス総数、一度でも通ったマス数、ハイウェイ経路のマス数、ハイウェイ経路以外の黒マス数
- 蟻1を原点とする相対座標・両方の向き・相対盤面ハッシュによるループ検出

## ローカルで実行する（Windows）

g++（MinGW-w64 / MSYS2など）とEmscripten SDKを用意します。Emscripten環境を有効にし、Git BashまたはMSYS2のシェルでWASMを生成してから、サーバーを起動します。

```bat
bash build_wasm.sh .
g++ -std=c++17 -O2 -pthread main.cpp -lws2_32 -o langtons_ant_server.exe
langtons_ant_server.exe
```

ブラウザーで `http://127.0.0.1:8080` を開きます。
8080番ポートがすでに使われている場合は、PowerShellで別のポートを指定して起動できます。

```powershell
$env:PORT = "8081"
.\langtons_ant_server.exe
```

起動時に指定したポートを使用できない場合は、サーバーがエラーを表示して終了します。

## GitHub Pagesで公開する

GitHub Pagesは静的ファイルを配信します。このプロジェクトではGitHub ActionsがEmscriptenでC++をWebAssemblyにビルドし、HTML・CSS・JavaScriptと一緒にPagesへデプロイします。シミュレーションは各訪問者のブラウザー内で実行されます。

1. このリポジトリをGitHubにプッシュします。
2. GitHubリポジトリの **Settings → Pages → Build and deployment → Source** で **GitHub Actions** を選択します。
3. `main` ブランチへのプッシュ後、**Actions** タブで **Build and deploy GitHub Pages** が成功したことを確認します。
4. `https://<ユーザー名>.github.io/<リポジトリ名>/` を開きます。

ワークフローが `main` へのプッシュごとにC++を再ビルドして公開します。ローカルでネイティブサーバーを起動する場合は、上記のWindows手順も引き続き利用できます。`build_wasm.sh` はGitHub ActionsとDockerで共有するビルド手順です。

## 公開時の注意

- シミュレーション状態は訪問者ごとにブラウザー内で独立しています。
- GitHub Actionsによる初回デプロイ前に、リポジトリのPages設定でGitHub Actionsを公開元に選択してください。
- 104歩周期の候補を20周期分確認してから開始歩数を推定します。ループ判定は独立した2つの64-bitハッシュを含む状態比較であり、ハッシュ衝突の可能性は理論上ゼロではありません。
- `ant.js` と `ant.wasm` はEmscriptenのビルド成果物で、GitHub Actionsが生成します。リポジトリに古い生成ファイルを追加しないでください。
