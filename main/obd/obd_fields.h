/* Every decoded SAE J1979 mode-01 field OpenCarMind collects.
 * One X-macro drives the enum, the decode table, the CSV header and the schema document.
 *   X(ident, csv_name, unit, pid, byte_offset, kind, scale, offset, fast)
 *   value = raw(kind at byte_offset) * scale + offset
 * Formulas follow SAE J1979 / ISO 15031-5 Annex B (as tabulated on the OBD-II PIDs reference). */
#pragma once

typedef enum { K_U8 = 0, K_U16, K_S16, K_U32 } obd_kind_t;

#define OBD_FIELDS(X) \
X(fuel_sys1,        "fuel_sys1_status",      "enum", 0x03, 0, K_U8,  1.0f,        0.0f,   0) \
X(fuel_sys2,        "fuel_sys2_status",      "enum", 0x03, 1, K_U8,  1.0f,        0.0f,   0) \
X(load,             "engine_load_pct",       "%",    0x04, 0, K_U8,  0.39215686f, 0.0f,   1) \
X(coolant,          "coolant_c",             "degC", 0x05, 0, K_U8,  1.0f,       -40.0f,  0) \
X(stft1,            "stft_b1_pct",           "%",    0x06, 0, K_U8,  0.78125f,   -100.0f, 0) \
X(ltft1,            "ltft_b1_pct",           "%",    0x07, 0, K_U8,  0.78125f,   -100.0f, 0) \
X(stft2,            "stft_b2_pct",           "%",    0x08, 0, K_U8,  0.78125f,   -100.0f, 0) \
X(ltft2,            "ltft_b2_pct",           "%",    0x09, 0, K_U8,  0.78125f,   -100.0f, 0) \
X(fuel_press,       "fuel_pressure_kpa",     "kPa",  0x0A, 0, K_U8,  3.0f,        0.0f,   0) \
X(map,              "map_kpa",               "kPa",  0x0B, 0, K_U8,  1.0f,        0.0f,   1) \
X(rpm,              "rpm",                   "rpm",  0x0C, 0, K_U16, 0.25f,       0.0f,   1) \
X(speed,            "speed_kph",             "km/h", 0x0D, 0, K_U8,  1.0f,        0.0f,   1) \
X(timing,           "timing_adv_deg",        "deg",  0x0E, 0, K_U8,  0.5f,       -64.0f,  1) \
X(iat,              "intake_temp_c",         "degC", 0x0F, 0, K_U8,  1.0f,       -40.0f,  0) \
X(maf,              "maf_gps",               "g/s",  0x10, 0, K_U16, 0.01f,       0.0f,   1) \
X(throttle,         "throttle_pct",          "%",    0x11, 0, K_U8,  0.39215686f, 0.0f,   1) \
X(sec_air,          "secondary_air_status",  "enum", 0x12, 0, K_U8,  1.0f,        0.0f,   0) \
X(o2_s1_v,          "o2_s1_v",               "V",    0x14, 0, K_U8,  0.005f,      0.0f,   0) \
X(o2_s2_v,          "o2_s2_v",               "V",    0x15, 0, K_U8,  0.005f,      0.0f,   0) \
X(o2_s3_v,          "o2_s3_v",               "V",    0x16, 0, K_U8,  0.005f,      0.0f,   0) \
X(o2_s4_v,          "o2_s4_v",               "V",    0x17, 0, K_U8,  0.005f,      0.0f,   0) \
X(o2_s5_v,          "o2_s5_v",               "V",    0x18, 0, K_U8,  0.005f,      0.0f,   0) \
X(o2_s6_v,          "o2_s6_v",               "V",    0x19, 0, K_U8,  0.005f,      0.0f,   0) \
X(o2_s7_v,          "o2_s7_v",               "V",    0x1A, 0, K_U8,  0.005f,      0.0f,   0) \
X(o2_s8_v,          "o2_s8_v",               "V",    0x1B, 0, K_U8,  0.005f,      0.0f,   0) \
X(run_time,         "run_time_s",            "s",    0x1F, 0, K_U16, 1.0f,        0.0f,   0) \
X(dist_mil,         "dist_mil_km",           "km",   0x21, 0, K_U16, 1.0f,        0.0f,   0) \
X(rail_rel,         "fuel_rail_rel_kpa",     "kPa",  0x22, 0, K_U16, 0.079f,      0.0f,   0) \
X(rail_gauge,       "fuel_rail_kpa",         "kPa",  0x23, 0, K_U16, 10.0f,       0.0f,   0) \
X(lambda_s1,        "lambda_s1",             "",     0x24, 0, K_U16, 3.0517578e-5f, 0.0f, 0) \
X(lambda_s2,        "lambda_s2",             "",     0x25, 0, K_U16, 3.0517578e-5f, 0.0f, 0) \
X(lambda_s3,        "lambda_s3",             "",     0x26, 0, K_U16, 3.0517578e-5f, 0.0f, 0) \
X(lambda_s4,        "lambda_s4",             "",     0x27, 0, K_U16, 3.0517578e-5f, 0.0f, 0) \
X(egr_cmd,          "egr_commanded_pct",     "%",    0x2C, 0, K_U8,  0.39215686f, 0.0f,   0) \
X(egr_err,          "egr_error_pct",         "%",    0x2D, 0, K_U8,  0.78125f,   -100.0f, 0) \
X(evap_purge,       "evap_purge_pct",        "%",    0x2E, 0, K_U8,  0.39215686f, 0.0f,   0) \
X(fuel_level,       "fuel_level_pct",        "%",    0x2F, 0, K_U8,  0.39215686f, 0.0f,   0) \
X(warmups,          "warmups_since_clear",   "",     0x30, 0, K_U8,  1.0f,        0.0f,   0) \
X(dist_clr,         "dist_since_clear_km",   "km",   0x31, 0, K_U16, 1.0f,        0.0f,   0) \
X(evap_vp,          "evap_vapor_pa",         "Pa",   0x32, 0, K_S16, 0.25f,       0.0f,   0) \
X(baro,             "baro_kpa",              "kPa",  0x33, 0, K_U8,  1.0f,        0.0f,   0) \
X(cat_b1s1,         "cat_temp_b1s1_c",       "degC", 0x3C, 0, K_U16, 0.1f,       -40.0f,  0) \
X(cat_b2s1,         "cat_temp_b2s1_c",       "degC", 0x3D, 0, K_U16, 0.1f,       -40.0f,  0) \
X(cat_b1s2,         "cat_temp_b1s2_c",       "degC", 0x3E, 0, K_U16, 0.1f,       -40.0f,  0) \
X(cat_b2s2,         "cat_temp_b2s2_c",       "degC", 0x3F, 0, K_U16, 0.1f,       -40.0f,  0) \
X(modv,             "module_voltage_v",      "V",    0x42, 0, K_U16, 0.001f,      0.0f,   0) \
X(abs_load,         "abs_load_pct",          "%",    0x43, 0, K_U16, 0.39215686f, 0.0f,   0) \
X(equiv,            "equiv_ratio_cmd",       "",     0x44, 0, K_U16, 3.0517578e-5f, 0.0f, 0) \
X(rel_throttle,     "rel_throttle_pct",      "%",    0x45, 0, K_U8,  0.39215686f, 0.0f,   0) \
X(ambient,          "ambient_c",             "degC", 0x46, 0, K_U8,  1.0f,       -40.0f,  0) \
X(throttle_b,       "abs_throttle_b_pct",    "%",    0x47, 0, K_U8,  0.39215686f, 0.0f,   0) \
X(throttle_c,       "abs_throttle_c_pct",    "%",    0x48, 0, K_U8,  0.39215686f, 0.0f,   0) \
X(pedal_d,          "pedal_d_pct",           "%",    0x49, 0, K_U8,  0.39215686f, 0.0f,   1) \
X(pedal_e,          "pedal_e_pct",           "%",    0x4A, 0, K_U8,  0.39215686f, 0.0f,   0) \
X(throttle_act,     "throttle_actuator_pct", "%",    0x4C, 0, K_U8,  0.39215686f, 0.0f,   0) \
X(time_mil,         "time_mil_min",          "min",  0x4D, 0, K_U16, 1.0f,        0.0f,   0) \
X(time_clr,         "time_since_clear_min",  "min",  0x4E, 0, K_U16, 1.0f,        0.0f,   0) \
X(fuel_type,        "fuel_type",             "enum", 0x51, 0, K_U8,  1.0f,        0.0f,   0) \
X(ethanol,          "ethanol_pct",           "%",    0x52, 0, K_U8,  0.39215686f, 0.0f,   0) \
X(evap_abs,         "evap_abs_kpa",          "kPa",  0x53, 0, K_U16, 0.005f,      0.0f,   0) \
X(rail_abs,         "fuel_rail_abs_kpa",     "kPa",  0x59, 0, K_U16, 10.0f,       0.0f,   0) \
X(pedal_rel,        "rel_pedal_pct",         "%",    0x5A, 0, K_U8,  0.39215686f, 0.0f,   0) \
X(hybrid_soc,       "hybrid_batt_pct",       "%",    0x5B, 0, K_U8,  0.39215686f, 0.0f,   0) \
X(oil,              "oil_temp_c",            "degC", 0x5C, 0, K_U8,  1.0f,       -40.0f,  0) \
X(inj_timing,       "fuel_inj_timing_deg",   "deg",  0x5D, 0, K_U16, 0.0078125f, -210.0f, 0) \
X(fuel_rate,        "fuel_rate_lph",         "L/h",  0x5E, 0, K_U16, 0.05f,       0.0f,   1) \
X(torque_demand,    "driver_torque_pct",     "%",    0x61, 0, K_U8,  1.0f,       -125.0f, 0) \
X(torque_actual,    "actual_torque_pct",     "%",    0x62, 0, K_U8,  1.0f,       -125.0f, 1) \
X(torque_ref,       "ref_torque_nm",         "Nm",   0x63, 0, K_U16, 1.0f,        0.0f,   0) \
X(odometer,         "odometer_km",           "km",   0xA6, 0, K_U32, 0.1f,        0.0f,   0)

#define OBD_ENUM(id, ...) F_##id,
typedef enum { OBD_FIELDS(OBD_ENUM) F_COUNT } obd_field_t;
#undef OBD_ENUM
