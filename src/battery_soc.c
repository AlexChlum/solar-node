#include "battery_soc.h"
#include <stddef.h>

/*
 * AGM / Lead-Acid 12 V discharge curve (resting open-circuit voltage).
 * Points are ordered highest to lowest voltage.
 * Linear interpolation is used between points; values outside the range
 * are clamped to 100 % (above) or 0 % (below).
 */
struct soc_point {
	float voltage;
	int   soc_pct;
};

static const struct soc_point agm_curve[] = {
	{ 12.70f, 100 },
	{ 12.50f,  90 },
	{ 12.40f,  80 },
	{ 12.20f,  70 },
	{ 12.00f,  50 },
	{ 11.90f,  40 },
	{ 11.80f,  30 },
	{ 11.70f,  20 },
	{ 11.60f,  10 },
};

#define CURVE_LEN (sizeof(agm_curve) / sizeof(agm_curve[0]))

int battery_soc_from_voltage(float v_batt)
{
	if (v_batt >= agm_curve[0].voltage) {
		return 100;
	}
	if (v_batt <= agm_curve[CURVE_LEN - 1].voltage) {
		return 0;
	}

	for (size_t i = 0; i < CURVE_LEN - 1; i++) {
		float vhi = agm_curve[i].voltage;
		float vlo = agm_curve[i + 1].voltage;

		if (v_batt <= vhi && v_batt >= vlo) {
			float frac = (v_batt - vlo) / (vhi - vlo);
			int phi = agm_curve[i].soc_pct;
			int plo = agm_curve[i + 1].soc_pct;
			return (int)(plo + frac * (float)(phi - plo) + 0.5f);
		}
	}

	return 0;
}
