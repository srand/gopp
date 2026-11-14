// Internals shared by the database/sql sources.
#pragma once

#include <gopp/database/sql.hpp>
#include <gopp/database/sql/driver.hpp>

#include <string>
#include <string_view>

namespace gopp::sql::detail {

// strconv.FormatFloat(f, 'g', -1, bits): the shortest representation, in
// %e form when the exponent is < -4 or >= 6, as fmt's %v prints floats.
std::string formatFloatG(double f, int bits);
// strconv.ParseFloat: ErrSyntax or ErrRange as a strconv.NumError.
std::pair<double, error> parseFloat(std::string_view s, int bits);
// time.Time formatted as RFC 3339 with nanoseconds (time.RFC3339Nano).
std::string formatRfc3339Nano(const time::Time& t);

// An error with a message that wraps another (fmt.Errorf with %w).
error wrapf(string msg, error inner);
// An error with the message (fmt.Errorf without %w).
error errorf(string msg);

// Converts and checks arguments for the driver (driverArgsConnLocked).
std::pair<std::vector<driver::NamedValue>, error> driverArgsConnLocked(driver::Conn* ci, driverStmt* ds,
                                                                          std::span<const arg> args);

// Go's %q of a string.
string quote(std::string_view s);

}  // namespace gopp::sql::detail
