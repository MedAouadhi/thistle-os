// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — single-cell LiPo discharge curve
//
// A cell's resting voltage mapped to a charge percentage. This describes the
// battery, not the part measuring it, which is why it does not live inside any
// one power driver: every board in the catalogue carries a single-cell LiPo,
// and more than one driver has to fall back on this when no fuel gauge is
// present to answer properly.
//
// It is a fallback and not a good one. A LiPo's voltage barely moves across
// the middle of its range, so a reading there says little about the charge
// remaining. A driver with a fuel gauge should use the gauge.

struct LipoPoint {
    mv: u16,
    pct: u8,
}

/// Eleven breakpoints, sorted descending by voltage.
static LIPO_CURVE: [LipoPoint; 11] = [
    LipoPoint { mv: 4200, pct: 100 },
    LipoPoint { mv: 4060, pct: 90 },
    LipoPoint { mv: 3980, pct: 80 },
    LipoPoint { mv: 3920, pct: 70 },
    LipoPoint { mv: 3870, pct: 60 },
    LipoPoint { mv: 3820, pct: 50 },
    LipoPoint { mv: 3750, pct: 40 },
    LipoPoint { mv: 3700, pct: 30 },
    LipoPoint { mv: 3620, pct: 20 },
    LipoPoint { mv: 3500, pct: 10 },
    LipoPoint { mv: 3000, pct: 0 },
];

/// Map a battery voltage in millivolts to a charge percentage, interpolating
/// linearly between the breakpoints and clamping outside them.
pub fn voltage_to_percent(mv: u16) -> u8 {
    // Above the top breakpoint → 100 %
    if mv >= LIPO_CURVE[0].mv {
        return 100;
    }
    // Below the bottom breakpoint → 0 %
    if mv <= LIPO_CURVE[LIPO_CURVE.len() - 1].mv {
        return 0;
    }

    // Find the two surrounding breakpoints and interpolate linearly.
    for i in 0..(LIPO_CURVE.len() - 1) {
        let upper = &LIPO_CURVE[i];
        let lower = &LIPO_CURVE[i + 1];

        if mv <= upper.mv && mv >= lower.mv {
            let mv_range = (upper.mv - lower.mv) as u32;
            let pct_range = (upper.pct - lower.pct) as u32;
            let mv_above_lower = (mv - lower.mv) as u32;
            // Weighted interpolation, rounded to nearest integer.
            let pct = lower.pct + ((mv_above_lower * pct_range + mv_range / 2) / mv_range) as u8;
            return pct;
        }
    }

    0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_vtp_top_clamp() {
        // Any voltage at or above 4200 mV should return 100 %.
        assert_eq!(voltage_to_percent(4200), 100);
        assert_eq!(voltage_to_percent(4300), 100);
        assert_eq!(voltage_to_percent(u16::MAX), 100);
    }

    #[test]
    fn test_vtp_bottom_clamp() {
        // Any voltage at or below 3000 mV should return 0 %.
        assert_eq!(voltage_to_percent(3000), 0);
        assert_eq!(voltage_to_percent(2999), 0);
        assert_eq!(voltage_to_percent(0), 0);
    }

    #[test]
    fn test_vtp_exact_breakpoints() {
        // Exact breakpoint values must match their table entries.
        assert_eq!(voltage_to_percent(4200), 100);
        assert_eq!(voltage_to_percent(4060), 90);
        assert_eq!(voltage_to_percent(3980), 80);
        assert_eq!(voltage_to_percent(3920), 70);
        assert_eq!(voltage_to_percent(3870), 60);
        assert_eq!(voltage_to_percent(3820), 50);
        assert_eq!(voltage_to_percent(3750), 40);
        assert_eq!(voltage_to_percent(3700), 30);
        assert_eq!(voltage_to_percent(3620), 20);
        assert_eq!(voltage_to_percent(3500), 10);
        assert_eq!(voltage_to_percent(3000), 0);
    }

    #[test]
    fn test_vtp_midpoint_3750() {
        // 3750 mV is an exact breakpoint → 40 %
        assert_eq!(voltage_to_percent(3750), 40);
    }

    #[test]
    fn test_vtp_interpolation_between_4060_and_4200() {
        // Segment: 4060 mV (90 %) to 4200 mV (100 %) — range 140 mV / 10 pct.
        // At 4130 mV: mv_above_lower = 4130 - 4060 = 70
        //   pct = 90 + (70 * 10 + 70) / 140 = 90 + 770/140 = 90 + 5 = 95
        assert_eq!(voltage_to_percent(4130), 95);
    }

    #[test]
    fn test_vtp_interpolation_3820_to_3870() {
        // Segment: 3820 mV (50 %) to 3870 mV (60 %) — range 50 mV / 10 pct.
        // At 3845 mV: mv_above_lower = 25
        //   pct = 50 + (25 * 10 + 25) / 50 = 50 + 275/50 = 50 + 5 = 55
        assert_eq!(voltage_to_percent(3845), 55);
    }

    #[test]
    fn test_vtp_interpolation_3700_to_3750() {
        // Segment: 3700 mV (30 %) to 3750 mV (40 %) — range 50 mV / 10 pct.
        // At 3725 mV: mv_above_lower = 25
        //   pct = 30 + (25 * 10 + 25) / 50 = 30 + 275/50 = 30 + 5 = 35
        assert_eq!(voltage_to_percent(3725), 35);
    }

    #[test]
    fn test_vtp_interpolation_3500_to_3620() {
        // Segment: 3500 mV (10 %) to 3620 mV (20 %) — range 120 mV / 10 pct.
        // At 3560 mV: mv_above_lower = 60
        //   pct = 10 + (60 * 10 + 60) / 120 = 10 + 660/120 = 10 + 5 = 15
        assert_eq!(voltage_to_percent(3560), 15);
    }

    #[test]
    fn test_vtp_interpolation_3000_to_3500() {
        // Segment: 3000 mV (0 %) to 3500 mV (10 %) — range 500 mV / 10 pct.
        // At 3250 mV: mv_above_lower = 250
        //   pct = 0 + (250 * 10 + 250) / 500 = 2750/500 = 5
        assert_eq!(voltage_to_percent(3250), 5);
    }

    #[test]
    fn test_vtp_monotonic() {
        // percentage must be non-decreasing as voltage increases over [3000, 4200].
        let mut prev: u8 = 0;
        let mut mv: u16 = 3000;
        while mv <= 4200 {
            let pct = voltage_to_percent(mv);
            assert!(
                pct >= prev,
                "monotonicity violated at {} mV: {} < {}",
                mv,
                pct,
                prev
            );
            prev = pct;
            mv = mv.saturating_add(10);
        }
    }
}
