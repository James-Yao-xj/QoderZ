#include "cli/output.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include <string>

namespace {

using qoderz::cli::table;
using qoderz::cli::table_column;

} // namespace

TEST_CASE("a table pads each column to its widest cell with two spaces", "[cli][output]") {
  table view;
  view.m_columns = {{.m_header = "NAME"}, {.m_header = "SITE"}};
  view.m_rows = {{"work", "qoder-cn"}, {"personal", "qoder"}};

  CHECK(qoderz::cli::render(view) == "NAME      SITE\n"
                                     "work      qoder-cn\n"
                                     "personal  qoder\n");
}

TEST_CASE("an empty cell renders as a dash", "[cli][output]") {
  table view;
  view.m_columns = {{.m_header = "NAME"}, {.m_header = "EMAIL"}};
  view.m_rows = {{"work", ""}};

  CHECK(qoderz::cli::render(view) == "NAME  EMAIL\n"
                                     "work  -\n");
}

TEST_CASE("a numeric column is right aligned", "[cli][output]") {
  table view;
  view.m_columns = {{.m_header = "ID"}, {.m_header = "CONTEXT", .m_right = true}};
  view.m_rows = {{"a", "1M"}, {"bb", "200K"}};

  CHECK(qoderz::cli::render(view) == "ID  CONTEXT\n"
                                     "a        1M\n"
                                     "bb     200K\n");
}

TEST_CASE("a painted cell keeps the column width", "[cli][output]") {
  table view;
  view.m_columns = {{.m_header = "NAME"}};
  view.m_rows = {{qoderz::cli::paint(qoderz::cli::tone::bold, "work", true)}};

  auto const painted = qoderz::cli::paint(qoderz::cli::tone::bold, "work", true);
  CHECK(qoderz::cli::visible_length(painted) == 4);
  CHECK(qoderz::cli::render(view) == "NAME\n" + painted + "\n");
  CHECK(qoderz::cli::paint(qoderz::cli::tone::bold, "work", false) == "work");
}

TEST_CASE("context sizes use the K and M forms", "[cli][output]") {
  CHECK(qoderz::cli::format_context(0) == "0");
  CHECK(qoderz::cli::format_context(128000) == "128K");
  CHECK(qoderz::cli::format_context(200000) == "200K");
  CHECK(qoderz::cli::format_context(1000000) == "1M");
  CHECK(qoderz::cli::format_context(1500000) == "1.5M");
  CHECK(qoderz::cli::format_context(512) == "512");
}

TEST_CASE("an absent instant renders as a dash", "[cli][output]") {
  CHECK(qoderz::cli::format_local_time(std::nullopt) == "-");
  CHECK(qoderz::cli::format_local_time(0) == "-");
  CHECK_FALSE(qoderz::cli::format_iso8601(std::nullopt).has_value());
}

TEST_CASE("an instant carries the local offset", "[cli][output]") {
  auto const text = qoderz::cli::format_local_time(std::int64_t{1760000000000});
  REQUIRE(text.size() > 6);
  CHECK(text[4] == '-');
  CHECK(text[7] == '-');
  CHECK(text[10] == ' ');
  CHECK(text[13] == ':');
  CHECK(((text.back() == '0' || text.back() == '5' || text.back() == '3' || text.back() == '4' ||
          text.back() == '9')));
  CHECK((text.find('+') != std::string::npos || text.find('-', 11) != std::string::npos));

  auto const iso = qoderz::cli::format_iso8601(std::int64_t{1760000000000});
  REQUIRE(iso.has_value());
  CHECK(iso->find('T') != std::string::npos);
}

TEST_CASE("JSON output is compact and newline terminated", "[cli][output]") {
  std::ostringstream out;
  qoderz::cli::write_json(out, qoderz::json{{"object", "list"}, {"data", qoderz::json::array()}});
  CHECK(out.str() == "{\"data\":[],\"object\":\"list\"}\n");
}

TEST_CASE("booleans render as yes and no", "[cli][output]") {
  CHECK(qoderz::cli::yes_no(true) == "yes");
  CHECK(qoderz::cli::yes_no(false) == "no");
}
