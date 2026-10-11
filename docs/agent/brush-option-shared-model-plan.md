# ブラシオプション共有モデル化 計画書

作成日: 2026年10月5日

状態: フェーズ1(基盤とDeformの試験移行)を実装済み。自動テストはLinuxの
部分ビルドに加え、2026年10月8日にWindows(Qt 6.8)でも実施した
(`KisPaintOpOptionsModelTest` 32件、`KisCurveOptionDataTest` 5件、
`kis_paintop_test` 3件、`KisPaintOpPresetTest` 6件、すべて通過。インストール済み
のバイナリと一致)。実アプリでの手動確認は2026年10月8日にユーザー報告で問題なし
(下記の6項目)。その際の指摘で、Deformの「Brush size」ページの項目が上詰めに
なっていなかった(上流から引き継いだ `wdgBrushSizeOptions.ui` に縦の伸縮余白が
なかった)ため、下端に余白を追加した。フェーズ0の調査結果は
`docs/agent/brush-option-shared-model-phase0.md`。

フェーズ2a(ブラシ先端とマスクブラシの状態の値への集約)を2026年10月8日に
実装した。自動テストは通過し、実アプリでの手動確認も2026年10月8日に
ユーザー報告で問題なし(下記の「フェーズ2aの実装結果」)。確認中に見つかった
Shift+ドラッグでの安全アサートは修正済み。

フェーズ2b(Pixel Brushの共有モデルへの移行)を2026年10月8日に実装した。
自動テストは通過し、実アプリでの手動確認も同日にユーザー報告で問題なし
(下記の「フェーズ2bの実装結果」)。次はフェーズ3(Tool Optionsへの外だし)。

ブラシエディタ(F5)が持つオプションの状態を画面から切り離し、プリセットごとの
1つのモデルに集約する。F5画面とTool Optionsドッカーなど複数の画面が同じ状態を
参照し、1つのパラメータの変更がそのオプション分の書き込みだけで済むようにする。

本書の最終目的は、F5画面の設定項目をTool Optionsドッカーへ外だしすること、
ブラシの切り替えに追従させること、外だしする項目を設定で管理できるようにする
ことである。共有モデル化はその前提となる基盤である。以下のクラス名・設定キー・
フェーズ分割は提案であり、確定済みの仕様ではない。

## 背景と現状

### 現在のデータの流れ

| 方向 | 処理 | 場所 |
| --- | --- | --- |
| 画面→プリセット | どれかのオプションが `sigSettingChanged` を出すと、`KisConfigWidget` の圧縮タイマーを経て `sigConfigurationUpdated` になる。この時点で、どのオプションが変わったかは失われる | `libs/ui/kis_paintop_settings_widget.cpp:109`、`libs/image/kis_config_widget.cpp:15` |
| 画面→プリセット | `KisPaintopBox::slotGuiChangedCurrentPreset()` が `resetSettings()` で `paintop` 以外のキーを消し、全オプションを書き直す | `libs/ui/kis_paintop_box.cc:1419` |
| プリセット→画面 | `sigSettingsChangedUncompressed` を圧縮した後、F5画面が表示中なら全オプションを `readOptionSetting()` で読み直す | `libs/ui/kis_paintop_box.cc:709`、`:750` |
| 循環防止 | `KisAcyclicSignalConnector` と `KisPaintOpConfigWidget` の `m_isInsideUpdateCall` | `kis_paintop_box.cc:707`、`libs/image/brushengine/kis_paintop_config_widget.cpp` |

`resetSettings()` の直前のコメントは、全消去の理由に
`KisPaintOpUtils::RequiredBrushFilesListTag` の積み増しを挙げている。
しかし、このシンボルは現在のコードに存在しない。実質的に残っている理由は、
条件付きで書かれるキーが古い値のまま残ることだと考えられる。これは
フェーズ0で確認する。

### 移行を可能にする既存の構造

- **データ構造:** オプションの状態は大半が `KisXxxOptionData` という値型として
  定義され、`read()`/`write()` で `KisPropertiesConfiguration` と相互変換できる。
- **lagerのカーソル:** オプションのウィジェットは、コンストラクタで
  `lager::cursor<Data>` を受け取る(例: `KisSizeOptionWidget`、
  `KisCompositeOpOptionWidget`、`KisFilterOptionWidget`)。
- **状態の持ち主:** 現在は `KisPaintOpOptionWidgetUtils::createOptionWidget()` が
  作るラッパー(`detail::DataStorage::m_data`)が、`lager::state` を
  **ウィジェットごとに**持っている
  (`plugins/paintops/libpaintop/KisPaintOpOptionWidgetUtils.h`)。
  この状態を外部に移せば、同じカーソルを複数のウィジェットに渡せる。
- **部分書き込みの前例:** `KisCurveOptionDataUniformProperty::writeValueImpl()` は、
  1つのオプションのデータだけを読み、値を変えて書き戻している。
- **設定画面の構成:** `KisPaintOpOptionWidgetUtils` を使う設定画面は16個ある
  (`plugins/paintops/*/…settings_widget.cpp`)。

### lagerに移行していない、または特殊なオプション

| オプション | 特徴 | 場所 |
| --- | --- | --- |
| ブラシ先端 `KisBrushOptionWidget` | `lager::state` を3つ(`brushData`、`brushPrecisionData`、`commonBrushSizeData`)内部に持ち、外部からカーソルを受け取らない。書き込み時は、内部モデルの `bakedOptionData()` で値を合成する。読み込みには `resourcesInterface` が必要 | `plugins/paintops/libpaintop/kis_brush_option_widget.cpp:40`、`:90`、`:106` |
| マスクブラシ `KisMaskingBrushOption` | 入れ子のブラシ設定を持ち、メインブラシの実効サイズ(`effectiveBrushSize`)に依存する | `plugins/paintops/libpaintop/KisMaskingBrushOption.cpp`、`kis_brushop_settings_widget.cpp:74` |
| カーブ系 `KisCurveOptionWidget` | 書き込みは、内部モデルの `bakedOptionData()` を使う | `plugins/paintops/libpaintop/KisCurveOptionWidget.cpp:234` |
| オプション間の依存 | `lightnessModeEnabled()` や `maskingBrushEnabledReader()` のように、他のオプションの状態をlagerのreaderとして参照するものがある | `kis_brushop_settings_widget.cpp:59`、`:75` |

### F5画面以外でブラシ設定を書き換える経路

共有モデルは、これらの経路による変更も取り込む必要がある。

- Uniform Property(On-Canvas Brush Editorドッカー、`plugins/dockers/brushhud/`)
- ツールバーとキャンバスリソース(サイズ、不透明度、フロー、ブレンドモード)と、
  `kis_derived_resources.cpp`
- ロックされた設定の保存・破棄(`KisPaintopBox::slotSaveLockedOptionToPreset`、
  `slotDropLockedOption`)
- プリセットの再読み込みと切り替え
- Python スクリプト(`libkis`)
- Quick Accessの各パネル(`docs/agent/quick-access.md`)

## 目標アーキテクチャ

```
                     ┌─────────────────────────────┐
  F5画面(ビュー) ──┐ │ KisPaintOpOptionsModel      │
                   ├─▶│  オプションID → lager::state │──書き込み(該当オプションのみ)──▶ KisPaintOpSettings
  Tool Options    ─┘ │  依存関係のreader            │◀─取り込み(変更キーに対応する分)── 外部経路
  (ビュー)           └─────────────────────────────┘
```

### 構成要素(名称は仮)

- **`KisPaintOpOptionsModel`**: 現在のプリセット1つ分のオプション状態を持つ。
  オプションIDごとに `lager::state<Data>` を保持し、ビューには
  `lager::cursor<Data>` を渡す。`KisPaintopBox` が、ブラシエンジンごとに
  1つ所有する(現在の `m_paintopOptionWidgets` と同じ単位)。
- **オプション記述子**: 各ブラシエンジンが、ウィジェットとは別にオプションの
  一覧を宣言する。項目は、オプションID、データ型、表示名、カテゴリ、
  カーソルからウィジェットを作る関数、依存するオプションである。現在は
  `kis_brushop_settings_widget.cpp:44-80` のように、データとウィジェットを
  同時に作っている。これを分離する。
- **ビュー**: F5画面の `KisPaintOpSettingsWidget` と、新しいTool Optionsの
  ビューである。どちらも記述子からウィジェットを作り、モデルのカーソルに
  結び付ける。同じオプションのウィジェットが2つ同時に存在してよい。
- **書き込み**: モデルが各状態を `lager::watch` で監視し、変わったオプションの
  データだけを `Data::write()` でプリセットに書く。そのオプションが前回書いた
  キーのうち、今回書かなかったものは削除する(キー集合の差分による)。
  通知は、既存の `KisPaintOpPreset::UpdatedPostponer` でまとめる。
- **取り込み**: `KisPaintOpSettings` 側で変更されたキー名を記録し、通知と一緒に
  渡す。モデルは、キーと担当オプションの対応表から該当するオプションだけを
  `Data::read()` で読み直し、値が異なる場合だけ状態を更新する。
- **自己反響の防止**: モデル自身の書き込みによる通知は、発生元の印を付けて
  取り込みの対象から外す。

### 保持する規則

- `.kpp` の設定キーと値の形式は変えない(`docs/agent/extension-points.md`)。
  新しい書き込み方式は、従来の全書き込みと**キー単位で同一の結果**を出すこと。
- ロックされた設定(`KisLockedPropertiesProxy`)は、読み込みと書き込みの両方で
  従来と同じ上書き規則を適用する(`kis_paintop_settings_widget.cpp:121`、`:140`)。
- LOD制限と実効ブラシサイズのreaderは、ウィジェットではなくモデルの状態から
  導出する。結果は変えない。
- プリセット切り替え、再読み込み、ロック設定の保存・破棄の後は、従来どおり
  全キーを作り直す経路を残す。部分書き込みは、通常のパラメータ編集にだけ使う。
- 未移行のブラシエンジンは、従来の全書き込み経路で動作し続けること。
- プリセットの読み込み・切り替え・再読み込みの後、モデルが最初に書き込むとき
  だけは、従来と同じ全消去と全書き込みを行う。旧形式のキー(`Custom<id>`、
  `Curve<id>` など)は書き込まれず読み込まれるだけなので、キー差分では
  消せないためである(フェーズ0の結果 4章)。
- キー差分による削除の対象は、そのオプションが前回書いたキーだけとする。
  保存しないキー、`*_previous`、`Saved*`、`MyPaint/json` は対象外とする。
- モデルは加工前の値を持つ。他のオプションの状態に応じた加工(焼き込み)は、
  依存readerを引数に取る純粋な関数としてオプション記述子に置き、書き込み時に
  適用する。

## フェーズ

### フェーズ0: 調査と棚卸し

**2026年10月5日に完了。** 結果は `docs/agent/brush-option-shared-model-phase0.md`
にまとめた。静的解析と同梱プリセット313件の解析によるもので、実行環境での
確認事項は同文書の12章に残している。以下は当初の調査項目である。

コードは変更しない。成果物は、本書への追記と調査結果の表である。

- **オプションの棚卸し:** 16個の設定画面に含まれる全オプションについて、
  次の項目を表にする。
  - データ型と、カーソルを受け取るかどうか
  - `bakedOptionData()` を使うかどうか
  - 依存するreader
  - 必要なリソース(`resourcesInterface`、`canvasResourcesInterface`、
    `setImage`/`setNode`)
- **キーの担当範囲:** 標準プリセット一式を使い、各オプションの書き込み結果から
  キーと担当オプションの対応を取る。次の点を確認する。
  - 複数のオプションが同じキーを書いていないか(`CompositeOp` など)
  - 条件によって書かれたり書かれなかったりするキーがあるか
  - `resetSettings()` が必要になる実際のケースがあるか
- **lagerの比較:** 値が同じときに `lager::state::set()` が通知を出さないか、
  データ型に `operator==` があるか、`automatic_tag` と `transactional_tag` の
  どちらが適切かを確認する。
- **外部経路:** 上の「F5画面以外でブラシ設定を書き換える経路」を網羅し、
  それぞれの通知経路を記録する。
- **既存テスト:** paintop系の既存テストと、F5画面に関係するテストを確認する。

完了条件: 棚卸し表と対応表が記録され、フェーズ1の設計で未決の点がない。

### フェーズ1: 基盤

- 先に、既存の挙動を固定するテストを追加する。対象は、更新通知の延期と圧縮
  (`KisPaintOpPresetUpdateProxy`)、ロックされた設定、Uniform Propertyの
  読み書き、F5画面の全書き込みによるキー集合の変化(同梱プリセットを入力に使う)。
- `KisPaintOpOptionsModel` と、オプション記述子の型を追加する。
- `KisPaintOpOptionWidgetUtils` に、外部の `lager::cursor<Data>` を受け取って
  ウィジェットを作る関数を追加する。既存の、状態を内部に持つ関数は残す。
- オプション単位の書き込み、キー差分による削除、変更キーの記録と取り込み、
  自己反響の防止を実装する。
- `KisPaintOpSettings` に変更キーの記録を加える。既存の通知とその順序は変えない。
  `setProperty` で値が変わったキーを記録し、`removeProperty` の上書きで削除も
  記録する。`resetSettings` と `setSettings` は全体の変更として扱う。集約と配信は
  `KisPaintOpPresetUpdateProxy` で行う。
- プリセット読み込み時の一括更新には `transactional_tag` と `lager::commit()` を
  使う。
- 難易度の低いエンジン1つ(候補: Deform。カーブ系、エアブラシ、LOD制限を含む)を
  試験的に移行し、基盤を実データで検証する。

完了条件:
- テスト用の小さなオプションを使った単体テストが通る。確認する項目は、
  部分書き込み、キー差分による削除、外部変更の取り込み、自己反響が起きないこと、
  `UpdatedPostponer` による通知のまとめである。
- 既存のブラシエンジンの動作が変わらない。

#### フェーズ1の実装結果(2026年10月5日)

自動テストはすべて通過した。実アプリでの手動確認はまだ行っていない。

**変更したファイル**

| 場所 | 内容 |
| --- | --- |
| `libs/image/brushengine/kis_paintop_settings.{h,cpp}` | `UpdateListener` に `recordChangedKey()`/`recordAllKeysChanged()`(既定は何もしない)を追加。`setProperty` は値が変わったキーを記録する。`removeProperty` を隠蔽する関数で追加し、通知なし・変更済みフラグ不変のまま削除を記録する。`resetSettings` は全体の変更として記録する |
| `libs/image/brushengine/KisPaintOpPresetUpdateProxy.{h,cpp}` | 変更キーを集約し、`sigSettingsKeysChanged(QSet<QString>, bool allKeys)` で配信する。EarlyWarningの後、Uncompressedの前に出す。延期中は解除時にまとめて出す |
| `libs/image/brushengine/kis_paintop_preset.cpp` | リスナーの記録をプロキシへ転送する。`setSettings` は全体の変更として記録する |
| `libs/ui/KisPaintOpOptionsModel.{h,cpp}`(新規) | 共有モデル本体。`KisPaintOpOptionState<Data>` がオプションの状態を持つ |
| `libs/ui/kis_paintop_settings_widget.{h,cpp}` | `setOptionsModel()`/`optionsModel()` を追加。モデルがあるとき、接続中のプリセットに対する `setConfiguration`/`writeConfiguration` はオプションを読み書きしない。それ以外の設定にはモデルが読み書きする |
| `libs/ui/kis_paintop_box.cc` | エンジン切り替え時にモデルをプリセットへ接続・切断する。LOD設定のキーを全書き込みの保持対象に指定する。モデル接続中は `slotGuiChangedCurrentPreset()` の全消去を行わない(LOD設定だけを書く) |
| `plugins/paintops/libpaintop/KisPaintOpOptionStateUtils.h`(新規) | モデルの状態に結び付けたウィジェットの作成、LOD制限付きの作成、カーブ系の焼き込み関数 |
| `plugins/paintops/libpaintop/KisCurveOptionModel.{h,cpp}` | 焼き込みを純粋な関数 `bakeOptionData()` として追加。ウィジェット側の `bakedOptionData()` と同じ規則 |
| `plugins/paintops/libpaintop/KisStandardOptionData.{h,cpp}` | Opacity、Rotation、Rateのカーソル版作成関数を追加。ラベルは既存版と共有 |
| `plugins/paintops/deform/`(試験移行) | 設定画面を共有モデルのビューに変更。テスト用に静的ライブラリ `kritadeformpaintop_static` を追加し、プラグインはそれにリンクする |
| `plugins/paintops/deform/tests/`(新規) | `KisPaintOpOptionsModelTest` と、Deformプリセット6件のテストデータ |

**計画からの変更点**

- **`removeProperty` を仮想関数にしなかった。** `kis_properties_configuration.h`
  はほぼ全体から参照されるため、変更すると再ビルドの範囲が非常に大きくなる。
  代わりに `KisPaintOpSettings` で同名の関数を定義した。基底クラスのポインタ
  経由の削除(ロック用プロキシによる `_previous` の削除)は記録されないが、
  モデルが担当するキーには影響しない。
- **`transactional_tag` は使わなかった。** 状態を `transactional_tag` にすると、
  ウィジェットからの変更も `commit()` まで伝播しなくなる。そのため状態は
  従来どおり `automatic_tag` とし、一括読み込み中は書き戻しを止めるフラグで
  途中の状態を書き込まないようにした。依存readerを持つエンジン(フェーズ2以降)
  で途中の状態が問題になる場合は、改めて検討する。
- **オプション記述子は、モデルへの状態の登録(`addOption`)で代用した。**
  ウィジェットを作る関数を記述子に持たせる作業は、Tool Optionsに2つ目の
  ビューを作るフェーズ3で行う。

**モデルの動作規則**

- 接続直後と、設定の差し替え・全消去(`allKeys`)の後は、最初の書き込みで
  全消去と全書き込みを行う。保持するキーはLOD設定の2つ。
- 2回目以降は、変更されたオプションだけを一時的な設定に書き出す。前回書いた
  キーとの差分を削除してから、ロック用プロキシ経由で値を書く。
- 外部からの変更は、変更キーを担当するオプションだけを読み直す。どの
  オプションも担当しないキーが含まれる場合は全オプションを読み直す(値が
  同じなら状態は変わらない)。読み直しでは書き戻さない。
- 1回の書き込みは `UpdatedPostponer` で囲み、通知を1回にまとめる。
- オプション単位の書き込みは、`Data::write()` が書き込み先の既存値を読まない
  ことを前提とする。MyPaintの共有JSONキーはこの前提を満たさない。

**テスト(`KisPaintOpOptionsModelTest`、32件すべて通過)**

- **既存の挙動の固定(変更前のコードでも14件通過):** 通知の回数と順序、
  延期中のまとめ、値が同じときの変更済みフラグ、削除の無通知、ロックの適用と
  解除、Uniform Propertyの書き込み、6プリセットでの従来の全書き込み
- **変更キーの配信:** 単独の変更、延期中、全消去、設定の差し替え
- **モデル:**
  - 6プリセットすべてで、最初の書き込みが従来の全書き込みとキー・値とも一致
    する(4件は `Custom<id>`/`Curve<id>` を含み、全書き込みで取り除かれる)
  - 2回目以降の書き込みでは、変更したオプションのキーだけが変わり、オプション
    外のキーは残る
  - Uniform Propertyやツールバー相当の書き込みが取り込まれ、書き戻されない
  - F5画面からの書き込み(LOD設定だけ)ではプリセットが変更済みにならない
  - 設定の差し替え後は全書き込みに戻る
  - 条件付きで書くオプションを無効にすると、そのキーが削除される
  - ロック前の値(`_previous`)が他のオプションの書き込みで失われない
  - 接続していない設定への書き込みが、従来のウィジェットと同じ結果になる
- **テストの有効性の確認:** 毎回全書き込みにする誤りを意図的に入れると、
  9件が失敗することを確認した。

**検証環境と限界**

- この環境ではUbuntu 24.04のQt 5.15.13とKF5で部分的にビルドした
  (`kritaimage`、`kritaui`、`kritalibpaintop`、Deform、関連テスト)。lager と
  zug は上流の現行版を使った。Solstice本来のQt 6.8とWindowsではビルドして
  いない。
- Qt 5でビルドするため、本作業と無関係な2ファイルをローカルでだけ修正した。
  コミットには含めていない(`libs/ui/KisWelcomeAssetLibraryWidget.cpp` の
  `QStringList` への波括弧代入、`libs/ui/widgets/KisPresetDockerFilters.cpp`
  のQt 6専用の `dataChanged` 引数型)。
- Quick AccessとRest Noteは、この環境のQtで `qt_add_resources` のターゲット
  形式が使えないため、ローカルのビルドから除外した。
- 既存テストのうち、リソースデータベースを初期化するもの
  (`KisPaintOpPresetTest`、`kis_derived_resources_test`、`KisBrushModelTest`、
  `KisBrushStrokePreviewTest`)は、この環境ではテスト関数の実行前にフォント
  サムネイル生成で停止するか、何も実行せずに終了した。`KisBrushOpTest` は
  ブラシ先端のデータがないため失敗し、`kis_properties_configuration_test` の
  `testGetColor` も失敗した。いずれも変更箇所とは無関係な環境要因と判断した。
  `KisCurveOptionDataTest` と `kis_paintop_test` は通過した。
- 実アプリでの手動確認(下記)は2026年10月8日に完了した(問題なし)。

**Windowsでの確認手順**

```bat
cmd.exe /d /s /c "call <krita-dev-root>\env.bat && cmake --build <krita-dev-root>\_build --target kritaimage kritaui kritalibpaintop kritadeformpaintop KisPaintOpOptionsModelTest -j 8"
cmd.exe /d /s /c "call <krita-dev-root>\env.bat && ctest --test-dir <krita-dev-root>\_build -R KisPaintOpOptionsModelTest --output-on-failure"
```

インストール対象は `libs/image`、`libs/ui`、`plugins/paintops/libpaintop`、
`plugins/paintops/deform` の各 `cmake_install.cmake`。

手動確認の項目(Deformブラシ):

1. F5画面で各オプションを変更し、描画結果と変更済み表示が従来どおり更新される。
2. ツールバーのサイズ・不透明度・ブレンドモード、On-Canvas Brush Editorの
   Amount を変更すると、F5画面の値が追従する。
3. プリセットの切り替え、再読み込み、上書き保存、新規保存で値が保たれる。
   `v)_Distort_Grow` などの旧形式キーを含むプリセットでも同様。
4. オプションをロックして別のプリセットに切り替え、ロックを「破棄」で解除
   すると元の値に戻る(承認済みの挙動変更)。
5. F5画面のLOD設定だけを変更しても、プリセットが変更済みにならない(承認済みの
   挙動変更)。
6. Pixel Brushなど未移行のエンジンが従来どおり動作する。

### フェーズ2: ブラシ先端・マスクブラシとPixel Brushの移行

フェーズ0の結果、Pixel Brushは移行が最も難しいエンジンの一つと分かった。
そのため、外部カーソルを受け取らない2つのオプションを先に独立して移行する。

- **2a: ブラシ先端とマスクブラシ。**
  - `KisBrushOptionWidget` の内部の3つの状態(`brushData`、
    `brushPrecisionData`、`commonBrushSizeData`)をモデルに移す。
  - `lightnessModeEnabled()`、`bakedBrushData()`、`effectiveBrushSize()` を、
    `BrushData` から導出する純粋な関数に作り直す。
  - `KisMaskingBrushOption` も同様に状態をモデルに移す。チェック状態の
    Qtシグナルによる同期と保持モードは、モデルの値として扱う。
  - マスクブラシはブラシ先端の実効サイズを読み書き両方で使い、読み込み順に
    依存する。モデルの依存関係として明示する。
- **2b: Pixel Brush本体。**
  - オプション記述子を定義し、F5画面をモデルのビューに置き換える。
  - 焼き込み(カーブの `enabledLink` と範囲、PaintingMode、LightnessStrength、
    Texture)は、モデル側の関数として実装する。
  - `KisPaintopBox` は、移行済みのブラシエンジンにだけ新しい経路を使う。

完了条件:
- **パリティテスト:** 標準のPixel Brushプリセットすべてについて、従来の
  全書き込みと新方式の結果が、キーと値の両方で一致する。
- 1つのパラメータを変えたとき、そのオプションのキーだけが書き換わることを
  テストで確認する。
- ロックされた設定、プリセットの再読み込み、汚れ表示(dirty)、Uniform
  Property、ツールバーのサイズ・不透明度との相互作用を手動で確認する。

#### フェーズ2aの実装結果(2026年10月8日)

ブラシ先端とマスクブラシの状態を、それぞれ1つの値の型にまとめた。ウィジェットは
その値を外部のカーソルとして受け取れる。既定のコンストラクタは従来どおり自分で
状態を持つため、どのブラシエンジンの設定画面も、この時点では従来の経路のまま
動く。モデルへの接続はフェーズ2b(Pixel Brush)で行う。ユーザーに見える挙動の
変更は意図していない。承認済みの2つの挙動変更(ロックの破棄で元の値に戻る、
LOD設定だけの変更で変更済みにならない)は、Deformと同じくモデルに接続した
エンジンでだけ有効になるため、2bで適用される。

**変更したファイル**

| 場所 | 内容 |
| --- | --- |
| `plugins/paintops/libpaintop/KisBrushTipOptionData.{h,cpp}`(新規) | `KisBrushTipOptionData`(`BrushData`、`PrecisionData`、共通サイズ)と `KisMaskingBrushOptionData`(`MaskingBrushData`、共通サイズ、保持モードの状態)。読み書き、焼き込み、`lightnessModeEnabled` を純粋な関数として持つ |
| `plugins/paintops/libpaintop/KisAutoBrushModel.{h,cpp}` | 焼き込みを静的関数 `bakedOptionData(data, commonBrushSize)` として追加。メンバー版はそれを呼ぶ |
| `plugins/paintops/libpaintop/KisPredefinedBrushModel.{h,cpp}` | 焼き込みを静的関数 `bakedOptionData(data, commonBrushSize, supportsHSLBrushTips)` として追加。予備のブラシ(fallback)を補う処理を `effectiveResourceData()` に切り出した |
| `plugins/paintops/libpaintop/kis_brush_option_widget.{h,cpp}` | 状態を `KisBrushTipOptionData` 1つにまとめ、外部カーソルを受け取るコンストラクタを追加。`lightnessModeEnabled()`、`bakedBrushData()`、`effectiveBrushSize()` は値から導出する |
| `plugins/paintops/libpaintop/KisMaskingBrushOption.{h,cpp}` | 状態を `KisMaskingBrushOptionData` 1つにまとめ、外部カーソルを受け取るコンストラクタを追加。保持モードは値の一部になった |
| `plugins/paintops/defaultpaintops/brush/tests/`(追加) | `KisBrushTipOptionParityTest` と、標準バンドルから取り出したPixel Brushプリセット16件、その基準ファイル |

**設計上の注意**

- **保持モード。** 読み込んだ直後は、保存されていたサイズ係数
  (`MaskingBrush/MasterSizeCoeff`)をそのまま書き戻す。マスクブラシ、
  ブラシ先端のサイズ(マスター)、マスクブラシの共通サイズのどれかが読み込み時
  から変わると終わる。モデルの監視がマスターの変化で保持モードを終えるので、
  値に残った状態と実際の書き込みが一致する。
- **保持モードはUIが落ち着いてから開始する。** サイズの入力欄は受け取った
  共通サイズを丸めて書き戻す(例: 5.96591を5.97)。値に読み込んだ時点の
  サイズを記録すると、この丸めで保持モードが終わり、係数が再計算されて
  しまう。旧コードは丸めの後に保持モードを開始していたため、
  `KisMaskingBrushOption::readOptionSetting()` は値を設定した後に
  `startPreserveMode()` で記録し直す。パリティテストで見つかった差分である。
- **保持モードの終了は変更として通知しない。** マスクブラシが変更を通知する
  のは、書き出す値(`masking` と共通サイズ)が変わったときだけ(旧コードと
  同じ)。値全体を監視すると、ブラシ先端の読み込みでマスターのサイズが
  変わったときに保持モードの終了が通知される。F5画面がプリセットを読み込んで
  いる最中にこの通知が出ると、`KisPaintopBox` が読み込み中のプリセットを
  全消去して全書き込みする。キャンバス上のShift+ドラッグでサイズを変えた
  ときに、`kis_brush_based_paintop_settings.cpp` の `this->brush()` と
  `kis_signal_compressor.cpp` の `!m_sanityIsStarting` の安全アサートが出た
  (ユーザー報告、2026年10月8日)。
- **非公開のコンストラクタは `Private *` を受け取る。** `std::optional` の
  カーソルを受け取る形にすると、`lager::state` を渡したときに公開の
  コンストラクタと曖昧になる。
- **比較演算子はcpp側のメンバー関数。** ヘッダーに `inline` で書くと、
  エクスポートされていない `PrecisionData`/`MaskingBrushData` の比較を
  呼ぶため、ライブラリの外で `lager::state` を使うとリンクできない。

**テスト(`KisBrushTipOptionParityTest`、42件すべて通過)**

- **従来の全書き込みとの一致(16件):** 標準のPixel Brushプリセット16件
  (Auto 5件、PNG 6件、GBR 5件、マスクブラシ有効4件)について、F5画面が
  読み込んで全書き込みした結果のキーと値が、変更前のコードで記録した基準
  ファイル(`data/brushtip/*.properties`)と一致する。基準は
  `SOLSTICE_WRITE_REFERENCES=1` で書き直せる。変更前のコードでも自身の基準と
  一致することを確認した(結果が決定的であること)。
- **外部カーソル(16件+4件):** 外部カーソル版と従来版のブラシ先端・
  マスクブラシが同じ値を書く。マスクブラシは、マスターのサイズを変えて保持
  モードが終わった後も一致する。
- **読み込み中の通知:** マスターのサイズの変更だけではマスクブラシが変更を
  通知しない。F5画面相当のウィジェットがプリセットを読み込む間に、編集の
  変更(`sigConfigurationItemChanged`)が出ない(Shift+ドラッグの再現。
  ツールが20回サイズを書き、`KisPaintopBox` と同じく読み込みと全書き込みを
  つなぐ)。修正前のコードでは、この2つのテストが失敗することを確認した。
- **カーソルからの変更:** カーソル経由で共通サイズを変えると、ウィジェットの
  実効サイズ、書き込み結果、`sigSettingChanged` に反映される。
- **保持モード:** 保存された係数の書き戻し、マスターまたは共通サイズの変更で
  の終了、`startPreserveMode()` による再開。
- **Lightness Map:** 画像の先端とHSL対応のエンジンでだけ有効になり、それ
  以外では表示と同じ用途に置き換わる。

テストのリソースデータベースにはブラシ先端とパターンがないため、テストの
`main()` で標準バンドル `Krita_4_Default_Resources.bundle` をストレージとして
追加している(理由は `docs/agent/wiki/pitfalls/build-format-test.md`)。

関連する既存のテスト(`KisPaintOpOptionsModelTest`、`KisBrushOpTest`、
`KisColorsmudgeOpTest`、`KisDabRenderingQueueTest`、`KisGpuBrushJobsTest`、
`KisGpuStrokeTest`、`KisMyPaintOpTest`、`KisBrushModelTest`、
`KisBrushStrokePreviewTest`、`KisPaintOpPresetTest`、
`KisCurveOptionDataTest`、`KisCurveOptionModelTest`)もすべて通過した。
全体をビルドしてインストールした。

手動確認の項目(フェーズ2a):

1. Pixel Brushで、Auto・画像(PNG/GBR)・アニメーション(GIH)・テキストの
   各ブラシ先端を選び、サイズ、角度、間隔、比率などを変更すると、描画と
   アウトラインが従来どおり更新される。
2. 画像の先端で、用途(Alpha mask、Color image、Lightness map、Gradient map)
   を切り替えられ、Lightness mapのときだけLightness Strengthが有効になる。
3. Precisionの設定(自動を含む)が保存され、読み込み直しても保たれる。
4. マスクブラシを有効にしたプリセット(`h)_Charcoal_Pencil_Medium`、
   `j)_Waterpaint_Soft_Edges` など)を選んでも変更済みにならない。
   ブラシのサイズを変えると、マスクブラシも比率を保って拡大縮小する。
5. プリセットの切り替え、再読み込み、上書き保存、新規保存で、ブラシ先端と
   マスクブラシの値が保たれる。
6. ブラシ先端を使う他のエンジン(Color Smudge、Clone、Filter、Hairy、
   Hatching、Sketch、Spray、Tangent Normal)でも、ブラシ先端の編集が従来
   どおり動く。Color Smudgeでは、画像の先端をLightness mapにしたときだけ
   Paint Thicknessが有効になることも確認する(マスクブラシを持つのは
   Pixel Brushだけ)。

#### フェーズ2bの実装結果(2026年10月8日)

Pixel Brush(`paintbrush`)の設定画面を共有モデルのビューに置き換えた。
全32オプションの状態をモデルが持ち、1つのオプションの変更はそのオプションの
キーだけを書く。Deformと同じく、承認済みの2つの挙動変更(ロックの破棄で
元の値に戻る、LOD設定だけの変更で変更済みにならない)が有効になる。
ユーザー向けの説明は `docs/brush-editor.md`。

**変更したファイル**

| 場所 | 内容 |
| --- | --- |
| `libs/ui/KisPaintOpOptionsModel.{h,cpp}` | オプション間の依存 `addDependency(dependent, source)` を追加。部分書き込みでは、変更されたオプションと、それに(推移的に)依存するオプションを書く。1オプションの書き込みを `writeOption()` に分けた |
| `plugins/paintops/libpaintop/KisBrushBasedOptionStates.{h,cpp}`(新規) | `KisBrushTipOptionState` と `KisMaskingBrushOptionState`(読み込みにリソースやブラシ先端のサイズが要るため、`KisPaintOpOptionState<Data>` ではなく基底クラスから実装)。Textureと Painting Modeの焼き込み関数 |
| `plugins/paintops/libpaintop/KisStandardOptionData.{h,cpp}` | Flow、Ratio、Softness、Darken、Mix、Hue、Saturation、Value、Strength、マスク用のOpacity・Flow・Ratio・Rotationのカーソル版作成関数。ラベルと分類は既存版と同じ |
| `plugins/paintops/libpaintop/KisTextureOptionModel.{h,cpp}` | 焼き込みを静的関数 `bakedOptionData(data, resourcesInterface)` として追加(このクラスはエクスポートされないため、外からは `KisBrushBasedOptionStates::bakeTextureOption()` を使う) |
| `plugins/paintops/libpaintop/KisPaintOpOptionStateUtils.h` | 有効リンク付きカーブの焼き込み `bakeLinkedCurveOption()` |
| `plugins/paintops/libpaintop/kis_brush_based_paintop_options_widget.{h,cpp}` | ブラシ先端のカーソルを受け取るコンストラクタ |
| `plugins/paintops/defaultpaintops/brush/kis_brushop_settings_widget.{h,cpp}` | モデルを作り、全オプションを登録してウィジェットを結び付ける。モデルは基底クラスがブラシ先端を作る前に必要なため、非公開のコンストラクタで受け取る |

**オプション間の依存**

| 依存するオプション | 依存先 | 理由 |
| --- | --- | --- |
| MaskingBrush | BrushTip | 保持モードの外では、サイズ係数を両方のサイズから計算する |
| LightnessStrength | BrushTip | 明度モードでないときは無効として書く |
| PaintingMode | MaskingBrush | マスクブラシが有効ならWASHとして書く |

読み込みは登録順に行う。ブラシ先端を最初に登録し、マスクブラシはその後に
読む(マスクブラシの読み込みはブラシ先端のサイズを使う)。外部からの読み直し
(ツールバー、Shift+ドラッグなど)では書き戻さない。これは従来のF5画面と同じ
動作である。

**テスト(`KisBrushTipOptionParityTest`、44件すべて通過)**

- 16プリセットの基準ファイルとの一致は、モデルの全読み込み・全書き込みでも
  そのまま通る(`testLegacyRewrite` はPixel Brushの設定画面を使うため、
  2b以降はモデルの経路を検証している)。
- **モデルでの編集:** 最初の編集で全書き込み、2回目以降は変更したオプションの
  キーだけが変わる。編集のたびに、プリセットの内容がモデルの全書き込みと
  一致する。マスクブラシの有効・無効によるPainting Mode、ブラシ先端の
  サイズ変更(保持モードの終了後も)によるマスクの係数、ブラシ先端の明度
  モードによるLightness Strengthを確認する。
- **Shift+ドラッグの再現:** `KisPaintopBox` と同じ配線(モデル接続時は
  全消去しない)で、サイズを20回書き換えても安全アサートが出ず、モデルが
  サイズに追従し、マスクの係数は書き換わらない。
- **依存のテストの有効性:** 3つの依存を外すと該当のテストが失敗することを、
  依存ごとに確認した。

関連する既存のテスト(`KisPaintOpOptionsModelTest`、`KisBrushOpTest`、
`KisColorsmudgeOpTest`、`KisDabRenderingQueueTest`、`KisGpuBrushJobsTest`、
`KisGpuStrokeTest`、`KisMyPaintOpTest`、`KisBrushModelTest`、
`KisBrushStrokePreviewTest`、`KisPaintOpPresetTest`、
`KisCurveOptionDataTest`、`KisCurveOptionModelTest`)もすべて通過した。
全体をビルドしてインストールした。

手動確認の項目(フェーズ2b、Pixel Brush):

1. F5画面で各ページ(ブラシ先端、Opacity・Flow・Sizeなどのカーブ、
   Spacing、Mirror、Sharpness、Scatter、色、Airbrush、Painting Mode、
   Texture、Masked Brush以下)を変更し、描画、アウトライン、プレビュー、
   変更済み表示が従来どおり更新される。
2. ツールバーのサイズ・不透明度・フロー・ブレンドモード、On-Canvas Brush
   Editor、Shift+ドラッグでのサイズ変更が、F5画面に追従する(エラーが出ない)。
3. プリセットの切り替え、再読み込み、上書き保存、新規保存で値が保たれる。
   マスクブラシやテクスチャを使うプリセットでも同様。
4. マスクブラシを有効にするとPainting ModeがWashに固定され、無効に戻すと元の
   モードに戻る。保存したプリセットにもそれが反映される。
5. 画像の先端をLightness mapにするとLightness Strengthが有効になり、他の用途
   に戻すと無効になる。
6. オプションをロックして別のプリセットに切り替え、ロックを「破棄」で解除
   すると元の値に戻る(承認済みの挙動変更)。
7. F5画面のLOD設定だけを変更しても、プリセットが変更済みにならない(承認済みの
   挙動変更)。
8. Color Smudgeなど未移行のエンジンが従来どおり動作する。

### フェーズ3: Tool Optionsへの外だし

**2026年10月8日の決定(ユーザー):** CSPのサブツール詳細の目のように、F5の
項目に目を付け、目のある項目だけをTool Optionsに表示する。カーブ系は有効・
無効のチェックボックスだけを出し、グラフは出さない。ページ内のパラメータは
主要なものから(フェーズ3b)。表示する項目はエンジン単位でkritarcに保存し、
Tool Optionsではツール設定の下に置く。下記の「設定による管理」と単位
(ページ単位)は、この決定で置き換えた。実装と手動確認の項目は
`docs/agent/tool-options-brush.md`。フェーズ3a(オプション一覧の目と
チェックボックスの外だし)は2026年10月8日に実装し、同日の手動確認で問題なし。
フェーズ3b(ページ内の主要パラメータ、図形ツール、Brush欄先頭のストローク
プレビュー)は同日に実装し、手動確認で問題なし。
フェーズ4はColor Smudgeを同日に実装し、手動確認で問題なし(下記の
「フェーズ4の実装結果: Color Smudge」)。SketchとBristleも同日に実装し、
手動確認で問題なし(「フェーズ4の実装結果: SketchとBristle(Hairy)」)。
Tangent Normal、Hatching、Filter、Quick Brushも同日に実装し、手動確認で
問題なし。Curve、Grid、Particle、Shape(Experiment)も同日に実装し、手動
確認で問題なし(2026年10月9日)。SprayとMyPaintも同日に実装し、手動確認で
問題なし。Clone(`duplicate`、計画の一覧になかった)も同日に実装し、手動確認で
問題なし。これで全エンジンが共有モデルを使う。フェーズ5も同日に実装し、手動確認で
問題なし(下記の「フェーズ5の実装結果」)。

本書の最終目的である3つの要件を実現する。

- **外だし:** Tool Optionsドッカーに、モデルのビューを追加する。対象は、F5画面の
  オプションのページ単位とする。`KisToolPaint::createOptionWidget()`
  (`libs/ui/tool/kis_tool_paint.cc:385`)のレイアウトに、ブラシ系ツールで
  共通の領域として置くことを想定する。
- **ブラシへの追従:** `CurrentPaintOpPreset` の変更を受けて、ビューを新しいモデルに
  結び直す。ブラシエンジンが変わったら、ウィジェットを作り直す。
- **設定による管理:** ブラシエンジンごとに、外だしするオプションIDの一覧を
  kritarcに保存する(キー名の例: `Solstice/ToolOptionsBrushOptions`)。
  On-Canvas Brush Editorの `brushHudSetting` とは別に管理する。設定画面は、
  `KisDlgConfigureBrushHud` の2列リスト形式を参考にする。
- Quick Accessの浮遊パッドは `sharedtooldocker` の中身を借りて表示する
  (`plugins/dockers/quickaccess/QuickAdjustDock.cpp:439`)。内容の高さが変わる
  ことに対する追従と、借りたウィジェットの返却を確認する。

完了条件:
- 選んだオプションがTool OptionsとF5画面の両方で同時に操作でき、互いに即座に
  反映される。
- ブラシを切り替えると、表示が新しいプリセットの値に更新される。
- 設定の変更がkritarcに保存され、再起動後も保たれる。
- ユーザーによる実アプリでの手動確認が完了している。

### フェーズ4: 残りのブラシエンジンの移行

Color Smudge、Hairy、Spray、Filter、MyPaintなど、残りの設定画面を順に移行する。
各エンジンで、フェーズ2と同じパリティテストを行う。移行していないエンジンでは、
Tool Optionsへの外だしを無効にする。

- Color Smudgeは、ブラシ先端の加工済みデータから始まる依存の連鎖がある。
- Sprayは、SprayShapeが `lager::with` のレンズでSprayOpに書き戻す。
- MyPaintは、すべてのオプションが共有のJSONキー `MyPaint/json` を読み書きする。
  モデルの共有キー(`addSharedKey()`)で扱う(下記の実装結果)。

#### フェーズ4の実装結果: Color Smudge(2026年10月8日)

Color Smudge(`colorsmudge`)の設定画面を共有モデルのビューに置き換えた。
全22オプションの状態をモデルが持ち、F5の目とTool Optionsの「Brush」欄も
Pixel Brushと同じく使える(Tool Options用idはモデルのid)。

**焼き込みとオプション間の依存**

| オプション | 焼き込み | 依存先 |
| --- | --- | --- |
| SmudgeLength | カーブに加え、先端が画像として使われる(用途がAlpha mask以外)ときは新エンジンを強制(`KisSmudgeLengthOptionModel::backedOptionData()` と同じ) | BrushTip |
| SmudgeRadius | 強さの範囲を、新エンジンなら0〜1、旧エンジンなら0〜3にして値を丸める(旧画面の `strengthRangeReader` と同じ) | SmudgeLength(推移的にBrushTip) |
| PaintThickness | 先端が明度モードのときだけ有効 | BrushTip |
| OverlayMode | 先端が明度モードのときは無効 | BrushTip |

ColorRate、Gradient、Strength(ラベルWeak/Strong、分類Color)は旧画面と同じ
ラベル・分類のカーブとして、モデルのカーソルで作る。Textureは旗なし。

**変更したファイル**

| 場所 | 内容 |
| --- | --- |
| `plugins/paintops/colorsmudge/kis_colorsmudgeop_settings_widget.{h,cpp}` | モデルを作り、全オプションを登録、依存を宣言、ウィジェットを結び付ける。非公開コンストラクタでモデルを受け取る(Pixel Brushと同じ) |
| `plugins/paintops/colorsmudge/CMakeLists.txt` | テスト用の静的ライブラリ `kritacolorsmudgepaintop_static`(Deformと同じ構成) |
| `plugins/paintops/colorsmudge/tests/`(追加) | `KisColorSmudgeParityTest`、Krita 3/4の同梱プリセット10件と基準ファイル30件 |
| `plugins/paintops/defaultpaintops/brush/tests/KisBrushTestMain.h` | 読み込むバンドルを指定できる `SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES` |

**テスト(`KisColorSmudgeParityTest`、34件すべて通過)**

- **従来の全書き込みとの一致(30件):** Krita 3/4の同梱プリセット10件を、
  そのまま、新エンジン+Overlay+Paint Thickness+範囲外のSmudge Radius
  (バージョン2で2.5)、RGBAバンドルのカラー先端を明度モードで使う変種の
  3通りで読み込み、全書き込みの結果を移行前のコードで記録した基準と
  比較する。同梱プリセットには新エンジンや明度モードを使うものがないため、
  変種で依存する焼き込み(新エンジンの強制、範囲の丸め、Overlayの無効化、
  Paint Thicknessの有効化)を通す。移行前のコードでも自身の基準と一致する
  ことを確認した。
- **モデルでの編集:** 編集のたびにプリセットがモデルの全書き込みと一致する。
  新エンジンの切り替えでSmudge Radiusが2.5と1を行き来し、明度モードの
  カラー先端で新エンジン強制・Paint Thickness有効・Overlay無効になる。
  4つの依存を1つずつ外すと、それぞれ失敗することを確認した。
- **Tool Options:** idが一意で、主なオプションに目が付く。

関連する既存のテスト(`KisColorsmudgeOpTest`、`KisToolOptionsBrushTest`、
`KisBrushTipOptionParityTest`、`KisPaintOpOptionsModelTest`、
`KisBrushStrokePreviewTest`、`KisPaintOpPresetTest`)もすべて通過した。
全体をビルドしてインストールした。

手動確認の項目(Color Smudge):

1. F5で各ページを変更し、描画、アウトライン、プレビュー、変更済み表示が
   従来どおり更新される。プリセットの切り替え・保存で値が保たれる。
2. 先端の用途をColor imageやLightness mapにすると、Smudge Lengthの新エンジンが
   強制され(チェックが外せない)、Smudge Radiusの範囲が0〜100%になる。
   Alpha maskに戻すと元の設定に戻る。
3. Lightness mapのときだけPaint Thicknessが有効、Overlay Modeが無効になる。
4. 目を入れた項目がTool Optionsに出て、そこでの変更が描画とF5に反映される。
5. ロックの破棄で元の値に戻り、LOD設定だけの変更では変更済みにならない
   (承認済みの挙動変更)。

#### フェーズ4の実装結果: SketchとBristle(Hairy)(2026年10月8日)

Sketch(`sketchbrush`)とBristle(`hairybrush`)の設定画面を共有モデルの
ビューに置き換えた。どちらもオプション間の依存はなく、焼き込みはカーブの
標準の焼き込みだけ。F5の目とTool Optionsの「Brush」欄も使える。

| エンジン | オプション(モデルのid) |
| --- | --- |
| Sketch | BrushTip、Sketch、CompositeOp、Opacity、Size、Rotation、LineWidth、OffsetScale、Density、Airbrush(間隔を無視する設定なし)、Rate、PaintingMode(初期値Build up) |
| Bristle | BrushTip、Bristle、Ink、CompositeOp、Opacity、Size、Rotation、PaintingMode |

あわせて次を変更した。

- **エンジン固有ページのTool Options項目:** SketchページのLine width、
  Offset scale、Density、BristleページのScale、Random offset、Shear、
  Densityを、ページのパラメータとして目を付けた。Ink depletionは
  チェックボックスのオプションなので、行の目で出せる(Ink Amountは
  `KisIntParseSpinBox` で写せないため対象外)。
- **隠した先端の設定:** Bristleは自動先端のFade、Density、Spacingを
  `hideOptions()` で隠す。`KisBrushOptionWidget::hideOptions()` が、
  隠れたコントロールのパラメータを `KisPaintOpOption::removeToolOptionsParameter()`
  で外し、目を出さないようにした。Bristleが隠そうとする事前定義先端の
  Spacing(`KisBrushChooser/Spacing`)は、該当するオブジェクト名がなく
  もともと表示されているため、目が付く。

**変更したファイル**

| 場所 | 内容 |
| --- | --- |
| `plugins/paintops/sketch/kis_sketch_paintop_settings_widget.{h,cpp}`、`plugins/paintops/hairy/kis_hairy_paintop_settings_widget.{h,cpp}` | モデルを作り、全オプションを登録し、ウィジェットを結び付ける |
| `plugins/paintops/sketch/KisSketchOpOptionWidget.cpp`、`plugins/paintops/hairy/KisHairyBristleOptionWidget.cpp` | ページのパラメータ |
| `plugins/paintops/{sketch,hairy}/CMakeLists.txt` | テスト用の静的ライブラリ |
| `plugins/paintops/libpaintop/kis_brush_option_widget.cpp`、`libs/ui/kis_paintop_option.{h,cpp}` | 隠した先端の設定の目を外す |
| `plugins/paintops/{sketch,hairy}/tests/`(追加) | `KisSketchParityTest`、`KisHairyParityTest`、Krita 3/4の同梱プリセット(Sketch 13件、Bristle 7件)と基準ファイル |
| `plugins/paintops/defaultpaintops/brush/tests/KisPaintOpParityTestUtils.h`(追加) | パリティテストの共通処理(プリセットの読み込み、全書き込み、基準との比較、Tool Optionsのid確認) |

**テスト(Sketch 30件、Bristle 18件、すべて通過)**

- **従来の全書き込みとの一致:** 同梱プリセットを、そのままと、RGBAバンドルの
  カラー先端を明度モードにした変種(両エンジンとも明度モードを持たない)で
  読み込み、移行前のコードで記録した基準と比較する。移行前のコードでも
  自身の基準と一致することを確認した。
- **モデルでの編集:** エンジン固有オプションとカーブを編集するたびに、
  プリセットがモデルの全書き込みと一致する。
- **Tool Options:** idが一意で、主なオプションに目が付く。Bristleでは
  隠した先端の設定に目がない(外す処理を無効にすると失敗することを確認)。

手動確認の項目(SketchとBristle):

1. F5で各ページを変更し、描画、プレビュー、変更済み表示が従来どおり
   更新される。プリセットの切り替え・保存で値が保たれる。
2. Bristleの自動先端にFade、Density、Spacingが出ず、目もない。
3. SketchのLine width・Offset scale・Density、BristleのScale・Random
   offset・Shear・Densityに目を入れるとTool Optionsに出て、そこでの変更が
   描画とF5に反映される。
4. Ink depletionなどチェックボックスのあるオプションは、行の目で
   Tool Optionsにチェックボックスとして出る。

#### フェーズ4の実装結果: Tangent Normal、Hatching、Filter、Quick Brush(2026年10月8日)

4つの設定画面を共有モデルのビューに置き換えた。オプション間の依存はない。
F5の目とTool Optionsの「Brush」欄も使える。

| エンジン(paintop) | オプション(モデルのid)と焼き込み |
| --- | --- |
| Tangent Normal(`tangentnormal`) | BrushTip(精度・HSL)、CompositeOp、Opacity、Flow、Size、TangentTilt、Spacing、Mirror、Softness、Sharpness、Scatter、Rotation、Airbrush、Rate、PaintingMode、Texture(パターン埋め込み)、Strength |
| Hatching(`hatchingbrush`) | BrushTip(精度)、HatchingOptions、HatchingPreferences、CompositeOp、Separation、Thickness、Angle、Crosshatching、Opacity、Size、Mirror、PaintingMode、Texture、Strength |
| Filter(`filter`) | BrushTip(精度)、CompositeOp、Opacity、Size、Rotation、Mirror、Filter(フィルタ未設定なら予備のフィルタと既定の設定を書く。`KisBrushBasedOptionStates::bakeFilterOption()` が `KisFilterOptionModel::bakeOptionData()` を呼ぶ) |
| Quick Brush(`roundmarker`) | RoundMarker、CompositeOp、Size、Spacing(ブラシ先端なし、Deformと同じ構成) |

エンジン固有ページのTool Options項目: TangentTiltのElevation Sensitivityと
Direction/Tilt Mix Value、HatchingOptionsのAngle・Separation・Thickness、
HatchingPreferencesの3つのチェックボックス、FilterのSmudge Mode、
RoundMarkerのDiameterとSpacing。フィルタの選択(`KisCmbIDList`)、
Tangent Normalの方向の種類、Hatchingの交差の種類は対象外。

**変更したファイル**

| 場所 | 内容 |
| --- | --- |
| `plugins/paintops/{tangentnormal,hatching,filterop,roundmarker}/` の設定ウィジェット | モデルを作り、全オプションを登録し、ウィジェットを結び付ける |
| `KisTangentTiltOptionWidget.cpp`、`KisHatchingOptionsWidget.cpp`、`KisHatchingPreferencesWidget.cpp`、`KisRoundMarkerOpOptionWidget.cpp`、`libpaintop/KisFilterOptionWidget.cpp` | ページのパラメータ |
| `libpaintop/KisFilterOptionModel.*`、`KisBrushBasedOptionStates.*` | Filterの静的な焼き込み |
| 4エンジンの `CMakeLists.txt` と `tests/`(追加) | テスト用の静的ライブラリ、パリティテスト、同梱プリセット(6、8、3、4件)と基準ファイル |

**テスト(すべて通過)**

- `KisTangentNormalParityTest` 16件、`KisHatchingParityTest` 20件、
  `KisFilterOpParityTest` 11件、`KisRoundMarkerParityTest` 8件。
- 同梱プリセットを、そのままと、ブラシ先端のあるエンジンではRGBAバンドルの
  カラー先端を明度モードにした変種で読み込み、移行前のコードで記録した
  基準と比較する(移行前のコードでも一致を確認)。
- 編集のたびにプリセットがモデルの全書き込みと一致する(合成モードとSize)。
- Filter: フィルタを消したプリセットでは、予備のフィルタが書かれる
  (同梱プリセットはすべてフィルタを持つため、別のテストで確認)。

手動確認の項目:

1. 4エンジンでF5の各ページを変更し、描画、プレビュー、変更済み表示が従来どおり
   更新される。プリセットの切り替え・保存で値が保たれる。
2. Filterでフィルタを切り替えると、フィルタの設定ページが切り替わり、描画に
   反映される。
3. 上のページ項目に目を入れるとTool Optionsに出て、そこでの変更が描画とF5に
   反映される。

#### フェーズ4の実装結果: Curve、Grid、Particle、Shape(Experiment)(2026年10月8日)

4つの設定画面を共有モデルのビューに置き換えた。どれもブラシ先端のない
エンジンで(Deformと同じ構成)、オプション間の依存も焼き込みもない。
F5の目とTool Optionsの「Brush」欄も使える。

| エンジン(paintop) | オプション(モデルのid) |
| --- | --- |
| Curve(`curvebrush`) | CurveOp、Opacity、LineWidth、CurvesOpacity、CompositeOp、PaintingMode |
| Grid(`gridbrush`) | GridOp、GridShape、CompositeOp、ColorOptions、PaintingMode |
| Particle(`particlebrush`) | ParticleOp(LOD制限付き)、CompositeOp、Airbrush(間隔を無視する設定なし)、Rate、PaintingMode |
| Shape(`experimentbrush`) | ExperimentOp、CompositeOp |

エンジン固有ページのTool Options項目: CurveのLine width・History size・
Curves opacity、GridのDiameter・Grid width・Grid height・Scale、
Color options(Grid、libpaintopの `KisColorOptionWidget`)のRandom HSVと
Random opacity、ParticleのParticles・Iterations・Gravity・Opacity weight、
ShapeのSpeed・Smoothing・Displace(各スライダー、目は前のチェックボックスの
前)。GridのParticle type(コンボボックス)とShapeの塗りの種類は対象外。

**テスト(すべて通過)**: `KisCurveParityTest` 9件、`KisGridParityTest` 6件、
`KisParticleParityTest` 6件、`KisExperimentParityTest` 7件。同梱プリセット
(5、2、2、3件)の全書き込みを移行前のコードで記録した基準と比較し(移行前の
コードでも一致を確認)、合成モードとエンジン固有オプションを編集するたびに
プリセットがモデルの全書き込みと一致することを確かめる。

手動確認の項目: 4エンジンでF5の各ページを変更し、描画、プレビュー、変更済み
表示が従来どおり更新される。プリセットの切り替え・保存で値が保たれる。上の
ページ項目に目を入れるとTool Optionsに出て、そこでの変更が描画とF5に反映
される。

#### フェーズ4の実装結果: SprayとMyPaint(2026年10月9日)

**Spray(`spraybrush`)**: SprayOp、SprayShape、BrushTip(ブラシ先端、基底の
ブラシ先端オプションではなく単独のページ)、Opacity、Size、CompositeOp、
ShapeDynamics、ColorOptions、Rotation、Airbrush、Rate、PaintingMode。
Spray Shapeのページは粒子の大きさをSpray Areaの直径と倍率に対する割合でも
表示するため、SprayOpの状態から直径と倍率のカーソル(`lager::with` のレンズ)を
受け取る。ただし書き込むのは自身のデータだけなので、依存は宣言しない。
Spray AreaページのDiameter、Aspect ratio、Angle、Scale、Spacingに目を付けた。

**MyPaint(`mypaintbrush`)**: 計画で保留にしていた共有JSONは、モデルの
「共有キー」で扱う。各カーブオプションは `MyPaint/json` のうち自分の
設定名の部分だけを読み書きする(読み込んで変更して書く)。全書き込みでは
`KisMyPaintOpSettings::resetSettings()` がこのキーを残すので、各オプションが
既存の文書に自分の部分を書き足す。1つのオプションの書き込みも同じ結果に
なるよう、`KisPaintOpOptionsModel::addSharedKey()` を追加した。共有キーは
オプション単位の書き込みの作業用設定に、プリセットの現在の値を入れてから
書き込ませる(空から作ると、そのオプションの部分しかない文書で上書きして
しまう。移行直後のテストで実際に起きた)。オプションのidはMyPaintの
設定名(`radius_logarithmic`、`hardness`、`opaque` など)と `Basic`、
`Airbrush`。BasicページのRadius、Hardness、Opacityは、それぞれのカーブ
オプションの基準値を編集するカーソルを受け取る(表示倍率は1)。Basic
ページのRadius Logarithmic、Hardness、Opacity、Eraserに目を付けた。

**テスト(すべて通過)**: `KisSprayParityTest` 19件(同梱プリセット5件を、
そのまま、カラー先端、比率指定の粒子形状の3通り)、`KisMyPaintParityTest`
11件(同梱のMyPaintブラシ7件。基準には `MyPaint/json` 全体を含む。編集の
テストでは、プリセットの文書にモデルの全オプションを書き足した結果と、
プリセットが一致することを確かめる)。基準はどちらも移行前のコードで記録し、
移行前のコードでも一致を確認した。`addSharedKey()` を外すとMyPaintの編集の
テストが失敗する。

手動確認の項目: SprayとMyPaintでF5の各ページを変更し、描画、プレビュー、
変更済み表示が従来どおり更新される。プリセットの切り替え・保存で値が
保たれる。MyPaintで1つの設定を変えても、他の設定(カーブを含む)が消えない。
上のページ項目に目を入れるとTool Optionsに出て、そこでの変更が描画とF5に
反映される。

#### フェーズ4の実装結果: Clone(2026年10月9日)

Clone(`duplicate`)の設定画面を共有モデルのビューに置き換えた。これで全
エンジンが共有モデルを使う。オプション: BrushTip(精度)、CompositeOp、
Opacity、Size、Rotation、Mirror、Clone(ページ)、Texture(パターン埋め込み)、
Strength。依存はない。CloneページのHealing、Correct the perspective、
Source point move、Source point reset before a new stroke、Clone From All
Visible Layersに目を付けた。

**テスト(`KisCloneParityTest` 10件、すべて通過)**: 同梱プリセット2件を、
そのまま、カラー先端、Cloneの全設定オンの3通りで、移行前のコードで記録した
基準と比較する(移行前のコードでも一致を確認)。合成モード、Clone、Sizeの
編集のたびに、プリセットがモデルの全書き込みと一致する。テストは
`plugins/paintops/defaultpaintops/brush/tests/`(静的ライブラリ
`kritadefaultpaintops_static` がCloneも含む)。

手動確認の項目: CloneでF5の各ページを変更し、描画、プレビュー、変更済み
表示が従来どおり更新される。プリセットの切り替え・保存で値が保たれる。
Ctrl+クリックでの複製元の指定とクローン描画が従来どおり動く。Cloneページの
項目に目を入れるとTool Optionsに出て、そこでの変更がF5に反映される。

### フェーズ5: 旧経路の整理

全エンジンの移行後に、`slotGuiChangedCurrentPreset()` の全消去による通常編集を
廃止する。プリセット読み込み後の最初の書き込みで全書き込みを行う規則は残す。プリセット切り替えなどの全書き込み経路は残す。Uniform Propertyを
モデルのカーソルの上に作り直すかどうかは、この時点で判断する。

#### フェーズ5の実装結果(2026年10月9日)

- **通常編集の全消去を廃止:** `KisPaintopBox::slotGuiChangedCurrentPreset()`
  は、全エンジンのモデルがプリセットに結び付いている前提で、F5の編集では
  プリセットを消さない(各オプションはモデルが1つずつ書く。ここで書くのは
  Brush Editor自身のLOD設定だけ)。モデルのない、または現在のプリセットに
  結び付いていない編集画面のための全消去と全書き込みは、警告
  (`warnKrita`)を出す安全策として残した。モデルはプリセットに結び付き、
  設定オブジェクトの差し替え(再読み込み)にも追従するため、通常は通らない。
- **全書き込みの経路は残す:** 結び付け後の最初の書き込み、プリセットの
  切り替え、`KisPaintOpSettingsWidget::writeConfiguration()` に結び付いて
  いない設定を渡したとき(保存や複製)は、モデルの全書き込み。
- **ロック:** `KisPaintOpSettingsWidget::lockProperties()` は、ロックする値を
  オプションのウィジェットではなく、モデルの同じidのオプションから書く
  (プリセットに届くのと同じデータ)。idが見つからないときだけ従来の
  ウィジェットの書き込み。テスト `KisToolOptionsBrushTest::testLockWritesModelOption`
  (ロックでモデルの書き込みと同じキーと値がロックされ、解除で消える)。
  ウィジェットとモデルの書き込みは全オプションで同じデータなので、この
  テストは両者を区別しない(契約の確認)。
- **基底クラスの旧経路は残す:** `KisPaintOpSettingsWidget` のモデルなしの
  読み書き(`startReadOptionSetting()`/`startWriteOptionSetting()` の
  ループ)は、モデルのないエンジンの安全策と、`KisPaintOpOptionsModelTest` の
  `LegacyDeformSettingsWidget`(移行前の挙動を同じテスト内で計算する)の
  ために残す。各オプションの `readOptionSetting()`/`writeOptionSetting()`
  も上流のコードのまま残す。
- **Uniform Propertyは作り直さない(判断):** (2026年10月9日、On-Canvas
  Brush Editor自体を廃止したため、Uniform Propertyを使う画面はなくなった。
  `docs/agent/popup-palette-removal.md`)On-Canvas Brush Editorの
  Uniform Propertyは、プリセットの設定のキーを直接読み書きする。モデルは
  更新プロキシの変更キー通知でその変更を読み込み、書き戻さないため、
  値の食い違いは起きない。モデルのカーソルの上に作り直すと、Tool Optionsの
  外だしと同じ仕組みを2つ持つことになり、エンジンごとのUniform Property
  定義(上流のコード)も書き換えが必要になる。利点が小さいので、現状のままと
  する。

手動確認の項目(フェーズ5): いくつかのエンジンでF5を変更し、描画と変更済み
表示が従来どおり。オプションのロックと解除(保存・破棄)が従来どおり。
On-Canvas Brush Editorでの変更がF5に反映される。プリセットの再読み込み
(Reload)後の編集で、他の設定が消えない。

**修正(2026年10月11日、ユーザー報告):** キャンバスでサイズを変えた
(Quick Brush Adjustments、Shift+ドラッグ)後、F5で別のオプション(例:
Rotation)を切り替えると、マスクブラシ付きプリセット(例: `z) 线稿 纹理`)の
線が細くなり、Rotationを戻しても細いままだった。原因: 外部の変更キー通知で
読み直すのは変わったキーを書くオプション(ブラシ先端)だけで、主ブラシの
サイズから値を導くマスクブラシは古いサイズのまま残り、次の書き込み(最初の
編集は全書き込み)で `MaskingBrush/MasterSizeCoeff` を「古いマスクサイズ /
新しい主サイズ」で書き直していた。修正: `KisPaintOpOptionsModel::
slotSettingsKeysChanged()` が、読み直すオプションに依存するオプション
(`addDependency()`)も推移的に読み直す。テスト:
`KisBrushTipOptionParityTest::testEditAfterResizeKeepsSize`(3倍にした後
Rotationを切り替えてもサイズと係数が変わらない)。

## リスク

| リスク | 影響 | 対策 |
| --- | --- | --- |
| キーの重複や条件付きの書き込み | 部分書き込みで古いキーが残る、または他のオプションのキーを消す | フェーズ0の対応表。キー差分による削除。パリティテスト |
| `bakedOptionData()` を使うオプション | 2つのビューで内部モデルの状態がずれる | フェーズ0で方式を決め、該当オプションを個別に検証する |
| オプション間の依存 | 依存先の変更が依存元に伝わらない | 依存関係を記述子に明示し、モデルのreaderで接続する |
| 外部経路の取りこぼし | Tool OptionsやF5画面の表示が古くなる | フェーズ0で経路を網羅し、取り込みのテストを用意する |
| 通知の循環や過剰な更新 | 無限ループ、スライダー操作中の負荷 | 発生元の印、値の比較、既存の圧縮タイマーの維持 |
| ロックされた設定 | 保存・破棄の動作が変わる | 全書き込み経路を残し、手動確認の項目に含める |
| 変更範囲の広さ | 16個の設定画面と共通部品に影響する | エンジン単位で移行し、未移行のエンジンは旧経路のまま動かす |
| Brush Stroke Preview | プリセットの読み込み経路に依存する | `docs/agent/brush-stroke-preview.md` の手動確認を併せて行う |

## 対象外

- 個々のスライダーなど、オプションのページより細かい単位での外だし。
  これにはオプションのウィジェットの分割が必要で、本計画の後に検討する。
- `.kpp` の設定キーの名前や形式の変更。
- ブラシエンジンの描画処理の変更。

## 手動確認の項目(フェーズ2以降)

- F5画面で各オプションを変更し、プレビュー、ブラシのアウトライン、
  汚れ表示(dirty)が従来どおり更新される。
- ツールバーのサイズ・不透明度・フロー・ブレンドモードの変更が、F5画面と
  Tool Optionsに反映される。
- On-Canvas Brush Editorでの変更が、F5画面とTool Optionsに反映される。
- プリセットの切り替え、再読み込み、上書き保存、新規保存で、値が正しく保たれる。
- ロックされた設定の保存・破棄が、従来どおりに動作する。
- マスクブラシとテクスチャを有効にしたプリセットで、値が失われない。
- Quick Accessの浮遊パッドが、Tool Optionsの高さの変化に追従する。

## 未決事項

- 外だしの単位をオプションのページにするか、ページ内の一部にするか
  (初回はページ単位を推奨)。
- Tool Options内での配置(ツール固有の設定の上か下か、折りたたみを付けるか)。
- 外だしの設定をブラシエンジンごとにするか、全エンジン共通にするか。
- **ユーザーの判断が必要な挙動の変更**(フェーズ0の結果 7章、8章)
  - オプション単位の書き込みでは、ロック前の値が `_previous` に残るようになる。
    現在は、F5画面で一度編集するとロック値で上書きされ、ロックを「破棄」しても
    元の値に戻らない。
  - 値が変わらない操作では、プリセットが変更済み(dirty)にならなくなる。
    現在は、F5画面で何か操作すると必ず変更済みになる。
- `lager::state` の伝播方式は、フェーズ0で決定済み。プリセット読み込み時の
  一括更新は `transactional_tag` と `commit()`、通常の編集は即時に伝播する。
