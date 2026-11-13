# time: locations, calendar, Format and Parse

## Intent

go++'s `time::Time` is an instant in Unix nanoseconds with a monotonic
reading. It has no time zones, no calendar fields, and no `Format` or
`Parse` with layouts. The SQLite driver (mattn's semantics, `_loc`,
`SQLiteTimestampFormats`) needs all three, and so will logging later.
Port them from Go 1.27.3 so that Go code using time ports line by line.

## Goals

- Go's zero time: `Time{}` is January 1, year 1, 00:00:00 UTC, and
  `IsZero` means exactly that, as in Go.
- Go's time zones: `Location`, `UTC`, `Local` (from `TZ`, else
  `/etc/localtime`), `LoadLocation` from the system zoneinfo
  (`ZONEINFO`, then `/usr/share/zoneinfo/`, `/usr/share/lib/zoneinfo/`,
  `/usr/lib/locale/TZ/`, `/etc/zoneinfo`), `LoadLocationFromTZData`,
  `FixedZone`, and POSIX TZ strings for times past a file's last
  transition.
- Go's calendar and formatting API, with Go's names: `Month`, `Weekday`
  (with `String`), `Date(...)`, `Year`/`Month`/`Day`/`Hour`/`Minute`/
  `Second`/`Nanosecond`/`Date()`/`Clock`/`YearDay`/`ISOWeek`/`Weekday`,
  `Location`/`Zone`/`ZoneBounds`/`IsDST`/`In`/`UTC`/`Local`, `AddDate`,
  `Truncate`/`Round`, `Compare`, `Format`/`AppendFormat`/`String`,
  `Parse`/`ParseInLocation`/`ParseError`, every layout constant
  (`Layout`, `ANSIC`, ... `RFC3339`, `RFC3339Nano`, `Kitchen`, `Stamp*`,
  `DateTime`, `DateOnly`, `TimeOnly`), `UnixMilli`/`UnixMicro` (both
  directions), and `MarshalText`/`UnmarshalText`/`MarshalJSON`/
  `UnmarshalJSON`.
- Go's tests: `format_test.go`, the calendar, zone and marshaling parts of
  `time_test.go`, and `zoneinfo_test.go`, run against the system tzdata,
  with Go's names.

## Non-goals

- The embedded tz database (`time/tzdata`, `$GOROOT/lib/time/zoneinfo.zip`),
  and Windows zones.
- `MarshalBinary`/`GobEncode` and the other encodings.
- Changes to timers, tickers or the monotonic clock.

## Anti-goals

- Making `Time` costly to copy or compare. It stays a small value type,
  with no reference counting and no heap allocation per value.
- Parking a fiber in a static initializer (commit 6a13def): zoneinfo is read
  with plain system calls, as Go's `zoneinfo_read.go` does.

## Approaches

- **Go's model, internal seconds since year 1 (chosen).** `Time` holds
  seconds since January 1, year 1, UTC (int64), nanoseconds (int32), the
  monotonic reading (int64, 0 if none) and a `const Location*` (nullptr is
  UTC, as Go's nil `loc` is). It covers Go's whole range, including the zero
  time, and Go's algorithms port as they are.
- *Keep Unix nanoseconds and add a location.* That is ±292 years, so it
  can't hold year 1 or Go's zero time, and `IsZero` would stay wrong.
  Rejected.
- *Go's packed wall/ext encoding.* It saves 8 bytes, at the cost of bit
  tricks Go needs only for its GC'd 24-byte layout. Rejected.

## Decided

- Locations live as long as the program and are shared. `UTC` and `Local`
  are built in. `LoadLocation` keeps one `Location` per name and
  `FixedZone` one per (name, offset), both interned in tables under a lock
  that never parks. `Time` holds a raw `const Location*`. `Local` is loaded
  on first use.
- `Time{}` changes meaning from 1970 to year 1. Code that relied on the old
  meaning gets fixed: `net/http`'s `is_zero_time` already checks both, as
  Go's does, and `database/sql`'s conversions switch to the new `Format`.
- `fmt` formats a `Time` with `String()`, as Go's `%v` does.

## Left to the builder

- How the file split goes (format, zoneinfo, calendar), following
  `src/time/`.
- Which `time_test.go` tests are left out because go++ lacks what they
  test (gob, reflection, allocation counts, `testing/quick`), each listed
  at the top of the test file as the other ported tests do.
