// ui/screens/jump.cpp — jump-to-time prompt screen implementation.
//
// Model is a port of cliamp ui/model: parseJumpTarget (jump.go, table-tested
// in jump_test.go), openJumpMode/closeJumpMode/handleJumpKey (keys.go).
#include "ui/screens/jump.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if BOOTAMP_HAS_FTXUI
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <memory>
#endif  // BOOTAMP_HAS_FTXUI

namespace bootamp::ui::screens {

namespace {

// Port of Go parseJumpTarget (ui/model/jump.go): "ss", "mm:ss", or
// "hh:mm:ss" with whitespace trimmed. Clock fields (minutes/seconds in the
// 2- and 3-part forms) must be at most two digits and ≤ 59; hours and the
// bare-seconds form accept any digit count. An empty part normalizes to "0"
// (Go normalizeClockField), so ":49", "58:", "1::03", and "1:02:" are valid.
// Returns the total seconds on success, nullopt on failure.
std::optional<double> parse_jump_target(std::string_view raw) {
  // Go: s := strings.TrimSpace(raw); s == "" → error.
  const std::string::size_type first = raw.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return std::nullopt;
  }
  raw.remove_prefix(first);
  const std::string::size_type last = raw.find_last_not_of(" \t\r\n");
  raw.remove_suffix(raw.size() - (last + 1));

  // Split on ':' (Go strings.Split). More than three parts → error.
  std::vector<std::string_view> parts;
  std::string_view::size_type   pos = 0;
  while (true) {
    const std::string_view::size_type colon = raw.find(':', pos);
    if (colon == std::string_view::npos) {
      parts.push_back(raw.substr(pos));
      break;
    }
    parts.push_back(raw.substr(pos, colon - pos));
    pos = colon + 1;
  }
  if (parts.size() > 3) {
    return std::nullopt;  // Go default: "use ss, mm:ss, or hh:mm:ss format"
  }

  // Go normalizeClockField: trim + empty → "0".
  const auto normalize = [](std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
      s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
      s.remove_suffix(1);
    }
    return s.empty() ? std::string_view("0") : s;
  };

  // Go parseNonNegativeInt: digits only (empty rejected). Accumulate in a
  // double — TUI time inputs stay well below 2^53, and unlike Go's Atoi the
  // port never errors on huge strings.
  const auto parse_digits = [](std::string_view s) -> std::optional<double> {
    if (s.empty()) {
      return std::nullopt;
    }
    double v = 0.0;
    for (const char c : s) {
      if (c < '0' || c > '9') {
        return std::nullopt;
      }
      v = v * 10.0 + static_cast<double>(c - '0');
    }
    return v;
  };

  // Go parseClockMinutes/parseClockSeconds: at most two digits, ≤ 59.
  const auto parse_clock = [&](std::string_view s) -> std::optional<double> {
    const std::string_view n = normalize(s);
    if (n.size() > 2) {
      return std::nullopt;
    }
    const auto v = parse_digits(n);
    if (!v || *v > 59.0) {
      return std::nullopt;
    }
    return v;
  };

  switch (parts.size()) {
    case 1: {
      // Bare seconds (Go parseTotalSeconds).
      const auto secs = parse_digits(normalize(parts[0]));
      if (!secs) {
        return std::nullopt;
      }
      return *secs;
    }
    case 2: {
      // mm:ss — minutes are total (any digit count), seconds are clock.
      const auto mins = parse_digits(normalize(parts[0]));
      const auto secs = parse_clock(parts[1]);
      if (!mins || !secs) {
        return std::nullopt;
      }
      return *mins * 60.0 + *secs;
    }
    case 3: {
      // hh:mm:ss — hours are total, minutes/seconds are clock.
      const auto hours = parse_digits(normalize(parts[0]));
      const auto mins  = parse_clock(parts[1]);
      const auto secs  = parse_clock(parts[2]);
      if (!hours || !mins || !secs) {
        return std::nullopt;
      }
      return *hours * 3600.0 + *mins * 60.0 + *secs;
    }
    default:
      return std::nullopt;
  }
}

}  // namespace

JumpModel::JumpModel(Actions actions) : actions_(std::move(actions)) {}

void JumpModel::open() {
  // Go openJumpMode (keys.go): jumping=true, resetJumpInput().
  active_ = true;
  query_.clear();
}

void JumpModel::cancel() {
  // Go esc (handleJumpKey): closeJumpMode() — jumping=false + reset.
  active_ = false;
  query_.clear();
  if (actions_.on_cancel) {
    actions_.on_cancel();
  }
}

void JumpModel::submit() {
  // Go enter (handleJumpKey): parseJumpTarget(jumpInput) — on error the
  // prompt stays open (Go also surfaces the parse error in the status line);
  // on success the host seeks and closeJumpMode() runs.
  const auto target = parse_jump_target(query_);
  if (!target) {
    // bootamp: clear the query for a retry (Go preserves the input text).
    query_.clear();
    return;
  }
  active_ = false;
  query_.clear();
  if (actions_.on_jump) {
    actions_.on_jump(*target);
  }
}

#if BOOTAMP_HAS_FTXUI
// FTXUI glue — compiled only when FTXUI is installed (AUR, user installs
// later). UNCHECKED until then (see report): the exact ftxui API surface
// (7.0.3) needs a compile pass once FTXUI lands. The shell claims every key,
// so the Input is display-only (the host feeds characters into model.query()
// and routes enter/esc via the model); the CatchEvent below is defensive,
// matching the other screens' glue.
std::shared_ptr<ftxui::ComponentBase> make_jump_component(JumpModel& model) {
  // Go renderJumpBody placeholder: "00:00" (formatJumpPlaceholder).
  auto input = ftxui::Input(&model.query(), "00:00");
  input = ftxui::CatchEvent(input, [&model](const ftxui::Event& e) {
    const std::string& c = e.character();
    if (c == "enter") {
      model.submit();
      return true;
    }
    if (c == "esc") {
      model.cancel();
      return true;
    }
    return false;
  });

  return ftxui::Renderer(input, [input, &model] {
    std::vector<ftxui::Element> lines = {
        ftxui::text("Jump to Time"),
        input->Render(),
        ftxui::dim(ftxui::text(
            "  Target in ss, mm:ss, or hh:mm:ss. Enter submits, esc cancels.")),
    };
    return ftxui::vbox(lines);
  });
}
#endif  // BOOTAMP_HAS_FTXUI

}  // namespace bootamp::ui::screens
