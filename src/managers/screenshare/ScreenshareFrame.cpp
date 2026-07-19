#include "ScreenshareManager.hpp"
#include "../../pointer/PointerManager.hpp"
#include "../input/InputManager.hpp"
#include "../permissions/DynamicPermissionManager.hpp"
#include "../../protocols/ColorManagement.hpp"
#include "../../protocols/XDGShell.hpp"
#include "../../Compositor.hpp"
#include "../../render/Renderer.hpp"
#include "../../render/OpenGL.hpp"
#include "../../output/Monitor.hpp"
#include "../../state/MonitorState.hpp"
#include "../../desktop/view/Window.hpp"
#include "../../desktop/state/FocusState.hpp"
#include "../../render/pass/ClearPassElement.hpp"
#include "../../render/pass/RectPassElement.hpp"
#include "helpers/cm/ColorManagement.hpp"
#include "../../managers/fullscreen/FullscreenController.hpp"
#include <hyprutils/math/Region.hpp>
#include <hyprgraphics/egl/Egl.hpp>

using namespace Hyprgraphics::Egl;
using namespace Screenshare;
using namespace Desktop::View;

CScreenshareFrame::CScreenshareFrame(WP<CScreenshareSession> session, bool overlayCursor, bool isFirst) :
    m_session(session), m_bufferSize(m_session->bufferSize()), m_overlayCursor(overlayCursor), m_isFirst(isFirst) {
    ;
}

CScreenshareFrame::~CScreenshareFrame() {
    if (m_failed || !m_shared)
        return;

    if (!m_copied && m_callback) {
        FScreenshareCallback cb;
        std::swap(cb, m_callback);
        cb(RESULT_NOT_COPIED);
    }
}

bool CScreenshareFrame::done() const {
    if (m_session.expired() || m_session->m_stopped)
        return true;

    if (m_session->m_type == SHARE_NONE || m_bufferSize == Vector2D(0, 0))
        return true;

    if (m_failed || m_copied)
        return true;

    if (m_session->m_type == SHARE_MONITOR && !m_session->monitor())
        return true;

    if (m_session->m_type == SHARE_REGION && !m_session->monitor())
        return true;

    if (m_session->m_type == SHARE_WINDOW && (!m_session->monitor() || !validMapped(m_session->m_window)))
        return true;

    if (!m_shared)
        return false;

    if (!m_buffer || !m_buffer->m_resource || !m_buffer->m_resource->good())
        return true;

    if (!m_callback)
        return true;

    return false;
}

eScreenshareError CScreenshareFrame::share(SP<IHLBuffer> buffer, const CRegion& clientDamage, FScreenshareCallback callback) {
    if UNLIKELY (done())
        return ERROR_STOPPED;

    if UNLIKELY (!m_session->monitor() || !State::monitorState()->contains(m_session->monitor())) {
        LOGM(Log::ERR, "Client requested sharing of a monitor that is gone");
        m_failed = true;
        return ERROR_STOPPED;
    }

    if UNLIKELY (m_session->m_type == SHARE_WINDOW && !validMapped(m_session->m_window)) {
        LOGM(Log::ERR, "Client requested sharing of window that is gone or not shareable!");
        m_failed = true;
        return ERROR_STOPPED;
    }

    if UNLIKELY (!buffer || !buffer->m_resource || !buffer->m_resource->good()) {
        LOGM(Log::ERR, "Client requested sharing to an invalid buffer");
        return ERROR_NO_BUFFER;
    }

    if UNLIKELY (buffer->size != m_bufferSize) {
        LOGM(Log::ERR, "Client requested sharing to an invalid buffer size");
        return ERROR_BUFFER_SIZE;
    }

    uint32_t bufFormat;
    if (buffer->dmabuf().success)
        bufFormat = buffer->dmabuf().format;
    else if (buffer->shm().success)
        bufFormat = buffer->shm().format;
    else {
        LOGM(Log::ERR, "Client requested sharing to an invalid buffer");
        return ERROR_NO_BUFFER;
    }

    if (std::ranges::count_if(m_session->allowedFormats(), [&](const DRMFormat& format) { return format == bufFormat; }) == 0) {
        LOGM(Log::ERR, "Invalid format {} in {:x}", bufFormat, (uintptr_t)this);
        return ERROR_BUFFER_FORMAT;
    }

    m_buffer   = buffer;
    m_callback = callback;
    m_shared   = true;

    if (m_session->m_type == SHARE_MONITOR || m_session->m_type == SHARE_REGION) {
        const auto PMONITOR = m_session->monitor();
        if (PMONITOR)
            PMONITOR->addDamage(PMONITOR->resources()->pendingMirrorFBDamage());
    }

    // schedule a frame so that when a screenshare starts it isn't black until the output is updated
    if (m_isFirst) {
        const auto PMONITOR = m_session->monitor();
        if (PMONITOR)
            PMONITOR->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_NEEDS_FRAME);
        g_pHyprRenderer->damageMonitor(PMONITOR);
    }

    // TODO: add a damage ring for output damage since last shared frame
    CRegion frameDamage = CRegion(0, 0, m_bufferSize.x, m_bufferSize.y);

    // copy everything on the first frame
    if (m_isFirst)
        m_damage = CRegion(0, 0, m_bufferSize.x, m_bufferSize.y);
    else
        m_damage = frameDamage.add(clientDamage);

    m_damage.intersect(0, 0, m_bufferSize.x, m_bufferSize.y);

    return ERROR_NONE;
}

void CScreenshareFrame::copy() {
    if (done() || m_copyInFlight)
        return;

    // tell client to send presented timestamp
    // TODO: is this right? this is right after we commit to aq, not when page flip happens..
    m_callback(RESULT_TIMESTAMP);

    // store a snapshot before the permission popup so we don't break screenshots
    const auto PERM = g_pDynamicPermissionManager->clientPermissionMode(m_session->m_client, PERMISSION_TYPE_SCREENCOPY);
    if (PERM == PERMISSION_RULE_ALLOW_MODE_PENDING) {
        if (!m_session->m_tempFB || !m_session->m_tempFB->isAllocated())
            storeTempFB();

        // don't copy a frame while allow is pending because screenshot tools will only take the first frame we give, which is empty
        return;
    }

    if (m_buffer->shm().success)
        m_failed = !copyShm();
    else if (m_buffer->dmabuf().success)
        m_failed = !copyDmabuf();

    if (!m_failed) {
        // screensharing has started again
        m_session->screenshareEvents(true);
        m_session->m_shareStopTimer->updateTimeout(std::chrono::milliseconds(500)); // check in half second
    } else
        m_callback(RESULT_NOT_COPIED);
}

void CScreenshareFrame::renderMonitor() {
    if ((m_session->m_type != SHARE_MONITOR && m_session->m_type != SHARE_REGION) || done())
        return;

    const auto PMONITOR = m_session->monitor();

    g_pHyprRenderer->m_renderData.transformDamage = false;
    g_pHyprRenderer->m_renderData.noSimplify      = true;

    const auto OLD                                    = g_pHyprRenderer->m_renderData.renderModif.enabled;
    g_pHyprRenderer->m_renderData.renderModif.enabled = false;
    g_pHyprRenderer->startRenderPass();

    // Re-composite from scratch, skipping noScreenShare windows/layers
    // so they appear transparent (showing wallpaper/windows behind) in screenshare
    g_pHyprRenderer->m_bScreenshareRender = true;
    const auto NOW = Time::steadyNow();
    CBox renderBox = CBox{-m_session->m_captureBox.pos(), PMONITOR->m_pixelSize};
    g_pHyprRenderer->renderWorkspace(PMONITOR, PMONITOR->m_activeWorkspace, NOW, renderBox);
    g_pHyprRenderer->renderLockscreen(PMONITOR, NOW, renderBox);
    g_pHyprRenderer->m_bScreenshareRender = false;

    g_pHyprRenderer->m_renderData.renderModif.enabled = OLD;



    if (m_overlayCursor) {
        CRegion  fakeDamage = {0, 0, INT16_MAX, INT16_MAX};
        Vector2D cursorPos  = g_pInputManager->getMouseCoordsInternal() - PMONITOR->m_position - m_session->m_captureBox.pos() / PMONITOR->m_scale;
        Pointer::mgr()->renderSoftwareCursorsFor(PMONITOR, Time::steadyNow(), fakeDamage, cursorPos, false);
        Pointer::mgr()->renderSoftwareCursorsFor(PMONITOR, Time::steadyNow(), fakeDamage, cursorPos, true);
    }
}

void CScreenshareFrame::renderWindow() {
    if (m_session->m_type != SHARE_WINDOW || done())
        return;

    const auto PWINDOW  = m_session->m_window.lock();
    const auto PMONITOR = m_session->monitor();

    const auto NOW = Time::steadyNow();

    // TODO: implement a monitor independent render mode to buffer that does this in CHyprRenderer::begin() or something like that
    g_pHyprRenderer->m_renderData.fbSize = m_bufferSize;
    g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
    g_pHyprRenderer->m_renderData.transformDamage = false;
    g_pHyprRenderer->setViewport(0, 0, m_bufferSize.x, m_bufferSize.y);

    g_pHyprRenderer->m_bBlockSurfaceFeedback = g_pHyprRenderer->shouldRenderWindow(PWINDOW); // block the feedback to avoid spamming the surface if it's visible
    g_pHyprRenderer->renderWindow(PWINDOW, PMONITOR, NOW, false, Render::RENDER_PASS_ALL, true, true);
    g_pHyprRenderer->m_bBlockSurfaceFeedback = false;

    if (!m_overlayCursor)
        return;

    auto pointerSurfaceResource = g_pSeatManager->m_state.pointerFocus.lock();

    if (!pointerSurfaceResource)
        return;

    auto pointerSurface = Desktop::View::CWLSurface::fromResource(pointerSurfaceResource);
    if (!pointerSurface)
        return;

    auto box = pointerSurface->getSurfaceBoxGlobal();
    if (!box.has_value() || box->intersection(m_session->m_window->getFullWindowBoundingBox()).empty())
        return;

    if (Desktop::focusState()->window() != m_session->m_window)
        return;

    CRegion fakeDamage = {0, 0, INT16_MAX, INT16_MAX};
    Pointer::mgr()->renderSoftwareCursorsFor(PMONITOR->m_self.lock(), NOW, fakeDamage,
                                             g_pInputManager->getMouseCoordsInternal() - PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT), true);
}

void CScreenshareFrame::render() {
    const auto PERM = g_pDynamicPermissionManager->clientPermissionMode(m_session->m_client, PERMISSION_TYPE_SCREENCOPY);

    CRegion    frameRegion = {0, 0, g_pHyprRenderer->m_renderData.pMonitor->m_pixelSize.x, g_pHyprRenderer->m_renderData.pMonitor->m_pixelSize.y};

    g_pHyprRenderer->draw(CClearPassElement::SClearData{{0, 0, 0, 0}}, frameRegion);

    if (PERM == PERMISSION_RULE_ALLOW_MODE_PENDING)
        return;

    bool windowShareDenied = m_session->m_type == SHARE_WINDOW && m_session->m_window->m_ruleApplicator && m_session->m_window->m_ruleApplicator->noScreenShare().valueOrDefault();
    g_pHyprRenderer->startRenderPass();
    if (PERM == PERMISSION_RULE_ALLOW_MODE_DENY || windowShareDenied) {
        CBox texbox = CBox{m_bufferSize / 2.F, g_pHyprRenderer->m_screencopyDeniedTexture->m_size}.translate(-g_pHyprRenderer->m_screencopyDeniedTexture->m_size / 2.F);
        g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = g_pHyprRenderer->m_screencopyDeniedTexture, .box = texbox}, texbox);
        return;
    }

    if (m_session->m_tempFB && m_session->m_tempFB->isAllocated()) {
        CBox texbox = {{}, m_bufferSize};
        g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = m_session->m_tempFB->getTexture(), .box = texbox}, texbox);
        m_session->m_tempFB->release();
        return;
    }

    switch (m_session->m_type) {
        case SHARE_REGION: // TODO: could this be better? this is how screencopy works
        case SHARE_MONITOR: renderMonitor(); break;
        case SHARE_WINDOW: renderWindow(); break;
        case SHARE_NONE:
        default: return;
    }
}

bool CScreenshareFrame::copyDmabuf() {
    if (done())
        return false;

    if (!g_pHyprRenderer->beginRender(m_session->monitor(), m_damage, Render::RENDER_MODE_TO_BUFFER, m_buffer, nullptr, true)) {
        LOGM(Log::ERR, "Can't copy: failed to begin rendering to dma frame");
        return false;
    }
    g_pHyprRenderer->m_renderData.currentFB->setImageDescription(NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION);

    render();

    g_pHyprRenderer->m_renderData.blockScreenShader = true;

    m_copyInFlight = true;

    g_pHyprRenderer->endRender([self = m_self]() {
        if (!self || self.expired())
            return;

        self->m_copyInFlight = false;

        if (self->m_copied)
            return;

        LOGM(Log::TRACE, "Copied frame via dma");
        self->m_callback(RESULT_COPIED);
        self->m_copied = true;
    });

    return true;
}

bool CScreenshareFrame::copyShm() {
    if (done())
        return false;

    auto       shm = m_buffer->shm();

    const auto PFORMAT = getPixelFormatFromDRM(shm.format);
    if (!PFORMAT) {
        LOGM(Log::ERR, "Can't copy: failed to find a pixel format");
        return false;
    }

    const auto PMONITOR = m_session->monitor();

    auto       outFB = g_pHyprRenderer->createFB();
    outFB->alloc(m_bufferSize.x, m_bufferSize.y, shm.format);
    outFB->setImageDescription(NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION);

    if (!g_pHyprRenderer->beginFullFakeRender(PMONITOR, m_damage, outFB)) {
        LOGM(Log::ERR, "Can't copy: failed to begin rendering");
        return false;
    }

    render();

    g_pHyprRenderer->m_renderData.blockScreenShader = true;

    g_pHyprRenderer->endRender();

    bool readSucceeded = true;
    m_damage.forEachRect([&](const auto& rect) {
        if (!readSucceeded)
            return;

        int width  = rect.x2 - rect.x1;
        int height = rect.y2 - rect.y1;
        if (!outFB->readPixels(m_buffer, rect.x1, rect.y1, width, height))
            readSucceeded = false;
    });

    g_pHyprRenderer->m_renderData.pMonitor.reset();

    if (!readSucceeded) {
        LOGM(Log::ERR, "Can't copy: failed to read pixels to shm");
        return false;
    }

    if (!m_copied) {
        LOGM(Log::TRACE, "Copied frame via shm");
        m_callback(RESULT_COPIED);
        m_copied = true;
    }

    return true;
}

void CScreenshareFrame::storeTempFB() {
    if (!m_session->m_tempFB)
        m_session->m_tempFB = g_pHyprRenderer->createFB();
    m_session->m_tempFB->alloc(m_bufferSize.x, m_bufferSize.y);
    m_session->m_tempFB->setImageDescription(NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION);

    CRegion fakeDamage = {0, 0, INT16_MAX, INT16_MAX};

    if (!g_pHyprRenderer->beginFullFakeRender(m_session->monitor(), fakeDamage, m_session->m_tempFB)) {
        LOGM(Log::ERR, "Can't copy: failed to begin rendering to temp fb");
        return;
    }

    switch (m_session->m_type) {
        case SHARE_REGION: // TODO: could this be better? this is how screencopy works
        case SHARE_MONITOR: renderMonitor(); break;
        case SHARE_WINDOW: renderWindow(); break;
        case SHARE_NONE:
        default: return;
    }

    g_pHyprRenderer->endRender();
}

Vector2D CScreenshareFrame::bufferSize() const {
    return m_bufferSize;
}

wl_output_transform CScreenshareFrame::transform() const {
    switch (m_session->m_type) {
        case SHARE_REGION:
        case SHARE_MONITOR: return m_session->monitor()->m_transform;
        default:
        case SHARE_WINDOW: return WL_OUTPUT_TRANSFORM_NORMAL;
    }
}

const CRegion& CScreenshareFrame::damage() const {
    return m_damage;
}
