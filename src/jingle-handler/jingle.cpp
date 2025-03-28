#include <iomanip>

#include "../crypto/sha.hpp"
#include "../jingle/jingle.hpp"
#include "../macros/logger.hpp"
#include "../random.hpp"
#include "../util/charconv.hpp"
#include "../util/pair-table.hpp"
#include "cert.hpp"
#include "jingle.hpp"
#include "pem.hpp"

#define CUTIL_MACROS_PRINT_FUNC(...) LOG_ERROR(logger, __VA_ARGS__)
#include "../macros/unwrap.hpp"

namespace {
auto logger = Logger("jingle");

template <class T>
auto replace_default(T& num, const T val) -> void {
    num = num == -1 ? val : num;
}

const auto source_type_str = make_pair_table<SourceType, std::string_view>({
    {SourceType::Audio, "audio"},
    {SourceType::Video, "video"},
});

const auto codec_type_str = make_pair_table<CodecType, std::string_view>({
    {CodecType::Opus, "opus"},
    {CodecType::H264, "H264"},
    {CodecType::Vp8, "VP8"},
    {CodecType::Vp9, "VP9"},
    {CodecType::Av1, "AV1"},
});

struct DescriptionParseResult {
    std::vector<Codec> codecs;

    int video_hdrext_transport_cc     = -1;
    int audio_hdrext_transport_cc     = -1;
    int audio_hdrext_ssrc_audio_level = -1;
};

auto parse_rtp_description(const jingle::RTPDescription& desc, SSRCMap& ssrc_map) -> std::optional<DescriptionParseResult> {
    unwrap(media, desc.media);
    unwrap(source_type, source_type_str.find(media), "unknown media {}", media);
    auto r = DescriptionParseResult{};

    // parse codecs
    for(const auto& pt : desc.payload_type) {
        if(pt.name == "rtx") {
            continue;
        }
        unwrap(name, pt.name);
        if(const auto e = codec_type_str.find(name); e != nullptr) {
            auto codec = Codec{
                .type     = *e,
                .tx_pt    = pt.id,
                .rtx_pt   = -1,
                .rtcp_fbs = pt.rtcp_fb,
            };
            r.codecs.push_back(codec);
        } else {
            LOG_WARN(logger, "unknown codec {}", name);
        }
    }
    // parse retransmission payload types
    for(const auto& pt : desc.payload_type) {
        if(pt.name != "rtx") {
            continue;
        }
        for(const auto& p : pt.parameter) {
            if(p.name != "apt") {
                continue;
            }
            const auto apt = from_chars<int>(p.value);
            if(!apt) {
                LOG_WARN(logger, "invalid apt {}", p.value);
                continue;
            }
            for(auto& codec : r.codecs) {
                if(codec.tx_pt == *apt) {
                    codec.rtx_pt = pt.id;
                    break;
                }
            }
            break;
        }
    }
    // parse extensions
    for(const auto& ext : desc.rtp_header_ext) {
        if(ext.uri == rtp_hdrext_ssrc_audio_level_uri) {
            r.audio_hdrext_ssrc_audio_level = ext.id;
        } else if(ext.uri == rtp_hdrext_transport_cc_uri) {
            switch(source_type) {
            case SourceType::Audio:
                r.audio_hdrext_transport_cc = ext.id;
                break;
            case SourceType::Video:
                r.video_hdrext_transport_cc = ext.id;
                break;
            }
        } else {
            LOG_WARN(logger, "unsupported rtp header extension {}", ext.uri);
        }
    }
    // parse ssrc
    for(const auto& source : desc.source) {
        ssrc_map[source.ssrc] = Source{
            .ssrc           = source.ssrc,
            .type           = source_type,
            .participant_id = source.ssrc_info[0].owner,
        };
    }
    return r;
}

auto digest_str(const std::span<const std::byte> digest) -> std::string {
    auto ss = std::stringstream();
    ss << std::hex;
    ss << std::uppercase;
    for(const auto b : digest) {
        ss << std::setw(2) << std::setfill('0') << static_cast<int>(b) << ":";
    }
    auto r = ss.str();
    r.pop_back();
    return r;
}
} // namespace

auto JingleSession::find_codec_by_type(const CodecType type) const -> const Codec* {
    for(const auto& codec : codecs) {
        if(codec.type == type) {
            return &codec;
        }
    }
    return nullptr;
}

auto JingleSession::find_codec_by_tx_pt(const int tx_pt) const -> const Codec* {
    for(const auto& codec : codecs) {
        if(codec.tx_pt == tx_pt) {
            return &codec;
        }
    }
    return nullptr;
}

auto JingleHandler::get_session() const -> const JingleSession& {
    return session;
}

auto JingleHandler::build_accept_jingle() const -> std::optional<jingle::Jingle> {
    auto& jingle = session.initiate_jingle;

    auto accept = jingle::Jingle{
        .action    = jingle::Action::SessionAccept,
        .sid       = jingle.sid,
        .initiator = jingle.initiator,
        .responder = jid.as_full(),
    };
    for(auto i = 0; i < 2; i += 1) {
        const auto is_audio   = i == 0;
        const auto codec_type = is_audio ? audio_codec_type : video_codec_type;
        const auto main_ssrc  = is_audio ? session.audio_ssrc : session.video_ssrc;

        // build rtp description
        auto rtp_desc = jingle::RTPDescription{
            .media = is_audio ? "audio" : "video",
            .ssrc  = main_ssrc,
        };
        // append payload type
        unwrap(codec, session.find_codec_by_type(codec_type));
        rtp_desc.payload_type.push_back(jingle::PayloadType{
            .id        = codec.tx_pt,
            .clockrate = is_audio ? 48000 : 90000,
            .channels  = is_audio ? std::optional(2) : std::nullopt,
            .name      = std::string(*codec_type_str.find(codec_type)),
            .rtcp_fb   = codec.rtcp_fbs,
        });
        if(codec.rtx_pt != -1) {
            auto rtx_pt = jingle::PayloadType{
                .id        = codec.rtx_pt,
                .clockrate = is_audio ? 48000 : 90000,
                .channels  = is_audio ? std::optional(2) : std::nullopt,
                .name      = "rtx",
                .parameter = {{"apt", std::to_string(codec.tx_pt)}},
            };
            for(const auto& fb : codec.rtcp_fbs) {
                if(fb.type != "transport-cc") {
                    rtx_pt.rtcp_fb.push_back(fb);
                }
            }
            rtp_desc.payload_type.push_back(std::move(rtx_pt));
        }
        // append source
        rtp_desc.source.push_back(jingle::Source{.ssrc = main_ssrc});
        if(!is_audio) {
            rtp_desc.source.push_back(jingle::Source{.ssrc = session.video_rtx_ssrc});
        }
        const auto stream_id = rng::generate_random_uint32();
        const auto label     = std::format("stream_label_{}", stream_id);
        const auto mslabel   = std::format("multi_stream_label_{}", stream_id);
        const auto msid      = std::format("{} {}", mslabel, label);
        const auto cname     = std::format("cname_{}", stream_id);
        for(auto& src : rtp_desc.source) {
            src.parameter.push_back({"cname", cname});
            src.parameter.push_back({"msid", msid});
        }
        // append hdrext
        if(is_audio) {
            rtp_desc.rtp_header_ext.push_back(
                jingle::RTPHeaderExt{
                    .id  = session.audio_hdrext_ssrc_audio_level,
                    .uri = rtp_hdrext_ssrc_audio_level_uri,
                });
            rtp_desc.rtp_header_ext.push_back(
                jingle::RTPHeaderExt{
                    .id  = session.audio_hdrext_transport_cc,
                    .uri = rtp_hdrext_transport_cc_uri,
                });
        } else {
            rtp_desc.rtp_header_ext.push_back(
                jingle::RTPHeaderExt{
                    .id  = session.video_hdrext_transport_cc,
                    .uri = rtp_hdrext_transport_cc_uri,
                });
        }
        // append ssrc-group
        if(!is_audio) {
            rtp_desc.ssrc_group.push_back(jingle::SSRCGroup{
                .semantics = jingle::SSRCSemantics::Fid,
                .source    = {{session.video_ssrc}, {session.video_rtx_ssrc}},
            });
        }
        // rtp description done

        // build transport
        auto transport = jingle::IceUdpTransport{
            .pwd   = session.local_cred.pwd.get(),
            .ufrag = session.local_cred.ufrag.get(),
        };
        // add candidates
        const auto local_candidates = ice::get_local_candidates(session.ice_agent);
        for(const auto lc : local_candidates.candidates) {
            unwrap(type, ice::candidate_type_from_nice(lc->type));
            const auto addr = ice::sockaddr_to_str(lc->addr);
            ensure(!addr.empty());
            const auto  port                = ice::sockaddr_to_port(lc->addr);
            static auto candidate_id_serial = std::atomic_int(0);
            transport.candidate.push_back(jingle::Candidate{
                .component  = uint8_t(lc->component_id),
                .generation = 0,
                .port       = port,
                .priority   = lc->priority,
                .type       = type,
                .foundation = lc->foundation,
                .id         = std::format("candidate_{}", candidate_id_serial.fetch_add(1)),
                .ip         = addr,
                .protocol   = "udp",
            });
        }
        // add fingerprint
        transport.fingerprint.push_back(jingle::FingerPrint{
            .hash     = "sha-256",
            .setup    = "active",
            .required = "false",
            .data     = session.fingerprint_str,
        });
        // transport done

        accept.content.push_back(
            jingle::Content{
                .name        = is_audio ? "audio" : "video",
                .senders     = jingle::Senders::Both,
                .creator     = "responder",
                .description = {rtp_desc},
                .transport   = {transport},
            });
    }

    accept.group.emplace_back(jingle::Group{
        .semantics = jingle::GroupSemantics::Bundle,
        .content   = {{"audio"}, {"video"}},
    });

    return accept;
}

auto JingleHandler::on_initiate(jingle::Jingle jingle) -> bool {
    auto codecs                        = std::vector<Codec>();
    auto ssrc_map                      = SSRCMap();
    auto video_hdrext_transport_cc     = -1;
    auto audio_hdrext_transport_cc     = -1;
    auto audio_hdrext_ssrc_audio_level = -1;
    auto transport                     = (const jingle::IceUdpTransport*)(nullptr);
    for(const auto& c : jingle.content) {
        for(const auto& d : c.description) {
            unwrap(desc, parse_rtp_description(d, ssrc_map));
            codecs.insert(codecs.end(), desc.codecs.begin(), desc.codecs.end());
            replace_default(video_hdrext_transport_cc, desc.video_hdrext_transport_cc);
            replace_default(audio_hdrext_transport_cc, desc.audio_hdrext_transport_cc);
            replace_default(audio_hdrext_ssrc_audio_level, desc.audio_hdrext_ssrc_audio_level);
        }
        if(!c.transport.empty()) {
            transport = &c.transport[0];
        }
    }

    const auto cert = cert::AutoCert(cert::cert_new());
    ensure(cert);
    const auto cert_der = cert::serialize_cert_der(cert.get());
    ensure(cert_der);
    const auto priv_key_der = cert::serialize_private_key_pkcs8_der(cert.get());
    ensure(priv_key_der);
    unwrap(fingerprint, crypto::sha::calc_sha256(*cert_der));
    auto fingerprint_str = digest_str(fingerprint);
    auto cert_pem        = pem::encode("CERTIFICATE", *cert_der);
    auto priv_key_pem    = pem::encode("PRIVATE KEY", *priv_key_der);
    LOG_DEBUG(logger, "fingerprint: {}", fingerprint_str.data());
    LOG_DEBUG(logger, "cert: {}", cert_pem.data());
    LOG_DEBUG(logger, "priv_key: {}", priv_key_pem.data());

    const auto audio_ssrc     = rng::generate_random_uint32();
    const auto video_ssrc     = rng::generate_random_uint32();
    const auto video_rtx_ssrc = rng::generate_random_uint32();

    unwrap_mut(ice_agent, ice::setup(external_services, transport));
    unwrap_mut(local_cred, ice::get_local_credentials(ice_agent));

    session = JingleSession{
        .initiate_jingle               = std::move(jingle),
        .ice_agent                     = std::move(ice_agent),
        .local_cred                    = std::move(local_cred),
        .fingerprint_str               = std::move(fingerprint_str),
        .dtls_cert_pem                 = std::move(cert_pem),
        .dtls_priv_key_pem             = std::move(priv_key_pem),
        .codecs                        = std::move(codecs),
        .ssrc_map                      = std::move(ssrc_map),
        .audio_ssrc                    = audio_ssrc,
        .video_ssrc                    = video_ssrc,
        .video_rtx_ssrc                = video_rtx_ssrc,
        .video_hdrext_transport_cc     = video_hdrext_transport_cc,
        .audio_hdrext_transport_cc     = audio_hdrext_transport_cc,
        .audio_hdrext_ssrc_audio_level = audio_hdrext_ssrc_audio_level,
    };

    // session initiation half-done
    // wakeup mainthread to create pipeline
    sync->notify();

    return true;
}

auto JingleHandler::on_add_source(jingle::Jingle jingle) -> bool {
    for(const auto& c : jingle.content) {
        for(const auto& desc : c.description) {
            if(!desc.media) {
                continue;
            }
            const auto& media = desc.media.value();
            auto        type  = SourceType();
            if(media == "audio") {
                type = SourceType::Audio;
            } else if(media == "video") {
                type = SourceType::Video;
            } else {
                LOG_WARN(logger, "unknown media {}", media);
                continue;
            }
            for(const auto& src : desc.source) {
                session.ssrc_map.insert(std::make_pair(src.ssrc, Source{
                                                                     .ssrc           = src.ssrc,
                                                                     .type           = type,
                                                                     .participant_id = src.ssrc_info[0].owner,
                                                                 }));
            }
        }
    }
    return true;
}

JingleHandler::JingleHandler(const CodecType                audio_codec_type,
                             const CodecType                video_codec_type,
                             xmpp::Jid                      jid,
                             std::span<const xmpp::Service> external_services,
                             coop::SingleEvent* const       sync)
    : sync(sync),
      audio_codec_type(audio_codec_type),
      video_codec_type(video_codec_type),
      jid(std::move(jid)),
      external_services(external_services) {
}
