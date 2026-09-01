#include <catch2/catch.hpp>

#include "commands.h"
#include "http_client.h"

// HttpClient's constructor doesn't touch the network -- cpp-httplib's Client connects lazily
// on the first actual request, so constructing one here is safe. These cases specifically rely
// on the bounds check happening *before* any request would be made (design.md §3.3); if a fix
// regressed that, this would hang or crash trying to reach a real daemon instead of returning 1.

TEST_CASE("commandPlay with no further arguments returns a usage error") {
  HttpClient client("192.168.86.28", 8080);
  char arg0[] = "gj";
  char arg1[] = "play";
  char* argv[] = {arg0, arg1};
  REQUIRE(commandPlay(client, 2, argv) == 1);
}

TEST_CASE("commandPlay with a type but no name returns a usage error") {
  HttpClient client("192.168.86.28", 8080);
  char arg0[] = "gj";
  char arg1[] = "play";
  char arg2[] = "track";
  char* argv[] = {arg0, arg1, arg2};
  REQUIRE(commandPlay(client, 3, argv) == 1);
}

TEST_CASE("commandList with no type returns a usage error") {
  HttpClient client("192.168.86.28", 8080);
  char arg0[] = "gj";
  char arg1[] = "list";
  char* argv[] = {arg0, arg1};
  REQUIRE(commandList(client, 2, argv) == 1);
}
