#include "tui/status_bar.h"

#include "tui/format.h"

using namespace ftxui;

namespace ratsn::tui {

namespace {

Element labeled(const std::string& label, const std::string& value)
{
    return hbox({ text(label) | dim, text(value) });
}

} // namespace

Element renderStatusBar(const StatusModel& model)
{
    return hbox({
               text("indexed " + std::to_string(model.indexStats.torrents) + " torrents ("
                   + humanSize(model.indexStats.totalSize) + ")"),
               text("  ·  "),
               text(model.dhtRunning ? "dht up (" + std::to_string(model.dhtNodes) + " nodes)" : "dht down")
                   | (model.dhtRunning ? color(Color::Green) : color(Color::Red)),
               text("  ·  "),
               text("spider pool=" + std::to_string(model.spiderPool) + " visited=" + std::to_string(model.spiderVisited)),
               text("  ·  "),
               text("discovered=" + std::to_string(model.discovered) + " fetching=" + std::to_string(model.activeFetches)),
           })
        | dim;
}

Element renderStatusTab(const StatusModel& model, const StatusInfo& info)
{
    return vbox({
        text("Node") | bold,
        labeled("  id:            ", info.nodeId.empty() ? "-" : info.nodeId),
        labeled("  listen port:   ", std::to_string(info.listenPort)),
        labeled("  data dir:      ", info.dataDir),
        labeled("  spider:        ", info.spiderEnabled ? "on" : "off"),
        labeled("  file index:    ", info.fileIndexEnabled ? "on" : "off"),
        separatorEmpty(),
        text("DHT / crawler") | bold,
        labeled("  dht:           ", model.dhtRunning ? "running" : "not running"),
        labeled("  routing table: ", std::to_string(model.dhtNodes) + " nodes"),
        labeled("  spider pool:   ", std::to_string(model.spiderPool)),
        labeled("  spider visited:", std::to_string(model.spiderVisited)),
        labeled("  discovered:    ", std::to_string(model.discovered)),
        labeled("  active fetches:", std::to_string(model.activeFetches)),
        separatorEmpty(),
        text("Index") | bold,
        labeled("  torrents:      ", std::to_string(model.indexStats.torrents)),
        labeled("  files:         ", std::to_string(model.indexStats.files)),
        labeled("  total size:    ", humanSize(model.indexStats.totalSize)),
    });
}

} // namespace ratsn::tui
