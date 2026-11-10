// Internals shared by the database/sql sources.
#pragma once

#include <gopp/database/sql.hpp>
#include <gopp/database/sql/driver.hpp>

#include <string>
#include <string_view>

namespace gopp::sql::detail {

// strconv.FormatFloat(f, 'g', -1, bits): the shortest representation, in
// %e form when the exponent is < -4 or >= 6, as fmt's %v prints floats.
std::string format_float_g(double f, int bits);
// strconv.ParseFloat: ErrSyntax or ErrRange as a strconv.NumError.
std::pair<double, error> parse_float(std::string_view s, int bits);
// time.Time formatted as RFC 3339 with nanoseconds (time.RFC3339Nano).
std::string format_rfc3339_nano(const time::Time& t);

// An error with a message that wraps another (fmt.Errorf with %w).
error wrapf(string msg, error inner);
// An error with the message (fmt.Errorf without %w).
error errorf(string msg);

// Converts and checks arguments for the driver (driverArgsConnLocked).
std::pair<std::vector<driver::NamedValue>, error> driver_args_conn_locked(driver::Conn* ci, driver_stmt* ds,
                                                                          std::span<const arg> args);

// Go's %q of a string.
string quote(std::string_view s);

}  // namespace gopp::sql::detail
