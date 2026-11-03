"""Canonical channel vocabulary shared by every dataset.

Column names from different sources are mapped to one canonical name so the channel embedding is
shared: `Engine_speed` (HCRL), `rpm` (CarMind) and `engine_rpm` all become `rpm`. Unknown
channels get a stable hashed id so they still train, just without cross-dataset sharing.
"""
import re
import zlib

CANONICAL = [
    "rpm", "speed_kph", "engine_load_pct", "coolant_c", "intake_temp_c", "maf_gps", "throttle_pct",
    "fuel_level_pct", "module_voltage_v", "oil_temp_c", "map_kpa", "ambient_c", "stft_b1_pct",
    "ltft_b1_pct", "stft_b2_pct", "ltft_b2_pct", "timing_adv_deg", "baro_kpa", "fuel_rail_kpa",
    "fuel_rate_lph", "run_time_s", "pedal_d_pct", "abs_load_pct", "equiv_ratio_cmd", "rel_throttle_pct",
    "torque_actual_pct", "driver_torque_pct", "ref_torque_nm", "odometer_km", "fuel_pressure_kpa",
    "steering_angle_deg", "steering_speed_dps", "accel_long_g", "accel_lat_g", "gear", "brake_pct",
    "wheel_speed_fl", "wheel_speed_fr", "wheel_speed_rl", "wheel_speed_rr", "yaw_rate_dps",
    "can_total_rate", "can_payload_entropy", "can_unique_ids",
    "torque_corrected_pct", "flywheel_torque_nm", "road_gradient_pct", "torque_converter_rpm", "fuel_cutoff",
    "pedal_e_pct", "abs_throttle_b_pct", "engine_soak_time", "fuel_cutoff_inhibit", "friction_torque_pct", "flywheel_torque_after_nm",
    "idle_target_rpm", "torque_min_pct", "torque_max_pct", "torque_scale", "torque_ratio", "spark_retard_deg", "tcu_torque_limit",
    "tcu_rpm_increase", "lockup_target_rpm", "glow_plug", "ac_compressor", "turbine_rpm", "clutch_ack", "converter_clutch", "gear_selection",
    "brake_switch", "brake_pressure_bar",
]
# substrings (lower-case, underscores removed) -> canonical
ALIASES = {
    "enginespeed": "rpm", "enginerpm": "rpm",
    "vehiclespeed": "speed_kph", "speedkph": "speed_kph", 
    "calculatedloadvalue": "engine_load_pct", "engineload": "engine_load_pct",
    "enginecoolanttemperature": "coolant_c", 
    "intakeairtemperature": "intake_temp_c", "intaketemp": "intake_temp_c",
    "massairflow": "maf_gps", 
    "throttlepositionsignal": "throttle_pct", "throttleposition": "throttle_pct", 
    "fuellevel": "fuel_level_pct", "controlmodulevoltage": "module_voltage_v", "batteryvoltage": "module_voltage_v",
    "engineoiltemperature": "oil_temp_c", "oiltemp": "oil_temp_c",
    "intakeairpressure": "map_kpa", "manifoldabsolutepressure": "map_kpa", 
    "ambientairtemperature": "ambient_c", 
    "shorttermfueltrimbank1": "stft_b1_pct", "stftb1": "stft_b1_pct",
    "longtermfueltrimbank1": "ltft_b1_pct", "ltftb1": "ltft_b1_pct",
    "shorttermfueltrimbank2": "stft_b2_pct", "longtermfueltrimbank2": "ltft_b2_pct",
    "timingadvance": "timing_adv_deg", "ignitiontiming": "timing_adv_deg", "sparktiming": "timing_adv_deg",
    "barometricpressure": "baro_kpa", 
    "fuelrailpressure": "fuel_rail_kpa", "fuelpressure": "fuel_pressure_kpa", "fuelconsumption": "fuel_rate_lph",
    "fuelrate": "fuel_rate_lph", "runtime": "run_time_s",
    "acceleratorpedalvalue": "pedal_d_pct", "acceleratorpedal": "pedal_d_pct", 
    "absoluteload": "abs_load_pct", "equivalenceratio": "equiv_ratio_cmd",
    "enginetorque": "torque_actual_pct", "torqueoffriction": "driver_torque_pct",
    "steeringwheelangle": "steering_angle_deg", "steeringangle": "steering_angle_deg",
    "steeringwheelspeed": "steering_speed_dps", "longitudinalacceleration": "accel_long_g",
    "accelerationspeedlongitudinal": "accel_long_g", "lateralacceleration": "accel_lat_g", "accelerationspeedlateral": "accel_lat_g",
    "enginetorqueaftercorrection": "torque_corrected_pct", "flywheeltorque": "flywheel_torque_nm", "mastercylinderpressure": "brake_pct",
    "roadgradient": "road_gradient_pct", "torqueconverterspeed": "torque_converter_rpm", "fuelcutoff": "fuel_cutoff", "currentgear": "gear", 
    "wheelvelocityfrontleft": "wheel_speed_fl", "wheelvelocityfrontright": "wheel_speed_fr",
    "wheelvelocityrearleft": "wheel_speed_rl", "wheelvelocityrearright": "wheel_speed_rr", "yawrate": "yaw_rate_dps",
}

# Exact matches for the HCRL Driving Dataset (KIA Soul) header, normalised with normalize_name().
EXACT = {
    "fuelconsumption": "fuel_rate_lph", "acceleratorpedalvalue": "pedal_d_pct", "throttlepositionsignal": "throttle_pct",
    "shorttermfueltrimbank1": "stft_b1_pct", "intakeairpressure": "map_kpa", "filteredacceleratorpedalvalue": "pedal_e_pct",
    "absolutethrottleposition": "abs_throttle_b_pct", "enginesoackingtime": "engine_soak_time", "inhibitionofenginefuelcutoff": "fuel_cutoff_inhibit",
    "engineinfuelcutoff": "fuel_cutoff", "fuelpressure": "fuel_pressure_kpa", "longtermfueltrimbank1": "ltft_b1_pct", "enginespeed": "rpm",
    "enginetorqueaftercorrection": "torque_corrected_pct", "torqueoffriction": "friction_torque_pct",
    "flywheeltorqueaftertorqueinterventions": "flywheel_torque_after_nm", "currentsparktiming": "timing_adv_deg",
    "enginecoolanttemperature": "coolant_c", "enginecoolanttemperature1": "coolant_c", "engineideltargetspeed": "idle_target_rpm",
    "enginetorque": "torque_actual_pct", "calculatedloadvalue": "engine_load_pct", "minimumindicatedenginetorque": "torque_min_pct",
    "maximumindicatedenginetorque": "torque_max_pct", "flywheeltorque": "flywheel_torque_nm", "torquescalingfactorstandardization": "torque_scale",
    "standardtorqueratio": "torque_ratio", "requestedsparkretardanglefromtcu": "spark_retard_deg", "tcurequestsenginetorquelimitetl": "tcu_torque_limit",
    "tcurequestedenginerpmincrease": "tcu_rpm_increase", "targetenginespeedusedinlockupmodule": "lockup_target_rpm",
    "glowplugcontrolrequest": "glow_plug", "activationofaircompressor": "ac_compressor", "torqueconverterspeed": "torque_converter_rpm",
    "currentgear": "gear", "wheelvelocityfrontlefthand": "wheel_speed_fl", "wheelvelocityrearrighthand": "wheel_speed_rr",
    "wheelvelocityfrontrighthand": "wheel_speed_fr", "wheelvelocityrearlefthand": "wheel_speed_rl",
    "torqueconverterturbinespeedunfiltered": "turbine_rpm", "clutchoperationacknowledge": "clutch_ack", "converterclutch": "converter_clutch",
    "gearselection": "gear_selection", "vehiclespeed": "speed_kph", "accelerationspeedlongitudinal": "accel_long_g",
    "indicationofbrakeswitchonoff": "brake_switch", "mastercylinderpressure": "brake_pressure_bar", "calculatedroadgradient": "road_gradient_pct",
    "accelerationspeedlateral": "accel_lat_g", "steeringwheelspeed": "steering_speed_dps", "steeringwheelangle": "steering_angle_deg",
}

# Unit conversions applied when the HCRL Driving Dataset is loaded, keyed by normalize_name(source column).
# Fuel_consumption is in microlitres per second (equivalently mm^3/s); x 0.0036 gives L/h. The release does not state
# the unit. Evidence (tools/check_fuel_unit.py): against a speed-density estimate (MAP, rpm, 1.591 L, VE 0.85, 40 C,
# AFR 14.7, 0.745 g/mL) the median ratio is 0.83 with r = 0.93 over 64,517 firing rows; idle reads 320 (1.15 L/h);
# fuel cut-off rows read 0. mL/h would put the ratio at 0.23.
HCRL_UNIT_SCALE = {"fuelconsumption": 0.0036}

VOCAB_SIZE = 4096          # canonical ids first, hashed ids after
CANONICAL_INDEX = {name: i for i, name in enumerate(CANONICAL)}


def normalize_name(col: str) -> str:
    return re.sub(r"[^a-z0-9]", "", col.lower())


def canonical_name(col: str) -> str:
    key = normalize_name(col)
    if col in CANONICAL_INDEX:
        return col
    if key in EXACT:
        return EXACT[key]
    # longest alias first so "shorttermfueltrimbank1" beats "fuel"
    for alias in sorted(ALIASES, key=len, reverse=True):
        if alias in key:
            return ALIASES[alias]
    return col


def channel_id(col: str) -> int:
    name = canonical_name(col)
    if name in CANONICAL_INDEX:
        return CANONICAL_INDEX[name]
    h = zlib.crc32(normalize_name(name).encode()) % (VOCAB_SIZE - len(CANONICAL))
    return len(CANONICAL) + h
