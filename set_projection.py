import hou
from pxr import Sdf


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
FOV          = 50.0     # match your scene camera's narrower-dim FOV in degrees
DEBUG        = 0        # 1 for verbose render-log diagnostics
# ───────────────────────────────────────────────────────────────────


def _apply(stage):
    prim = stage.GetPrimAtPath(RENDER_SETTINGS_PRIM)
    if not prim.IsValid():
        raise RuntimeError(
            "Prim {0!r} not found. Check RENDER_SETTINGS_PRIM."
            .format(RENDER_SETTINGS_PRIM))

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
    set_attr(ns + "fov",         Sdf.ValueTypeNames.Float,  FOV)
    set_attr(ns + "debug",       Sdf.ValueTypeNames.Int,    DEBUG)

node = hou.pwd()
stage = node.editableStage()
_apply(stage)

