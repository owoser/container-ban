#include "mod/MyMod.h"

#include "mod/ContainerBan.h"

#include "ll/api/mod/RegisterHelper.h"

namespace container_ban {

MyMod& MyMod::getInstance() {
    static MyMod instance;
    return instance;
}

bool MyMod::load() {
    getSelf().getLogger().debug("Loading...");
    // Code for loading the mod goes here.
    return true;
}

bool MyMod::enable() {
    if (!enableContainerBan(getSelf().getConfigDir())) return false;
    getSelf().getLogger().info("ContainerBan 已启用：按 config.json 规则拦截物品放入受限容器。");
    return true;
}

bool MyMod::disable() {
    disableContainerBan();
    getSelf().getLogger().info("ContainerBan 已停用。");
    return true;
}

} // namespace container_ban

LL_REGISTER_MOD(container_ban::MyMod, container_ban::MyMod::getInstance());
