#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <functional>

// One reservation and one copy per DMA plane. No allocation or IO in append.
// Readers may iterate only after ALL producers have joined.
class MemoryArena {
public:
    static size_t payloadCapacity(size_t budget, uint64_t available) {
        constexpr size_t workspace=32ULL<<20, reserve=256ULL<<20;
        if(budget<64ULL<<20 || budget>2048ULL<<20 || available<budget+reserve)
            throw std::runtime_error("Insufficient MemAvailable: max-mib (64..2048) plus 256 MiB SDK/system reserve required");
        return budget-workspace;
    }
    struct Header {
        uint64_t bytes, hostNs, getNs, copyNs;
        uint32_t stream, width, height, stride, frameId, format;
    };
    explicit MemoryArena(size_t capacity): capacity_(capacity), data_(new uint8_t[capacity]()) {}
    static size_t extent(size_t bytes) {
        if(bytes > SIZE_MAX-sizeof(Header)-63) throw std::overflow_error("frame too large");
        return (sizeof(Header)+bytes+63)&~size_t(63);
    }
    bool append(Header h, const void* source) {
        if(!source || !h.bytes) throw std::runtime_error("empty DMA plane");
        const size_t n=extent(h.bytes);
        size_t at=used_.load(std::memory_order_relaxed);
        do {
            if(at>capacity_ || n>capacity_-at) return false;
        } while(!used_.compare_exchange_weak(at,at+n,std::memory_order_relaxed));
        std::memcpy(data_.get()+at,&h,sizeof(h));
        std::memcpy(data_.get()+at+sizeof(h),source,h.bytes);
        return true;
    }
    template<class F> void each(F f) const {
        for(size_t at=0;at<used();) {
            Header h; std::memcpy(&h,data_.get()+at,sizeof(h));
            f(h,data_.get()+at+sizeof(h)); at+=extent(h.bytes);
        }
    }
    size_t used() const {return used_.load();}
    size_t capacity() const {return capacity_;}
    void* data() {return data_.get();}
private:
    size_t capacity_;
    std::unique_ptr<uint8_t[]> data_;
    std::atomic<size_t> used_{0};
};
