/*
 * PxrMaskProjection.cpp
 * RenderMan 27 — Matched to actual RixProjection.h / RixShading.h
 *
 * EXR loading uses tinyexr (single-header, self-contained) so we don't
 * link against Houdini's OpenEXR DLLs — avoids ABI drift at runtime.
 */

#include "RixProjection.h"
#include "RixIntegrator.h"
#include "RixShadingUtils.h"

#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

#include <vector>
#include <string>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <atomic>

// ── Parameter IDs ────────────────────────────────────────
enum ParamId
{
    k_maskFile = 0,
    k_maskChannel,
    k_maskFit,
    k_centerX,
    k_centerY,
    k_invert,
    k_threshold,
    k_fov,
    k_numParams
};


// ============================================================
// Forward-declare
// ============================================================
class PxrMaskProjection;


// ============================================================
// FACTORY
// ============================================================
class PxrMaskProjectionFactory : public RixProjectionFactory
{
public:
    PxrMaskProjectionFactory() {}

    // Public destructor so we can delete through derived pointer
    ~PxrMaskProjectionFactory() override {}

    // ── RenderMan 27: Init takes RtUString, not char const* ──
    int Init(RixContext& ctx, RtUString const pluginPath) override
    {
        return 0;
    }

    void Finalize(RixContext& ctx) override {}

    RixSCParamInfo const* GetParamTable() override
    {
        static RixSCParamInfo s_ptable[] =
        {
            RixSCParamInfo(RtUString("maskFile"),    k_RixSCString,  k_RixSCInput),
            RixSCParamInfo(RtUString("maskChannel"), k_RixSCString,  k_RixSCInput),
            RixSCParamInfo(RtUString("maskFit"),     k_RixSCInteger, k_RixSCInput),
            RixSCParamInfo(RtUString("centerX"),     k_RixSCFloat,   k_RixSCInput),
            RixSCParamInfo(RtUString("centerY"),     k_RixSCFloat,   k_RixSCInput),
            RixSCParamInfo(RtUString("invert"),      k_RixSCInteger, k_RixSCInput),
            RixSCParamInfo(RtUString("threshold"),   k_RixSCFloat,   k_RixSCInput),
            RixSCParamInfo(RtUString("fov"),         k_RixSCFloat,   k_RixSCInput),
            RixSCParamInfo(RtUString("debug"),       k_RixSCInteger, k_RixSCInput),
            RixSCParamInfo()  // terminator
        };
        return &s_ptable[0];
    }

    void Synchronize(
        RixContext& ctx, RixSCSyncMsg syncMsg,
        RixParameterList const* parms) override
    {}

    // ── RenderMan 27 signature: (RixContext&, RtUString, const RixParameterList*) ──
    RixProjection* CreateProjection(
        RixContext& ctx,
        RtUString const handle,
        RixParameterList const* parms) override;

    // ── RenderMan 27: takes const pointer ──
    void DestroyProjection(RixProjection const* projection) override;
};


// ============================================================
// PROJECTION INSTANCE
// ============================================================
class PxrMaskProjection : public RixProjection
{
public:
    PxrMaskProjection()
        : m_maskFit(0)
        , m_centerX(0.0f), m_centerY(0.0f)
        , m_invert(0)
        , m_threshold(0.0f)
        , m_fov(90.0f), m_focalScale(1.0f)
        , m_renderAspect(1.0f), m_maskAspect(1.0f)
        , m_maskWidth(0), m_maskHeight(0)
        , m_bufWidth(0), m_bufHeight(0)
        , m_dwMinX(0), m_dwMinY(0)
        , m_maskLoaded(false)
        , m_uScale(0.5f), m_vScale(0.5f)
        , m_uOffset(0.0f), m_vOffset(0.0f)
        , m_clipNear(0.01f), m_clipFar(1e6f)
        , m_swL(-1.0f), m_swR(1.0f), m_swB(-1.0f), m_swT(1.0f)
        , m_debug(0)
        , m_sMinX( 1e30f), m_sMaxX(-1e30f)
        , m_sMinY( 1e30f), m_sMaxY(-1e30f)
        , m_msgs(nullptr)
        , m_projectLogBudget(4)
    {}

    // Public destructor — base class has protected dtor,
    // but our derived dtor can be public.
    ~PxrMaskProjection() override {}

    // ── GetProperty: const method, pure virtual in base ──
    RixSCDetail GetProperty(
        ProjectionProperty property,
        void const** result) const override
    {
        return k_RixSCInvalidDetail;
    }

    // ── RenderBegin: THIS is where we get env + params ──
    // (In RenderMan 27, this lives on the Projection, not Factory)
    void RenderBegin(
        RixContext& ctx,
        RixProjectionEnvironment const& env,
        RixParameterList const* parms) override
    {
        RixMessages* msgs =
            (RixMessages*)ctx.GetRixInterface(k_RixMessages);
        m_msgs = msgs;
        m_projectLogBudget = 4;

        m_sMinX =  1e30f;  m_sMaxX = -1e30f;
        m_sMinY =  1e30f;  m_sMaxY = -1e30f;

        int w = env.width  > 0 ? env.width  : 1;
        int h = env.height > 0 ? env.height : 1;
        m_renderAspect = (float)w / (float)h;

        m_clipNear = env.clippingNear > 0.0f ? env.clippingNear : 0.01f;
        m_clipFar  = env.clippingFar  > 0.0f ? env.clippingFar  : 1e6f;

        m_swL = env.screenWindowLeft;
        m_swR = env.screenWindowRight;
        m_swB = env.screenWindowBottom;
        m_swT = env.screenWindowTop;
        if (m_swR <= m_swL) { m_swL = -1.0f; m_swR = 1.0f; }
        if (m_swT <= m_swB) { m_swB = -1.0f; m_swT = 1.0f; }

        m_maskLoaded = false;

        if (!parms)
        {
            if (m_debug && msgs)
                msgs->Warning("PxrMaskProjection: parms is NULL — "
                              "no EXR will be loaded.");
            return;
        }

        // Resolve paramIds by name. hdPrman assigns its own internal IDs
        // that do NOT match our enum order, so asking for k_maskFit=2
        // directly is undefined behavior.
        auto readString = [&](const char* name, RtUString* out) -> bool
        {
            int pid = -1;
            if (parms->GetParamId(RtUString(name), &pid) != 0) return false;
            return parms->EvalParam(pid, 0, out) != k_RixSCInvalidDetail;
        };
        auto readInt = [&](const char* name, int* out) -> bool
        {
            int pid = -1;
            if (parms->GetParamId(RtUString(name), &pid) != 0) return false;
            return parms->EvalParam(pid, 0, out) != k_RixSCInvalidDetail;
        };
        auto readFloat = [&](const char* name, float* out) -> bool
        {
            int pid = -1;
            if (parms->GetParamId(RtUString(name), &pid) != 0) return false;
            return parms->EvalParam(pid, 0, out) != k_RixSCInvalidDetail;
        };

        RtUString maskFileUS, maskChannelUS;
        readString("maskFile",    &maskFileUS);
        readString("maskChannel", &maskChannelUS);
        readInt   ("maskFit",     &m_maskFit);
        readFloat ("centerX",     &m_centerX);
        readFloat ("centerY",     &m_centerY);
        readInt   ("invert",      &m_invert);
        readFloat ("threshold",   &m_threshold);
        m_debug = 0;
        readInt   ("debug",       &m_debug);

        // fov: narrower-dimension FOV in degrees (matches PxrPerspective convention).
        // hdPrman delivers screen[] samples in NDC [-1,1] space, not screen-window
        // units, so we can't derive this from screenWindow — it must be set to match
        // the scene camera. focalScale = half_narrow / tan(fov/2) then makes
        // direction = normalize(sx, sy, focalScale) trace the correct frustum.
        m_fov = 90.0f;
        readFloat ("fov",         &m_fov);
        {
            const float PI = 3.14159265358979323846f;
            float half_w = 0.5f * (m_swR - m_swL);
            float half_h = 0.5f * (m_swT - m_swB);
            float half_narrow = (half_h < half_w) ? half_h : half_w;
            float fov_clamped = m_fov;
            if (fov_clamped < 1.0f)   fov_clamped = 1.0f;
            if (fov_clamped > 179.0f) fov_clamped = 179.0f;
            float tan_half = std::tan(fov_clamped * 0.5f * PI / 180.0f);
            m_focalScale = (tan_half > 1e-6f) ? (half_narrow / tan_half) : 1.0f;
        }

        const char* maskFile = maskFileUS.CStr();
        const char* maskChannel = maskChannelUS.CStr();
        if (!maskFile    || maskFile[0]    == '\0') maskFile    = "";
        if (!maskChannel || maskChannel[0] == '\0') maskChannel = "A";

        if (m_debug && msgs)
        {
            char buf[2048];
            snprintf(buf, sizeof(buf),
                "PxrMaskProjection: RenderBegin env %dx%d "
                "screenWindow=[%.3f,%.3f]x[%.3f,%.3f] clip=[%.4f,%.4f] "
                "pixelAspect=%.3f",
                w, h,
                env.screenWindowLeft, env.screenWindowRight,
                env.screenWindowBottom, env.screenWindowTop,
                m_clipNear, m_clipFar, env.pixelAspectRatio);
            msgs->Info(buf);

            snprintf(buf, sizeof(buf),
                "PxrMaskProjection: params maskFile='%s' channel='%s' "
                "fit=%d invert=%d centerX=%.3f centerY=%.3f "
                "threshold=%.3f fov=%.2f focalScale=%.4f",
                maskFile, maskChannel, m_maskFit, m_invert,
                m_centerX, m_centerY, m_threshold,
                m_fov, m_focalScale);
            msgs->Info(buf);
        }

        // ── Load EXR ──
        if (maskFile[0] != '\0')
        {
            loadExrMask(maskFile, maskChannel, msgs);
        }
        else if (m_debug && msgs)
        {
            msgs->Warning("PxrMaskProjection: No maskFile specified.");
        }
    }

    void RenderEnd(RixContext& ctx) override
    {
        if (m_debug && m_msgs && m_sMaxX > m_sMinX)
        {
            // Implied per-axis FOV: direction = normalize(sx, sy, focalScale),
            // so half-angle = arctan(max |s| / focalScale).
            float fs = (m_focalScale > 1e-6f) ? m_focalScale : 1.0f;
            float sx_abs   = std::max(std::fabs(m_sMinX), std::fabs(m_sMaxX));
            float sy_abs   = std::max(std::fabs(m_sMinY), std::fabs(m_sMaxY));
            float fovH_deg = 2.0f * std::atan(sx_abs / fs) * 57.29578f;
            float fovV_deg = 2.0f * std::atan(sy_abs / fs) * 57.29578f;

            char buf[1024];
            snprintf(buf, sizeof(buf),
                "PxrMaskProjection: actual screen sample range "
                "x=[%.4f,%.4f] y=[%.4f,%.4f]  env.screenWindow "
                "x=[%.4f,%.4f] y=[%.4f,%.4f]  "
                "impliedFOV=%.2fx%.2f deg (configured fov=%.2f)",
                m_sMinX, m_sMaxX, m_sMinY, m_sMaxY,
                m_swL, m_swR, m_swB, m_swT,
                fovH_deg, fovV_deg, m_fov);
            m_msgs->Info(buf);
        }

        m_maskData.clear();
        m_maskData.shrink_to_fit();
        m_maskLoaded = false;
    }

    // ── Project: generate perspective rays + apply mask ──
    //
    // RixProjection is a full camera plugin: we must fill each ray's
    // origin + direction from the screen samples. Then we apply the
    // mask on top — zeroed directions skip shading for that ray.
    void Project(RixProjectionContext& pCtx) override
    {
        const bool doLog = m_debug && (m_projectLogBudget > 0) &&
                           m_msgs && pCtx.numRays > 0;
        if (doLog)
        {
            --m_projectLogBudget;
            char buf[1024];
            snprintf(buf, sizeof(buf),
                "PxrMaskProjection: Project call, numRays=%d "
                "screen[0]=(%.4f,%.4f) screen[last]=(%.4f,%.4f)",
                pCtx.numRays,
                pCtx.screen[0].x, pCtx.screen[0].y,
                pCtx.screen[pCtx.numRays-1].x,
                pCtx.screen[pCtx.numRays-1].y);
            m_msgs->Info(buf);
        }

        int killed = 0;
        float logU0 = 0, logV0 = 0, logMask0 = 0;
        float logDirZ0 = 0;

        for (int i = 0; i < pCtx.numRays; ++i)
        {
            float sx = pCtx.screen[i].x;
            float sy = pCtx.screen[i].y;

            // Racy min/max — fine for debug snapshotting.
            if (sx < m_sMinX) m_sMinX = sx;
            if (sx > m_sMaxX) m_sMaxX = sx;
            if (sy < m_sMinY) m_sMinY = sy;
            if (sy > m_sMaxY) m_sMaxY = sy;

            // Perspective: direction = normalize(sx, sy, focalScale).
            // focalScale = half_screenWindow / tan(fov/2) converts NDC
            // screen samples to the correct camera-space frustum angle.
            const float dz = m_focalScale;
            float invLen = 1.0f / std::sqrt(sx*sx + sy*sy + dz*dz);

            RtRayGeometry& r = pCtx.rays[i];
            r.origin.x = 0.0f;
            r.origin.y = 0.0f;
            r.origin.z = 0.0f;
            r.direction.x = sx * invLen;
            r.direction.y = sy * invLen;
            r.direction.z = dz * invLen;
            r.minDist = m_clipNear;
            r.maxDist = m_clipFar;
            r.originRadius = 0.0f;
            r.raySpread    = 0.0f;
            r.time         = pCtx.time ? pCtx.time[i] : 0.0f;

            if (i == 0) logDirZ0 = r.direction.z;

            // ── Mask lookup ──
            // Normalize screen coord to [-1, +1] using the actual screen
            // window. ny=+1 is the top of screen, -1 is the bottom.
            float nx = 2.0f * (sx - m_swL) / (m_swR - m_swL) - 1.0f;
            float ny = 2.0f * (sy - m_swB) / (m_swT - m_swB) - 1.0f;

            nx -= m_centerX;
            ny -= m_centerY;

            float u = (nx * m_uScale) + 0.5f + m_uOffset;
            float v = (ny * m_vScale) + 0.5f + m_vOffset;

            // EXR row 0 is at the TOP of the image; our v=0 currently
            // sits at the screen bottom. Flip so screen-top (ny=+1)
            // samples EXR row 0.
            v = 1.0f - v;

            if (i == 0) { logU0 = u; logV0 = v; }

            float maskVal = 1.0f;
            if (m_maskLoaded)
            {
                if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
                    maskVal = 0.0f;
                else
                    maskVal = sampleBilinear(u, v);
            }

            if (i == 0) logMask0 = maskVal;

            if (m_invert)
                maskVal = 1.0f - maskVal;

            // Binary kill. The plugin's goal is to skip shading for masked
            // pixels, so naively we'd just zero `direction` — but hdPrman
            // treats a zero-direction ray as "skip" and never writes to the
            // output pixel, leaving whatever the framebuffer was initialised
            // to (typically flat gray, not black).
            //
            // Workaround: fire a degenerate ray with zero length (maxDist=0)
            // and a zero tint. The integrator still walks the write path
            // (so the pixel gets clobbered) but can't hit any geometry,
            // and the final radiance is multiplied by tint=0 → true black.
            // Cost is a handful of cycles per killed ray — far cheaper
            // than full shading, which is what we set out to avoid.
            if (maskVal <= m_threshold)
            {
                ++killed;
                // Non-zero direction so hdPrman doesn't short-circuit.
                r.direction.x = 0.0f;
                r.direction.y = 0.0f;
                r.direction.z = 1.0f;
                r.minDist = 0.0f;
                r.maxDist = 0.0f;   // zero-length → guaranteed miss
                pCtx.tint[i].r = 0.0f;
                pCtx.tint[i].g = 0.0f;
                pCtx.tint[i].b = 0.0f;
            }
        }

        if (doLog)
        {
            char buf[1024];
            snprintf(buf, sizeof(buf),
                "PxrMaskProjection: Project done. ray[0] dir.z=%.4f "
                "u=%.4f v=%.4f maskVal=%.4f  killed=%d/%d",
                logDirZ0, logU0, logV0, logMask0, killed, pCtx.numRays);
            m_msgs->Info(buf);
        }
    }

private:

    int   m_maskFit;
    float m_centerX, m_centerY;
    int   m_invert;
    float m_threshold;
    float m_fov, m_focalScale;

    std::vector<float> m_maskData;
    int   m_maskWidth, m_maskHeight;   // logical mask size (dataWindow size)
    int   m_bufWidth, m_bufHeight;     // allocated buffer size (from origin 0,0)
    int   m_dwMinX, m_dwMinY;          // dataWindow origin into the buffer
    bool  m_maskLoaded;
    float m_renderAspect, m_maskAspect;

    float m_uScale, m_vScale;
    float m_uOffset, m_vOffset;

    float m_clipNear, m_clipFar;
    float m_swL, m_swR, m_swB, m_swT;  // screen window (camera space)
    int   m_debug;

    // Sample-range tracking (debug). Updated racily across threads, but
    // that's fine — worst case we miss a few extrema.
    mutable float m_sMinX, m_sMaxX, m_sMinY, m_sMaxY;

    RixMessages* m_msgs;
    mutable int  m_projectLogBudget;  // log first N Project() calls

    // ── Load EXR (via tinyexr) ───────────────────────────
    void loadExrMask(const char* filePath, const char* channelName,
                     RixMessages* msgs)
    {
        EXRVersion exr_version;
        int ret = ParseEXRVersionFromFile(&exr_version, filePath);
        if (ret != TINYEXR_SUCCESS)
        {
            if (m_debug && msgs)
            {
                char buf[1024];
                snprintf(buf, sizeof(buf),
                    "PxrMaskProjection: ParseEXRVersion failed (%d) for '%s'.",
                    ret, filePath);
                msgs->Error(buf);
            }
            return;
        }
        if (exr_version.multipart)
        {
            if (m_debug && msgs)
                msgs->Error("PxrMaskProjection: Multipart EXR not supported.");
            return;
        }

        EXRHeader exr_header;
        InitEXRHeader(&exr_header);
        const char* err = nullptr;
        ret = ParseEXRHeaderFromFile(&exr_header, &exr_version, filePath, &err);
        if (ret != TINYEXR_SUCCESS)
        {
            if (m_debug && msgs)
            {
                char buf[2048];
                snprintf(buf, sizeof(buf),
                    "PxrMaskProjection: ParseEXRHeader failed (%d): %s",
                    ret, err ? err : "(no msg)");
                msgs->Error(buf);
            }
            if (err) FreeEXRErrorMessage(err);
            return;
        }

        if (m_debug && msgs)
        {
            std::string avail;
            for (int i = 0; i < exr_header.num_channels; ++i)
            {
                if (!avail.empty()) avail += ", ";
                avail += exr_header.channels[i].name;
            }
            char buf[2048];
            snprintf(buf, sizeof(buf),
                "PxrMaskProjection: EXR channels available: [%s]",
                avail.c_str());
            msgs->Info(buf);
        }

        int chanIdx = -1;
        for (int i = 0; i < exr_header.num_channels; ++i)
        {
            if (strcmp(exr_header.channels[i].name, channelName) == 0)
            {
                chanIdx = i;
                break;
            }
        }
        if (chanIdx < 0)
        {
            if (m_debug && msgs)
            {
                char buf[1024];
                snprintf(buf, sizeof(buf),
                    "PxrMaskProjection: Channel '%s' not found in '%s'.",
                    channelName, filePath);
                msgs->Error(buf);
            }
            FreeEXRHeader(&exr_header);
            return;
        }

        // Force HALF channels up to FLOAT so our float buffer is valid.
        for (int i = 0; i < exr_header.num_channels; ++i)
            exr_header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;

        EXRImage exr_image;
        InitEXRImage(&exr_image);
        err = nullptr;
        ret = LoadEXRImageFromFile(&exr_image, &exr_header, filePath, &err);
        if (ret != TINYEXR_SUCCESS)
        {
            if (m_debug && msgs)
            {
                char buf[2048];
                snprintf(buf, sizeof(buf),
                    "PxrMaskProjection: LoadEXRImage failed (%d): %s",
                    ret, err ? err : "(no msg)");
                msgs->Error(buf);
            }
            if (err) FreeEXRErrorMessage(err);
            FreeEXRHeader(&exr_header);
            return;
        }

        m_maskWidth  = exr_image.width;
        m_maskHeight = exr_image.height;
        m_maskAspect = (float)m_maskWidth / (float)m_maskHeight;
        m_bufWidth   = m_maskWidth;
        m_bufHeight  = m_maskHeight;
        m_dwMinX     = 0;
        m_dwMinY     = 0;

        const float* src =
            reinterpret_cast<const float*>(exr_image.images[chanIdx]);
        size_t n = (size_t)m_maskWidth * (size_t)m_maskHeight;
        m_maskData.assign(src, src + n);

        m_maskLoaded = true;

        if (m_debug && msgs)
        {
            char buf[1024];
            snprintf(buf, sizeof(buf),
                "PxrMaskProjection: loaded '%s' channel '%s' (%dx%d)",
                filePath, channelName, m_maskWidth, m_maskHeight);
            msgs->Info(buf);
        }

        if (m_debug && msgs)
        {
            float mn =  1e30f, mx = -1e30f;
            size_t nonZero = 0;
            for (size_t i = 0; i < n; ++i)
            {
                float x = m_maskData[i];
                if (x < mn) mn = x;
                if (x > mx) mx = x;
                if (x > 0.0f) ++nonZero;
            }
            char buf[1024];
            snprintf(buf, sizeof(buf),
                "PxrMaskProjection: mask stats min=%.4f max=%.4f "
                "nonZero=%zu/%zu renderAspect=%.3f maskAspect=%.3f",
                mn, mx, nonZero, n, m_renderAspect, m_maskAspect);
            msgs->Info(buf);
        }

        FreeEXRImage(&exr_image);
        FreeEXRHeader(&exr_header);

        computeScaling();

        if (m_debug && msgs)
        {
            char buf[512];
            snprintf(buf, sizeof(buf),
                "PxrMaskProjection: scaling uScale=%.3f vScale=%.3f "
                "uOffset=%.3f vOffset=%.3f",
                m_uScale, m_vScale, m_uOffset, m_vOffset);
            msgs->Info(buf);
        }
    }

    // ── Auto-scale ───────────────────────────────────────
    void computeScaling()
    {
        float aspectRatio = m_renderAspect / m_maskAspect;
        m_uOffset = 0.0f;
        m_vOffset = 0.0f;

        switch (m_maskFit)
        {
            default:
            case 0:  // STRETCH
                m_uScale = 0.5f;
                m_vScale = 0.5f;
                break;
            case 1:  // FILL
                if (aspectRatio > 1.0f) {
                    m_uScale = 0.5f;
                    m_vScale = 0.5f / aspectRatio;
                } else {
                    m_uScale = 0.5f * aspectRatio;
                    m_vScale = 0.5f;
                }
                break;
            case 2:  // FIT
                if (aspectRatio > 1.0f) {
                    m_uScale = 0.5f * aspectRatio;
                    m_vScale = 0.5f;
                } else {
                    m_uScale = 0.5f;
                    m_vScale = 0.5f / aspectRatio;
                }
                break;
        }
    }

    // ── Bilinear sampling ────────────────────────────────
    // u,v are in [0,1] over the logical mask (dataWindow). We add
    // m_dwMinX/Y to index into the (0,0)-origin buffer.
    float sampleBilinear(float u, float v) const
    {
        float fx = u * (m_maskWidth  - 1);
        float fy = v * (m_maskHeight - 1);
        int x0 = (int)fx;
        int y0 = (int)fy;
        int x1 = std::min(x0 + 1, m_maskWidth  - 1);
        int y1 = std::min(y0 + 1, m_maskHeight - 1);
        float dx = fx - (float)x0;
        float dy = fy - (float)y0;

        int ax0 = x0 + m_dwMinX, ax1 = x1 + m_dwMinX;
        int ay0 = y0 + m_dwMinY, ay1 = y1 + m_dwMinY;

        float v00 = m_maskData[ay0 * m_bufWidth + ax0];
        float v10 = m_maskData[ay0 * m_bufWidth + ax1];
        float v01 = m_maskData[ay1 * m_bufWidth + ax0];
        float v11 = m_maskData[ay1 * m_bufWidth + ax1];

        float top    = v00 + dx * (v10 - v00);
        float bottom = v01 + dx * (v11 - v01);
        return top + dy * (bottom - top);
    }
};


// ============================================================
// Factory method implementations
// ============================================================

RixProjection* PxrMaskProjectionFactory::CreateProjection(
    RixContext& ctx,
    RtUString const handle,
    RixParameterList const* parms)
{
    // Just create the instance — RenderBegin will read params + load EXR
    return new PxrMaskProjection();
}

void PxrMaskProjectionFactory::DestroyProjection(
    RixProjection const* projection)
{
    // Delete through derived type (our dtor is public)
    delete static_cast<PxrMaskProjection const*>(projection);
}


// ============================================================
// Entry points — RenderMan 27 factory macros
// ============================================================
RIX_PROJECTIONFACTORYCREATE
{
    return new PxrMaskProjectionFactory();
}

RIX_PROJECTIONFACTORYDESTROY
{
    delete static_cast<PxrMaskProjectionFactory const*>(factory);
}


// ============================================================
// TROUBLESHOOTING (remaining possible issues)
// ============================================================
//
// 1. "env.width" not found
//    → grep -n "width\|height" $RMANTREE/include/RixProjection.h
//    Might be screenWidth, imageWidth, or inside a sub-struct.
//
// 2. "direction.x" not a member
//    → grep -A5 "direction" $RMANTREE/include/RixIntegrator.h
//    Try: pCtx.rays[i].direction[0] = 0.0f;  (array style)
//
// 3. "EvalParam" for RtUString doesn't match
//    → The string overload might write into an existing RtUString
//    rather than taking a pointer. Check:
//      grep "EvalParam.*UString" $RMANTREE/include/RixShading.h
//
// 4. "RenderBegin" signature mismatch
//    → grep -A3 "RenderBegin" $RMANTREE/include/RixProjection.h
//    Make sure const& matches exactly.
