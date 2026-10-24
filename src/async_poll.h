// PollCore for a host that cannot block on a request: the browser, where a
// fetch answers in a callback (RFC 0030, section 2, "In the browser stage
// this becomes emscripten_fetch callbacks on the main thread").
//
// AsyncPoll does not re-implement the poll loop. advance() runs PollCore's
// own step() on a copy of the core, against a transport that answers from
// the responses fetched so far. When the copy asks for a path that has not
// been fetched yet, the copy is thrown away and advance() names the path; the
// host fetches it, hands the answer to provide(), and calls advance() again.
// When a copy gets through a whole step without a miss, it becomes the core
// and the cache is cleared. Each step is therefore PollCore's step exactly:
// the same requests in the same order, the same handling of every answer.
//
// The transport still has only get(): nothing here can send a write.
#pragma once

#include <map>
#include <memory>
#include <string>

#include "poller.h"

namespace viron_strata {

class AsyncPoll {
public:
    explicit AsyncPoll(PollConfig config);
    AsyncPoll(const AsyncPoll&) = delete;
    AsyncPoll& operator=(const AsyncPoll&) = delete;

    struct Next {
        bool needs = false;  // true: fetch `path`, provide() it, then advance() again
        std::string path;
        double delay = 0;    // when !needs: seconds until the next poll, negative once stopped
        bool stepped = false;  // when !needs: a whole step was taken (the core changed)
    };
    Next advance();
    void provide(const std::string& path, Response response);

    const PollCore& core() const { return *core_; }
    const Status& status() const { return core_->status(); }
    bool hasSnapshot() const { return core_->hasSnapshot(); }
    const Snapshot& snapshot() const { return core_->snapshot(); }
    // Responses fetched in the step under way.
    size_t cached() const { return cache_.ready.size(); }

private:
    class Cache : public Transport {
    public:
        std::map<std::string, Response> ready;
        std::string missed;
        Response get(const std::string& path) override;
    };
    Cache cache_;  // declared before core_: PollCore keeps a reference to it
    std::unique_ptr<PollCore> core_;
};

}  // namespace viron_strata
