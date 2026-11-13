# log and log/slog

## Intent

Every service logs. The examples print their errors with `fmt` because go++
has neither `log` nor `log/slog`. Go's structured logger is the standard
one now, so Go code with `slog.Info("msg", "k", v)` should port line by
line.

## Goals

- `log`: `Logger` with `New`, `Default`, the flags (`Ldate`, `Ltime`,
  `Lmicroseconds`, `Llongfile`, `Lshortfile`, `LUTC`, `Lmsgprefix`,
  `LstdFlags`), `Print`, `Println`, `Fatal`, `Fatalln`, `Panic`, `Panicln`,
  `Output`, `SetOutput`, `SetFlags`, `SetPrefix`, `Writer`, and the package
  functions.
- `log/slog`: `Level`, `LevelVar` and `Leveler`; `Value` with every `Kind`
  (including `LogValuer`); `Attr` and its constructors; `Record` and
  `Source`; the `Handler` interface; `TextHandler`, `JSONHandler`,
  `DiscardHandler` and `MultiHandler` with `HandlerOptions` (`AddSource`,
  `Level`, `ReplaceAttr`); `Logger` with `With`, `WithGroup`, `Log`,
  `LogAttrs`, `Debug`, `Info`, `Warn`, `Error` and their `Context`
  variants; the package functions, `Default`, `SetDefault` and
  `SetLogLoggerLevel`. The default handler writes through `log`, and
  `SetDefault` sends `log` through the slog handler, as in Go.
- Go's tests, ported with their names.

## Non-goals

- `testing/slogtest`, and `slog`'s benchmarks.

## Decided

- Formatting: `log::Print(fmt, args...)` takes a `std::format` string, as
  `fmt::Print` does. `Print` and `Println` produce the same output, since
  `log` appends a missing newline anyway.
- The source location (`Lshortfile`, `AddSource`) comes from
  `std::source_location`. A wrapper captures it when the format string or
  message converts, so callers write nothing extra.
- Arguments: `logger.Info("msg", "k", v, slog::Int("n", 1))`. Keys and
  values are sorted out at compile time from the argument types, with Go's
  rules: a string is a key taking the next argument as its value, an `Attr`
  is itself, and anything else is a `!BADKEY` value.
- `slog::AnyValue(v)` picks the kind from the type, as Go's does. Any other
  type is held type-erased. Text output uses `MarshalText` if the type has
  it, then `std::format`, then a reflected `{Field:value ...}` rendering
  like `%+v`. JSON output uses `MarshalJSON`, then `json::Marshal`, then the
  text form. Errors become their `Error()` string.
- Writers are `ptr<io::Writer>`, as in `bufio` and `os/exec`. A `Context`
  argument is `const ptr<context::Context>&`.
- Float formatting reuses Go's `strconv.FormatFloat`, which go++ has for
  `text/template`. It becomes the public `strconv::FormatFloat`.
- The headers are `<gopp/log.hpp>` and `<gopp/log/slog.hpp>`, with namespaces
  `gopp::log` and `gopp::slog`.
