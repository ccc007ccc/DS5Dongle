cmake_minimum_required(VERSION 3.15)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(m61_opus_1_2_1 C)

if(NOT DEFINED OPUS_SOURCE_DIR OR NOT EXISTS "${OPUS_SOURCE_DIR}/include/opus.h")
    message(FATAL_ERROR "OPUS_SOURCE_DIR must point to the patched Opus 1.2.1 source tree")
endif()

set(OPUS_SOURCES
    "${OPUS_SOURCE_DIR}/celt/bands.c"
    "${OPUS_SOURCE_DIR}/celt/celt.c"
    "${OPUS_SOURCE_DIR}/celt/celt_encoder.c"
    "${OPUS_SOURCE_DIR}/celt/celt_decoder.c"
    "${OPUS_SOURCE_DIR}/celt/cwrs.c"
    "${OPUS_SOURCE_DIR}/celt/entcode.c"
    "${OPUS_SOURCE_DIR}/celt/entdec.c"
    "${OPUS_SOURCE_DIR}/celt/entenc.c"
    "${OPUS_SOURCE_DIR}/celt/kiss_fft.c"
    "${OPUS_SOURCE_DIR}/celt/laplace.c"
    "${OPUS_SOURCE_DIR}/celt/mathops.c"
    "${OPUS_SOURCE_DIR}/celt/mdct.c"
    "${OPUS_SOURCE_DIR}/celt/modes.c"
    "${OPUS_SOURCE_DIR}/celt/pitch.c"
    "${OPUS_SOURCE_DIR}/celt/celt_lpc.c"
    "${OPUS_SOURCE_DIR}/celt/quant_bands.c"
    "${OPUS_SOURCE_DIR}/celt/rate.c"
    "${OPUS_SOURCE_DIR}/celt/vq.c"
    "${OPUS_SOURCE_DIR}/silk/CNG.c"
    "${OPUS_SOURCE_DIR}/silk/code_signs.c"
    "${OPUS_SOURCE_DIR}/silk/init_decoder.c"
    "${OPUS_SOURCE_DIR}/silk/decode_core.c"
    "${OPUS_SOURCE_DIR}/silk/decode_frame.c"
    "${OPUS_SOURCE_DIR}/silk/decode_parameters.c"
    "${OPUS_SOURCE_DIR}/silk/decode_indices.c"
    "${OPUS_SOURCE_DIR}/silk/decode_pulses.c"
    "${OPUS_SOURCE_DIR}/silk/decoder_set_fs.c"
    "${OPUS_SOURCE_DIR}/silk/dec_API.c"
    "${OPUS_SOURCE_DIR}/silk/enc_API.c"
    "${OPUS_SOURCE_DIR}/silk/encode_indices.c"
    "${OPUS_SOURCE_DIR}/silk/encode_pulses.c"
    "${OPUS_SOURCE_DIR}/silk/gain_quant.c"
    "${OPUS_SOURCE_DIR}/silk/interpolate.c"
    "${OPUS_SOURCE_DIR}/silk/LP_variable_cutoff.c"
    "${OPUS_SOURCE_DIR}/silk/NLSF_decode.c"
    "${OPUS_SOURCE_DIR}/silk/NSQ.c"
    "${OPUS_SOURCE_DIR}/silk/NSQ_del_dec.c"
    "${OPUS_SOURCE_DIR}/silk/PLC.c"
    "${OPUS_SOURCE_DIR}/silk/shell_coder.c"
    "${OPUS_SOURCE_DIR}/silk/tables_gain.c"
    "${OPUS_SOURCE_DIR}/silk/tables_LTP.c"
    "${OPUS_SOURCE_DIR}/silk/tables_NLSF_CB_NB_MB.c"
    "${OPUS_SOURCE_DIR}/silk/tables_NLSF_CB_WB.c"
    "${OPUS_SOURCE_DIR}/silk/tables_other.c"
    "${OPUS_SOURCE_DIR}/silk/tables_pitch_lag.c"
    "${OPUS_SOURCE_DIR}/silk/tables_pulses_per_block.c"
    "${OPUS_SOURCE_DIR}/silk/VAD.c"
    "${OPUS_SOURCE_DIR}/silk/control_audio_bandwidth.c"
    "${OPUS_SOURCE_DIR}/silk/quant_LTP_gains.c"
    "${OPUS_SOURCE_DIR}/silk/VQ_WMat_EC.c"
    "${OPUS_SOURCE_DIR}/silk/HP_variable_cutoff.c"
    "${OPUS_SOURCE_DIR}/silk/NLSF_encode.c"
    "${OPUS_SOURCE_DIR}/silk/NLSF_VQ.c"
    "${OPUS_SOURCE_DIR}/silk/NLSF_unpack.c"
    "${OPUS_SOURCE_DIR}/silk/NLSF_del_dec_quant.c"
    "${OPUS_SOURCE_DIR}/silk/process_NLSFs.c"
    "${OPUS_SOURCE_DIR}/silk/stereo_LR_to_MS.c"
    "${OPUS_SOURCE_DIR}/silk/stereo_MS_to_LR.c"
    "${OPUS_SOURCE_DIR}/silk/check_control_input.c"
    "${OPUS_SOURCE_DIR}/silk/control_SNR.c"
    "${OPUS_SOURCE_DIR}/silk/init_encoder.c"
    "${OPUS_SOURCE_DIR}/silk/control_codec.c"
    "${OPUS_SOURCE_DIR}/silk/A2NLSF.c"
    "${OPUS_SOURCE_DIR}/silk/ana_filt_bank_1.c"
    "${OPUS_SOURCE_DIR}/silk/biquad_alt.c"
    "${OPUS_SOURCE_DIR}/silk/bwexpander_32.c"
    "${OPUS_SOURCE_DIR}/silk/bwexpander.c"
    "${OPUS_SOURCE_DIR}/silk/debug.c"
    "${OPUS_SOURCE_DIR}/silk/decode_pitch.c"
    "${OPUS_SOURCE_DIR}/silk/inner_prod_aligned.c"
    "${OPUS_SOURCE_DIR}/silk/lin2log.c"
    "${OPUS_SOURCE_DIR}/silk/log2lin.c"
    "${OPUS_SOURCE_DIR}/silk/LPC_analysis_filter.c"
    "${OPUS_SOURCE_DIR}/silk/LPC_inv_pred_gain.c"
    "${OPUS_SOURCE_DIR}/silk/table_LSF_cos.c"
    "${OPUS_SOURCE_DIR}/silk/NLSF2A.c"
    "${OPUS_SOURCE_DIR}/silk/NLSF_stabilize.c"
    "${OPUS_SOURCE_DIR}/silk/NLSF_VQ_weights_laroia.c"
    "${OPUS_SOURCE_DIR}/silk/pitch_est_tables.c"
    "${OPUS_SOURCE_DIR}/silk/resampler.c"
    "${OPUS_SOURCE_DIR}/silk/resampler_down2_3.c"
    "${OPUS_SOURCE_DIR}/silk/resampler_down2.c"
    "${OPUS_SOURCE_DIR}/silk/resampler_private_AR2.c"
    "${OPUS_SOURCE_DIR}/silk/resampler_private_down_FIR.c"
    "${OPUS_SOURCE_DIR}/silk/resampler_private_IIR_FIR.c"
    "${OPUS_SOURCE_DIR}/silk/resampler_private_up2_HQ.c"
    "${OPUS_SOURCE_DIR}/silk/resampler_rom.c"
    "${OPUS_SOURCE_DIR}/silk/sigm_Q15.c"
    "${OPUS_SOURCE_DIR}/silk/sort.c"
    "${OPUS_SOURCE_DIR}/silk/sum_sqr_shift.c"
    "${OPUS_SOURCE_DIR}/silk/stereo_decode_pred.c"
    "${OPUS_SOURCE_DIR}/silk/stereo_encode_pred.c"
    "${OPUS_SOURCE_DIR}/silk/stereo_find_predictor.c"
    "${OPUS_SOURCE_DIR}/silk/stereo_quant_pred.c"
    "${OPUS_SOURCE_DIR}/silk/LPC_fit.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/LTP_analysis_filter_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/LTP_scale_ctrl_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/corrMatrix_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/encode_frame_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/find_LPC_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/find_LTP_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/find_pitch_lags_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/find_pred_coefs_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/noise_shape_analysis_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/process_gains_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/regularize_correlations_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/residual_energy16_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/residual_energy_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/warped_autocorrelation_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/apply_sine_window_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/autocorr_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/burg_modified_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/k2a_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/k2a_Q16_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/pitch_analysis_core_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/vector_ops_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/schur64_FIX.c"
    "${OPUS_SOURCE_DIR}/silk/fixed/schur_FIX.c"
    "${OPUS_SOURCE_DIR}/src/opus.c"
    "${OPUS_SOURCE_DIR}/src/opus_decoder.c"
    "${OPUS_SOURCE_DIR}/src/opus_encoder.c"
    "${OPUS_SOURCE_DIR}/src/opus_multistream.c"
    "${OPUS_SOURCE_DIR}/src/opus_multistream_encoder.c"
    "${OPUS_SOURCE_DIR}/src/opus_multistream_decoder.c"
    "${OPUS_SOURCE_DIR}/src/repacketizer.c"
    "${OPUS_SOURCE_DIR}/src/analysis.c"
    "${OPUS_SOURCE_DIR}/src/mlp.c"
    "${OPUS_SOURCE_DIR}/src/mlp_data.c"
)

add_library(opus STATIC ${OPUS_SOURCES})
set_target_properties(opus PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/.libs"
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
)

target_include_directories(opus PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${OPUS_SOURCE_DIR}"
    "${OPUS_SOURCE_DIR}/include"
    "${OPUS_SOURCE_DIR}/celt"
    "${OPUS_SOURCE_DIR}/silk"
    "${OPUS_SOURCE_DIR}/silk/float"
    "${OPUS_SOURCE_DIR}/silk/fixed"
)

target_compile_definitions(opus PRIVATE
    HAVE_CONFIG_H=1
    M61_OPUS_E907_CLZ32=1
    M61_OPUS_E907_EXACT_RCP=1
    M61_OPUS_E907_POW2_DIV=1
    M61_OPUS_E907_Q16_SMMWB=1
    M61_OPUS_E907_Q15_KMMWB2=1
    M61_OPUS_TCM_PVQ_CLUSTER=1
    M61_OPUS_TCM_MDCT_CLUSTER=1
)

target_compile_options(opus PRIVATE
    -O2
    -flto
    -g0
    -ffunction-sections
    -fdata-sections
    -fno-common
    -fstrict-volatile-bitfields
    -march=rv32imafcp_zpn_zpsfoperand_xtheade
    -mabi=ilp32f
    -mtune=e907
)
