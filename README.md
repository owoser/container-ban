# ContainerBan

通用版「任意物品不能放入任意容器」插件（LeviLamina / BDS 1.26.20）。

由 `bundle-guard`（仅限 收纳袋→发射器/投掷器）泛化而来：用一份 JSON 规则表描述
“哪些物品不能进哪些容器”，玩家 GUI、漏斗/漏斗矿车、addItem 兜底、投发器对相邻容器
四条路径全部生效。

## 安装

1. 把 `bin/container-ban/` 整个目录放进服务端 `plugins/`。
2. 启动一次服务端：插件会在 `plugins/container-ban/config.json` 自动生成默认配置
   （收纳袋→发射器/投掷器，与 bundle-guard 行为一致）。
3. 按需编辑 `config.json`，重启生效。

## 配置格式

```jsonc
{
  "rules": [
    {
      "label": "bundle -> dispenser/dropper (default)",
      "items": ["minecraft:bundle", "*_bundle"],
      "containers": ["Dispenser", "Dropper"],
      "player": true,
      "auto": true,
      "logPlayer": true,
      "logAuto": true
    }
  ]
}
```

| 字段 | 说明 |
|---|---|
| `label` | 规则名，仅用于日志 |
| `items` | 物品匹配列表。精确名（`minecraft:bundle` 或去命名空间 `bundle`）；`*` 通配：`*_bundle` 后缀、`minecraft:*` 前缀、`*tnt*` 包含。匹配同时作用于全名与去命名空间名 |
| `containers` | 容器类型名列表（大小写不敏感，见下表）。填错会在日志里 warn 并跳过 |
| `player` | 是否拦玩家 GUI 放置/交换，默认 true |
| `auto`   | 是否拦漏斗推入 / addItem / 投发器对相邻容器推入，默认 true |
| `logPlayer` | 玩家路径拦截记 warn（含玩家名），默认 true |
| `logAuto`   | 自动化路径拦截记 debug（防漏斗重试刷屏），默认 true |

规则按数组顺序独立生效（任一命中即拦）。`items` 或 `containers` 为空 → 该规则被跳过并 warn。

### 可用容器名（ContainerType 枚举）

```
None Inventory Container Workbench Furnace Enchantment BrewingStand Anvil
Dispenser(发射器) Dropper(投掷器) Hopper Cauldron MinecartChest MinecartHopper
Horse Beacon StructureEditor Trade CommandBlock Jukebox Armor Hand
CompoundCreator ElementConstructor MaterialReducer LabTable Loom Lectern
Grindstone BlastFurnace Smoker Stonecutter Cartography Hud JigsawEditor
SmithingTable ChestBoat DecoratedPot Crafter
```

> 说明：Barrel/箱子/潜影盒等在 BDS 界面里通常复用 `Container` 这一通用容器类型；
> 如需精确覆盖某方块请用日志实测（warn/debug 会打印实际命中的容器名）。未知容器名会
> 在加载配置时 warn，不会使插件崩溃。

## 行为与日志

- 玩家操作被拒 → 原版错误码（物品自动弹回、不丢失），记 `warn`（含玩家名/物品/容器/规则名）。
- 自动化路径被拒 → 容器收下失败/物品留在原容器，只记 `debug`。
- 取出、容器内部挪动、往未受限容器放物品 → 一律放行。

## 稳定性

沿用 bundle-guard 的崩溃加固：物品读取全部带 SEH 保护 + 野指针过滤（MCFOLD 折叠函数
可能以错位形参调用钩子，实测出现过 Item 指针 == 0x1），任何异常按“不匹配”放行，
不会崩服。

## 构建

```bash
xmake f -y -p windows -a x64 -m release
xmake
# 产物：bin/container-ban/
```

注意：本机构建需要把 VS 自带 LLVM（如 `D:\vs\VC\Tools\Llvm\x64\bin`）加入 PATH，
xmake 才能找到 clang-cl 工具链。
