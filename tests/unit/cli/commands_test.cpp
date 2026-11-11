#include "cli/app.hpp"
#include "cli/commands/account.hpp"
#include "cli/commands/login.hpp"
#include "cli/options.hpp"
#include "common/json.hpp"
#include "config/accounts.hpp"
#include "config/credentials.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using qoderz::cli::app_context;
using qoderz::cli::command_context;
using qoderz::testing::temp_dir;

/// One command line driven through `cli::run` with pristine streams.
struct cli_run {
  std::istringstream m_in;
  std::ostringstream m_out;
  std::ostringstream m_err;
  qoderz::cli::app_context m_context;

  cli_run(std::vector<std::string> args, std::map<std::string, std::string> env,
          std::string stdin_text = {})
      : m_in{std::move(stdin_text)}, m_context{.m_args = std::move(args),
                                               .m_env = std::move(env),
                                               .m_out = m_out,
                                               .m_err = m_err,
                                               .m_in = m_in} {}

  int operator()() { return qoderz::cli::run(m_context); }
};

/// A rendered table split into cells: columns are separated by two spaces, and a narrow cell's
/// padding is what makes a gap wider than two.
std::vector<std::vector<std::string>> table_rows(std::string const& text) {
  std::vector<std::vector<std::string>> rows;
  std::istringstream stream{text};
  std::string line;
  while (std::getline(stream, line)) {
    std::vector<std::string> cells;
    std::size_t start = 0;
    while (start < line.size()) {
      auto const gap = line.find("  ", start);
      auto const end = gap == std::string::npos ? line.size() : gap;
      cells.emplace_back(line.substr(start, end - start));
      if (gap == std::string::npos) {
        break;
      }
      start = line.find_first_not_of(' ', gap);
      if (start == std::string::npos) {
        break;
      }
    }
    rows.push_back(std::move(cells));
  }
  return rows;
}

/// The first parsed row whose NAME cell equals `first`, or nullptr.
std::vector<std::string> const* row_starting_with(std::vector<std::vector<std::string>> const& rows,
                                                  std::string const& first) {
  for (auto const& row : rows) {
    if (!row.empty() && row.front() == first) {
      return &row;
    }
  }
  return nullptr;
}

qoderz::config::account_record make_account(std::string site, std::string name,
                                            std::int64_t expires_at = 0) {
  qoderz::config::account_record record;
  record.m_site = std::move(site);
  record.m_name = std::move(name);
  record.m_access_token = "test-access-token-not-real";
  record.m_refresh_token = "test-refresh-token-not-real";
  record.m_uid = "uid-" + record.m_name;
  record.m_display_name = "Display " + record.m_name;
  record.m_email = record.m_name + "@example.invalid";
  record.m_account_id = record.m_email;
  record.m_machine_id = "machine-not-real";
  record.m_device_token = "test-device-token-not-real";
  record.m_expires_at = expires_at;
  record.m_created_at = 1'760'000'000'000;
  return record;
}

struct store_fixture {
  temp_dir m_dir;
  qoderz::config::credential_store m_store{m_dir.file("credentials.json")};

  std::map<std::string, std::string> env() const {
    return {{"QODERZ_CREDENTIALS", m_dir.file("credentials.json").string()}};
  }

  void write(qoderz::config::credential_file file) const {
    REQUIRE(m_store.save(file).has_value());
  }

  qoderz::config::credential_file load() const {
    auto file = m_store.load();
    REQUIRE(file.has_value());
    return *file;
  }
};

} // namespace

TEST_CASE("accounts prints the documented columns and marks the default name", "[cli][commands]") {
  store_fixture fixture;
  qoderz::config::credential_file file;
  file.m_accounts.emplace("qoder-cn/ada", make_account("qoder-cn", "ada", 1'760'000'000'000));
  file.m_accounts.emplace("qoder-cn/bob", make_account("qoder-cn", "bob"));
  file.m_defaults["qoder-cn"] = "qoder-cn/ada";
  fixture.write(file);

  cli_run run{{"accounts"}, fixture.env()};
  CHECK(run() == 0);
  auto const text = run.m_out.str();
  CHECK(text.starts_with("NAME"));
  CHECK(text.find("SITE") != std::string::npos);
  CHECK(text.find("EMAIL") != std::string::npos);
  CHECK(text.find("UID") != std::string::npos);
  CHECK(text.find("VPC") != std::string::npos);
  CHECK(text.find("EXPIRES") != std::string::npos);
  CHECK(text.find("KIND") != std::string::npos);
  CHECK(text.find("DEFAULT") != std::string::npos);
  CHECK(text.find("qoder-cn") != std::string::npos);
  CHECK(text.find("device") != std::string::npos);
  CHECK(text.find("test-access-token-not-real") == std::string::npos);

  // `docs/design/cli.md` §3: the default account is marked in both places, the NAME column for a
  // human copying a name and the DEFAULT column as the machine-readable bit.
  auto const rows = table_rows(text);
  REQUIRE(rows.size() == 3); // the header and the two accounts
  auto const* chosen = row_starting_with(rows, "ada*");
  REQUIRE(chosen != nullptr);
  CHECK(chosen->at(0) == "ada*");
  CHECK(chosen->at(7) == "*");
  auto const* other = row_starting_with(rows, "bob");
  REQUIRE(other != nullptr);
  CHECK(other->at(0) == "bob");
  CHECK(other->at(7) == "-");
}

TEST_CASE("accounts --json is pure JSON without a token", "[cli][commands]") {
  store_fixture fixture;
  qoderz::config::credential_file file;
  file.m_accounts.emplace("qoder-cn/work", make_account("qoder-cn", "work", 1'760'000'000'000));
  file.m_defaults["qoder-cn"] = "qoder-cn/work";
  fixture.write(file);

  cli_run run{{"accounts", "--json"}, fixture.env()};
  CHECK(run() == 0);
  auto const parsed = qoderz::json::parse(run.m_out.str(), nullptr, false);
  REQUIRE(parsed.is_array());
  REQUIRE(parsed.size() == 1);
  auto const& account = parsed.at(0);
  CHECK(account.at("name") == "work");
  CHECK(account.at("site") == "qoder-cn");
  CHECK(account.at("kind") == "device");
  CHECK(account.at("default") == true);
  CHECK(account.at("uid") == "uid-work");
  CHECK(run.m_out.str().find("test-access-token-not-real") == std::string::npos);
}

TEST_CASE("accounts --site filters and --all-sites and --site cannot combine", "[cli][commands]") {
  store_fixture fixture;
  qoderz::config::credential_file file;
  file.m_accounts.emplace("qoder-cn/work", make_account("qoder-cn", "work"));
  file.m_accounts.emplace("qoder/abroad", make_account("qoder", "abroad"));
  fixture.write(file);

  cli_run filtered{{"accounts", "--site", "qoder"}, fixture.env()};
  CHECK(filtered() == 0);
  CHECK(filtered.m_out.str().find("abroad") != std::string::npos);
  CHECK(filtered.m_out.str().find("work") == std::string::npos);

  cli_run both{{"accounts", "--site", "qoder", "--all-sites"}, fixture.env()};
  CHECK(both() == 2);
}

TEST_CASE("account use sets the default and refuses ambiguity", "[cli][commands]") {
  store_fixture fixture;
  qoderz::config::credential_file file;
  file.m_accounts.emplace("qoder-cn/work", make_account("qoder-cn", "work"));
  file.m_accounts.emplace("qoder-cn/home", make_account("qoder-cn", "home"));
  file.m_defaults["qoder-cn"] = "qoder-cn/work";
  fixture.write(file);

  cli_run use{{"account", "use", "HOME"}, fixture.env()};
  CHECK(use() == 0);
  CHECK(fixture.load().m_defaults.at("qoder-cn") == "qoder-cn/home");

  cli_run prefix{{"account", "use", "h"}, fixture.env()};
  CHECK(prefix() == 0);

  cli_run missing{{"account", "use", "nobody"}, fixture.env()};
  CHECK(missing() == 1);
  CHECK(missing.m_err.str().find("no account matches") != std::string::npos);
}

TEST_CASE("account remove needs confirmation only on a terminal", "[cli][commands]") {
  store_fixture fixture;
  qoderz::config::credential_file file;
  file.m_accounts.emplace("qoder-cn/work", make_account("qoder-cn", "work"));
  file.m_accounts.emplace("qoder-cn/home", make_account("qoder-cn", "home"));
  file.m_defaults["qoder-cn"] = "qoder-cn/work";
  fixture.write(file);

  cli_run remove{{"account", "remove", "work", "--json"}, fixture.env()};
  CHECK(remove() == 0);
  auto const parsed = qoderz::json::parse(remove.m_out.str(), nullptr, false);
  REQUIRE(parsed.is_object());
  CHECK(parsed.at("removed").size() == 1);
  CHECK(parsed.at("removed").at(0) == "qoder-cn/work");
  CHECK(parsed.at("default") == "qoder-cn/home");
  CHECK(fixture.load().m_accounts.size() == 1);
}

TEST_CASE("removing the last account removes the file", "[cli][commands]") {
  store_fixture fixture;
  qoderz::config::credential_file file;
  file.m_accounts.emplace("qoder-cn/work", make_account("qoder-cn", "work"));
  file.m_defaults["qoder-cn"] = "qoder-cn/work";
  fixture.write(file);

  cli_run logout{{"logout", "--yes"}, fixture.env()};
  CHECK(logout() == 0);
  CHECK_FALSE(std::filesystem::exists(fixture.m_dir.file("credentials.json")));
}

TEST_CASE("logout with several accounts refuses and lists them", "[cli][commands]") {
  store_fixture fixture;
  qoderz::config::credential_file file;
  file.m_accounts.emplace("qoder-cn/work", make_account("qoder-cn", "work"));
  file.m_accounts.emplace("qoder-cn/home", make_account("qoder-cn", "home"));
  fixture.write(file);

  cli_run run{{"logout", "--yes"}, fixture.env()};
  CHECK(run() == 1);
  CHECK(run.m_err.str().find("work") != std::string::npos);
  CHECK(run.m_err.str().find("home") != std::string::npos);
  CHECK(run.m_err.str().find("--account") != std::string::npos);
  CHECK(fixture.load().m_accounts.size() == 2);
}

TEST_CASE("account show prints the fields and never the tokens", "[cli][commands]") {
  store_fixture fixture;
  qoderz::config::credential_file file;
  file.m_accounts.emplace("qoder-cn/work", make_account("qoder-cn", "work", 1'760'000'000'000));
  file.m_defaults["qoder-cn"] = "qoder-cn/work";
  fixture.write(file);

  cli_run text{{"account", "show", "work"}, fixture.env()};
  CHECK(text() == 0);
  CHECK(text.m_out.str().find("uid-work") != std::string::npos);
  CHECK(text.m_out.str().find("work@example.invalid") != std::string::npos);
  CHECK(text.m_out.str().find("test-access-token-not-real") == std::string::npos);

  cli_run json{{"account", "show", "work", "--json"}, fixture.env()};
  CHECK(json() == 0);
  auto const parsed = qoderz::json::parse(json.m_out.str(), nullptr, false);
  REQUIRE(parsed.is_object());
  CHECK(parsed.at("name") == "work");
  CHECK(parsed.at("default") == true);
  CHECK(json.m_out.str().find("test-refresh-token-not-real") == std::string::npos);
}

TEST_CASE("login validates --name before touching the network", "[cli][commands]") {
  store_fixture fixture;
  cli_run run{{"login", "--name", "-bad", "--pat", "pt-not-real"}, fixture.env()};
  CHECK(run() == 1);
  CHECK(run.m_err.str().find("valid account name") != std::string::npos);
  CHECK(run.m_out.str().empty());
}

TEST_CASE("login refuses --pat together with --pat-stdin", "[cli][commands]") {
  store_fixture fixture;
  cli_run both{{"login", "--pat", "pt-explicit-not-real", "--pat-stdin"}, fixture.env()};
  CHECK(both() == 2);
  CHECK(both.m_err.str().find("--pat-stdin") != std::string::npos);

  cli_run device{{"login", "--device", "--pat", "pt-explicit-not-real"}, fixture.env()};
  CHECK(device() == 2);
}

TEST_CASE("an explicit PAT flag outranks the QODERZ_PAT fallback", "[cli][commands]") {
  qoderz::cli::login_options from_flag;
  from_flag.m_pat = "pt-explicit-not-real";
  qoderz::cli::login_options from_stdin;
  from_stdin.m_pat_stdin = true;
  qoderz::cli::login_options neither;

  qoderz::cli::env_options env;
  env.m_pat = "pt-from-env-not-real";

  std::istringstream in{"  pt-stdin-not-real  \n"};
  std::ostringstream out;
  std::ostringstream err;
  qoderz::cli::app_context app{.m_args = {}, .m_env = {}, .m_out = out, .m_err = err, .m_in = in};

  auto flag = qoderz::cli::resolve_pat(from_flag, env, app);
  REQUIRE(flag.has_value());
  CHECK(flag->value_or("") == "pt-explicit-not-real");

  auto piped = qoderz::cli::resolve_pat(from_stdin, env, app);
  REQUIRE(piped.has_value());
  CHECK(piped->value_or("") == "pt-stdin-not-real");

  auto fallback = qoderz::cli::resolve_pat(neither, env, app);
  REQUIRE(fallback.has_value());
  CHECK(fallback->value_or("") == "pt-from-env-not-real");
}

TEST_CASE("login --pat-stdin reads one trimmed line from stdin", "[cli][commands]") {
  store_fixture fixture;
  cli_run empty{{"login", "--pat-stdin"}, fixture.env(), "\n"};
  CHECK(empty() == 1);
  CHECK(empty.m_err.str().find("empty line") != std::string::npos);
}

TEST_CASE("an unknown command and a --json on a plain command are usage errors",
          "[cli][commands]") {
  store_fixture fixture;

  cli_run unknown{{"nope"}, fixture.env()};
  CHECK(unknown() == 2);

  cli_run json_on_use{{"account", "use", "work", "--json"}, fixture.env()};
  CHECK(json_on_use() == 2);
}

TEST_CASE("version, help and no command all exit zero", "[cli][commands]") {
  store_fixture fixture;

  cli_run version{{"--version"}, fixture.env()};
  CHECK(version() == 0);
  CHECK(version.m_out.str().starts_with("qoderz "));

  cli_run version_cmd{{"version"}, fixture.env()};
  CHECK(version_cmd() == 0);
  CHECK(version_cmd.m_out.str().starts_with("qoderz "));

  cli_run help{{"--help"}, fixture.env()};
  CHECK(help() == 0);
  CHECK(help.m_out.str().find("serve") != std::string::npos);

  cli_run no_args{{}, fixture.env()};
  CHECK(no_args() == 0);
  CHECK(no_args.m_out.str().starts_with("qoderz "));
  CHECK(no_args.m_out.str().find("credentials:") != std::string::npos);
}

TEST_CASE("the global options work before and after the command", "[cli][commands]") {
  store_fixture fixture;

  cli_run after{{"accounts", "--site", "qoder"}, fixture.env()};
  CHECK(after() == 0);

  cli_run before{{"--site", "qoder", "accounts"}, fixture.env()};
  CHECK(before() == 0);
}

TEST_CASE("an interactive choice reads one number and defaults to the first", "[cli][commands]") {
  auto const context_with = [](std::istringstream& in, bool tty) {
    static std::ostringstream out;
    static std::ostringstream err;
    qoderz::cli::app_context app{.m_args = {},
                                 .m_env = {},
                                 .m_out = out,
                                 .m_err = err,
                                 .m_in = in,
                                 .m_stdin_tty = tty,
                                 .m_stdout_tty = tty};
    return app;
  };

  std::istringstream second{"2\n"};
  auto tty = context_with(second, true);
  CHECK(qoderz::cli::choose_interactive({"work", "home"}, "pick:", tty) == 1);

  std::istringstream blank{"\n"};
  auto defaulted = context_with(blank, true);
  CHECK(qoderz::cli::choose_interactive({"work", "home"}, "pick:", defaulted) == 0);

  std::istringstream out_of_range{"9\n"};
  auto bad = context_with(out_of_range, true);
  CHECK_FALSE(qoderz::cli::choose_interactive({"work", "home"}, "pick:", bad).has_value());

  std::istringstream piped{"1\n"};
  auto plain = context_with(piped, false);
  CHECK_FALSE(qoderz::cli::choose_interactive({"work", "home"}, "pick:", plain).has_value());
}

TEST_CASE("a confirmation accepts only y or yes", "[cli][commands]") {
  auto const answered = [](std::string const& text) {
    std::istringstream in{text};
    std::ostringstream out;
    std::ostringstream err;
    qoderz::cli::app_context app{.m_args = {},
                                 .m_env = {},
                                 .m_out = out,
                                 .m_err = err,
                                 .m_in = in,
                                 .m_stdin_tty = true,
                                 .m_stdout_tty = true};
    return qoderz::cli::confirm("Remove?", app);
  };

  CHECK(answered("y\n"));
  CHECK(answered("YES\n"));
  CHECK_FALSE(answered("n\n"));
  CHECK_FALSE(answered("\n"));
  CHECK_FALSE(answered(""));
}

TEST_CASE("an ambiguous selector asks on a terminal and fails without one", "[cli][commands]") {
  store_fixture fixture;
  qoderz::config::credential_file file;
  file.m_accounts.emplace("qoder-cn/work", make_account("qoder-cn", "work"));
  file.m_accounts.emplace("qoder-cn/worker", make_account("qoder-cn", "worker"));
  file.m_defaults["qoder-cn"] = "qoder-cn/work";
  fixture.write(file);

  cli_run exact{{"account", "use", "work", "--site", "qoder-cn"}, fixture.env()};
  CHECK(exact() == 0); // an exact name wins over a prefix, so no prompt is needed

  cli_run interactive{{"account", "use", "wo", "--site", "qoder-cn"}, fixture.env(), "2\n"};
  interactive.m_context.m_stdin_tty = true;
  interactive.m_context.m_stdout_tty = true;
  CHECK(interactive() == 0);
  CHECK(fixture.load().m_defaults.at("qoder-cn") == "qoder-cn/worker");

  qoderz::config::credential_file again;
  again.m_accounts.emplace("qoder-cn/work", make_account("qoder-cn", "work"));
  again.m_accounts.emplace("qoder-cn/worker", make_account("qoder-cn", "worker"));
  again.m_defaults["qoder-cn"] = "qoder-cn/work";
  fixture.write(again);

  cli_run ambiguous{{"account", "use", "wo", "--site", "qoder-cn"}, fixture.env()};
  CHECK(ambiguous() == 1);
  CHECK(ambiguous.m_err.str().find("ambiguous") != std::string::npos);
}
