#include "mod/ContainerBan.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <excpt.h>
#include <nlohmann/json.hpp>

#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"

#include "mc/deps/shared_types/legacy/ContainerType.h"
#include "mc/world/Container.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/containers/ContainerEnumName.h"
#include "mc/world/inventory/network/ContainerScreenContext.h"
#include "mc/world/inventory/network/ItemStackNetResult.h"
#include "mc/world/inventory/network/ItemStackRequestActionHandler.h"
#include "mc/world/inventory/network/ItemStackRequestActionTransferBase.h"
#include "mc/world/inventory/network/ItemStackRequestActionType.h"
#include "mc/world/inventory/network/ItemStackRequestHandlerSlotInfo.h"
#include "mc/world/inventory/network/ItemStackRequestSlotInfo.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/actor/DropperBlockActor.h"

// LL_TYPE_*_HOOK 手动注册宏会生成未被引用的 _AutoHookCount，属预期行为；
// mOldItem 空引用比较按 C++ 语义恒为假，告警可忽略（真正的保护在 SEH 兜底里）。
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wtautological-undefined-compare"
#endif

namespace container_ban {
namespace {

using CT = ::SharedTypes::Legacy::ContainerType;

ll::io::Logger& logger() { return ll::mod::NativeMod::current()->getLogger(); }

// ---------------------------------------------------------------------------
// 容器类型：索引 / 名字表
// ---------------------------------------------------------------------------

// ContainerType 取值区间 -9..36，统一平移 16 后落到 [7, 52]，用 64 长度数组做位图/索引。
constexpr int kTypeShift = 16;
constexpr int kTypeCount = 64;

[[nodiscard]] int typeIdx(CT type) { return static_cast<int>(type) + kTypeShift; }

struct TypeNameEntry {
    char const* name;
    CT          type;
};

constexpr TypeNameEntry kTypeNames[] = {
    {"None",               CT::None              },
    {"Inventory",          CT::Inventory         },
    {"Container",          CT::Container         },
    {"Workbench",          CT::Workbench         },
    {"Furnace",            CT::Furnace           },
    {"Enchantment",        CT::Enchantment       },
    {"BrewingStand",       CT::BrewingStand      },
    {"Anvil",              CT::Anvil             },
    {"Dispenser",          CT::Dispenser         },
    {"Dropper",            CT::Dropper           },
    {"Hopper",             CT::Hopper            },
    {"Cauldron",           CT::Cauldron          },
    {"MinecartChest",      CT::MinecartChest     },
    {"MinecartHopper",     CT::MinecartHopper    },
    {"Horse",              CT::Horse             },
    {"Beacon",             CT::Beacon            },
    {"StructureEditor",    CT::StructureEditor   },
    {"Trade",              CT::Trade             },
    {"CommandBlock",       CT::CommandBlock      },
    {"Jukebox",            CT::Jukebox           },
    {"Armor",              CT::Armor             },
    {"Hand",               CT::Hand              },
    {"CompoundCreator",    CT::CompoundCreator   },
    {"ElementConstructor", CT::ElementConstructor},
    {"MaterialReducer",    CT::MaterialReducer   },
    {"LabTable",           CT::LabTable          },
    {"Loom",               CT::Loom              },
    {"Lectern",            CT::Lectern           },
    {"Grindstone",         CT::Grindstone        },
    {"BlastFurnace",       CT::BlastFurnace      },
    {"Smoker",             CT::Smoker            },
    {"Stonecutter",        CT::Stonecutter       },
    {"Cartography",        CT::Cartography       },
    {"Hud",                CT::Hud               },
    {"JigsawEditor",       CT::JigsawEditor      },
    {"SmithingTable",      CT::SmithingTable     },
    {"ChestBoat",          CT::ChestBoat         },
    {"DecoratedPot",       CT::DecoratedPot      },
    {"Crafter",            CT::Crafter           },
};

[[nodiscard]] char const* typeName(CT type) {
    for (auto const& e : kTypeNames) {
        if (e.type == type) return e.name;
    }
    return "Unknown";
}

[[nodiscard]] bool parseTypeName(std::string_view name, CT& out) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    for (auto const& e : kTypeNames) {
        std::string cand(e.name);
        for (char& c : cand) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (cand == lower) {
            out = e.type;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 规则模型
// ---------------------------------------------------------------------------

struct Pattern {
    enum class Kind : unsigned char { Exact, Prefix, Suffix, Contains };
    Kind        kind{Kind::Exact};
    std::string text;
};

[[nodiscard]] Pattern parsePattern(std::string_view raw) {
    Pattern p;
    if (raw.size() >= 2 && raw.front() == '*' && raw.back() == '*') {
        p.kind = Pattern::Kind::Contains;
        p.text = std::string(raw.substr(1, raw.size() - 2));
    } else if (raw.front() == '*') {
        p.kind = Pattern::Kind::Suffix;
        p.text = std::string(raw.substr(1));
    } else if (raw.back() == '*') {
        p.kind = Pattern::Kind::Prefix;
        p.text = std::string(raw.substr(0, raw.size() - 1));
    } else {
        p.kind = Pattern::Kind::Exact;
        p.text = std::string(raw);
    }
    return p;
}

[[nodiscard]] bool matchOnePattern(Pattern const& p, std::string_view cand) {
    switch (p.kind) {
    case Pattern::Kind::Exact:
        return cand == p.text;
    case Pattern::Kind::Prefix:
        return cand.size() >= p.text.size() && cand.substr(0, p.text.size()) == p.text;
    case Pattern::Kind::Suffix:
        return cand.size() >= p.text.size() && cand.substr(cand.size() - p.text.size()) == p.text;
    case Pattern::Kind::Contains:
        return cand.find(p.text) != std::string_view::npos;
    }
    return false;
}

struct Rule {
    std::string                  label;
    std::vector<Pattern>         items;             // 空 = 规则不参与匹配
    std::array<bool, kTypeCount> containerBits{};   // 按 typeIdx 索引
    bool                         player    = true;  // 是否拦玩家 GUI Place/Swap
    bool                         autoApply = true;  // 是否拦漏斗/addItem/投发器推入
    bool                         logPlayer = true;  // 玩家路径拦截记 warn
    bool                         logAuto   = true;  // 自动化路径拦截记 debug
};

std::vector<Rule>                               gRules;
std::array<std::vector<Rule const*>, kTypeCount> gByType;

void rebuildIndex() {
    for (auto& v : gByType) v.clear();
    for (auto const& r : gRules) {
        for (int i = 0; i < kTypeCount; ++i) {
            if (r.containerBits[i]) gByType[i].push_back(&r);
        }
    }
}

// ---------------------------------------------------------------------------
// 受 SEH 保护的物品读取（吸取 canPushInItem 野指针崩溃教训：任何异常/非法指针
// 一律按“不匹配”处理，绝不把服务端读崩）
// ---------------------------------------------------------------------------

[[nodiscard]] bool safeMatchesRule(::ItemStack const& item, Rule const& rule) {
    if (rule.items.empty()) return false;
    ::Item const* raw = nullptr;
    __try {
        raw = item.getItem();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (raw == nullptr) return false;
    if (reinterpret_cast<uintptr_t>(raw) < 0x10000) return false;
    __try {
        ::std::string const& full   = raw->getFullItemName();
        std::string_view     fullV(full);
        std::string_view     shortV = fullV;
        auto const           pos    = fullV.rfind(':');
        if (pos != std::string_view::npos) shortV = fullV.substr(pos + 1);
        for (auto const& p : rule.items) {
            if (matchOnePattern(p, fullV) || matchOnePattern(p, shortV)) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

[[nodiscard]] std::string safeItemText(::ItemStack const& item) {
    ::Item const* raw = nullptr;
    __try {
        raw = item.getItem();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "<invalid>";
    }
    if (raw == nullptr) return "<empty>";
    if (reinterpret_cast<uintptr_t>(raw) < 0x10000) return "<invalid>";
    __try {
        return raw->getFullItemName();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "<invalid>";
    }
}

/// 自动化路径：目标容器是受限类型且物品命中规则 → 返回命中的规则（用于日志）。
[[nodiscard]] Rule const* matchAutoRule(::Container const& container, ::ItemStack const& item) {
    CT type{};
    __try {
        type = container.getContainerType();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    int const idx = typeIdx(type);
    if (idx < 0 || idx >= kTypeCount) return nullptr;
    for (Rule const* r : gByType[idx]) {
        if (r->autoApply && safeMatchesRule(item, *r)) return r;
    }
    return nullptr;
}

[[nodiscard]] std::string containerText(::Container const& container) {
    CT type{};
    __try {
        type = container.getContainerType();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "?";
    }
    return typeName(type);
}

// ---------------------------------------------------------------------------
// 玩家 GUI 路径
// ---------------------------------------------------------------------------

/// 请求槽位是否属于玩家一侧（物品栏 / 快捷栏 / 光标 / 副手 / 盔甲 / 手持物品界面）。
bool isPlayerSideSlot(::ItemStackRequestSlotInfo const& slot) {
    switch (slot.mFullContainerName.mName) {
    case ::ContainerEnumName::CursorContainer:
    case ::ContainerEnumName::CombinedHotbarAndInventoryContainer:
    case ::ContainerEnumName::HotbarContainer:
    case ::ContainerEnumName::InventoryContainer:
    case ::ContainerEnumName::OffhandContainer:
    case ::ContainerEnumName::ArmorContainer:
    case ::ContainerEnumName::DynamicContainer:
        return true;
    default:
        return false;
    }
}

struct SlotMatch {
    Rule const* rule{nullptr};
    std::string itemText;
};

/// 读取槽位当前物品；命中“该界面容器类型”的玩家规则时连同物品文本一起返回。
/// 槽位解析失败 / 空引用 / 任何异常都按“未命中”处理（交回原版校验）。
[[nodiscard]] SlotMatch
matchSlotRule(::ItemStackRequestActionHandler& handler, ::ItemStackRequestSlotInfo const& slot, int screenIdx) {
    __try {
        auto info = handler._validateRequestSlot(slot, true, false);
        for (Rule const* r : gByType[screenIdx]) {
            if (r->player && safeMatchesRule(info.mOldItem, *r)) {
                return SlotMatch{r, safeItemText(info.mOldItem)};
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // 读槽位/读物品时任何异常都视为“未命中”，交回原版。
    }
    return SlotMatch{};
}

::ItemStackNetResult checkPlayerTransfer(
    ::ItemStackRequestActionHandler&            handler,
    ::ItemStackRequestActionTransferBase const& action
) {
    CT const screenType = handler.getScreenContext().getScreenContainerType();
    int const screenIdx = typeIdx(screenType);
    if (screenIdx < 0 || screenIdx >= kTypeCount) return ::ItemStackNetResult::Success;
    bool anyPlayerRule = false;
    for (Rule const* r : gByType[screenIdx]) {
        if (r->player) {
            anyPlayerRule = true;
            break;
        }
    }
    if (!anyPlayerRule) return ::ItemStackNetResult::Success;

    auto const& srcSlot = action.mSrc.get();
    auto const& dstSlot = action.mDst.get();
    bool const  srcPlayer = isPlayerSideSlot(srcSlot);
    bool const  dstPlayer = isPlayerSideSlot(dstSlot);
    // 两侧都在同一侧（都在容器内 = 内部挪动 / 都在玩家侧 = 整理背包）→ 放行。
    if (srcPlayer == dstPlayer) return ::ItemStackNetResult::Success;

    ::ItemStackRequestActionType const actionType = action.getActionType();

    auto const deny = [&](::ItemStackNetResult code, SlotMatch const& m, char const* verb) -> ::ItemStackNetResult {
        if (m.rule && m.rule->logPlayer) {
            logger().warn(
                "已拒绝玩家 {} {} {}：不能放入{}（规则: {}）",
                handler.mPlayer.getNameTag(),
                verb,
                m.itemText,
                typeName(screenType),
                m.rule->label.empty() ? "(unnamed)" : m.rule->label
            );
        }
        return code;
    };

    switch (actionType) {
    case ::ItemStackRequestActionType::Place:
        // dst 在容器侧、src 在玩家侧：放进容器的就是 src 上的物品。
        if (!dstPlayer && srcPlayer) {
            if (auto m = matchSlotRule(handler, srcSlot, screenIdx); m.rule) {
                return deny(::ItemStackNetResult::CannotPlaceItem, m, "放置");
            }
        }
        break;
    case ::ItemStackRequestActionType::Swap:
        // 交换后进入容器的是“另一侧（玩家侧）”的物品，两端都可能，故两种朝向都查。
        if (!dstPlayer && srcPlayer) {
            if (auto m = matchSlotRule(handler, srcSlot, screenIdx); m.rule) {
                return deny(::ItemStackNetResult::CannotSwapItem, m, "交换");
            }
        } else if (dstPlayer && !srcPlayer) {
            if (auto m = matchSlotRule(handler, dstSlot, screenIdx); m.rule) {
                return deny(::ItemStackNetResult::CannotSwapItem, m, "交换");
            }
        }
        break;
    default:
        break;
    }
    return ::ItemStackNetResult::Success;
}

// ---------------------------------------------------------------------------
// 5 个钩子（与 bundle-guard 同款选点，判定换成规则引擎）
// ---------------------------------------------------------------------------

LL_TYPE_INSTANCE_HOOK(
    ItemStackRequestTransferHook,
    HookPriority::Normal,
    ::ItemStackRequestActionHandler,
    &::ItemStackRequestActionHandler::_handleTransfer,
    ::ItemStackNetResult,
    ::ItemStackRequestActionTransferBase const& requestAction,
    bool                                        isSrcHintSlot,
    bool                                        isDstHintSlot,
    bool                                        isSwap
) {
    ::ItemStackNetResult denied = checkPlayerTransfer(*this, requestAction);
    if (denied != ::ItemStackNetResult::Success) return denied;
    return origin(requestAction, isSrcHintSlot, isDstHintSlot, isSwap);
}

LL_TYPE_INSTANCE_HOOK(
    ContainerCanPushInItemHook,
    HookPriority::Normal,
    ::Container,
    &::Container::$canPushInItem,
    bool,
    int                slot,
    int                face,
    ::ItemStack const& item
) {
    if (auto* r = matchAutoRule(*this, item)) {
        if (r->logAuto) {
            logger().debug("已阻止 {} 被自动化推入{}（规则: {}）", safeItemText(item), containerText(*this), r->label);
        }
        return false;
    }
    return origin(slot, face, item);
}

LL_TYPE_INSTANCE_HOOK(
    ContainerAddItemHook,
    HookPriority::Normal,
    ::Container,
    &::Container::$addItem,
    bool,
    ::ItemStack& item
) {
    if (auto* r = matchAutoRule(*this, item)) {
        if (r->logAuto) {
            logger().debug("已阻止 {} 通过 addItem 进入{}（规则: {}）", safeItemText(item), containerText(*this), r->label);
        }
        return false;
    }
    return origin(item);
}

LL_TYPE_STATIC_HOOK(
    DropperMoveInItemsHook,
    HookPriority::Normal,
    ::DropperBlockActor,
    &::DropperBlockActor::_tryMoveInItemsAndDepleteStack,
    bool,
    ::Container& container,
    ::ItemStack& item,
    int          stackSizeLimit,
    int          slot,
    int          face
) {
    if (auto* r = matchAutoRule(container, item)) {
        if (r->logAuto) {
            logger().debug("已阻止 {} 被投发器移入{}（规则: {}）", safeItemText(item), containerText(container), r->label);
        }
        return false;
    }
    return origin(container, item, stackSizeLimit, slot, face);
}

LL_TYPE_STATIC_HOOK(
    DropperAddItemsToContainerHook,
    HookPriority::Normal,
    ::DropperBlockActor,
    &::DropperBlockActor::addItemsToContainerAndDepleteStack,
    bool,
    ::Container& container,
    ::ItemStack& item,
    int          stackSizeLimit,
    int          face
) {
    if (auto* r = matchAutoRule(container, item)) {
        if (r->logAuto) {
            logger().debug("已阻止 {} 被投发器添加进{}（规则: {}）", safeItemText(item), containerText(container), r->label);
        }
        return false;
    }
    return origin(container, item, stackSizeLimit, face);
}

// ---------------------------------------------------------------------------
// 配置加载
// ---------------------------------------------------------------------------

constexpr std::string_view kDefaultConfig = R"cfg({
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
)cfg";

[[nodiscard]] bool applyConfig(nlohmann::json const& root) {
    gRules.clear();
    if (!root.contains("rules") || !root.at("rules").is_array()) {
        logger().error("config.json 缺少 rules 数组");
        return false;
    }
    for (auto const& entry : root.at("rules")) {
        Rule r;
        if (entry.contains("label") && entry.at("label").is_string()) {
            r.label = entry.at("label").get<std::string>();
        }
        if (entry.contains("items") && entry.at("items").is_array()) {
            for (auto const& it : entry.at("items")) {
                if (it.is_string()) r.items.push_back(parsePattern(it.get<std::string>()));
            }
        }
        if (entry.contains("containers") && entry.at("containers").is_array()) {
            for (auto const& c : entry.at("containers")) {
                if (!c.is_string()) continue;
                CT type{};
                if (!parseTypeName(c.get<std::string>(), type)) {
                    logger().warn("规则 \"{}\" 含未知容器名: {}", r.label, c.get<std::string>());
                    continue;
                }
                int const idx = typeIdx(type);
                if (idx >= 0 && idx < kTypeCount) r.containerBits[idx] = true;
            }
        }
        if (entry.contains("player") && entry.at("player").is_boolean()) r.player = entry.at("player").get<bool>();
        if (entry.contains("auto") && entry.at("auto").is_boolean()) r.autoApply = entry.at("auto").get<bool>();
        if (entry.contains("logPlayer") && entry.at("logPlayer").is_boolean()) {
            r.logPlayer = entry.at("logPlayer").get<bool>();
        }
        if (entry.contains("logAuto") && entry.at("logAuto").is_boolean()) {
            r.logAuto = entry.at("logAuto").get<bool>();
        }
        bool const hasContainers = std::any_of(r.containerBits.begin(), r.containerBits.end(), [](bool b) { return b; });
        if (r.items.empty() || !hasContainers) {
            logger().warn("规则 \"{}\" 被跳过：items 与 containers 均不能为空", r.label.empty() ? "(unnamed)" : r.label);
            continue;
        }
        int containerCount = 0;
        for (bool b : r.containerBits) containerCount += b ? 1 : 0;
        logger().info("规则已加载: {} ({} 个物品匹配, {} 种容器)", r.label, r.items.size(), containerCount);
        gRules.push_back(std::move(r));
    }
    rebuildIndex();
    return true;
}

void loadConfig(std::filesystem::path const& configDir) {
    std::error_code ec;
    (void)std::filesystem::create_directories(configDir, ec);

    auto const configPath = configDir / "config.json";
    std::string content;
    if (std::filesystem::exists(configPath, ec)) {
        std::ifstream ifs(configPath);
        content.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    } else {
        content = kDefaultConfig;
        std::ofstream ofs(configPath);
        ofs << content;
        logger().info("未找到 config.json，已生成默认配置: {}", configPath.string());
    }

    try {
        if (applyConfig(nlohmann::json::parse(content))) return;
    } catch (std::exception const& e) {
        logger().error("config.json 解析失败（{}），回退到默认规则", e.what());
    }
    // 文件损坏或解析失败：用内置默认规则兜底，保证基础防护不失效。
    try {
        (void)applyConfig(nlohmann::json::parse(kDefaultConfig));
    } catch (...) {
        logger().error("内置默认规则解析失败，ContainerBan 将不拦截任何物品");
    }
}

} // namespace

bool enableContainerBan(std::filesystem::path const& configDir) {
    loadConfig(configDir);

    bool ok = true;
    auto tryHook = [&](int result, char const* name) {
        if (result != 0) {
            logger().error("注册钩子 {} 失败（代码 {}）", name, result);
            ok = false;
        }
        return ok;
    };

    if (tryHook(ItemStackRequestTransferHook::hook(), "ItemStackRequestTransferHook")
        && tryHook(ContainerCanPushInItemHook::hook(), "ContainerCanPushInItemHook")
        && tryHook(ContainerAddItemHook::hook(), "ContainerAddItemHook")
        && tryHook(DropperMoveInItemsHook::hook(), "DropperMoveInItemsHook")
        && tryHook(DropperAddItemsToContainerHook::hook(), "DropperAddItemsToContainerHook")) {
        return true;
    }

    disableContainerBan();
    return false;
}

void disableContainerBan() {
    DropperAddItemsToContainerHook::unhook();
    DropperMoveInItemsHook::unhook();
    ContainerAddItemHook::unhook();
    ContainerCanPushInItemHook::unhook();
    ItemStackRequestTransferHook::unhook();
    gRules.clear();
    rebuildIndex();
}

} // namespace container_ban

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
