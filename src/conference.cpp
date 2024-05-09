#include "conference.hpp"
#include "assert.hpp"
#include "base64.hpp"
#include "caps.hpp"
#include "config.hpp"
#include "jingle/jingle.hpp"
#include "sha.hpp"
#include "unwrap.hpp"
#include "util/misc.hpp"
#include "xmpp/elements.hpp"

namespace conference {
namespace {
auto to_span(const std::string_view str) -> std::span<std::byte> {
    return std::span<std::byte>(std::bit_cast<std::byte*>(str.data()), str.size());
}

constexpr auto disco_node = "https://misskey.io/@mojyack";
const auto     disco_info = xmpp::elm::query.clone()
                            .append_children({
                                xmpp::elm::identity.clone()
                                    .append_attrs({
                                        {"category", "client"},
                                        {"name", "libjitsimeet"},
                                        {"type", "bot"},
                                        {"xml:lang", "en"},
                                    }),
                                xmpp::elm::feature.clone()
                                    .append_attrs({
                                        {"var", "http://jabber.org/protocol/disco#info"},
                                    }),
                                xmpp::elm::feature.clone()
                                    .append_attrs({
                                        {"var", "urn:xmpp:jingle:apps:rtp:video"},
                                    }),
                                xmpp::elm::feature.clone()
                                    .append_attrs({
                                        {"var", "urn:xmpp:jingle:apps:rtp:audio"},
                                    }),
                                xmpp::elm::feature.clone()
                                    .append_attrs({
                                        {"var", "urn:xmpp:jingle:transports:ice-udp:1"},
                                    }),
                                xmpp::elm::feature.clone()
                                    .append_attrs({
                                        {"var", "urn:xmpp:jingle:apps:dtls:0"},
                                    }),
                                xmpp::elm::feature.clone()
                                    .append_attrs({
                                        {"var", "urn:ietf:rfc:5888"},
                                    }),
                                xmpp::elm::feature.clone()
                                    .append_attrs({
                                        {"var", "urn:ietf:rfc:5761"},
                                    }),
                                xmpp::elm::feature.clone()
                                    .append_attrs({
                                        {"var", "urn:ietf:rfc:4588"},
                                    }),
                                xmpp::elm::feature.clone()
                                    .append_attrs({
                                        {"var", "http://jitsi.org/tcc"},
                                    }),
                            });

auto handle_iq_get(Conference* const conf, const xml::Node& iq) -> bool {
    unwrap_ob(from, iq.find_attr("from"));
    unwrap_ob(id, iq.find_attr("id"));
    unwrap_pb(query, iq.find_first_child("query"));
    auto iqr = xmpp::elm::iq.clone()
                   .append_attrs({
                       {"from", conf->jid.as_full()},
                       {"to", std::string(from)},
                       {"id", std::string(id)},
                       {"type", "result"},
                   });
    const auto node = query.find_attr("node");
    if(node.has_value()) {
        const auto sep = node->rfind("#");
        if(sep == std::string::npos) {
            return false;
        }
        const auto uri  = node->substr(0, sep);
        const auto hash = node->substr(sep + 1);
        if(uri != disco_node || hash != conf->disco_sha1_base64) {
            return false;
        }

        iqr.append_children({
            disco_info.clone()
                .append_attrs({
                    {"node", std::string(*node)},
                }),
        });
    } else {
        iqr.append_children({
            disco_info,
        });
    }
    conf->callbacks->send_payload(xml::deparse(iqr));
    return true;
}

auto handle_iq_set(Conference* const conf, const xml::Node& iq) -> bool {
    unwrap_ob(from, iq.find_attr("from"));
    unwrap_ob(from_jid, xmpp::Jid::parse(from));
    if(from_jid.resource != "focus") {
        return false;
    }
    unwrap_ob(id, iq.find_attr("id"));
    unwrap_pb(jingle_node, iq.find_first_child("jingle"));
    unwrap_ob_mut(jingle, jingle::parse(jingle_node));

    if(config::debug_conference) {
        PRINT("jingle action ", int(jingle.action));
    }
    switch(jingle.action) {
    case jingle::Jingle::Action::SessionInitiate:
        conf->callbacks->on_jingle_initiate(std::move(jingle));
        goto ack;
    case jingle::Jingle::Action::SourceAdd:
        conf->callbacks->on_jingle_add_source(std::move(jingle));
        goto ack;
    default:
        WARN("unimplemented jingle action: ", int(jingle.action));
        return true;
    }

ack:
    const auto iqr = xmpp::elm::iq.clone()
                         .append_attrs({
                             {"from", conf->jid.as_full()},
                             {"to", std::string(from)},
                             {"id", std::string(id)},
                             {"type", "result"},
                         });
    conf->callbacks->send_payload(xml::deparse(iqr));
    return true;
}

auto handle_iq_result(Conference* const conf, const xml::Node& iq, bool success) -> bool {
    unwrap_ob(id, iq.find_attr("id"));
    for(auto i = conf->sent_iqs.begin(); i != conf->sent_iqs.end(); i += 1) {
        if(i->id != id) {
            continue;
        }
        if(!success) {
            WARN("iq ", id, " failed");
        }
        if(i->on_result) {
            i->on_result(success);
        }
        conf->sent_iqs.erase(i);
        return true;
    }
    WARN("stray iq result");
    return false;
}

auto handle_iq(Conference* const conf, const xml::Node& iq) -> bool {
    unwrap_ob(type, iq.find_attr("type"));
    if(type == "get") {
        return handle_iq_get(conf, iq);
    } else if(type == "set") {
        return handle_iq_set(conf, iq);
    } else if(type == "result") {
        return handle_iq_result(conf, iq, true);
    } else if(type == "error") {
        return handle_iq_result(conf, iq, false);
    }
    return false;
}

auto handle_presence(Conference* const conf, const xml::Node& presence) -> bool {
    const auto response = xml::parse(conf->worker_arg).as_value();

    unwrap_ob(from_str, presence.find_attr("from"));
    unwrap_ob(from, xmpp::Jid::parse(from_str));
    if(config::debug_conference) {
        PRINT("got presence from ", from_str);
    }
    if(const auto type = presence.find_attr("type"); type) {
        if(*type == "unavailable") {
            if(const auto i = conf->participants.find(from.resource); i != conf->participants.end()) {
                conf->callbacks->on_participant_left(i->second);
                conf->participants.erase(i);
            } else {
                WARN("got unavailable presence from unknown participant");
            }
        }
        return true;
    }

    auto participant = (Participant*)(nullptr);
    auto joined      = false;
    if(const auto i = conf->participants.find(from.resource); i == conf->participants.end()) {
        const auto& p = conf->participants.insert({from.resource,
                                                   Participant{
                                                       .participant_id = from.resource,
                                                       .nick           = "",
                                                   }});
        participant   = &p.first->second;
        joined        = true;
    } else {
        participant = &i->second;
    }

    for(const auto& payload : presence.children) {
        if(payload.name == xmpp::elm::nick.name && payload.is_attr_equal("xmlns", xmpp::ns::nick)) {
            participant->nick = payload.data;
        }
    }

    if(joined) {
        conf->callbacks->on_participant_joined(*participant);
    }

    return true;
}

auto handle_received(Conference* const conf) -> Conference::Worker::Generator {
    // disco
    {
        const auto id = conf->generate_iq_id();

        // DEBUG
        const auto muid = "0cf847e2-4e3b-4271-b847-8e79c82e872a";
        const auto iq   = xmpp::elm::iq.clone()
                            .append_attrs({
                                {"to", conf->focus_jid.as_full()},
                                {"id", id},
                                {"type", "set"},
                            })
                            .append_children({
                                xmpp::elm::conference.clone()
                                    .append_attrs({
                                        {"machine-uid", muid},
                                        {"room", conf->muc_jid.as_bare()},
                                    })
                                    .append_children({
                                        xmpp::elm::property.clone()
                                            .append_attrs({
                                                {"stereo", "false"},
                                            }),
                                        xmpp::elm::property.clone()
                                            .append_attrs({
                                                {"startBitrate", "800"},
                                            }),
                                    }),
                            });
        conf->callbacks->send_payload(xml::deparse(iq));
        co_yield true;

        const auto response = xml::parse(conf->worker_arg).as_value();
        DYN_ASSERT(response.name == "iq", "unexpected response");
        DYN_ASSERT(response.is_attr_equal("id", id), "unexpected iq");
        DYN_ASSERT(response.is_attr_equal("type", "result"), "unexpected iq");
        const auto conference = response.find_first_child("conference");
        DYN_ASSERT(conference != nullptr);
        DYN_ASSERT(conference->is_attr_equal("ready", "true"), "conference not ready");
    }
    // presence
    {
        // DEBUG
        const auto presence =
            xmpp::elm::presence.clone()
                .append_attrs({
                    {"to", conf->muc_local_jid.as_full()},
                })
                .append_children({
                    xmpp::elm::muc,
                    xmpp::elm::caps.clone()
                        .append_attrs({
                            {"hash", "sha-1"},
                            {"node", disco_node},
                            {"ver", conf->disco_sha1_base64},
                        }),
                    xmpp::elm::ecaps2.clone()
                        .append_children({
                            xmpp::elm::hash.clone()
                                .set_data(conf->disco_sha256_base64)
                                .append_attrs({
                                    {"algo", "sha-256"},
                                }),
                        }),
                    xml::Node{
                        .name = "stats-id",
                        .data = "libjitsimeet",
                    },
                    xml::Node{
                        .name = "jitsi_participant_codecType",
                        // DEBUG
                        .data = "h264",
                    },
                    xml::Node{
                        .name = "videomuted",
                        .data = "false",
                    },
                    xml::Node{
                        .name = "audiomuted",
                        .data = "false",
                    },
                    xmpp::elm::nick.clone()
                        // DEBUG
                        .set_data("mojyack"),
                });

        conf->callbacks->send_payload(xml::deparse(presence));
        co_yield true;
    }

    // idle
    auto buffer = std::string();
loop:
    auto yield = false;
    do {
        buffer += conf->worker_arg;
        const auto response_r = xml::parse(buffer);
        if(!response_r) {
            if(response_r.as_error() != xml::Error::Incomplete) {
                buffer.clear();
                WARN("xml parse error: ", int(response_r.as_error()));
            }
            break;
        }
        buffer.clear();
        const auto& response = response_r.as_value();
        if(response.name == "iq") {
            yield = handle_iq(conf, response);
        } else if(response.name == "presence") {
            yield = handle_presence(conf, response);
        } else {
            WARN("not implemented xmpp message ", response.name);
        }
    } while(0);
    co_yield yield;
    goto loop;
}

auto jid_node_to_muc_resource(const std::string_view node) -> std::string {
    for(const auto elm : split(node, "-")) {
        if(!elm.empty()) {
            return std::string(elm);
        }
    }
    return std::string(node);
}
} // namespace

auto Conference::generate_iq_id() -> std::string {
    return build_string("iq_", (iq_serial += 1));
}

auto Conference::start_negotiation() -> void {
    worker.start(handle_received, this);
    worker.resume();
}

auto Conference::feed_payload(const std::string_view payload) -> bool {
    worker_arg = payload;
    worker.resume();
    return worker.done();
}

auto Conference::send_iq(xml::Node node, std::function<void(bool)> on_result) -> void {
    const auto id = generate_iq_id();
    node.append_attrs({{"id", id}});
    sent_iqs.push_back(SentIq{
        .id        = id,
        .on_result = on_result,
    });
    callbacks->send_payload(xml::deparse(node));
}

auto Conference::create(const std::string_view     room,
                        const xmpp::Jid&           jid,
                        ConferenceCallbacks* const callbacks) -> std::unique_ptr<Conference> {
    const auto muc_domain = std::string("conference.") + jid.domain;

    auto conf = new Conference{
        .jid       = jid,
        .focus_jid = xmpp::Jid{
            .node     = "focus",
            .domain   = std::string("auth.") + jid.domain,
            .resource = "focus",
        },
        .muc_jid = xmpp::Jid{
            .node     = std::string(room),
            .domain   = muc_domain,
            .resource = "",
        },
        .muc_local_jid = xmpp::Jid{
            .node     = std::string(room),
            .domain   = muc_domain,
            .resource = jid_node_to_muc_resource(jid.node),
        },
        .muc_local_focus_jid = xmpp::Jid{
            .node     = std::string(room),
            .domain   = muc_domain,
            .resource = "focus",
        },
        .callbacks = callbacks,
    };

    const auto disco_str      = compute_disco_str(disco_info);
    const auto disco_sha1     = sha::calc_sha1(to_span(disco_str));
    const auto disco_sha256   = sha::calc_sha256(to_span(disco_str));
    conf->disco_sha1_base64   = base64::encode(disco_sha1);
    conf->disco_sha256_base64 = base64::encode(disco_sha256);
    return std::unique_ptr<Conference>(conf);
}
} // namespace conference
