#include "common/json.hpp"
#include "qoder/catalog/listing.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace {

using qoderz::json;
using qoderz::qoder::catalog::merge_listing;
using qoderz::qoder::catalog::parse_models;

json array_of(std::string_view text) {
  return json::parse(text);
}

std::string key_of(json const& item) {
  if (!item.is_object()) {
    return "<none>";
  }
  auto const key = item.find("key");
  return key != item.end() && key->is_string() ? key->get<std::string>() : std::string{"<none>"};
}

/// The `key` of each entry, or `<none>` for an entry without a usable key. The placeholder keeps a
/// wrong merge readable in the failure message instead of aborting the whole run.
std::vector<std::string> keys_of(std::vector<json> const& items) {
  std::vector<std::string> keys;
  keys.reserve(items.size());
  for (auto const& item : items) {
    keys.push_back(key_of(item));
  }
  return keys;
}

std::vector<std::string> model_keys(std::vector<qoderz::qoder::catalog::model_info> const& models) {
  std::vector<std::string> keys;
  keys.reserve(models.size());
  for (auto const& model : models) {
    keys.push_back(model.m_key);
  }
  return keys;
}

json empty() {
  return json{};
}

} // namespace

TEST_CASE("merge_listing clones chat when app is missing", "[qoder][catalog][listing]") {
  auto const chat = array_of(R"([{"key":"a"},{"key":"b"}])");
  auto const merged = merge_listing(empty(), chat);

  CHECK(keys_of(merged) == std::vector<std::string>{"a", "b"});
}

TEST_CASE("merge_listing returns nothing when both sides are missing",
          "[qoder][catalog][listing]") {
  CHECK(merge_listing(empty(), empty()).empty());
  CHECK(merge_listing(array_of("{}"), array_of(R"("not-an-array")")).empty());
}

TEST_CASE("merge_listing returns app when chat is missing", "[qoder][catalog][listing]") {
  auto const app = array_of(R"([{"key":"a"},{"key":"b"}])");
  CHECK(keys_of(merge_listing(app, empty())) == std::vector<std::string>{"a", "b"});
}

TEST_CASE("merge_listing inserts new chat entries after the anchor", "[qoder][catalog][listing]") {
  auto const app = array_of(R"([{"key":"a"},{"key":"b"},{"key":"c"}])");
  auto const chat = array_of(R"([{"key":"b"},{"key":"x"},{"key":"y"}])");

  CHECK(keys_of(merge_listing(app, chat)) == std::vector<std::string>{"a", "b", "x", "y", "c"});
}

TEST_CASE("merge_listing keeps the app version of a duplicated key", "[qoder][catalog][listing]") {
  auto const app = array_of(R"([{"key":"a","display_name":"From app"}])");
  auto const chat = array_of(R"([{"key":"a","display_name":"From chat"},{"key":"z"}])");

  auto const merged = merge_listing(app, chat);
  REQUIRE(merged.size() == 2);
  CHECK(merged[0].value("display_name", std::string{"<none>"}) == "From app");
  CHECK(key_of(merged[1]) == "z");
}

TEST_CASE("merge_listing appends new keys while the anchor is unset", "[qoder][catalog][listing]") {
  auto const app = array_of(R"([{"key":"a"}])");
  auto const chat = array_of(R"([{"key":"n1"},{"key":"n2"}])");

  CHECK(keys_of(merge_listing(app, chat)) == std::vector<std::string>{"a", "n1", "n2"});
}

TEST_CASE("merge_listing skips chat entries that are not objects", "[qoder][catalog][listing]") {
  auto const app = array_of(R"([{"key":"a"}])");
  auto const chat = array_of(R"([42,{"key":"b"},"x",{"key":"a"}])");

  CHECK(keys_of(merge_listing(app, chat)) == std::vector<std::string>{"a", "b"});
}

TEST_CASE("merge_listing anchors on the first entry of a duplicated key",
          "[qoder][catalog][listing]") {
  auto const app = array_of(R"([{"key":"a"},{"key":"a"},{"key":"b"}])");

  // The first "a" is the anchor, so a new key lands directly behind it rather than behind "b".
  CHECK(keys_of(merge_listing(app, array_of(R"([{"key":"a"},{"key":"x"}])"))) ==
        std::vector<std::string>{"a", "x", "a", "b"});
  // With no anchor yet a brand-new key is appended, duplicates in the app list notwithstanding.
  CHECK(keys_of(merge_listing(app, array_of(R"([{"key":"x"}])"))) ==
        std::vector<std::string>{"a", "a", "b", "x"});
}

TEST_CASE("parse_models filters unusable entries", "[qoder][catalog][listing]") {
  auto const merged = array_of(R"([
    {"key":"auto","enable":true},
    {"key":"default","enable":true},
    {"key":"","enable":true},
    {"key":"disabled","enable":false},
    {"key":"missing-enable"},
    {"key":"text-enable","enable":"true"},
    {"key":"numeric-enable","enable":1},
    {"key":"zero-enable","enable":0},
    {"key":"kept","enable":true}
  ])");

  CHECK(model_keys(parse_models(merged)) ==
        std::vector<std::string>{"text-enable", "numeric-enable", "kept"});
}

TEST_CASE("parse_models keeps the first entry of a duplicated key", "[qoder][catalog][listing]") {
  auto const merged = array_of(R"([
    {"key":"dup","enable":true,"display_name":"First"},
    {"key":"dup","enable":true,"display_name":"Second"}
  ])");

  auto const models = parse_models(merged);
  REQUIRE(models.size() == 1);
  CHECK(models[0].m_name == "First");
}

TEST_CASE("parse_models lets a later enabled entry win after a disabled duplicate",
          "[qoder][catalog][listing]") {
  auto const merged = array_of(R"([
    {"key":"m","enable":false,"display_name":"Disabled"},
    {"key":"m","enable":true,"display_name":"Enabled"}
  ])");

  // A filtered-out entry must not consume the key, or the model would vanish from the catalogue.
  auto const models = parse_models(merged);
  REQUIRE(models.size() == 1);
  CHECK(models[0].m_name == "Enabled");
}

TEST_CASE("parse_models parses every usable entry in listing order", "[qoder][catalog][listing]") {
  auto const merged = array_of(R"([
    {"key":"a","enable":true,"display_name":"Alpha","source":"app"},
    {"key":"b","enable":true}
  ])");

  auto const models = parse_models(merged);
  REQUIRE(models.size() == 2);
  CHECK(models[0].m_name == "Alpha");
  CHECK(models[0].m_source == "app");
  CHECK(models[1].m_name == "b");
}

TEST_CASE("parse_models ignores a non-object entry and a null key", "[qoder][catalog][listing]") {
  auto const merged = array_of(R"([1,{"key":null,"enable":true},{"key":"ok","enable":true}])");

  CHECK(model_keys(parse_models(merged)) == std::vector<std::string>{"ok"});
}
