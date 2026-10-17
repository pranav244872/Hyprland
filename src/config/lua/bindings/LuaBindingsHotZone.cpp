#include "LuaBindingsInternal.hpp"

#include "../../../managers/HotZoneManager.hpp"

using namespace Config;
using namespace Config::Lua;
using namespace Config::Lua::Bindings;

static int hlHotZoneEnable(lua_State* L) {
    if (g_pHotZoneManager)
        g_pHotZoneManager->setRuntimeEnabled(true);
    return 0;
}

static int hlHotZoneDisable(lua_State* L) {
    if (g_pHotZoneManager)
        g_pHotZoneManager->setRuntimeEnabled(false);
    return 0;
}

static int hlHotZoneToggle(lua_State* L) {
    if (g_pHotZoneManager)
        g_pHotZoneManager->toggleRuntime();
    return 0;
}

static int hlHotZoneIsEnabled(lua_State* L) {
    lua_pushboolean(L, g_pHotZoneManager && g_pHotZoneManager->isActive());
    return 1;
}

void Internal::registerHotZoneBindings(lua_State* L) {
    lua_newtable(L);
    Internal::setFn(L, "enable", hlHotZoneEnable);
    Internal::setFn(L, "disable", hlHotZoneDisable);
    Internal::setFn(L, "toggle_active", hlHotZoneToggle);
    Internal::setFn(L, "is_enabled", hlHotZoneIsEnabled);
    lua_setfield(L, -2, "hotzone");
}
