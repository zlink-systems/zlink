#[allow(dead_code)]
#[path = "../src/perf_measurement.rs"]
mod measurement;

#[test]
fn diagnostics_use_c_significant_digits() {
    for (number, precision, expected) in [
        (90875.20000000001, 12, "90875.2"),
        (0.589274767, 6, "0.589275"),
        (0.000092, 6, "9.2e-05"),
        (1_234_567.0, 6, "1.23457e+06"),
        (0.0, 12, "0"),
        (-1.0, 12, "-1"),
    ] {
        assert_eq!(measurement::diagnostic_number(number, precision), expected);
    }
}
