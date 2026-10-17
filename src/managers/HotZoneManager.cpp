#include "HotZoneManager.hpp"

#include "../Compositor.hpp"
#include "../macros.hpp"
#include "./eventLoop/EventLoopManager.hpp"
#include "./input/InputManager.hpp"
#include "./fullscreen/FullscreenController.hpp"
#include "./KeybindManager.hpp"
#include "../state/MonitorState.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>

static constexpr const char* ZONE_NAMES[] = {"topleft", "topright", "bottomleft", "bottomright", "top", "bottom", "left", "right"};

static std::string trimStr(std::string s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

bool CHotZoneManager::boolValue(const UP<CConfigValue<Config::BOOL>>& v) {
    return v && v->good() && **v;
}

Config::INTEGER CHotZoneManager::intValue(const UP<CConfigValue<Config::INTEGER>>& v) {
    return v && v->good() ? **v : 0;
}

std::string CHotZoneManager::strValue(const UP<CConfigValue<Config::STRING>>& v) {
    return v && v->good() ? **v : std::string{};
}

CHotZoneManager::CHotZoneManager() {
    m_enabled              = makeUnique<CConfigValue<Config::BOOL>>("hotzone:enabled");
    m_slam                 = makeUnique<CConfigValue<Config::BOOL>>("hotzone:slam");
    m_dwellMs              = makeUnique<CConfigValue<Config::INTEGER>>("hotzone:dwell_ms");
    m_triggerPx            = makeUnique<CConfigValue<Config::INTEGER>>("hotzone:trigger_px");
    m_rearmMarginPx        = makeUnique<CConfigValue<Config::INTEGER>>("hotzone:rearm_margin_px");
    m_cornerMarginPx       = makeUnique<CConfigValue<Config::INTEGER>>("hotzone:corner_margin_px");
    m_suppressOnFullscreen = makeUnique<CConfigValue<Config::BOOL>>("hotzone:suppress_on_fullscreen");

    for (size_t i = 0; i < static_cast<size_t>(HOTZONE_COUNT); ++i) {
        const std::string BASE = std::format("hotzone:{}", ZONE_NAMES[i]);
        m_zones[i].enabled     = makeUnique<CConfigValue<Config::BOOL>>(BASE + ":enabled");
        m_zones[i].action      = makeUnique<CConfigValue<Config::STRING>>(BASE + ":action");
        m_zones[i].dwellMs     = makeUnique<CConfigValue<Config::INTEGER>>(BASE + ":dwell_ms");
    }

    m_dwellTimer = makeShared<CEventLoopTimer>(
        std::nullopt,
        [this](SP<CEventLoopTimer> self, void* data) {
            const auto ZONE = m_armedZone;
            m_armedZone     = HOTZONE_NONE;
            if (ZONE != HOTZONE_NONE)
                triggerZone(ZONE);
        },
        nullptr);

    if (g_pEventLoopManager)
        g_pEventLoopManager->addTimer(m_dwellTimer);

    // expose the runtime dispatcher for keybinds / hyprctl: `hotzone <enable|disable|toggle>`
    if (g_pKeybindManager)
        g_pKeybindManager->m_dispatchers["hotzone"] = [](std::string args) -> SDispatchResult {
            return g_pHotZoneManager ? g_pHotZoneManager->dispatch(args) : SDispatchResult{.success = false, .error = "hotzone unavailable"};
        };
}

CHotZoneManager::~CHotZoneManager() {
    if (g_pEventLoopManager && m_dwellTimer)
        g_pEventLoopManager->removeTimer(m_dwellTimer);
}

void CHotZoneManager::onCursorMoved(const Vector2D& pos, PHLMONITOR pMonitor) {
    if (!pMonitor) {
        reset();
        return;
    }

    if (!isActive()) {
        reset();
        return;
    }

    if (isSuppressed(pMonitor)) {
        // cancel a pending dwell, but keep the fired-zone lockout so a suppression
        // lifting does not immediately re-fire the same zone
        disarmDwell();
        m_activeZone = HOTZONE_NONE;
        return;
    }

    const auto ZONE = zoneAt(pos, pMonitor);

    if (ZONE == HOTZONE_NONE) {
        m_activeZone = HOTZONE_NONE;
        disarmDwell();
        if (m_triggeredZone != HOTZONE_NONE && !withinRearmBand(pos, pMonitor, m_triggeredZone))
            m_triggeredZone = HOTZONE_NONE;
        return;
    }

    if (ZONE == m_activeZone)
        return; // same zone; the dwell timer or the lockout owns the rest

    // a monitor neighbour just outside this edge makes it an internal boundary
    if (!isCorner(ZONE) && isSharedEdge(pMonitor, ZONE, pos)) {
        m_activeZone = HOTZONE_NONE;
        disarmDwell();
        return;
    }

    m_activeZone = ZONE;

    if (ZONE == m_triggeredZone) {
        disarmDwell(); // already fired; wait for the cursor to leave the re-arm band
        return;
    }

    // Instant slam is opt-in and corner-only: a corner is a deliberate throw,
    // but an edge is a line the cursor clamps onto while dragging. With the
    // default (slam = false) every zone waits out the dwell.
    if (boolValue(m_slam) && isCorner(ZONE) && isSlam(pos, pMonitor)) {
        disarmDwell();
        triggerZone(ZONE); // exact bezel pixel at a corner: bypass the dwell
    } else {
        armDwell(ZONE);
    }
}

void CHotZoneManager::armDwell(eHotZone zone) {
    const auto ZONEDWELL = intValue(m_zones[zone].dwellMs);
    const auto DWELL     = ZONEDWELL >= 0 ? ZONEDWELL : intValue(m_dwellMs);

    if (DWELL <= 0) {
        m_armedZone = HOTZONE_NONE;
        triggerZone(zone);
        return;
    }

    m_armedZone = zone;
    if (m_dwellTimer)
        m_dwellTimer->updateTimeout(std::chrono::milliseconds(DWELL));
}

void CHotZoneManager::disarmDwell() {
    m_armedZone = HOTZONE_NONE;
    if (m_dwellTimer)
        m_dwellTimer->updateTimeout(std::nullopt);
}

void CHotZoneManager::reset() {
    m_activeZone    = HOTZONE_NONE;
    m_armedZone     = HOTZONE_NONE;
    m_triggeredZone = HOTZONE_NONE;
    if (m_dwellTimer)
        m_dwellTimer->updateTimeout(std::nullopt);
}

void CHotZoneManager::triggerZone(eHotZone zone) {
    if (zone == HOTZONE_NONE)
        return;

    m_triggeredZone = zone; // lock out until re-arm, even for disabled/empty zones

    if (!boolValue(m_zones[zone].enabled))
        return;

    const auto ACTION = strValue(m_zones[zone].action);
    if (ACTION.empty())
        return;

    runAction(ACTION);
}

void CHotZoneManager::runAction(const std::string& action) {
    if (!g_pKeybindManager)
        return;

    // accept both "handler, args" (classic) and "handler args"
    std::string handler;
    std::string args;

    if (const auto COMMA = action.find_first_of(','); COMMA != std::string::npos) {
        handler = trimStr(action.substr(0, COMMA));
        args    = trimStr(action.substr(COMMA + 1));
    } else if (const auto SPACE = action.find_first_of(" \t"); SPACE != std::string::npos) {
        handler = trimStr(action.substr(0, SPACE));
        args    = trimStr(action.substr(SPACE + 1));
    } else {
        handler = trimStr(action);
    }

    const auto DISPATCHER = g_pKeybindManager->m_dispatchers.find(handler);
    if (DISPATCHER == g_pKeybindManager->m_dispatchers.end()) {
        Log::logger->log(Log::ERR, "[hotzone] unknown action \"{}\"", action);
        return;
    }

    const auto RES = DISPATCHER->second(args);
    if (!RES.success)
        Log::logger->log(Log::ERR, "[hotzone] action \"{}\" failed: {}", action, RES.error);
}

bool CHotZoneManager::isSuppressed(PHLMONITOR pMonitor) const {
    if (!g_pInputManager)
        return true;

    if (g_pInputManager->isLocked() || g_pInputManager->isConstrained() || g_pInputManager->hasHeldButtons())
        return true;

    // an exclusive keyboard-interactive layer surface is up (launcher, screenshot overlay, …)
    if (!g_pInputManager->m_exclusiveLSes.empty())
        return true;

    if (boolValue(m_suppressOnFullscreen) && Fullscreen::controller() && Fullscreen::controller()->hasFullscreen(pMonitor))
        return true;

    return false;
}

bool CHotZoneManager::isSharedEdge(PHLMONITOR pMonitor, eHotZone zone, const Vector2D& pos) const {
    if (!State::monitorState())
        return false;

    Vector2D probe = pos;
    switch (zone) {
        case HOTZONE_TOP: probe.y = pMonitor->m_position.y - 1; break;
        case HOTZONE_BOTTOM: probe.y = pMonitor->m_position.y + pMonitor->m_size.y; break;
        case HOTZONE_LEFT: probe.x = pMonitor->m_position.x - 1; break;
        case HOTZONE_RIGHT: probe.x = pMonitor->m_position.x + pMonitor->m_size.x; break;
        default: return false;
    }

    const auto OTHER = State::monitorState()->query().vec(probe).run();
    return OTHER && OTHER.get() != pMonitor.get();
}

bool CHotZoneManager::isSlam(const Vector2D& pos, PHLMONITOR pMonitor) const {
    const auto FLOORED = pos.floor();
    const auto X0      = pMonitor->m_position.x;
    const auto Y0      = pMonitor->m_position.y;
    const auto X1      = X0 + pMonitor->m_size.x - 1;
    const auto Y1      = Y0 + pMonitor->m_size.y - 1;

    return FLOORED.x == X0 || FLOORED.x == X1 || FLOORED.y == Y0 || FLOORED.y == Y1;
}

bool CHotZoneManager::withinRearmBand(const Vector2D& pos, PHLMONITOR pMonitor, eHotZone zone) const {
    const auto T  = sc<double>(intValue(m_triggerPx) + intValue(m_rearmMarginPx));
    const auto X0 = pMonitor->m_position.x;
    const auto Y0 = pMonitor->m_position.y;
    const auto X1 = X0 + pMonitor->m_size.x;
    const auto Y1 = Y0 + pMonitor->m_size.y;

    const bool L  = pos.x < X0 + T;
    const bool R  = pos.x >= X1 - T;
    const bool TT = pos.y < Y0 + T;
    const bool B  = pos.y >= Y1 - T;

    switch (zone) {
        case HOTZONE_TOPLEFT: return L && TT;
        case HOTZONE_TOPRIGHT: return R && TT;
        case HOTZONE_BOTTOMLEFT: return L && B;
        case HOTZONE_BOTTOMRIGHT: return R && B;
        case HOTZONE_TOP: return TT;
        case HOTZONE_BOTTOM: return B;
        case HOTZONE_LEFT: return L;
        case HOTZONE_RIGHT: return R;
        default: return false;
    }
}

CHotZoneManager::eHotZone CHotZoneManager::zoneAt(const Vector2D& pos, PHLMONITOR pMonitor) const {
    const auto T   = sc<double>(intValue(m_triggerPx));
    const auto CUT = T + sc<double>(intValue(m_cornerMarginPx));

    const auto X0 = pMonitor->m_position.x;
    const auto Y0 = pMonitor->m_position.y;
    const auto X1 = X0 + pMonitor->m_size.x;
    const auto Y1 = Y0 + pMonitor->m_size.y;

    if (pos.x < X0 || pos.x >= X1 || pos.y < Y0 || pos.y >= Y1)
        return HOTZONE_NONE;

    const bool L  = pos.x < X0 + T;
    const bool R  = pos.x >= X1 - T;
    const bool TT = pos.y < Y0 + T;
    const bool B  = pos.y >= Y1 - T;

    // corners win, and are never edge-suppressed
    if (TT && L) return HOTZONE_TOPLEFT;
    if (TT && R) return HOTZONE_TOPRIGHT;
    if (B && L) return HOTZONE_BOTTOMLEFT;
    if (B && R) return HOTZONE_BOTTOMRIGHT;

    // edges, with the corner dead-margin carved out of both ends
    if (TT && pos.x >= X0 + CUT && pos.x < X1 - CUT) return HOTZONE_TOP;
    if (B && pos.x >= X0 + CUT && pos.x < X1 - CUT) return HOTZONE_BOTTOM;
    if (L && pos.y >= Y0 + CUT && pos.y < Y1 - CUT) return HOTZONE_LEFT;
    if (R && pos.y >= Y0 + CUT && pos.y < Y1 - CUT) return HOTZONE_RIGHT;

    return HOTZONE_NONE;
}

bool CHotZoneManager::isCorner(eHotZone zone) {
    return zone == HOTZONE_TOPLEFT || zone == HOTZONE_TOPRIGHT || zone == HOTZONE_BOTTOMLEFT || zone == HOTZONE_BOTTOMRIGHT;
}

void CHotZoneManager::setRuntimeEnabled(bool enabled) {
    m_runtimeEnabled = enabled;
    if (!enabled)
        reset();
}

bool CHotZoneManager::isRuntimeEnabled() const {
    return m_runtimeEnabled;
}

void CHotZoneManager::toggleRuntime() {
    setRuntimeEnabled(!m_runtimeEnabled);
}

bool CHotZoneManager::isActive() const {
    return boolValue(m_enabled) && m_runtimeEnabled;
}

SDispatchResult CHotZoneManager::dispatch(const std::string& args) {
    const auto A = trimStr(args);

    if (A == "enable") {
        setRuntimeEnabled(true);
        return {.success = true};
    }
    if (A == "disable") {
        setRuntimeEnabled(false);
        return {.success = true};
    }
    if (A == "toggle") {
        toggleRuntime();
        return {.success = true};
    }

    return {.success = false, .error = "hotzone: expected \"enable\", \"disable\" or \"toggle\""};
}
