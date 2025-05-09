#if !defined _WIN32
#include <arpa/inet.h>
#else
#include <WinSock2.h>
#endif

#include "../macros/logger.hpp"
#include "hostaddr.hpp"
#include "ice.hpp"

#define CUTIL_MACROS_PRINT_FUNC(...) LOG_ERROR(logger, __VA_ARGS__)
#include "../macros/unwrap.hpp"

namespace ice {
namespace {
constexpr auto default_stun_port = 3478;
constexpr auto default_turn_port = 5349;

auto logger = Logger("ice");

auto set_stun_turn(NiceAgent* const                     agent,
                   const std::span<const xmpp::Service> external_services,
                   const guint                          stream_id,
                   const guint                          component_id) -> bool {
    auto stun = false;
    auto turn = false;
    for(const auto& es : external_services) {
        if(!stun && es.type == "stun") {
            const auto hostaddr = hostname_to_addr(es.host.data());
            ensure(!hostaddr.empty(), "failed to resolve stun server address {}", es.host);
            const auto port = es.port != 0 ? es.port : default_stun_port;
            LOG_DEBUG(logger, "stun address: {}:{}", hostaddr, port);
            g_object_set(agent,
                         "stun-server", hostaddr.data(),
                         "stun-server-port", port,
                         NULL);
            stun = true;
        } else if(!turn && es.type == "turns") {
            const auto hostaddr = hostname_to_addr(es.host.data());
            ensure(!hostaddr.empty(), "failed to resolve turn server address {}", es.host);
            const auto port = es.port != 0 ? es.port : default_turn_port;
            LOG_DEBUG(logger, "turn address: {}:{}", hostaddr, port);
            if(nice_agent_set_relay_info(agent,
                                         stream_id,
                                         component_id,
                                         hostaddr.data(),
                                         port,
                                         es.username.data(),
                                         es.password.data(),
                                         NICE_RELAY_TYPE_TURN_TLS) != TRUE) {
                bail("failed to set relay info");
            }
            turn = true;
        }
        if(turn && stun) {
            break;
        }
    }
    return true;
}

auto agent_recv_callback(NiceAgent* const /*agent*/, const guint /*stream_id*/, const guint /*component_id*/, const guint /*len*/, gchar* const buf, const gpointer /*user_data*/) -> void {
    LOG_DEBUG(logger, "agent-recv: {}", buf);
}

auto candidate_gathering_done(NiceAgent* const /*agent*/, const guint /*stream_id*/, const gpointer /*user_data*/) -> void {
    LOG_DEBUG(logger, "candidate-gathering-done");
}

auto candidate_type_conv_table = std::array<std::pair<jingle::CandidateType, NiceCandidateType>, 4>{{
    {jingle::CandidateType::Host, NiceCandidateType::NICE_CANDIDATE_TYPE_HOST},
    {jingle::CandidateType::Srflx, NiceCandidateType::NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE},
    {jingle::CandidateType::Prflx, NiceCandidateType::NICE_CANDIDATE_TYPE_PEER_REFLEXIVE},
    {jingle::CandidateType::Relay, NiceCandidateType::NICE_CANDIDATE_TYPE_RELAYED},
}};

auto set_remote_candidates(NiceAgent* const               agent,
                           const jingle::IceUdpTransport& transport,
                           const guint                    stream_id,
                           const guint                    component_id) -> bool {

    auto r    = true;
    auto list = (GSList*)(NULL);
    for(const auto& tc : transport.candidate) {
        unwrap(type, candidate_type_to_nice(tc.type));
        auto nc = nice_candidate_new(type);
        if(const auto addr = str_to_sockaddr(tc.ip.data(), tc.port); addr.s.addr.sa_family != AF_UNSPEC) {
            nc->addr = addr;
        } else {
            LOG_ERROR(logger, "failed to parse candidate ip address");
            r = false;
            goto end;
        }
        nc->priority     = tc.priority;
        nc->stream_id    = stream_id;
        nc->component_id = tc.component;
        memcpy(nc->foundation, tc.foundation.data(), std::min(tc.foundation.size() + 1, size_t(NICE_CANDIDATE_MAX_FOUNDATION)));
        nc->username = g_strdup(transport.ufrag.data());
        nc->password = g_strdup(transport.pwd.data());

        list = g_slist_prepend(list, nc);
    }
    if(nice_agent_set_remote_candidates(agent, stream_id, component_id, list) != int(transport.candidate.size())) {
        LOG_ERROR(logger, "failed to add candidates");
        r = false;
        goto end;
    }
end:
    g_slist_free_full(list, (GDestroyNotify)nice_candidate_free);
    return r;
}
} // namespace

auto MainloopWithRunner::create() -> MainloopWithRunner* {
    const auto mainloop = g_main_loop_new(NULL, FALSE);
    ensure(mainloop != NULL, "failed to create mainloop");

    auto ret = new MainloopWithRunner();
    ret->mainloop.reset(mainloop);
    return ret;
}

auto MainloopWithRunner::start_runner() -> void {
    runner = std::thread(g_main_loop_run, mainloop.get());
}

MainloopWithRunner::~MainloopWithRunner() {
    if(runner.joinable()) {
        g_main_loop_quit(mainloop.get());
        runner.join();
    }
}

auto setup(const std::span<const xmpp::Service> external_services,
           const jingle::IceUdpTransport* const transport) -> std::optional<Agent> {
    auto mainloop = AutoMainloop(MainloopWithRunner::create());
    ensure(mainloop.get() != nullptr);
    const auto mainloop_ctx = g_main_loop_get_context(mainloop->mainloop.get());

    auto agent = AutoNiceAgent(nice_agent_new(mainloop_ctx, NICE_COMPATIBILITY_RFC5245));
    ensure(agent.get() != NULL, "failed to create nice agent");
    g_object_set(agent.get(),
                 "ice-tcp", FALSE,
                 "upnp", FALSE,
                 NULL);

    const auto stream_id    = nice_agent_add_stream(agent.get(), 1);
    const auto component_id = guint(1);
    ensure(stream_id > 0, "failed to add stream");
    ensure(set_stun_turn(agent.get(), external_services, stream_id, component_id),
           "failed to setup stun & turn servers");
    ensure(nice_agent_attach_recv(agent.get(), stream_id, component_id, mainloop_ctx, agent_recv_callback, nullptr) == TRUE,
           "failed to attach recv callback");
    if(transport) {
        ensure(nice_agent_set_remote_credentials(agent.get(), stream_id, transport->ufrag.data(), transport->pwd.data()) == TRUE,
               "failed to set credentials");
    }
    ensure(g_signal_connect(agent.get(), "candidate-gathering-done", G_CALLBACK(candidate_gathering_done), nullptr) > 0,
           "failed to register candidate-gathering-done callback");
    ensure(nice_agent_gather_candidates(agent.get(), stream_id) == TRUE,
           "failed to gather candidates");
    if(transport) {
        ensure(set_remote_candidates(agent.get(), *transport, stream_id, component_id), "failed to add candidates");
    }

    nice_debug_enable(logger.loglevel == Loglevel::Debug ? TRUE : FALSE);
    mainloop->start_runner();

    return Agent{
        .mainloop     = std::move(mainloop),
        .agent        = std::move(agent),
        .stream_id    = stream_id,
        .component_id = component_id,
    };
}

auto str_to_sockaddr(const char* const addr, const uint16_t port) -> NiceAddress {
    auto r = NiceAddress();
    if(inet_pton(AF_INET, addr, &r.s.ip4.sin_addr) == 1) {
        auto& ip      = r.s.ip4;
        ip.sin_family = AF_INET;
        ip.sin_port   = htons(port);
        return r;
    }
    if(inet_pton(AF_INET6, addr, &r.s.ip6.sin6_addr) == 1) {
        auto& ip         = r.s.ip6;
        ip.sin6_family   = AF_INET6;
        ip.sin6_port     = htons(port);
        ip.sin6_flowinfo = 0;
        ip.sin6_scope_id = 0;
        return r;
    }
    r.s.addr.sa_family = AF_UNSPEC;
    return r;
}

auto sockaddr_to_str(const NiceAddress& addr) -> std::string {
    // in order to avoid extra nulls in the conversion result, store it once in a buffer and convert it to a string.
    switch(addr.s.addr.sa_family) {
    case AF_INET: {
        auto buf = std::array<char, INET_ADDRSTRLEN>();
        inet_ntop(AF_INET, &addr.s.ip4.sin_addr, buf.data(), buf.size());
        return std::string(buf.data());
    }
    case AF_INET6: {
        auto buf = std::array<char, INET6_ADDRSTRLEN>();
        inet_ntop(AF_INET6, &addr.s.ip6.sin6_addr, buf.data(), buf.size());
        return std::string(buf.data());
    }
    default: {
        return {};
    }
    }
}

auto sockaddr_to_port(const NiceAddress& addr) -> uint16_t {
    switch(addr.s.addr.sa_family) {
    case AF_INET:
        return addr.s.ip4.sin_port;
    case AF_INET6:
        return addr.s.ip6.sin6_port;
    default:
        return -1;
    }
}

auto candidate_type_to_nice(const jingle::CandidateType type) -> std::optional<NiceCandidateType> {
    for(const auto& c : candidate_type_conv_table) {
        if(c.first == type) {
            return c.second;
        }
    }
    return std::nullopt;
}

auto candidate_type_from_nice(const NiceCandidateType type) -> std::optional<jingle::CandidateType> {
    for(const auto& c : candidate_type_conv_table) {
        if(c.second == type) {
            return c.first;
        }
    }
    return std::nullopt;
}

auto get_local_credentials(const Agent& agent) -> std::optional<LocalCredential> {
    auto ufrag = (gchar*)(NULL);
    auto pwd   = (gchar*)(NULL);
    ensure(nice_agent_get_local_credentials(agent.agent.get(), agent.stream_id, &ufrag, &pwd) == TRUE,
           "failed to get local credentials");
    return LocalCredential{AutoGChar(ufrag), AutoGChar(pwd)};
}

NiceCandidates::~NiceCandidates() {
    g_slist_free_full(list, (GDestroyNotify)nice_candidate_free);
}

auto get_local_candidates(const Agent& agent) -> NiceCandidates {
    auto r = std::vector<NiceCandidate*>();

    const auto list = nice_agent_get_local_candidates(agent.agent.get(), agent.stream_id, agent.component_id);
    for(auto item = list; item != NULL; item = item->next) {
        r.push_back(std::bit_cast<NiceCandidate*>(item->data));
    }
    return {list, std::move(r)};
}
} // namespace ice
