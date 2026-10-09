#ifndef SRC_BATTERY_SOC_H_
#define SRC_BATTERY_SOC_H_

/*
 * Battery State of Charge estimation for 12V Lead-Acid / AGM batteries.
 *
 * SoC is derived from resting open-circuit voltage using a piecewise linear
 * interpolation over a measured AGM discharge curve. Readings are only valid
 * when the battery is at rest (no charge or load current). Call-site code is
 * responsible for detecting the charging condition and clamping SoC to 100.
 */

/* Returns estimated SoC in the range [0, 100].
 * Clamps below 11.60 V to 0 and above 12.70 V to 100. */
int battery_soc_from_voltage(float v_batt);

#endif /* SRC_BATTERY_SOC_H_ */
