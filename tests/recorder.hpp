#pragma once

#include <string>
#include <vector>

#include "matchbook/events.hpp"

namespace matchbook::testing {

// Renders an event stream to strings so two implementations can be compared
// exactly, including ordering.
inline std::string render(const Event& event) {
    switch (event.kind) {
        case Event::Kind::Accepted:
            return "ACC " + std::to_string(event.accepted.id) + " " +
                   std::to_string(event.accepted.price) + " " +
                   std::to_string(event.accepted.quantity);
        case Event::Kind::Rejected:
            return "REJ " + std::to_string(event.rejected.id) + " " +
                   std::to_string(static_cast<int>(event.rejected.reason));
        case Event::Kind::Trade:
            return "TRD " + std::to_string(event.trade.taker) + " " +
                   std::to_string(event.trade.maker) + " " + std::to_string(event.trade.price) +
                   " " + std::to_string(event.trade.quantity);
        case Event::Kind::Canceled:
            return "CXL " + std::to_string(event.canceled.id) + " " +
                   std::to_string(event.canceled.remaining);
        case Event::Kind::Replaced:
            return "RPL " + std::to_string(event.replaced.old_id) + " " +
                   std::to_string(event.replaced.new_id);
    }
    return "???";
}

struct Recorder {
    std::vector<std::string> log;
    std::vector<Sequence> sequences;

    void operator()(const Event& event) {
        log.push_back(render(event));
        sequences.push_back(event.sequence);
    }

    [[nodiscard]] std::size_t size() const { return log.size(); }
    [[nodiscard]] const std::string& back() const { return log.back(); }
    void clear() {
        log.clear();
        sequences.clear();
    }
};

}  // namespace matchbook::testing
