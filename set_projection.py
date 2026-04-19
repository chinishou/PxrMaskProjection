import math

import hou
from pxr import Sdf, UsdGeom, UsdRender


# ── CONFIG ─────────────────────────────────────────────────────────
RENDER_SETTINGS_PRIM = "/Render/rendersettings"
PROJECTION_NAME      = "PxrMaskProjection"

MASK_FILE    = "D:/dev/PxrMaskProjection/test_mask.exr"   # << set me
MASK_CHANNEL = "R"
MASK_FIT     = 0        # 0=stretch, 1=fill, 2=fit
CENTER_X     = 0.0
CENTER_Y     = 0.0
INVERT       = 0        # 1 to invert
THRESHOLD    = 0.0
SOFT_EDGE    = 0.0
DEBUG        = 0        # 1 for verbose render-log diagnostics

# FOV is derived from the render camera's focalLength + aperture.
# Leave FOV_OVERRIDE = None to auto-compute (recommended).
# Set a float to force a specific value (degrees, narrower image dim).
FOV_OVERRIDE = None

# Which camera to read. Leave CAMERA_PRIM = None to auto-resolve from
# the rendersettings prim's `camera` relationship (UsdRenderSettings).
# Set to a prim path (e.g. "/cameras/camera1") to override.
CAMERA_PRIM  = None
# ───────────────────────────────────────────────────────────────────


def _resolve_camera_prim(stage, rs_prim):
    if CAMERA_PRIM:
        p = stage.GetPrimAtPath(CAMERA_PRIM)
        if not p.IsValid():
            raise RuntimeError(
                "CAMERA_PRIM {0!r} not found on stage.".format(CAMERA_PRIM))
        return p

    # UsdRenderSettings.camera is a relationship pointing at the render cam.
    rs = UsdRender.Settings(rs_prim)
    cam_rel = rs.GetCameraRel()
    targets = cam_rel.GetTargets() if cam_rel else []
    if not targets:
        raise RuntimeError(
            "No camera bound to {0!r}. Either set the rendersettings' "
            "'camera' relationship, or set CAMERA_PRIM in this script, "
            "or set FOV_OVERRIDE to skip camera auto-derivation."
            .format(RENDER_SETTINGS_PRIM))
    cam_prim = stage.GetPrimAtPath(targets[0])
    if not cam_prim.IsValid():
        raise RuntimeError(
            "Camera prim {0!r} (from rendersettings.camera) not found."
            .format(str(targets[0])))
    return cam_prim


def _derive_fov_deg(cam_prim):
    """Narrower-dim FOV in degrees for a UsdGeomCamera prim.

    Matches PxrPerspective's convention: the plugin's `fov` is the
    FOV along the shorter image dimension (vertical for landscape,
    horizontal for portrait).
    """
    cam = UsdGeom.Camera(cam_prim)
    focal = cam.GetFocalLengthAttr().Get()
    h_ap  = cam.GetHorizontalApertureAttr().Get()
    v_ap  = cam.GetVerticalApertureAttr().Get()
    if not focal or not h_ap or not v_ap:
        raise RuntimeError(
            "Camera {0!r} is missing focalLength / horizontalAperture / "
            "verticalAperture.".format(cam_prim.GetPath()))
    narrower = min(float(h_ap), float(v_ap))
    return math.degrees(2.0 * math.atan(narrower * 0.5 / float(focal)))


def _apply(stage):
    prim = stage.GetPrimAtPath(RENDER_SETTINGS_PRIM)
    if not prim.IsValid():
        raise RuntimeError(
            "Prim {0!r} not found. Check RENDER_SETTINGS_PRIM."
            .format(RENDER_SETTINGS_PRIM))

    # Resolve fov (either override or auto-derive from the render camera).
    if FOV_OVERRIDE is not None:
        fov_deg = float(FOV_OVERRIDE)
        print("[PxrMaskProjection] Using FOV_OVERRIDE={0:.4f} deg"
              .format(fov_deg))
    else:
        cam_prim = _resolve_camera_prim(stage, prim)
        fov_deg  = _derive_fov_deg(cam_prim)
        print("[PxrMaskProjection] Derived fov={0:.4f} deg from {1}"
              .format(fov_deg, cam_prim.GetPath()))

    def set_attr(name, sdf_type, value):
        attr = prim.CreateAttribute(name, sdf_type, custom=False)
        attr.Set(value)

    # Which projection to use
    set_attr("ri:projection:name", Sdf.ValueTypeNames.String, PROJECTION_NAME)

    # Plugin params — namespaced under ri:projection:<PluginName>:<param>
    ns = "ri:projection:" + PROJECTION_NAME + ":"
    set_attr(ns + "maskFile",    Sdf.ValueTypeNames.String, MASK_FILE)
    set_attr(ns + "maskChannel", Sdf.ValueTypeNames.String, MASK_CHANNEL)
    set_attr(ns + "maskFit",     Sdf.ValueTypeNames.Int,    MASK_FIT)
    set_attr(ns + "centerX",     Sdf.ValueTypeNames.Float,  CENTER_X)
    set_attr(ns + "centerY",     Sdf.ValueTypeNames.Float,  CENTER_Y)
    set_attr(ns + "invert",      Sdf.ValueTypeNames.Int,    INVERT)
    set_attr(ns + "threshold",   Sdf.ValueTypeNames.Float,  THRESHOLD)
    set_attr(ns + "softEdge",    Sdf.ValueTypeNames.Float,  SOFT_EDGE)
    set_attr(ns + "fov",         Sdf.ValueTypeNames.Float,  fov_deg)
    set_attr(ns + "debug",       Sdf.ValueTypeNames.Int,    DEBUG)

node = hou.pwd()
stage = node.editableStage()
_apply(stage)
