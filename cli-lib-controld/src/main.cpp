#include <string>
#include <vector>

#include "commands.h"
#include "http_client.h"
#include "listen_command.h"

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#endif

namespace {
// Fixed target, matching requirements.md §6 -- no config file, mirroring playback-controld's
// own single-deployment-target, no-config-file approach. This is the Jetson's actual LAN
// address, not 0.0.0.0 -- that's meaningful only on the listening side.
constexpr const char* kDaemonHost = "192.168.86.28";
constexpr int kDaemonPort = 8080;

#ifdef _WIN32
// main(argc, char** argv)'s narrow argv is decoded by the CRT through the Windows ANSI
// codepage, not UTF-8 -- found via real testing: an em-dash typed at the prompt silently
// became a single invalid-UTF-8 byte by the time gj saw it, which then broke the request to
// playback-controld. Rebuild argv from the process's actual UTF-16 command line instead, so
// any Unicode characters in a track/album/artist/playlist name survive intact.
std::vector<std::string> utf8CommandLineArgs() {
  int wargc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  std::vector<std::string> args;
  if (!wargv) return args;
  args.reserve(static_cast<size_t>(wargc));
  for (int i = 0; i < wargc; ++i) {
    int size = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string arg(size > 0 ? static_cast<size_t>(size - 1) : 0, '\0');  // exclude the null
    if (size > 0) {
      WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, arg.data(), size, nullptr, nullptr);
    }
    args.push_back(std::move(arg));
  }
  LocalFree(wargv);
  return args;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // Reconstruct argc/argv from the UTF-8 conversion above before anything else touches them,
  // so every downstream command (commandPlay's urlEncode in particular) sees correct bytes.
  std::vector<std::string> utf8Args = utf8CommandLineArgs();
  std::vector<char*> utf8Argv;
  utf8Argv.reserve(utf8Args.size());
  for (auto& arg : utf8Args) utf8Argv.push_back(arg.data());
  argc = static_cast<int>(utf8Argv.size());
  argv = utf8Argv.data();
#endif

  if (argc < 2) {
    printUsage();
    return 1;
  }
  std::string cmd = argv[1];
  HttpClient client(kDaemonHost, kDaemonPort);

  if (cmd == "listen") return ListenCommand(client).run();
  if (cmd == "play") return commandPlay(client, argc, argv);
  if (cmd == "pause") return commandPause(client);
  if (cmd == "resume") return commandResume(client);
  if (cmd == "stop") return commandStop(client);
  if (cmd == "skip") return commandNext(client);
  if (cmd == "previous") return commandPrevious(client);
  if (cmd == "list") return commandList(client, argc, argv);

  printUsage();
  return 1;
}
