// foundation/tomlutil.hpp — shared TOML helpers for config/providers.
//
// Faithful port of cliamp/internal/tomlutil (sections.go + unquote.go). This
// is a minimal [[<section>]] block reader used by the local and radio
// providers to parse `radios.toml` / `radio_favorites.toml` / `directories`
// lists without pulling in the full TOML grammar: it understands
// `[[name]]` headers and `key = "value"` lines, with values unquoted via
// unquote(). Blank lines, comments (#), and lines outside any recognized
// section are ignored; an empty section still emits. When a key repeats within
// a section, the last value wins. Unknown [[...]] headers flush the current
// section so their fields cannot leak into the next recognized one.
#pragma once

#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace bootamp::foundation {

// Field map for a parsed section: key -> unquoted value. Last key wins on
// repeat, matching cliamp's `fields[k] = v` overwrite.
using TomlFields = std::map<std::string, std::string>;

// unquote strips surrounding double quotes from a TOML string value and
// interprets Go-style escape sequences (\n \t \\ \" \xHH \uHHHH \UHHHHHHHH,
// plus the other Go byte escapes). If the value is not wrapped in double
// quotes it is returned unchanged. If unquoting fails (e.g. an invalid escape
// like `\z`), it falls back to stripping the first and last character — the
// same fallback cliamp's strconv.Unquote path uses. Port of tomlutil.Unquote.
std::string unquote(std::string_view s);

// parse_sections parses a minimal TOML document made up of repeated
// `[[section]]` blocks of `key = "value"` lines. For each section header it
// invokes `emit` once with the accumulated fields (values unquoted via
// unquote). Blank lines, comments (#), and lines outside any section are
// ignored. An empty section still triggers emit, so callers apply their own
// validation. When a key repeats within a section, the last value wins.
// Port of tomlutil.ParseSections.
void parse_sections(std::string_view data, std::string_view section,
                    const std::function<void(const TomlFields&)>& emit);

// parse_named_sections is like parse_sections but accepts several section
// names and passes the matched section name to emit. Sections may be
// interleaved; emit follows document order. Unknown [[...]] headers flush
// the current recognized section without emitting. Port of
// tomlutil.ParseNamedSections.
void parse_named_sections(
    std::string_view data, std::span<const std::string> sections,
    const std::function<void(std::string_view section, const TomlFields&)>& emit);

}  // namespace bootamp::foundation