// Self-check for the hand-rolled JSON scanning in
// platforms/esp32/wifi_sync.h. That code only ever parses replies from our own
// server, but it runs on a device with no way to report a crash, so the
// truncation loop at the bottom matters more than the happy path.
//
// It cannot include wifi_sync.h (that pulls in ESP-IDF), so the three functions
// are extracted by line range. Re-extract if they move:
//
//   sed -n '187,262p' platforms/esp32/wifi_sync.h > frag.inc
//
// Run:
//   nix-shell -p gcc --run \
//     'g++ -std=c++17 -fsanitize=address,undefined -o /tmp/t tools/tests/json_scan_test.cpp && /tmp/t'
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "frag.inc"
int main() {
  // A realistic server reply.
  const std::string r =
    R"({"get":["Snow Crash","The Hobbit"],"delete":["Dune"],)"
    R"("pos":{"Neuromancer":[9,3,0,41002],"Alice":[0,1,2,3]}})";

  auto get = parse_string_array_(r, find_key_(r, "get"));
  assert(get.size() == 2);
  assert(get[0] == "Snow Crash");
  assert(get[1] == "The Hobbit");

  auto del = parse_string_array_(r, find_key_(r, "delete"));
  assert(del.size() == 1 && del[0] == "Dune");

  auto pos = parse_pos_object_(r, find_key_(r, "pos"));
  assert(pos.size() == 2);
  assert(pos[0].dir == "Neuromancer");
  assert(pos[0].v[0]==9 && pos[0].v[1]==3 && pos[0].v[2]==0 && pos[0].v[3]==41002);
  assert(pos[1].dir == "Alice");
  assert(pos[1].v[3] == 3);

  // Empty everything — the no-op sync response.
  const std::string e = R"({"get":[],"delete":[],"pos":{}})";
  assert(parse_string_array_(e, find_key_(e, "get")).empty());
  assert(parse_pos_object_(e, find_key_(e, "pos")).empty());

  // Missing keys must not crash or invent entries.
  const std::string m = R"({})";
  assert(parse_string_array_(m, find_key_(m, "get")).empty());
  assert(parse_pos_object_(m, find_key_(m, "pos")).empty());

  // Escapes in a book name.
  const std::string esc = R"({"get":["A \"quoted\" book","back\\slash"]})";
  auto g2 = parse_string_array_(esc, find_key_(esc, "get"));
  assert(g2.size() == 2);
  assert(g2[0] == "A \"quoted\" book");
  assert(g2[1] == "back\\slash");

  // Truncated input must terminate rather than run off the end.
  for (size_t n = 0; n < r.size(); ++n) {
    const std::string cut = r.substr(0, n);
    parse_string_array_(cut, find_key_(cut, "get"));
    parse_pos_object_(cut, find_key_(cut, "pos"));
  }
  std::printf("json ok\n");
}
