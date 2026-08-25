//! Pure time helpers: ISO-8601 UTC and IMF-fixdate.
//! Port of `SubmissionManager::isoUtc` / `parseHttpDateMs`.

const MS_PER_DAY: i64 = 86_400_000;

fn floor_div(a: i64, b: i64) -> i64 {
    let q = a / b;
    if a % b != 0 && (a < 0) != (b < 0) {
        q - 1
    } else {
        q
    }
}

/// Howard Hinnant's civil-date algorithms (public domain formulation).
fn days_from_civil(mut y: i64, m: u32, d: u32) -> i64 {
    y -= i64::from(m <= 2);
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = (y - era * 400) as u32;
    let month_term = if m > 2 { m as i32 - 3 } else { m as i32 + 9 };
    let doy = (153 * month_term as u32 + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146097 + i64::from(doe) - 719468
}

fn civil_from_days(mut z: i64) -> (i64, u32, u32) {
    z += 719468;
    let era = if z >= 0 { z } else { z - 146096 } / 146097;
    let doe = (z - era * 146097) as u32;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let mut y = i64::from(yoe) + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    y += i64::from(m <= 2);
    (y, m, d)
}

fn month_from_name(mon: &str) -> u32 {
    match mon {
        "Jan" => 1,
        "Feb" => 2,
        "Mar" => 3,
        "Apr" => 4,
        "May" => 5,
        "Jun" => 6,
        "Jul" => 7,
        "Aug" => 8,
        "Sep" => 9,
        "Oct" => 10,
        "Nov" => 11,
        "Dec" => 12,
        _ => 0,
    }
}

/// Format epoch milliseconds as `YYYY-MM-DDTHH:MM:SSZ` (seconds precision, matching C++).
pub fn iso_utc(epoch_ms: i64) -> String {
    let days = floor_div(epoch_ms, MS_PER_DAY);
    let mut rem = epoch_ms - days * MS_PER_DAY;
    let (y, mo, da) = civil_from_days(days);
    let h = (rem / 3_600_000) as i32;
    rem %= 3_600_000;
    let mi = (rem / 60_000) as i32;
    rem %= 60_000;
    let s = (rem / 1000) as i32;
    format!("{y:04}-{mo:02}-{da:02}T{h:02}:{mi:02}:{s:02}Z")
}

/// Parse IMF-fixdate: `"Sun, 06 Nov 1994 08:49:37 GMT"`.
pub fn parse_http_date_ms(date_header: &str) -> Option<i64> {
    let rest = match date_header.find(',') {
        Some(comma) => &date_header[comma + 1..],
        None => date_header,
    };
    let mut parts = rest.split_whitespace();
    let day: i32 = parts.next()?.parse().ok()?;
    let mon = parts.next()?;
    let year: i64 = parts.next()?.parse().ok()?;
    let hms = parts.next()?;
    let tz = parts.next()?;
    if tz != "GMT" && tz != "UTC" {
        return None;
    }
    let month = month_from_name(mon);
    if month == 0 || day < 1 || day > 31 || year < 1970 {
        return None;
    }
    let hms = hms.as_bytes();
    if hms.len() != 8 || hms[2] != b':' || hms[5] != b':' {
        return None;
    }
    for i in [0usize, 1, 3, 4, 6, 7] {
        if !hms[i].is_ascii_digit() {
            return None;
        }
    }
    let h = i32::from(hms[0] - b'0') * 10 + i32::from(hms[1] - b'0');
    let mi = i32::from(hms[3] - b'0') * 10 + i32::from(hms[4] - b'0');
    let s = i32::from(hms[6] - b'0') * 10 + i32::from(hms[7] - b'0');
    if h > 23 || mi > 59 || s > 60 {
        return None;
    }
    let days = days_from_civil(year, month, day as u32);
    Some(days * MS_PER_DAY + (i64::from(h) * 3600 + i64::from(mi) * 60 + i64::from(s)) * 1000)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn iso_utc_formats_epoch_ms_as_iso_8601_utc() {
        assert_eq!(iso_utc(0), "1970-01-01T00:00:00Z");
        assert_eq!(iso_utc(784_111_777_000), "1994-11-06T08:49:37Z");
        assert_eq!(iso_utc(1_767_225_600_000), "2026-01-01T00:00:00Z");
    }

    #[test]
    fn parse_http_date_ms_parses_imf_fixdate() {
        let ms = parse_http_date_ms("Sun, 06 Nov 1994 08:49:37 GMT");
        assert_eq!(ms, Some(784_111_777_000));
        assert!(parse_http_date_ms("not a date").is_none());
        assert!(parse_http_date_ms("Sun, 06 Nov 1994 08:49:37 PST").is_none());
    }

    #[test]
    fn days_from_civil_round_trips_civil_from_days() {
        let days = days_from_civil(1994, 11, 6);
        let (y, m, d) = civil_from_days(days);
        assert_eq!((y, m, d), (1994, 11, 6));
        let days = days_from_civil(2026, 1, 1);
        let (y, m, d) = civil_from_days(days);
        assert_eq!((y, m, d), (2026, 1, 1));
    }
}
