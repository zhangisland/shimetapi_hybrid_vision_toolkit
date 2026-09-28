#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
namespace hv_live {
using SyncClock=std::chrono::steady_clock;
inline int64_t hostNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(SyncClock::now().time_since_epoch()).count(); }
template<class T> struct Timed { int64_t ns=0; T value; };
// All receive timestamps must use the SAME host monotonic clock. No sensor time
// or inferred EVS subframe timestamps are mixed into this comparison.
template<class T> class FrameSync {
public:
    struct Pair { Timed<T> aps; std::optional<Timed<T>> evs; int64_t deltaNs=0; };
    struct Stats { uint64_t evsDropped=0, apsDropped=0, unmatched=0, matched=0; };
    FrameSync(int64_t toleranceNs, int64_t waitNs, size_t evsCapacity=16, size_t apsCapacity=3)
        : tolerance(toleranceNs), wait(waitNs), ec(evsCapacity), ac(apsCapacity) {
        if(tolerance<0 || wait<0 || !ec || !ac) throw std::invalid_argument("Invalid sync bounds");
    }
    void push(bool aps, Timed<T> frame) {
        std::lock_guard<std::mutex> l(mutex);
        auto& q=aps?a:e; const auto cap=aps?ac:ec;
        if(!q.empty() && frame.ns<q.back().ns) throw std::invalid_argument("Non-monotonic receive time");
        if(q.size()==cap) {q.pop_front(); if(aps) ++s.apsDropped; else ++s.evsDropped;}
        q.push_back(std::move(frame));
    }
    std::optional<Pair> poll(int64_t now) {
        std::lock_guard<std::mutex> l(mutex);
        if(a.empty()) return {};
        const auto target=a.front().ns;
        if((e.empty() || e.back().ns<target) && now-target<wait) return {};
        Pair p; p.aps=std::move(a.front()); a.pop_front();
        if(!e.empty()) {
            auto it=std::min_element(e.begin(),e.end(),[&](const auto& x,const auto& y){
                return std::abs(x.ns-target)<std::abs(y.ns-target); });
            p.deltaNs=it->ns-target;
            if(std::abs(p.deltaNs)<=tolerance) p.evs=*it;
        }
        if(p.evs) ++s.matched; else ++s.unmatched;
        // Retain a predecessor for the next APS; pruning is normal expiration,
        // while capacity evictions above are separately counted.
        while(e.size()>1 && e[1].ns<=target) e.pop_front();
        return p;
    }
    Stats stats() {std::lock_guard<std::mutex> l(mutex); return s;}
private:
    std::mutex mutex; std::deque<Timed<T>> e,a; Stats s;
    int64_t tolerance,wait; size_t ec,ac;
};
}
