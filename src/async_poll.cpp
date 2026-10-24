#include "async_poll.h"

namespace viron_strata {

Response AsyncPoll::Cache::get(const std::string& path) {
    const auto it = ready.find(path);
    if (it != ready.end()) return it->second;
    if (missed.empty()) missed = path;
    Response r;  // status 0: the copy that sees this is thrown away
    r.error = "not fetched yet";
    return r;
}

AsyncPoll::AsyncPoll(PollConfig config) : core_(std::make_unique<PollCore>(cache_, std::move(config))) {}

AsyncPoll::Next AsyncPoll::advance() {
    Next next;
    if (isStopped(core_->status().state)) {
        next.delay = -1;
        return next;
    }
    auto trial = std::make_unique<PollCore>(*core_);
    cache_.missed.clear();
    const double delay = trial->step();
    if (!cache_.missed.empty()) {
        next.needs = true;
        next.path = cache_.missed;
        return next;
    }
    core_ = std::move(trial);
    cache_.ready.clear();
    next.delay = delay;
    next.stepped = true;
    return next;
}

void AsyncPoll::provide(const std::string& path, Response response) { cache_.ready[path] = std::move(response); }

}  // namespace viron_strata
