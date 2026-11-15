#pragma once
// Multiplayer wiring for fledge_runtime (docs/NETWORKING.md):
//   --server [host:]port     authoritative server for the loaded scene (a bare port binds all interfaces)
//   --connect host:port      client: replicated entities come from the server, the local player is predicted
//   --net-clients N          with --server: also launch N headless bot clients of this executable and exit when
//                            they are done (local multiplayer test; prints NET-SERVER OK / FAILED)
//   --net-bot                client input is scripted (walks in circles, jumps) instead of keyboard / mouse
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fledge/net/net.h"
#include "fledge/platform/window.h"
#include "fledge/render/renderer.h"

namespace fledge {

struct NetOptions {
    std::string server, connect;
    int spawnClients = 0;
    int clientFrames = 300;
    bool bot = false;
    std::vector<std::string> contentArgs;  // passed on to spawned clients (--project / --pak / --content ...)
    bool active() const { return !server.empty() || !connect.empty(); }
};

class NetSession {
public:
    static std::unique_ptr<NetSession> start(const NetOptions& options, Scene& scene, double now);
    ~NetSession();

    // One fixed 60 Hz step (input is read for clients that are not bots).
    void update(double now, const InputState* input);
    std::optional<ViewItem> view(float aspect);  // clients: follow camera behind the local player
    bool spawnedClientsDone() const;             // server with --net-clients: every child has exited
    bool isServer() const { return m_server != nullptr; }
    int finish();                                // prints the NET-* summary line, returns the exit code

private:
    NetSession() = default;
    void launchClients(const NetOptions& options);

    Scene* m_scene = nullptr;
    std::unique_ptr<net::NetServer> m_server;
    std::unique_ptr<net::NetClient> m_client;
    NetOptions m_options;
    float m_yaw = 0;
    int m_ticks = 0;
    size_t m_maxRemotePlayers = 0;
    std::vector<void*> m_children;  // process handles (Windows)
};

} // namespace fledge
