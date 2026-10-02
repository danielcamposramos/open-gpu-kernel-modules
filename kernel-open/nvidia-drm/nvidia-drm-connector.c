/*
 * Copyright (c) 2015-2026, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "nvidia-drm-conftest.h" /* NV_DRM_AVAILABLE */

#if defined(NV_DRM_AVAILABLE)

#include "nvidia-drm-helper.h"
#include "nvidia-drm-priv.h"
#include "nvidia-drm-connector.h"
#include "nvidia-drm-crtc.h"
#include "nvidia-drm-utils.h"
#include "nvidia-drm-encoder.h"
#include "nv_drm_common_ioctl.h"

/*
 * Commit fcd70cd36b9b ("drm: Split out drm_probe_helper.h")
 * moves a number of helper function definitions from
 * drm/drm_crtc_helper.h to a new drm_probe_helper.h.
 */
#if defined(NV_DRM_DRM_PROBE_HELPER_H_PRESENT)
#include <drm/drm_probe_helper.h>
#endif
#include <drm/drm_crtc_helper.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_sysfs.h>

/*
 * Dithering support requires connector atomic_check, which we only enable on
 * kernels with HDR metadata support (v5.14+). This serves as a proxy for
 * "new enough kernel" with the new atomic_check signature that takes
 * drm_atomic_state.
 */
#if defined(NV_DRM_CONNECTOR_ATTACH_HDR_OUTPUT_METADATA_PROPERTY_PRESENT)
static int
nv_drm_connector_translate_dither_mode(enum nv_drm_dithering_mode mode,
                                       enum NvKmsDpyAttributeRequestedDitheringValue *state,
                                       enum NvKmsDpyAttributeRequestedDitheringModeValue *hw_mode)
{
    switch (mode) {
    case NV_DRM_DITHERING_MODE_AUTO:
        *state = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_AUTO;
        *hw_mode = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_MODE_AUTO;
        break;
    case NV_DRM_DITHERING_MODE_OFF:
        *state = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_DISABLED;
        *hw_mode = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_MODE_AUTO;
        break;
    case NV_DRM_DITHERING_MODE_STATIC_2X2:
        *state = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_ENABLED;
        *hw_mode = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_MODE_STATIC_2X2;
        break;
    case NV_DRM_DITHERING_MODE_DYNAMIC_2X2:
        *state = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_ENABLED;
        *hw_mode = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_MODE_DYNAMIC_2X2;
        break;
    case NV_DRM_DITHERING_MODE_TEMPORAL:
        *state = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_ENABLED;
        *hw_mode = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_MODE_TEMPORAL;
        break;
    case NV_DRM_DITHERING_MODE_ON:
        *state = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_ENABLED;
        *hw_mode = NV_KMS_DPY_ATTRIBUTE_REQUESTED_DITHERING_MODE_AUTO;
        break;
    default:
        return -EINVAL;
    }

    return 0;
}
#endif /* NV_DRM_CONNECTOR_ATTACH_HDR_OUTPUT_METADATA_PROPERTY_PRESENT */

static void nv_drm_connector_destroy(struct drm_connector *connector)
{
    struct nv_drm_connector *nv_connector = to_nv_connector(connector);

    drm_connector_unregister(connector);

    drm_connector_cleanup(connector);

    if (nv_connector->edid != NULL) {
        nv_drm_free(nv_connector->edid);
    }

    nv_drm_free(nv_connector);
}

static bool
__nv_drm_detect_encoder(struct NvKmsKapiDynamicDisplayParams *pDetectParams,
                        struct drm_connector *connector,
                        struct drm_encoder *encoder)
{
    struct nv_drm_connector *nv_connector = to_nv_connector(connector);
    struct drm_device *dev = connector->dev;
    struct nv_drm_device *nv_dev = to_nv_device(dev);
    struct nv_drm_encoder *nv_encoder;

    /*
     * DVI-I connectors can drive both digital and analog
     * encoders.  If a digital connection has been forced then
     * skip analog encoders.
     */

    if (connector->connector_type == DRM_MODE_CONNECTOR_DVII &&
        connector->force == DRM_FORCE_ON_DIGITAL &&
        encoder->encoder_type == DRM_MODE_ENCODER_DAC) {
        return false;
    }

    nv_encoder = to_nv_encoder(encoder);

    memset(pDetectParams, 0, sizeof(*pDetectParams));

    pDetectParams->handle = nv_encoder->hDisplay;

    switch (connector->force) {
        case DRM_FORCE_ON:
        case DRM_FORCE_ON_DIGITAL:
            pDetectParams->forceConnected = NV_TRUE;
            break;
        case DRM_FORCE_OFF:
            pDetectParams->forceDisconnected = NV_TRUE;
            break;
        case DRM_FORCE_UNSPECIFIED:
            break;
    }

#if defined(NV_DRM_CONNECTOR_HAS_OVERRIDE_EDID)
    if (connector->override_edid) {
#else
    if (drm_edid_override_connector_update(connector) > 0) {
#endif
        const struct drm_property_blob *edid = connector->edid_blob_ptr;

        if (edid->length <= sizeof(pDetectParams->edid.buffer)) {
            memcpy(pDetectParams->edid.buffer, edid->data, edid->length);
            pDetectParams->edid.bufferSize = edid->length;
            pDetectParams->overrideEdid = NV_TRUE;
        } else {
            WARN_ON(edid->length >
                    sizeof(pDetectParams->edid.buffer));
        }
    }

    if (!nvKms->getDynamicDisplayInfo(nv_dev->pDevice, pDetectParams)) {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to detect display state");
        return false;
    }

    dev->mode_config.max_width  = pDetectParams->maxWidthInPixels;
    dev->mode_config.max_height = pDetectParams->maxHeightInPixels;

#if defined(NV_DRM_CONNECTOR_HAS_VRR_CAPABLE_PROPERTY)
    drm_connector_attach_vrr_capable_property(&nv_connector->base);
    drm_connector_set_vrr_capable_property(&nv_connector->base, pDetectParams->vrrSupported ? true : false);
#endif

    if (pDetectParams->connected) {
        if (!pDetectParams->overrideEdid && pDetectParams->edid.bufferSize) {

            if ((nv_connector->edid = nv_drm_calloc(
                        1,
                        pDetectParams->edid.bufferSize)) != NULL) {

                memcpy(nv_connector->edid,
                       pDetectParams->edid.buffer,
                       pDetectParams->edid.bufferSize);
            } else {
                NV_DRM_LOG_ERR("Out of Memory");
            }
        }

        return true;
    }

    return false;
}

static enum drm_connector_status __nv_drm_connector_detect_internal(
    struct drm_connector *connector)
{
    struct drm_device *dev = connector->dev;
    struct nv_drm_connector *nv_connector = to_nv_connector(connector);

    enum drm_connector_status status = connector_status_disconnected;

    struct drm_encoder *detected_encoder = NULL;
    struct nv_drm_encoder *nv_detected_encoder = NULL;
    struct drm_encoder *encoder;

    struct NvKmsKapiDynamicDisplayParams *pDetectParams = NULL;

    BUG_ON(!mutex_is_locked(&dev->mode_config.mutex));

    if (nv_connector->edid != NULL) {
        nv_drm_free(nv_connector->edid);
        nv_connector->edid = NULL;
    }

    if ((pDetectParams = nv_drm_calloc(
                1,
                sizeof(*pDetectParams))) == NULL) {
        WARN_ON(pDetectParams == NULL);
        goto done;
    }

    nv_drm_connector_for_each_possible_encoder(connector, encoder) {
        if (__nv_drm_detect_encoder(pDetectParams, connector, encoder)) {
            detected_encoder = encoder;
            break;
        }
    } nv_drm_connector_for_each_possible_encoder_end;

    if (detected_encoder == NULL) {
        goto done;
    }

    nv_detected_encoder = to_nv_encoder(detected_encoder);

    status = connector_status_connected;

    nv_connector->nv_detected_encoder = nv_detected_encoder;

    if (nv_connector->type == NVKMS_CONNECTOR_TYPE_DVI_I) {
        drm_object_property_set_value(
            &connector->base,
            dev->mode_config.dvi_i_subconnector_property,
            detected_encoder->encoder_type == DRM_MODE_ENCODER_DAC ?
                DRM_MODE_SUBCONNECTOR_DVIA :
                DRM_MODE_SUBCONNECTOR_DVID);
    }

done:

    nv_drm_free(pDetectParams);

    if (status == connector_status_disconnected &&
        nv_connector->modeset_permission_filep) {
        nv_drm_connector_revoke_permissions(dev, nv_connector);
    }

    return status;
}

static void nv_drm_connector_reset(struct drm_connector *connector)
{
    struct nv_drm_connector_state  * nv_connector_state =
            nv_drm_calloc(1, sizeof(*nv_connector_state));

    if (!nv_connector_state) {
        return;
    }

    if (connector->state)
    {
        struct nv_drm_connector_state *nv_drm_connector_state_old =
                to_nv_drm_connector_state(connector->state);
        __drm_atomic_helper_connector_destroy_state(connector->state);
        nv_drm_free(nv_drm_connector_state_old);
    }

    __drm_atomic_helper_connector_reset(connector, &nv_connector_state->base);
}

static struct drm_connector_state* nv_drm_connector_atomic_duplicate_state(struct drm_connector *connector)
{
    struct nv_drm_connector_state *nv_drm_old_connector_state =
           to_nv_drm_connector_state(connector->state);
    struct nv_drm_connector_state *nv_drm_new_connector_state =
           nv_drm_calloc(1, sizeof(*nv_drm_new_connector_state));

    if (!nv_drm_new_connector_state) {
        return NULL;
    }

    __drm_atomic_helper_connector_duplicate_state(connector, &nv_drm_new_connector_state->base);

    nv_drm_new_connector_state->dithering_mode = nv_drm_old_connector_state->dithering_mode;
    nv_drm_new_connector_state->hdcp_topology_blob = nv_drm_old_connector_state->hdcp_topology_blob;
    if (nv_drm_new_connector_state->hdcp_topology_blob) {
        drm_property_blob_get(nv_drm_new_connector_state->hdcp_topology_blob);
    }

    nv_drm_new_connector_state->hdmi_vsif_metadata =
        nv_drm_old_connector_state->hdmi_vsif_metadata;
    if (nv_drm_new_connector_state->hdmi_vsif_metadata) {
        drm_property_blob_get(nv_drm_new_connector_state->hdmi_vsif_metadata);
    }

    return &nv_drm_new_connector_state->base;
}

static void nv_drm_connector_atomic_destroy_state(
            struct drm_connector *connector,
            struct drm_connector_state *state)
{
    struct nv_drm_connector_state *nv_drm_connector_state =
           to_nv_drm_connector_state(state);

    __drm_atomic_helper_connector_destroy_state(state);
    drm_property_blob_put(nv_drm_connector_state->hdcp_topology_blob);

    drm_property_blob_put(nv_drm_connector_state->hdmi_vsif_metadata);

    nv_drm_free(nv_drm_connector_state);
}

static void __nv_drm_connector_force(struct drm_connector *connector)
{
    __nv_drm_connector_detect_internal(connector);
}

static enum drm_connector_status
nv_drm_connector_detect(struct drm_connector *connector, bool force)
{
    return __nv_drm_connector_detect_internal(connector);
}

static int nv_drm_connector_atomic_set_property(
    struct drm_connector *connector,
    struct drm_connector_state *state,
    struct drm_property *property,
    uint64_t val)
{
    struct nv_drm_device *nv_dev = to_nv_device(connector->dev);
    struct nv_drm_connector_state *nv_connector_state =
        to_nv_drm_connector_state(state);

    if (property == nv_dev->nv_connector_dithering_mode_property) {
        if (val >= NV_DRM_DITHERING_MODE_MAX) {
            return -EINVAL;
        }
        nv_connector_state->dithering_mode = val;
        return 0;
    } else if (property == nv_dev->nv_hdcp_level_property) {
        // nv_hdcp_level_property is read only
        return -EINVAL;
    }
    

    if (property == nv_dev->nv_connector_hdmi_vsif_metadata_property) {
        return nv_drm_atomic_replace_property_blob_from_id_size_range(
                    nv_dev->dev,
                    &nv_connector_state->hdmi_vsif_metadata,
                    val,
                    NV_DRM_HDMI_VSIF_METADATA_MIN_PAYLOAD_SIZE,
                    NV_DRM_HDMI_VSIF_METADATA_MAX_PAYLOAD_SIZE);
    }

    /* Unknown property - DRM core handles standard connector properties */
    return -EINVAL;
}

static int nv_drm_connector_atomic_get_property(
    struct drm_connector *connector,
    const struct drm_connector_state *state,
    struct drm_property *property,
    uint64_t *val)
{
    struct nv_drm_device *nv_dev = to_nv_device(connector->dev);
    struct nv_drm_connector *nv_conn = to_nv_connector(connector);    
    const struct nv_drm_connector_state *nv_connector_state =
        to_nv_drm_connector_state_const(state);

    if (property == nv_dev->nv_connector_dithering_mode_property) {
        *val = nv_connector_state->dithering_mode;
        return 0;
    } else if (property == nv_dev->nv_hdcp_level_property) {
        *val = nv_conn->cp;
        return 0;
    }

    if (property == nv_dev->nv_connector_hdmi_vsif_metadata_property) {
        *val = nv_connector_state->hdmi_vsif_metadata ?
            nv_connector_state->hdmi_vsif_metadata->base.id : 0;
        return 0;
    }

    /* Unknown property - DRM core handles standard connector properties */
    return -EINVAL;
}

static struct drm_connector_funcs nv_connector_funcs = {
    .destroy                = nv_drm_connector_destroy,
    .reset                  = nv_drm_connector_reset,
    .force                  = __nv_drm_connector_force,
    .detect                 = nv_drm_connector_detect,
    .fill_modes             = drm_helper_probe_single_connector_modes,
    .atomic_duplicate_state = nv_drm_connector_atomic_duplicate_state,
    .atomic_destroy_state   = nv_drm_connector_atomic_destroy_state,
    .atomic_set_property    = nv_drm_connector_atomic_set_property,
    .atomic_get_property    = nv_drm_connector_atomic_get_property,
};

/*
 * HDMI VSDB stereo-mode synthesis
 * --------------------------------
 *
 * NVKMS lists only 2D timings through nvKms->getDisplayMode() and
 * NvKmsKapiDisplayMode carries no stereo information, so no 3D-flagged
 * mode ever reaches DRM's mode list and the stereo_allowed admission gate
 * has nothing to admit.  Mirror the DRM core reference implementation
 * (drivers/gpu/drm/drm_edid.c: do_hdmi_vsdb_modes() plus the CTA-mandatory
 * stereo table) here in the open glue, using the EDID NVKMS already
 * fetched: synthesize DRM_MODE_FLAG_3D_* variants by cloning base timings
 * the driver itself already listed.
 */

struct nv_cea_vic_info {
    NvU8   vic;
    NvU16  width;
    NvU16  height;
    NvU8   vrefresh;    /* drm_mode_vrefresh() style, rounded */
    NvBool interlaced;
};

/*
 * CTA/CEA-861 VICs that 3D televisions declare in VSDB 3D fields; timings
 * condensed from edid_cea_modes[] in drivers/gpu/drm/drm_edid.c (that table
 * is static, i.e. not usable from here).  A VIC missing from this table is
 * silently skipped: no stereo variant is synthesized for it.
 */
static const struct nv_cea_vic_info nv_cea_vic_info[] = {
    {   1,  640,  480,  60, NV_FALSE },
    {   2,  720,  480,  60, NV_FALSE },
    {   3,  720,  480,  60, NV_FALSE },
    {   4, 1280,  720,  60, NV_FALSE },
    {   5, 1920, 1080,  60, NV_TRUE  },
    {  16, 1920, 1080,  60, NV_FALSE },
    {  17,  720,  576,  50, NV_FALSE },
    {  18,  720,  576,  50, NV_FALSE },
    {  19, 1280,  720,  50, NV_FALSE },
    {  20, 1920, 1080,  50, NV_TRUE  },
    {  31, 1920, 1080,  50, NV_FALSE },
    {  32, 1920, 1080,  24, NV_FALSE },
    {  33, 1920, 1080,  25, NV_FALSE },
    {  34, 1920, 1080,  30, NV_FALSE },
    {  39, 1920, 1080,  50, NV_TRUE  },
    {  40, 1920, 1080, 100, NV_TRUE  },
    {  41, 1280,  720, 100, NV_FALSE },
    {  44, 1440,  576, 100, NV_TRUE  },
    {  46, 1920, 1080, 120, NV_TRUE  },
    {  47, 1280,  720, 120, NV_FALSE },
    {  50, 1440,  480, 120, NV_TRUE  },
    {  60, 1280,  720,  24, NV_FALSE },
    {  61, 1280,  720,  25, NV_FALSE },
    {  62, 1280,  720,  30, NV_FALSE },
    {  63, 1920, 1080, 120, NV_FALSE },
    {  64, 1920, 1080, 100, NV_FALSE },
    {  93, 3840, 2160,  24, NV_FALSE },
    {  94, 3840, 2160,  25, NV_FALSE },
    {  95, 3840, 2160,  30, NV_FALSE },
    {  96, 3840, 2160,  50, NV_FALSE },
    {  97, 3840, 2160,  60, NV_FALSE },
    {  98, 4096, 2160,  24, NV_FALSE },
    {  99, 4096, 2160,  25, NV_FALSE },
    { 100, 4096, 2160,  30, NV_FALSE },
    { 101, 4096, 2160,  50, NV_FALSE },
    { 102, 4096, 2160,  60, NV_FALSE },
};

/* CTA-mandatory stereo timings, verbatim from drm_edid.c. */
struct nv_stereo_mandatory_mode {
    int width, height, vrefresh;
    NvU32 flags;
};

static const struct nv_stereo_mandatory_mode nv_stereo_mandatory_modes[] = {
    { 1920, 1080, 24, DRM_MODE_FLAG_3D_TOP_AND_BOTTOM },
    { 1920, 1080, 24, DRM_MODE_FLAG_3D_FRAME_PACKING },
    { 1920, 1080, 50,
      DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF },
    { 1920, 1080, 60,
      DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF },
    { 1280, 720,  50, DRM_MODE_FLAG_3D_TOP_AND_BOTTOM },
    { 1280, 720,  50, DRM_MODE_FLAG_3D_FRAME_PACKING },
    { 1280, 720,  60, DRM_MODE_FLAG_3D_TOP_AND_BOTTOM },
    { 1280, 720,  60, DRM_MODE_FLAG_3D_FRAME_PACKING },
};

/*
 * Clone every already-listed mode whose timing matches
 * (@width, @height, @interlaced, @vrefresh +- 1 Hz for 1000/1001 variants
 * of one VIC), tag the clone with @stereo_flags and append it to the
 * probed list.  Only base timings the driver itself surfaced are cloned.
 */
static int nv_stereo_clone_match(struct drm_connector *connector,
                                 int width, int height,
                                 NvBool interlaced, int vrefresh,
                                 NvU32 stereo_flags)
{
    struct drm_device *dev = connector->dev;
    const struct drm_display_mode *mode;
    struct list_head synth_modes;
    int modes = 0;

    INIT_LIST_HEAD(&synth_modes);

    list_for_each_entry(mode, &connector->probed_modes, head) {
        struct drm_display_mode *new_mode;

        if (mode->hdisplay != width || mode->vdisplay != height)
            continue;

        if (!!(mode->flags & DRM_MODE_FLAG_INTERLACE) != !!interlaced)
            continue;

        if (abs(drm_mode_vrefresh(mode) - vrefresh) > 1)
            continue;

        /* never derive a stereo mode from another stereo mode */
        if (mode->flags & DRM_MODE_FLAG_3D_MASK)
            continue;

        new_mode = drm_mode_duplicate(dev, mode);
        if (!new_mode)
            continue;

        new_mode->flags |= stereo_flags;
        list_add_tail(&new_mode->head, &synth_modes);
        modes++;
    }

    list_splice_tail(&synth_modes, &connector->probed_modes);

    return modes;
}

static int nv_stereo_add_mandatory(struct drm_connector *connector)
{
    int modes = 0, i;

    for (i = 0; i < ARRAY_SIZE(nv_stereo_mandatory_modes); i++) {
        const struct nv_stereo_mandatory_mode *m =
            &nv_stereo_mandatory_modes[i];

        modes += nv_stereo_clone_match(connector,
                                       m->width, m->height,
                                       !!(m->flags & DRM_MODE_FLAG_INTERLACE),
                                       m->vrefresh,
                                       m->flags & ~DRM_MODE_FLAG_INTERLACE);
    }

    return modes;
}

/*
 * Add @stereo_flag variants for the CTA VIC at position @vic_index in the
 * sink's Video Data Block list (@vics; 3D_MASK and 2D_VIC_order index into
 * this list, not into the VIC namespace).  Returns 0 when the index is out
 * of range or the VIC has no entry in nv_cea_vic_info[].
 */
static int nv_stereo_add_vic_index(struct drm_connector *connector,
                                   const NvU8 *vics, NvU32 vics_len,
                                   NvU8 vic_index, NvU32 stereo_flag)
{
    size_t i;
    NvU8 vic;
    const struct nv_cea_vic_info *vi = NULL;

    if (vic_index >= vics_len)
        return 0;

    vic = vics[vic_index];
    if ((vic >= 1 && vic <= 64) || (vic >= 129 && vic <= 192))
        vic &= 0x7f;    /* strip the native-mode indicator bit */

    for (i = 0; i < ARRAY_SIZE(nv_cea_vic_info); i++) {
        if (nv_cea_vic_info[i].vic == vic) {
            vi = &nv_cea_vic_info[i];
            break;
        }
    }

    if (vi == NULL)
        return 0;

    return nv_stereo_clone_match(connector, vi->width, vi->height,
                                 vi->interlaced, vi->vrefresh, stereo_flag);
}

/*
 * Parse one HDMI VSDB (OUI 00-0C-03) for stereo declarations and add the
 * corresponding stereo mode variants.  @db points at the data block
 * including its header byte and @len is the payload length in bytes, the
 * same convention do_hdmi_vsdb_modes() uses in drm_edid.c.
 */
static int nv_hdmi_vsdb_stereo_modes(struct drm_connector *connector,
                                     const NvU8 *db, NvU8 len,
                                     const NvU8 *vics, NvU32 vics_len)
{
    int modes = 0, offset = 0, i, multi_present = 0, multi_len;
    NvU8 vic_len, hdmi_3d_len = 0;
    NvU16 mask, structure_all;

    if (len < 8)
        return 0;

    /* HDMI_Video_present */
    if (!(db[8] & (1 << 5)))
        return 0;

    /* latency fields, as hdmi_vsdb_latency_length(): none / 2 / 4 bytes */
    if (db[8] & (1 << 7))
        offset = (db[8] & (1 << 6)) ? 4 : 2;

    if (len < 8 + offset + 2)
        return 0;

    /* 3D_Present */
    offset++;
    if (db[8 + offset] & (1 << 7)) {
        modes += nv_stereo_add_mandatory(connector);

        /* 3D_Multi_present */
        multi_present = (db[8 + offset] & 0x60) >> 5;
    }

    offset++;
    vic_len = db[8 + offset] >> 5;
    hdmi_3d_len = db[8 + offset] & 0x1f;

    /*
     * Additional 4K-only HDMI VIC entries (add_hdmi_mode() in drm_edid.c)
     * are not synthesized here: when valid they are already on the NVKMS
     * list.  Only the bytes matter, for offset arithmetic.
     */
    if (len < 8 + offset + vic_len)
        return modes;
    offset += 1 + vic_len;

    if (multi_present == 1)
        multi_len = 2;
    else if (multi_present == 2)
        multi_len = 4;
    else
        multi_len = 0;

    if (len < 8 + offset + hdmi_3d_len - 1)
        return modes;

    if (hdmi_3d_len < multi_len)
        return modes;

    if (multi_present == 1 || multi_present == 2) {
        /* 3D_Structure_ALL */
        structure_all = (db[8 + offset] << 8) | db[9 + offset];

        /* 3D_MASK, present only for multi_present == 2 */
        if (multi_present == 2)
            mask = (db[10 + offset] << 8) | db[11 + offset];
        else
            mask = 0xffff;

        for (i = 0; i < 16; i++) {
            if (!(mask & (1 << i)))
                continue;

            /* as add_3d_struct_modes(): structures 0, 3, 6 and 8 */
            if (structure_all & (1 << 0))
                modes += nv_stereo_add_vic_index(
                             connector, vics, vics_len, (NvU8)i,
                             DRM_MODE_FLAG_3D_FRAME_PACKING);
            if (structure_all & (1 << 3))
                modes += nv_stereo_add_vic_index(
                             connector, vics, vics_len, (NvU8)i,
                             DRM_MODE_FLAG_3D_SIDE_BY_SIDE_FULL);
            if (structure_all & (1 << 6))
                modes += nv_stereo_add_vic_index(
                             connector, vics, vics_len, (NvU8)i,
                             DRM_MODE_FLAG_3D_TOP_AND_BOTTOM);
            if (structure_all & (1 << 8))
                modes += nv_stereo_add_vic_index(
                             connector, vics, vics_len, (NvU8)i,
                             DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF);
        }
    }

    offset += multi_len;

    /* per-VIC 3D_Structure_X entries (2D_VIC_order_X | 3D_Structure_X) */
    for (i = 0; i < hdmi_3d_len - multi_len; i++) {
        NvU8 vic_index;
        NvU32 stereo_flag = 0;
        NvBool detail_present;

        detail_present = ((db[8 + offset + i] & 0x0f) > 7);

        if (detail_present && (i + 1 == hdmi_3d_len - multi_len))
            break;

        vic_index = db[8 + offset + i] >> 4;

        switch (db[8 + offset + i] & 0x0f) {
        case 0:
            stereo_flag = DRM_MODE_FLAG_3D_FRAME_PACKING;
            break;
        case 3:
            stereo_flag = DRM_MODE_FLAG_3D_SIDE_BY_SIDE_FULL;
            break;
        case 6:
            stereo_flag = DRM_MODE_FLAG_3D_TOP_AND_BOTTOM;
            break;
        case 8:
            /* 3D_Detail_X: 1 = horizontal subsampling (SbS half) */
            if ((db[9 + offset + i] >> 4) == 1)
                stereo_flag = DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF;
            break;
        }

        if (stereo_flag != 0)
            modes += nv_stereo_add_vic_index(connector, vics, vics_len,
                                             vic_index, stereo_flag);

        if (detail_present)
            i++;
    }

    return modes;
}

/*
 * Walk the cached EDID's CTA extensions: first collect the Video Data
 * Block VIC list that 3D_MASK/2D_VIC_order index into, then parse every
 * HDMI VSDB for stereo declarations and synthesize the corresponding
 * DRM_MODE_FLAG_3D_* mode variants.
 */
static int nv_edid_vsdb_stereo_modes(struct drm_connector *connector,
                                     const NvU8 *edid)
{
    NvU8 vics[64];
    NvU32 vics_len = 0;
    NvU32 ext, db, i;
    int modes = 0;

    /* pass 1: collect VICs from all CTA Video Data Blocks */
    for (ext = 1; ext <= edid[126] && ext < 16; ext++) {
        const NvU8 *ceab = edid + ext * EDID_LENGTH;
        NvU8 end;

        if (ceab[0] != 0x02)
            continue;

        end = ceab[2] ? ceab[2] : 127;
        if (end < 4)
            continue;

        for (db = 4; db < end; ) {
            const NvU8 tag = ceab[db] >> 5, len = ceab[db] & 0x1f;
            const NvU8 hdr = (tag == 0x7) ? 2 : 1;

            if (db + hdr + len > end)
                break;

            if (tag == 0x2) {
                for (i = 0; i < len && vics_len < sizeof(vics); i++)
                    vics[vics_len++] = ceab[db + hdr + i];
            }

            db += hdr + len;
        }
    }

    /* pass 2: parse HDMI VSDBs (OUI 00-0C-03, least significant byte first) */
    for (ext = 1; ext <= edid[126] && ext < 16; ext++) {
        const NvU8 *ceab = edid + ext * EDID_LENGTH;
        NvU8 end;

        if (ceab[0] != 0x02)
            continue;

        end = ceab[2] ? ceab[2] : 127;
        if (end < 4)
            continue;

        for (db = 4; db < end; ) {
            const NvU8 tag = ceab[db] >> 5, len = ceab[db] & 0x1f;
            const NvU8 hdr = (tag == 0x7) ? 2 : 1;

            if (db + hdr + len > end)
                break;

            if (tag == 0x3 && len >= 4 &&
                ceab[db + 1] == 0x03 &&
                ceab[db + 2] == 0x0c &&
                ceab[db + 3] == 0x00) {
                modes += nv_hdmi_vsdb_stereo_modes(connector, ceab + db,
                                                   len, vics, vics_len);
            }

            db += hdr + len;
        }
    }

    return modes;
}

static int nv_drm_connector_get_modes(struct drm_connector *connector)
{
    struct drm_device *dev = connector->dev;
    struct nv_drm_device *nv_dev = to_nv_device(dev);
    struct nv_drm_connector *nv_connector = to_nv_connector(connector);
    struct nv_drm_encoder *nv_detected_encoder =
                           nv_connector->nv_detected_encoder;
    NvU32 modeIndex = 0;
    int   count = 0;


    if (nv_connector->edid != NULL) {
        nv_drm_connector_update_edid_property(connector, nv_connector->edid);
    }

    while (1) {
        struct drm_display_mode *mode;
        struct NvKmsKapiDisplayMode displayMode;
        NvBool valid = 0;
        NvBool preferredMode = NV_FALSE;
        int ret;

        ret = nvKms->getDisplayMode(nv_dev->pDevice,
                                    nv_detected_encoder->hDisplay,
                                    modeIndex++, &displayMode, &valid,
                                    &preferredMode);

        if (ret < 0) {
            NV_DRM_DEV_LOG_ERR(
                nv_dev,
                "Failed to get mode at modeIndex %d of NvKmsKapiDisplay 0x%08x",
                modeIndex, nv_detected_encoder->hDisplay);
            break;
        }

        /* Is end of mode-list */

        if (ret == 0) {
            break;
        }

        /* Ignore invalid modes */

        if (!valid) {
            continue;
        }

        mode = drm_mode_create(connector->dev);

        if (mode == NULL) {
            NV_DRM_DEV_LOG_ERR(
                nv_dev,
                "Failed to create mode for NvKmsKapiDisplay 0x%08x",
                nv_detected_encoder->hDisplay);
            continue;
        }

        nvkms_display_mode_to_drm_mode(&displayMode, mode);

        if (preferredMode) {
            mode->type |= DRM_MODE_TYPE_PREFERRED;
        }

        /* Add a mode to a connector's probed_mode list */

        drm_mode_probed_add(connector, mode);

        count++;
    }

    /*
     * Synthesize HDMI VSDB stereo-mode variants from the cached EDID.
     * Runs only when the connector opted into stereo admission
     * (stereo_allowed), so the synthesized modes pass DRM core's
     * MODE_NO_STEREO validation and become visible to userspace.
     */
    if (count > 0 && connector->stereo_allowed && nv_connector->edid != NULL)
        count += nv_edid_vsdb_stereo_modes(connector,
                                           (const NvU8 *)nv_connector->edid);

    return count;
}

static enum drm_mode_status nv_drm_connector_mode_valid(struct drm_connector    *connector,
#if defined(NV_DRM_CONNECTOR_HELPER_FUNCS_MODE_VALID_HAS_CONST_MODE_ARG)
                                                        const struct drm_display_mode *mode)
#else
                                                        struct drm_display_mode *mode)
#endif
{
    struct drm_device *dev = connector->dev;
    struct nv_drm_device *nv_dev = to_nv_device(dev);
    struct nv_drm_encoder *nv_detected_encoder =
                           to_nv_connector(connector)->nv_detected_encoder;
    struct NvKmsKapiDisplayMode displayMode;

    if (nv_detected_encoder == NULL) {
        return MODE_BAD;
    }

    drm_mode_to_nvkms_display_mode(mode, &displayMode);

    if (!nvKms->validateDisplayMode(nv_dev->pDevice,
                                    nv_detected_encoder->hDisplay,
                                    &displayMode)) {
        return MODE_BAD;
    }

    return MODE_OK;
}

static struct drm_encoder*
nv_drm_connector_best_encoder(struct drm_connector *connector)
{
    struct nv_drm_connector *nv_connector = to_nv_connector(connector);

    if (nv_connector->nv_detected_encoder != NULL) {
        return &nv_connector->nv_detected_encoder->base;
    }

    return NULL;
}

#if defined(NV_DRM_MODE_CREATE_DP_COLORSPACE_PROPERTY_HAS_SUPPORTED_COLORSPACES_ARG)
static const NvU32 __nv_drm_connector_supported_colorspaces =
    BIT(DRM_MODE_COLORIMETRY_BT2020_RGB) |
    BIT(DRM_MODE_COLORIMETRY_BT2020_YCC);
#endif

#if defined(NV_DRM_CONNECTOR_ATTACH_HDR_OUTPUT_METADATA_PROPERTY_PRESENT)
static int
__nv_drm_connector_atomic_check(struct drm_connector *connector,
                                nv_drm_atomic_state_base_t *state)
{
    struct drm_connector_state *new_connector_state =
        drm_atomic_get_new_connector_state(state, connector);
    struct drm_connector_state *old_connector_state =
        drm_atomic_get_old_connector_state(state, connector);
    struct nv_drm_device *nv_dev = to_nv_device(connector->dev);
    struct nv_drm_connector_state *nv_new_connector_state =
        to_nv_drm_connector_state(new_connector_state);
    struct nv_drm_connector_state *nv_old_connector_state =
        to_nv_drm_connector_state(old_connector_state);

    struct drm_crtc *crtc = new_connector_state->crtc;
    struct drm_crtc_state *crtc_state;
    struct nv_drm_crtc_state *nv_crtc_state;
    struct NvKmsKapiHeadRequestedConfig *req_config;

    if (!crtc) {
        return 0;
    }

    crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
    nv_crtc_state = to_nv_crtc_state(crtc_state);
    req_config = &nv_crtc_state->req_config;

    /* Handle dithering changes */
    req_config->flags.ditheringChanged =
        nv_old_connector_state->dithering_mode !=
        nv_new_connector_state->dithering_mode;
    if (req_config->flags.ditheringChanged) {
        enum NvKmsDpyAttributeRequestedDitheringValue state;
        enum NvKmsDpyAttributeRequestedDitheringModeValue mode;
        int ret = nv_drm_connector_translate_dither_mode(
            nv_new_connector_state->dithering_mode, &state, &mode);
        if (ret != 0) {
            return ret;
        }
        req_config->modeSetConfig.dithering.state = state;
        req_config->modeSetConfig.dithering.mode = mode;
    }

    /* Handle HDMI VSIF metadata changes */
    req_config->flags.hdmiVsifMetadataChanged =
        !nv_drm_blobs_equal(nv_old_connector_state->hdmi_vsif_metadata,
                            nv_new_connector_state->hdmi_vsif_metadata);
    if (req_config->flags.hdmiVsifMetadataChanged) {
        if (nv_new_connector_state->hdmi_vsif_metadata &&
            nv_new_connector_state->hdmi_vsif_metadata->data) {
            struct drm_nvidia_hdmi_vsif_metadata* vsif_metadata =
                (struct drm_nvidia_hdmi_vsif_metadata*)
                nv_new_connector_state->hdmi_vsif_metadata->data;
            NvU32 payload_size =
                nv_new_connector_state->hdmi_vsif_metadata->length;
            WARN_ON(payload_size < NV_DRM_HDMI_VSIF_METADATA_MIN_PAYLOAD_SIZE ||
                    payload_size > NV_DRM_HDMI_VSIF_METADATA_MAX_PAYLOAD_SIZE);
            memcpy(req_config->modeSetConfig.hdmiVsifMetadata.payload,
                   vsif_metadata->payload,
                   payload_size);
            req_config->modeSetConfig.hdmiVsifMetadata.payloadSize =
                payload_size;
        } else {
            req_config->modeSetConfig.hdmiVsifMetadata.payloadSize = 0;
        }
    }

    /*
     * Override metadata for the entire head instead of allowing NVKMS to derive
     * it from the layers' metadata.
     *
     * This is the metadata that will sent to the display, and if applicable,
     * layers will be tone mapped to this metadata rather than that of the
     * display.
     */
    req_config->flags.hdrInfoFrameChanged =
        !drm_connector_atomic_hdr_metadata_equal(old_connector_state,
                                                 new_connector_state);
    if (new_connector_state->hdr_output_metadata &&
        new_connector_state->hdr_output_metadata->data) {

        /*
         * Note that HDMI definitions are used here even though we might not
         * be using HDMI. While that seems odd, it is consistent with
         * upstream behavior.
         */

        struct hdr_output_metadata *hdr_metadata =
            new_connector_state->hdr_output_metadata->data;
        struct hdr_metadata_infoframe *info_frame =
            &hdr_metadata->hdmi_metadata_type1;
        unsigned int i;

        if (hdr_metadata->metadata_type != HDMI_STATIC_METADATA_TYPE1) {
            return -EINVAL;
        }

        for (i = 0; i < ARRAY_SIZE(info_frame->display_primaries); i++) {
            req_config->modeSetConfig.hdrInfoFrame.staticMetadata.displayPrimaries[i].x =
                info_frame->display_primaries[i].x;
            req_config->modeSetConfig.hdrInfoFrame.staticMetadata.displayPrimaries[i].y =
                info_frame->display_primaries[i].y;
        }

        req_config->modeSetConfig.hdrInfoFrame.staticMetadata.whitePoint.x =
            info_frame->white_point.x;
        req_config->modeSetConfig.hdrInfoFrame.staticMetadata.whitePoint.y =
            info_frame->white_point.y;
        req_config->modeSetConfig.hdrInfoFrame.staticMetadata.maxDisplayMasteringLuminance =
            info_frame->max_display_mastering_luminance;
        req_config->modeSetConfig.hdrInfoFrame.staticMetadata.minDisplayMasteringLuminance =
            info_frame->min_display_mastering_luminance;
        req_config->modeSetConfig.hdrInfoFrame.staticMetadata.maxCLL =
            info_frame->max_cll;
        req_config->modeSetConfig.hdrInfoFrame.staticMetadata.maxFALL =
            info_frame->max_fall;

        req_config->modeSetConfig.hdrInfoFrame.eotf = info_frame->eotf;

        req_config->modeSetConfig.hdrInfoFrame.enabled = NV_TRUE;
    } else {
        req_config->modeSetConfig.hdrInfoFrame.enabled = NV_FALSE;
    }

    req_config->flags.colorimetryChanged =
        (old_connector_state->colorspace != new_connector_state->colorspace);
    // When adding a case here, also add to __nv_drm_connector_supported_colorspaces
    switch (new_connector_state->colorspace) {
        case DRM_MODE_COLORIMETRY_DEFAULT:
            req_config->modeSetConfig.colorimetry =
                NVKMS_OUTPUT_COLORIMETRY_DEFAULT;
            break;
        case DRM_MODE_COLORIMETRY_BT601_YCC:
            req_config->modeSetConfig.colorimetry =
                NVKMS_OUTPUT_COLORIMETRY_BT601;
            break;
        case DRM_MODE_COLORIMETRY_BT709_YCC:
            req_config->modeSetConfig.colorimetry =
                NVKMS_OUTPUT_COLORIMETRY_BT709;
            break;
        case DRM_MODE_COLORIMETRY_BT2020_RGB:
        case DRM_MODE_COLORIMETRY_BT2020_YCC:
            // Ignore RGB/YCC
            // See https://patchwork.freedesktop.org/patch/525496/?series=111865&rev=4
            req_config->modeSetConfig.colorimetry =
                NVKMS_OUTPUT_COLORIMETRY_BT2100;
            break;
        default:
            // XXX HDR TODO: Add support for more color spaces
            NV_DRM_DEV_LOG_ERR(nv_dev, "Unsupported color space");
            return -EINVAL;
    }

    return 0;
}
#endif /* defined(NV_DRM_CONNECTOR_ATTACH_HDR_OUTPUT_METADATA_PROPERTY_PRESENT) */

static const struct drm_connector_helper_funcs nv_connector_helper_funcs = {
    .get_modes    = nv_drm_connector_get_modes,
    .mode_valid   = nv_drm_connector_mode_valid,
    .best_encoder = nv_drm_connector_best_encoder,
#if defined(NV_DRM_CONNECTOR_ATTACH_HDR_OUTPUT_METADATA_PROPERTY_PRESENT)
    .atomic_check = __nv_drm_connector_atomic_check,
#endif
};

static struct drm_connector*
nv_drm_connector_new(struct drm_device *dev,
                     NvU32 physicalIndex, NvKmsConnectorType type,
                     NvBool internal,
                     char dpAddress[NVKMS_DP_ADDRESS_STRING_LENGTH])
{
    struct nv_drm_device *nv_dev = to_nv_device(dev);
    struct nv_drm_connector *nv_connector = NULL;
    struct nv_drm_connector_state *nv_connector_state = NULL;
    int ret = -ENOMEM;

    if ((nv_connector = nv_drm_calloc(1, sizeof(*nv_connector))) == NULL) {
        goto failed;
    }

    if ((nv_connector_state =
            nv_drm_calloc(1, sizeof(*nv_connector_state))) == NULL) {
        goto failed_state_alloc;
    }

    nv_connector->base.state = &nv_connector_state->base;
    nv_connector->base.state->connector = &nv_connector->base;

    nv_connector->physicalIndex = physicalIndex;
    nv_connector->type     = type;
    nv_connector->internal = internal;
    nv_connector->modeset_permission_filep = NULL;
    nv_connector->modeset_permission_crtc = NULL;
    nv_connector->cp = NVKMS_CONTENT_PROTECTION_OFF;

    strcpy(nv_connector->dpAddress, dpAddress);

    ret = drm_connector_init(
        dev,
        &nv_connector->base, &nv_connector_funcs,
        nvkms_connector_type_to_drm_connector_type(type, internal));

    if (ret != 0) {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to initialize connector created from physical index %u",
            nv_connector->physicalIndex);
        goto failed_connector_init;
    }

    drm_connector_helper_add(&nv_connector->base, &nv_connector_helper_funcs);

    /* HDMI 1.4 3D support (community patch, sony-bravia-linux).
     *
     * What: stop the DRM core pruning the stereo modes the sink itself
     * advertises in its EDID. The core keeps DRM_MODE_FLAG_3D modes only
     * when connector->stereo_allowed is set (drm_probe_helper.c) and
     * still exposes them solely to clients that opt in with
     * DRM_CLIENT_CAP_STEREO_3D (drm_connector.c) -- so nothing changes
     * for 2D-only userspace.
     *
     * Why this is safe to apply: top-and-bottom and side-by-side (half)
     * ride the SAME link timing as the plain 2D mode (HDMI 1.4), so
     * nv_drm_connector_mode_valid (NVKMS validateDisplayMode) already
     * accepts them. The 3D announcement is carried with the modeset by
     * the existing, shipped NV_HDMI_VSIF_METADATA blob property.
     *
     * Proven on 615.71.09: SBS-half 1080p60 and TaB 1080p24 drive a 2012
     * Sony KDL-46HX855 into 3D with picture (runs + client:
     * github.com/danielcamposramos/sony-bravia-linux tools/stereo-modeset/).
     *
     * Frame packing is sent as the whole frame-packed frame (both eyes and
     * the active space, at twice the pixel clock), which the client draws
     * into one buffer, as DRM defines it (drm_mode_to_nvkms_display_mode).
     */
    if (nv_connector->type == NVKMS_CONNECTOR_TYPE_HDMI) {
        nv_connector->base.stereo_allowed = true;
    }

    nv_connector->base.polled = DRM_CONNECTOR_POLL_HPD;

    if (nv_connector->type == NVKMS_CONNECTOR_TYPE_VGA) {
        nv_connector->base.polled =
            DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;
    }

#ifdef NV_DRM_SUPPORT_CONTENT_PROTECTION_PROPERTY
    /* attach content protection properties */
    if ((nv_connector->type == NVKMS_CONNECTOR_TYPE_DP) ||
        (nv_connector->type == NVKMS_CONNECTOR_TYPE_HDMI)) {
        ret = drm_connector_attach_content_protection_property(&nv_connector->base, true);
        if (ret != 0) {
            NV_DRM_DEV_LOG_ERR(
                nv_dev,
                "Failed to attach content protection properties to connector created from physical index %u",
                nv_connector->physicalIndex);
            goto failed_connector_init;
        }
        /* attach nvidia defined connector properties */
        drm_object_attach_property(&nv_connector->base.base,
                                   nv_dev->nv_hdcp_topology_property,
                                   0);
        drm_object_attach_property(&nv_connector->base.base,
                                   nv_dev->nv_hdcp_level_property,
                                   0);
    }
#endif


#if defined(NV_DRM_CONNECTOR_ATTACH_HDR_OUTPUT_METADATA_PROPERTY_PRESENT)
    if (nv_connector->type == NVKMS_CONNECTOR_TYPE_HDMI) {
#if defined(NV_DRM_MODE_CREATE_DP_COLORSPACE_PROPERTY_HAS_SUPPORTED_COLORSPACES_ARG)
        if (drm_mode_create_hdmi_colorspace_property(
                &nv_connector->base,
                __nv_drm_connector_supported_colorspaces) == 0) {
#else
        if (drm_mode_create_hdmi_colorspace_property(&nv_connector->base) == 0) {
#endif
            drm_connector_attach_colorspace_property(&nv_connector->base);
        }
        drm_connector_attach_hdr_output_metadata_property(&nv_connector->base);

        if (nv_dev->nv_connector_hdmi_vsif_metadata_property) {
            drm_object_attach_property(&nv_connector->base.base,
                                       nv_dev->nv_connector_hdmi_vsif_metadata_property,
                                       0);
        }
    } else if (nv_connector->type == NVKMS_CONNECTOR_TYPE_DP) {
#if defined(NV_DRM_MODE_CREATE_DP_COLORSPACE_PROPERTY_HAS_SUPPORTED_COLORSPACES_ARG)
        if (drm_mode_create_dp_colorspace_property(
                &nv_connector->base,
                __nv_drm_connector_supported_colorspaces) == 0) {
#else
        if (drm_mode_create_dp_colorspace_property(&nv_connector->base) == 0) {
#endif
            drm_connector_attach_colorspace_property(&nv_connector->base);
        }
        drm_connector_attach_hdr_output_metadata_property(&nv_connector->base);
    }
#endif /* defined(NV_DRM_CONNECTOR_ATTACH_HDR_OUTPUT_METADATA_PROPERTY_PRESENT) */

    /* Attach dithering mode property */
    if (nv_dev->nv_connector_dithering_mode_property) {
        drm_object_attach_property(&nv_connector->base.base,
                                   nv_dev->nv_connector_dithering_mode_property,
                                   NV_DRM_DITHERING_MODE_AUTO);
    }

    /* Register connector with DRM subsystem */

    ret = drm_connector_register(&nv_connector->base);

    if (ret != 0) {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to register connector created from physical index %u",
            nv_connector->physicalIndex);
        goto failed_connector_register;
    }

    return &nv_connector->base;

failed_connector_register:
    drm_connector_cleanup(&nv_connector->base);

failed_connector_init:
    nv_drm_free(nv_connector->base.state);

failed_state_alloc:
    nv_drm_free(nv_connector);

failed:
    return ERR_PTR(ret);
}

/*
 * Get connector with given physical index one exists. Otherwise, create and
 * return a new connector.
 */
struct drm_connector*
nv_drm_get_connector(struct drm_device *dev,
                     NvU32 physicalIndex, NvKmsConnectorType type,
                     NvBool internal,
                     char dpAddress[NVKMS_DP_ADDRESS_STRING_LENGTH])
{
    struct drm_connector *connector = NULL;
    struct drm_connector_list_iter conn_iter;
    drm_connector_list_iter_begin(dev, &conn_iter);

    /* Lookup for existing connector with same physical index */
    drm_for_each_connector_iter(connector, &conn_iter) {
        struct nv_drm_connector *nv_connector = to_nv_connector(connector);

        if (nv_connector->physicalIndex == physicalIndex) {
            BUG_ON(nv_connector->type != type ||
                   nv_connector->internal != internal);

            if (strcmp(nv_connector->dpAddress, dpAddress) == 0) {
                goto done;
            }
        }
    }
    connector = NULL;

done:
    drm_connector_list_iter_end(&conn_iter);

    if (!connector) {
        connector = nv_drm_connector_new(dev,
                                         physicalIndex, type, internal,
                                         dpAddress);
    }

    return connector;
}

/*
 * Revoke the permissions on this connector.
 */
bool nv_drm_connector_revoke_permissions(struct drm_device *dev,
                                         struct nv_drm_connector* nv_connector)
{
    struct nv_drm_device *nv_dev = to_nv_device(dev);
    bool ret = true;

    if (nv_connector->modeset_permission_crtc) {
        if (nv_connector->nv_detected_encoder) {
            ret = nvKms->revokePermissions(
                nv_dev->pDevice, nv_connector->modeset_permission_crtc->head,
                nv_connector->nv_detected_encoder->hDisplay);
        }
        nv_connector->modeset_permission_crtc->modeset_permission_filep = NULL;
        nv_connector->modeset_permission_crtc = NULL;
    }
    nv_connector->modeset_permission_filep = NULL;
    return ret;
}

void nv_drm_connector_update_content_protection(struct nv_drm_connector *nv_connector)
{
#ifdef NV_DRM_SUPPORT_CONTENT_PROTECTION_PROPERTY
    struct drm_connector *connector = &nv_connector->base;
    struct drm_connector_state *state = connector->state;
    unsigned int content_protection = state->content_protection;
    unsigned int hdcp_content_type = state->hdcp_content_type;
    bool update_cp = false;
    unsigned int cp_val;

    if ((content_protection == DRM_MODE_CONTENT_PROTECTION_DESIRED) &&
        (hdcp_content_type == DRM_MODE_HDCP_CONTENT_TYPE0)) {
        if ((nv_connector->cp == NVKMS_CONTENT_PROTECTION_HDCP1X_ON) ||
            (nv_connector->cp == NVKMS_CONTENT_PROTECTION_HDCP2X_TYPE0_ON) ||
            (nv_connector->cp == NVKMS_CONTENT_PROTECTION_HDCP2X_TYPE1_ON)) {
            cp_val = DRM_MODE_CONTENT_PROTECTION_ENABLED;
            update_cp = true;
        }
    } else if ((content_protection == DRM_MODE_CONTENT_PROTECTION_DESIRED) &&
             (hdcp_content_type == DRM_MODE_HDCP_CONTENT_TYPE1)) {
        if (nv_connector->cp == NVKMS_CONTENT_PROTECTION_HDCP2X_TYPE1_ON) {
            cp_val = DRM_MODE_CONTENT_PROTECTION_ENABLED;
            update_cp = true;
        }
    } else if (content_protection == DRM_MODE_CONTENT_PROTECTION_ENABLED) {
        if ((nv_connector->cp == NVKMS_CONTENT_PROTECTION_OFF) || 
            (nv_connector->cp == NVKMS_CONTENT_PROTECTION_FAILED)) {
            cp_val = DRM_MODE_CONTENT_PROTECTION_DESIRED;
            update_cp = true;
        }
    }

    if (update_cp) {
        drm_hdcp_update_content_protection(connector, cp_val);
    }
#endif
}

int nv_drm_connector_update_topology_property(struct nv_drm_connector *nv_connector,
                                              const void *topology)
{
#ifdef NV_DRM_SUPPORT_CONTENT_PROTECTION_PROPERTY
    struct drm_connector *connector = &nv_connector->base;
    struct drm_connector_state *state = connector->state;
    struct nv_drm_connector_state *nv_state = to_nv_drm_connector_state(state);
    struct drm_device *dev = connector->dev;
    struct nv_drm_device *nv_dev = to_nv_device(dev);
    int ret;

    ret = drm_property_replace_global_blob(dev,
                                            &nv_state->hdcp_topology_blob,
                                            NVKMS_HDCP_TOPOLOGY_SIZE,
                                            topology,
                                            &connector->base,
                                            nv_dev->nv_hdcp_topology_property);
    // Generate uevent on cp property when topology is updated
#if defined(NV_DRM_SYSFS_CONNECTOR_PROPERTY_EVENT_PRESENT)
    drm_sysfs_connector_property_event(connector,
        dev->mode_config.content_protection_property);
#elif defined(NV_DRM_SYSFS_CONNECTOR_STATUS_EVENT_PRESENT)
    drm_sysfs_connector_status_event(connector,
        dev->mode_config.content_protection_property);
#endif

    return ret;
#else
    return 0;
#endif
}

#endif
