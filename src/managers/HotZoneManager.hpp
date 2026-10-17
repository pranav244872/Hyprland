#pragma once

#include "../defines.hpp"
#include "../config/ConfigValue.hpp"

#include <array>
#include <cstdint>
#include <string>

class CEventLoopTimer;

// Edge/corner "hot zones" (fork feature): resting the cursor in a monitor's
// outer bezel strip runs a dispatcher string. Fired from CInputManager on
// pointer motion; never touches the render path.
class CHotZoneManager {
  public:
    CHotZoneManager();
    ~CHotZoneManager();

    enum eHotZone : uint8_t {
        HOTZONE_TOPLEFT = 0,
        HOTZONE_TOPRIGHT,
        HOTZONE_BOTTOMLEFT,
        HOTZONE_BOTTOMRIGHT,
        HOTZONE_TOP,
        HOTZONE_BOTTOM,
        HOTZONE_LEFT,
        HOTZONE_RIGHT,
        HOTZONE_NONE,
        HOTZONE_COUNT = HOTZONE_NONE,
    };

    // called on every real mouse-origin cursor move (InputManager::mouseMoveUnified)
    void onCursorMoved(const Vector2D& pos, PHLMONITOR pMonitor);

    // runtime API (hl.hotzone.*)
    void setRuntimeEnabled(bool enabled);
    bool isRuntimeEnabled() const;
    void toggleRuntime();
    bool isActive() const; // config master switch && runtime flag

    // dispatcher "hotzone <enable|disable|toggle>"
    SDispatchResult dispatch(const std::string& args);

  private:
    struct SZone {
        UP<CConfigValue<Config::BOOL>>    enabled;
        UP<CConfigValue<Config::STRING>>  action;
        UP<CConfigValue<Config::INTEGER>> dwellMs;
    };

    void     reset();
    void     armDwell(eHotZone zone);
    void     disarmDwell();
    void     triggerZone(eHotZone zone);
    void     runAction(const std::string& action);
    bool     isSuppressed(PHLMONITOR pMonitor) const;
    bool     isSharedEdge(PHLMONITOR pMonitor, eHotZone zone, const Vector2D& pos) const;
    bool     isSlam(const Vector2D& pos, PHLMONITOR pMonitor) const;
    bool     withinRearmBand(const Vector2D& pos, PHLMONITOR pMonitor, eHotZone zone) const;
    eHotZone zoneAt(const Vector2D& pos, PHLMONITOR pMonitor) const;

    static bool isCorner(eHotZone zone);

    static bool            boolValue(const UP<CConfigValue<Config::BOOL>>& v);
    static Config::INTEGER intValue(const UP<CConfigValue<Config::INTEGER>>& v);
    static std::string     strValue(const UP<CConfigValue<Config::STRING>>& v);

    std::array<SZone, HOTZONE_COUNT> m_zones;

    UP<CConfigValue<Config::BOOL>>    m_enabled;
    UP<CConfigValue<Config::BOOL>>    m_slam;
    UP<CConfigValue<Config::INTEGER>> m_dwellMs;
    UP<CConfigValue<Config::INTEGER>> m_triggerPx;
    UP<CConfigValue<Config::INTEGER>> m_rearmMarginPx;
    UP<CConfigValue<Config::INTEGER>> m_cornerMarginPx;
    UP<CConfigValue<Config::BOOL>>    m_suppressOnFullscreen;

    SP<CEventLoopTimer> m_dwellTimer;

    eHotZone m_activeZone    = HOTZONE_NONE; // zone the cursor currently sits in
    eHotZone m_armedZone     = HOTZONE_NONE; // zone the dwell timer is counting for
    eHotZone m_triggeredZone = HOTZONE_NONE; // fired zone, locked out until the cursor re-arms

    bool m_runtimeEnabled = true;
};

inline UP<CHotZoneManager> g_pHotZoneManager;
