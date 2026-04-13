# Cue Engine 設計案: GameWorld / GameScene / GameObject

## 目的

Cue Engine における **GameWorld / GameScene / GameObject** の責務を明確に分離し、
以下を破綻なく扱える実行時構造を定義する。

- Scene のロード / アンロード
- GameObject の生成 / 破棄
- ECS との接続
- Persistent Object の保持
- Scene 単位の管理
- Editor 保存用データと Runtime 実体の分離

---

## 結論

設計の中心は次の 3 点で固定する。

1. **GameWorld が Runtime 上の唯一の所有者である**
2. **GameScene は実行中の実体ではなく、データとインスタンスを分離する**
3. **GameObject は Entity への薄いハンドルであり、所有者ではない**

この方針により、責務の衝突と二重所有を防ぐ。

---

## 問題設定

現在の案では、以下の概念がまだ混ざっている。

- Scene を保存するための静的データ
- World にロードされた実行中 Scene
- 実体として生きている Entity
- Entity へアクセスするための GameObject API

このまま進めると、後で次の問題が出る。

- Scene が live object を持つのか、World が持つのか曖昧になる
- Scene unload 時の削除責任が不明になる
- Persistent Object の所属先が壊れる
- Editor 保存用の Scene と Runtime の Scene が混ざる
- 古い GameObject ハンドルが破棄済み Entity を指す

したがって、まず概念を分離する必要がある。

---

## 推奨する概念分離

### 1. GameWorld

Runtime 上の世界そのもの。

**責務**

- 実行中 Entity の唯一の所有
- ECS の所有
- SceneInstance の管理
- Scene のロード / アンロード
- Persistent Object の保持
- Entity と Scene の所属関係の管理

**ポイント**

- live entity はすべて GameWorld に存在する
- Scene は World にオブジェクトを投入するが、所有はしない

---

### 2. SceneAsset

Scene の保存データ。
JSON などで保存・読み込みされる静的データ。

**責務**

- Scene 内に配置されるオブジェクト定義の保持
- 各オブジェクトの初期コンポーネント値の保持
- 親子関係、Prefab 参照、名前などの保存
- Editor で編集される単位

**ポイント**

- SceneAsset は Entity を持たない
- まだ Runtime 実体ではない

---

### 3. SceneInstance

SceneAsset を GameWorld にロードした結果として存在する Runtime インスタンス。

**責務**

- どの SceneAsset を元にしているか保持
- この Scene から生成された Entity の一覧を保持
- Active / Loaded / Unloading などの状態を管理
- unload 時に、自分由来の Entity をまとめて削除する

**ポイント**

- SceneAsset と SceneInstance は分ける
- 名前を `GameScene` にするなら、役割はこの SceneInstance 側に寄せる

---

### 4. GameObject

Entity へアクセスするための軽量ハンドル。

**責務**

- EntityId を保持する
- GameWorld 経由で Component にアクセスする
- `get_component<T>()` `add_component<T>()` などを提供する
- validity を確認する

**ポイント**

- GameObject は所有者ではない
- GameObject 自体に大量の状態を持たせない
- 薄いラッパーに徹する

---

## 全体構造

```text
SceneAsset(JSON)
    ↓ instantiate
GameWorld
    ├─ ECSManager
    ├─ live entities
    ├─ SceneInstance[]
    └─ Persistent entities

GameObject = { GameWorld* + EntityId + Generation }
```

---

## 所有権の原則

### Runtime 所有権

- **GameWorld** が live entity を所有する
- **SceneInstance** は「この Scene から生成された entity の一覧」を保持するだけ
- **GameObject** は非所有ハンドル

### 保存データ所有権

- **SceneAsset** が object definition を保持する
- object definition は GameObject ではなく保存用データ

---

## なぜ Scene が live object を所有してはいけないか

Scene まで live object を所有すると、次の二重管理が起こる。

- World も object を管理する
- Scene も object を管理する

この時点で次が壊れる。

- 破棄順
- cross-scene object
- persistent object
- scene unload 時の管理
- ECS との整合性

したがって、Scene は live entity を **所有しない**。
保持するのは「この Scene 由来の Entity 一覧」までにする。

---

## ID と生存確認

Entity を安全に扱うには、単なる `EntityId` だけでは足りない。
再利用された ID に古い GameObject がアクセスできてしまうためである。

そのため、次を持つ。

- `EntityId`
- `Generation`

GameObject はこの両方を保持し、アクセス時に World 側で一致を確認する。

---

## 基本データ

### BaseComponent

すべての Entity に最低限持たせたい共通情報。

```cpp
struct BaseComponent final
{
    std::string name;
    std::string tag;
    SceneId owningSceneId = k_invalidSceneId;
    EntityId parent = k_invalidEntityId;
    bool isActiveSelf = true;
    bool isPersistent = false;
};
```

### 役割

- 名前
- タグ
- Scene 所属
- 親 Entity
- Active 状態
- Persistent 判定

これがないと、Scene 管理と Editor 連携が散らかる。

---

## 保存用データ構造

### ObjectDefinition

SceneAsset に含まれる保存用オブジェクト定義。

```cpp
struct ObjectDefinition final
{
    uint64_t localObjectId = 0;
    std::string name;
    std::optional<uint64_t> parentLocalObjectId;
    bool isActive = true;
    bool isPersistent = false;
    std::vector<ComponentBlob> components;
};
```

### ポイント

- `localObjectId` は SceneAsset 内だけで有効
- Runtime の EntityId とは別物
- 親子解決のために local id を持つ

---

## Runtime データ構造

### SceneInstance

```cpp
struct SceneInstance final
{
    SceneId sceneId = k_invalidSceneId;
    const SceneAsset* asset = nullptr;
    std::vector<EntityId> entities;
    bool isLoaded = false;
    bool isActive = true;
};
```

### EntityRecord

```cpp
struct EntityRecord final
{
    uint32_t generation = 0;
    bool isAlive = false;
};
```

---

## クラス責務

## GameWorld

### 主な責務

- Entity の作成 / 破棄
- Scene のロード / アンロード
- ECSManager の保持
- SceneInstance の保持
- Persistent Object の管理
- entity validity の検証

### 想定 API

```cpp
class GameWorld final
{
public:
    GameObject create_object(std::string_view a_name);
    void destroy_object(EntityId a_entityId);

    SceneId load_scene(const SceneAsset& a_asset);
    void unload_scene(SceneId a_sceneId);

    GameObject find_object(EntityId a_entityId) noexcept;

    template <typename T>
    T* get_component(EntityId a_entityId) noexcept;

private:
    ECSManager m_ecs;
    std::unordered_map<SceneId, SceneInstance> m_scenes;
};
```

---

## GameObject

### 主な責務

- Entity に対する user-facing API
- Component の取得 / 追加 / 削除
- validity 確認

### 想定 API

```cpp
class GameObject final
{
public:
    GameObject() = default;
    GameObject(GameWorld* a_world, EntityId a_entityId, uint32_t a_generation) noexcept;

    bool is_valid() const noexcept;
    EntityId entity_id() const noexcept;

    template <typename T>
    T* get_component() noexcept;

    template <typename T, typename... Args>
    T& add_component(Args&&... a_args);

    template <typename T>
    bool has_component() const noexcept;

    template <typename T>
    void remove_component() noexcept;

private:
    GameWorld* m_world = nullptr;
    EntityId m_entityId = k_invalidEntityId;
    uint32_t m_generation = 0;
};
```

### ポイント

- 軽量コピー可能な値型ハンドルにする
- 所有権は持たない
- World を通して ECS へアクセスする

---

## SceneAsset

### 主な責務

- 保存用 object definition の保持
- Serialize / Deserialize
- Editor 編集対象

### 想定 API

```cpp
class SceneAsset final
{
public:
    const std::vector<ObjectDefinition>& objects() const noexcept;

private:
    std::vector<ObjectDefinition> m_objects;
};
```

---

## Scene ロードの流れ

`GameWorld::load_scene()` は次の順で処理する。

1. `SceneId` を発行する
2. `SceneInstance` を作成する
3. `SceneAsset` の `ObjectDefinition` を走査する
4. 各定義に対して Entity を生成する
5. `BaseComponent.owningSceneId = sceneId` を設定する
6. Component を Entity に追加する
7. `localObjectId -> EntityId` の変換表を作る
8. 全 object 生成後に親子関係を解決する
9. `SceneInstance.entities` に登録する
10. `GameWorld.m_scenes` に追加する

### なぜ 2 パス必要か

1 パスで親子解決しようとすると、親がまだ生成されていないケースで詰む。
先に全部生成し、あとで local id を使って接続する方が安定する。

---

## Scene アンロードの流れ

`GameWorld::unload_scene()` は次の順で処理する。

1. `SceneInstance` を取得する
2. その Scene 由来 Entity 一覧を走査する
3. `isPersistent == false` の entity を削除する
4. `SceneInstance` を削除する

### ポイント

- 全 entity を総当たりして `owningSceneId` を探すのはやめる
- `SceneInstance.entities` を持っているので、それを使う
- unload の責務は GameWorld に置く

---

## Scene 所属のルール

推奨は **1 Entity は 0 または 1 Scene にのみ所属** である。

### パターン

- 通常 entity: `owningSceneId = valid scene id`
- persistent entity: `owningSceneId = invalid`

### 理由

複数 Scene 所属を許すと、次が面倒になる。

- unload 条件
- hierarchy 所属
- serialize 対象
- editor 表示
- cross-scene dependency

最初は単一所属で固定した方がよい。

---

## Persistent Object

Persistent Object は、Scene をまたいで残る Runtime Entity。

### ルール案

- `BaseComponent.isPersistent = true`
- `BaseComponent.owningSceneId = k_invalidSceneId`
- Scene unload 時に削除対象から除外する

### 注意点

Persistent Object を後で追加しようとすると API が壊れやすい。
最初から設計に入れておく。

---

## 親子関係

親子関係は SceneAsset 側では local id で保存し、Runtime 側では EntityId に変換する。

### 保存時

- `parentLocalObjectId`

### Runtime

- `BaseComponent.parent`
- 必要なら `TransformComponent` でも親参照を持つ

### 注意点

- Scene 跨ぎ parent-child は最初は禁止でよい
- Persistent Object を親にするかどうかは後で仕様化する

---

## ECS との関係

### 推奨方針

- 実データは ECSManager が持つ
- GameWorld は ECSManager の所有者
- GameObject は ECS へのアクセス窓口

### こうしない理由

GameObject 自身にコンポーネントを持たせると、ECS と OO の二重構造になり破綻しやすい。

---

## Editor との関係

Editor で触るのは基本的に `SceneAsset`。
Runtime で存在するのは `SceneInstance + live entity`。

### つまり

- 保存対象: `SceneAsset`
- 実行対象: `GameWorld`
- 表示対象: `SceneAsset` と `SceneInstance` を使い分ける

### Editor 上のメリット

- 未ロード Scene も編集できる
- Runtime の変更を保存用データと分けられる
- Play 中変更の扱いを整理しやすい

---

## よくある失敗

### 1. GameScene に live object を持たせる

World と Scene の二重所有になる。
やめるべき。

### 2. GameObject を巨大クラスにする

責務過多になる。
Handle に徹する。

### 3. SceneAsset と SceneInstance を同一クラスにする

保存と実行の責務が混ざる。
後で壊れる。

### 4. EntityId だけで validity を保証した気になる

ID 再利用で破綻する。
Generation を入れる。

### 5. Persistent Object を後回しにする

あとから足すと unload / serialize / hierarchy が崩れる。

---

## 実装順序

### 第1段階

- EntityId + Generation
- ECSManager
- BaseComponent
- GameObject ハンドル

### 第2段階

- SceneAsset
- ObjectDefinition
- JSON serialize / deserialize

### 第3段階

- GameWorld::load_scene()
- GameWorld::unload_scene()
- SceneInstance

### 第4段階

- parent-child
- active state
- persistent object

### 第5段階

- Prefab instantiate
- additive scene load
- cross-scene validation
- save / load 連携

---

## 推奨命名

曖昧さを減らすため、次の命名を推奨する。

- `GameWorld`
- `SceneAsset`
- `SceneInstance`
- `GameObject`

もし `GameScene` という名前を残したいなら、少なくとも次のように分ける。

- `GameSceneData` = 保存用
- `GameScene` = Runtime 用

ただし、意味が明快なのは `SceneAsset / SceneInstance` である。

---

## 最終方針

Cue Engine では、次の原則で設計する。

### 原則

- live entity は **GameWorld のみが所有** する
- Scene は **保存データ** と **Runtime インスタンス** に分離する
- GameObject は **Entity への薄いハンドル** に留める
- Scene unload は **GameWorld が責任を持つ**
- Persistent Object は **最初から考慮する**

### これにより得られるもの

- ownership が明快になる
- Scene の出し入れが単純になる
- Editor と Runtime の責務を分けられる
- ECS との整合性が保ちやすい
- 後で Prefab / additive load / persistence を拡張しやすい

---

## 次の具体化候補

この設計案の次段階として、以下を定義すると実装に進める。

1. `GameWorld.h`
2. `GameObject.h`
3. `SceneAsset.h`
4. `SceneInstance.h`
5. `BaseComponent.h`
6. `ObjectDefinition` の JSON 形式
7. `load_scene()` / `unload_scene()` の詳細フロー

必要なら次に、Cue Engine のコーディング規約に合わせて
**ヘッダ / cpp 分割の最小実装案** をそのまま出す。

