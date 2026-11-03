"""Checks the unit of the HCRL Driving Dataset's Fuel_consumption column against engine physics.

  python tools/check_fuel_unit.py "data/driving_dataset/extracted/Driving Data(KIA SOUL)_(150728-160714)_(10 Drivers_A-J).csv"

Speed-density air mass: m_air = MAP * Vd * (rpm / 120) * VE / (R * T). Fuel mass = m_air / AFR, volume = mass / density.
If the column is in uL/s the printed ratio is near 1.
"""
import sys

import numpy as np
import pandas as pd

VD_M3, VE, R, T_K, AFR, RHO_G_PER_ML = 1.591e-3, 0.85, 287.0, 313.15, 14.7, 0.745

df = pd.read_csv(sys.argv[1])
d = df[(df.Engine_speed > 600) & (df.Intake_air_pressure > 15) & (df.Engine_in_fuel_cut_off == 0)]
air_gps = d.Intake_air_pressure * 1000 * VD_M3 * (d.Engine_speed / 120) * VE / (R * T_K) * 1000
fuel_uls = air_gps / AFR / RHO_G_PER_ML * 1000
ratio = d.Fuel_consumption / fuel_uls
print(f"firing rows {len(d)}: column / speed-density uL/s median {ratio.median():.3f} "
      f"(IQR {ratio.quantile(.25):.3f}-{ratio.quantile(.75):.3f}), r = {np.corrcoef(d.Fuel_consumption, fuel_uls)[0, 1]:.3f}")
idle = d[(d.Vehicle_speed == 0) & (d.Engine_speed < 900)]
print(f"idle median {idle.Fuel_consumption.median():.0f} -> {idle.Fuel_consumption.median() * 0.0036:.2f} L/h if uL/s")
print(f"fuel cut-off median {df[df.Engine_in_fuel_cut_off == 1].Fuel_consumption.median():.0f}")
