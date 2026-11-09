#ifndef MP_DOVI_MEL_H
#define MP_DOVI_MEL_H

#include <stdbool.h>
#include <stdint.h>
#include <libavutil/dovi_meta.h>

// Recognize the canonical zero-residual MEL mapping, not merely P7 or 12-bit
// signalling. Unknown/unsupported mappings must retain the original EL path.
static inline bool mp_dovi_is_mel(const AVDOVIMetadata *metadata)
{
    if (!metadata)
        return false;
    const AVDOVIRpuDataHeader *header = av_dovi_get_header(metadata);
    const AVDOVIDataMapping *mapping = av_dovi_get_mapping(metadata);
    if (header->disable_residual_flag ||
        mapping->nlq_method_idc != AV_DOVI_NLQ_LINEAR_DZ ||
        header->coef_log2_denom >= 63)
        return false;
    for (int c = 0; c < 3; c++) {
        const AVDOVINLQParams *nlq = &mapping->nlq[c];
        if (nlq->nlq_offset || nlq->linear_deadzone_slope ||
            nlq->linear_deadzone_threshold ||
            nlq->vdr_in_max != (UINT64_C(1) << header->coef_log2_denom))
            return false;
    }
    return true;
}

#endif
