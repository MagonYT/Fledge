#include "net_session.h"

#include <cstdio>
#include <format>

#include "fledge/core/log.h"
#include "fledge/core/utility.h"
#include "fledge/scene/components.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fledge {

namespace {
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (wchar_t c : a) {
        if (c == L'"') q += L'\\';
        q += c;
    }
    return q + L"\"";
}
} // namespace

std::unique_ptr<NetSession> NetSession::start(const NetOptions& options, Scene& scene, double now) {
    std::unique_ptr<NetSession> s(new NetSession());
    s->m_scene = &scene;
    s->m_options = options;
    std::string err;
    if (!options.server.empty()) {
        auto bind = net::NetAddress::parse(options.server, true);
        if (!bind) {
            FLEDGE_LOG_ERROR("net", "--server {}: expected [host:]port", options.server);
            return nullptr;
        }
        auto t = net::UdpTransport::open(*bind, &err);
        if (!t) {
            FLEDGE_LOG_ERROR("net", "{}", err);
            return nullptr;
        }
        s->m_server = std::make_unique<net::NetServer>(scene, std::move(t));
        FLEDGE_LOG_INFO("net", "server listening on UDP {}", s->m_server->address().toString());
        if (options.spawnClients > 0) s->launchClients(options);
        return s;
    }
    auto server = net::NetAddress::parse(options.connect);
    if (!server || server->host == 0) {
        FLEDGE_LOG_ERROR("net", "--connect {}: expected host:port", options.connect);
        return nullptr;
    }
    // Loopback servers get a loopback socket (no firewall prompt); anything else binds all interfaces.
    bool loop = (server->host >> 24) == 127;
    auto t = net::UdpTransport::open({loop ? 0x7F000001u : 0u, 0}, &err);
    if (!t) {
        FLEDGE_LOG_ERROR("net", "{}", err);
        return nullptr;
    }
    // Replicated entities come from the server: drop the local copies the scene file contains.
    std::vector<EntityId> replicated(scene.pool<NetIdentityComponent>().entities());
    for (EntityId e : replicated)
        if (scene.isValid(e)) scene.destroyEntity(e);
    s->m_client = std::make_unique<net::NetClient>(scene, std::move(t), *server);
    s->m_client->connect(now);
    FLEDGE_LOG_INFO("net", "connecting to {}", server->toString());
    return s;
}

void NetSession::launchClients(const NetOptions& o) {
#ifdef _WIN32
    std::wstring exe = executablePath().wstring();
    net::NetAddress a = m_server->address();
    std::string target = std::format("127.0.0.1:{}", a.port);
    for (int i = 0; i < o.spawnClients; ++i) {
        std::wstring cmd = quoteArg(exe);
        for (auto& arg : o.contentArgs) cmd += L" " + quoteArg(widen(arg));
        cmd += widen(std::format(" --connect {} --net-bot --headless --no-audio --smoke-test --frames {}", target, o.clientFrames));
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
            FLEDGE_LOG_ERROR("net", "could not launch client {} ({})", i + 1, GetLastError());
            continue;
        }
        CloseHandle(pi.hThread);
        m_children.push_back(pi.hProcess);
    }
    FLEDGE_LOG_INFO("net", "launched {} bot client(s) -> {}", m_children.size(), target);
#else
    (void)o;
    FLEDGE_LOG_ERROR("net", "--net-clients is only implemented on Windows; start clients with --connect");
#endif
}

NetSession::~NetSession() {
#ifdef _WIN32
    for (void* h : m_children) {
        if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) TerminateProcess(h, 5);
        CloseHandle(h);
    }
#endif
}

bool NetSession::spawnedClientsDone() const {
#ifdef _WIN32
    if (m_children.empty()) return m_options.spawnClients > 0;  // all launches failed
    for (void* h : m_children)
        if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) return false;
    return true;
#else
    return true;
#endif
}

void NetSession::update(double now, const InputState* input) {
    ++m_ticks;
    if (m_server) {
        m_server->update(now);
        return;
    }
    net::NetInput in;
    if (m_options.bot) {  // walk in a circle, jump every 1.5 s
        m_yaw += 0.015f;
        in.moveZ = -1;
        in.buttons = (m_ticks % 90 == 0) ? net::kButtonJump : 0;
    } else if (input) {
        m_yaw -= input->mouseDelta.x * 0.003f;
        in.moveZ = (input->keyDown(Key::S) ? 1.0f : 0.0f) - (input->keyDown(Key::W) ? 1.0f : 0.0f);
        in.moveX = (input->keyDown(Key::D) ? 1.0f : 0.0f) - (input->keyDown(Key::A) ? 1.0f : 0.0f);
        in.buttons = input->keyDown(Key::Space) ? net::kButtonJump : 0;
    }
    in.yaw = m_yaw;
    m_client->setInput(in);
    m_client->update(now);
    size_t remote = 0;
    auto& pool = m_scene->pool<NetPlayerComponent>();
    for (EntityId e : pool.entities())
        if (auto* ni = m_scene->get<NetIdentityComponent>(e); ni && ni->owner != static_cast<int>(m_client->id())) ++remote;
    m_maxRemotePlayers = std::max(m_maxRemotePlayers, remote);
}

std::optional<ViewItem> NetSession::view(float aspect) {
    if (!m_client || m_client->localPlayer() == kNullEntity) return std::nullopt;
    auto* t = m_scene->get<TransformComponent>(m_client->localPlayer());
    if (!t) return std::nullopt;
    Vec3 forward = Quat::fromAxisAngle({0, 1, 0}, m_yaw).rotate({0, 0, -1});
    ViewItem v;
    v.position = t->position - forward * 6.0f + Vec3{0, 3, 0};
    v.view = Mat4::lookAt(v.position, t->position + Vec3{0, 1, 0}, {0, 1, 0});
    v.projection = Mat4::perspective(radians(60), aspect, 0.1f, 500);
    return v;
}

int NetSession::finish() {
    if (m_server) {
        bool ok = true;
        std::string codes;
#ifdef _WIN32
        for (void* h : m_children) {
            DWORD code = 1;
            if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) ok = false;
            else GetExitCodeProcess(h, &code);
            codes += std::format("{}{}", codes.empty() ? "" : ",", code);
            ok = ok && code == 0;
        }
#endif
        if (m_options.spawnClients > 0) ok = ok && static_cast<int>(m_children.size()) == m_options.spawnClients &&
                                             static_cast<int>(m_server->peakClients()) == m_options.spawnClients;
        std::printf("NET-SERVER %s clients=%d peak=%zu ticks=%u snapshots=%llu snapshotBytes=%llu malformed=%llu childExit=%s\n",
                    ok ? "OK" : "FAILED", m_options.spawnClients, m_server->peakClients(), m_server->tick(),
                    static_cast<unsigned long long>(m_server->snapshotsSent()), static_cast<unsigned long long>(m_server->snapshotBytes()),
                    static_cast<unsigned long long>(m_server->malformedPackets()), codes.empty() ? "-" : codes.c_str());
        std::fflush(stdout);
        return ok ? 0 : 4;
    }
    const auto& st = m_client->stats();
    bool ok = m_client->state() == net::ClientState::Connected && m_client->localPlayer() != kNullEntity && st.snapshotsApplied > 0;
    Vec3 pos = ok ? m_scene->get<TransformComponent>(m_client->localPlayer())->position : Vec3{};
    std::printf("NET-CLIENT %s id=%u state=%s entities=%zu remotePlayers=%zu snapshots=%llu corrections=%llu inputs=%u rtt=%.1fms pos=%.2f,%.2f,%.2f\n",
                ok ? "OK" : "FAILED", m_client->id(), net::clientStateName(m_client->state()), m_client->entityCount(), m_maxRemotePlayers,
                static_cast<unsigned long long>(st.snapshotsApplied), static_cast<unsigned long long>(st.corrections), m_client->inputTick(),
                m_client->connectionStats().rtt * 1000.0, pos.x, pos.y, pos.z);
    std::fflush(stdout);
    m_client->disconnect();
    return ok ? 0 : 4;
}

} // namespace fledge
