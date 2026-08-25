#pragma once

#include "librats/util/json.h"

#include <cstdint>
#include <string>

// Statistics a rats-search peer advertises about itself during the
// "client_info" mesh handshake. Port of src/domain/peer.h -- field names kept
// verbatim (wire compat).
namespace ratsn::domain {

struct PeerStats {
    std::string clientVersion;
    int64_t torrents = 0;
    int64_t files = 0;
    int64_t totalSize = 0;
    int peersConnected = 0;
    int64_t connectedAt = 0; // ms since epoch, set locally on connect

    librats::Json toJson() const
    {
        librats::Json obj = librats::Json::object();
        obj["clientVersion"] = clientVersion;
        obj["torrents"] = torrents;
        obj["files"] = files;
        obj["totalSize"] = totalSize;
        obj["peersConnected"] = peersConnected;
        obj["connectedAt"] = connectedAt;
        return obj;
    }

    static PeerStats fromJson(const librats::Json& obj)
    {
        PeerStats s;
        if (!obj.is_object())
            return s;
        s.clientVersion = obj.value("clientVersion", "");
        // Accept both the new keys and the legacy *Count spellings.
        s.torrents = obj.contains("torrents") ? obj.value("torrents", int64_t { 0 })
                                               : obj.value("torrentsCount", int64_t { 0 });
        s.files = obj.contains("files") ? obj.value("files", int64_t { 0 }) : obj.value("filesCount", int64_t { 0 });
        s.totalSize = obj.value("totalSize", int64_t { 0 });
        s.peersConnected = obj.value("peersConnected", 0);
        s.connectedAt = obj.value("connectedAt", int64_t { 0 });
        return s;
    }
};

} // namespace ratsn::domain
