#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

// Dmitry Vyukov의 bounded MPMC 큐.
//
// 고정 크기 링 버퍼 + 슬롯별 시퀀스 번호로, 여러 생산자/소비자가 락 없이
// 동시에 push/pop할 수 있다. capacity는 내부적으로 2의 거듭제곱으로 올림해서
// 인덱스 계산을 모듈로 대신 마스크 연산 하나로 끝낸다.
//
// TryPush/TryPop은 실패(가득 참 / 비어 있음) 시 false를 반환한다 — 블로킹하지 않는다.

template <typename T>
class MpmcBoundedQueue
{
private:
    struct Cell
    {
        std::atomic<std::size_t> Sequence{ 0 };
        T Data{};
    };

    static std::size_t RoundUpToPowerOfTwo(std::size_t n) noexcept
    {
        std::size_t p = 1;
        while (p < n)
        {
            p <<= 1;
        }

        return p;
    }

    std::size_t capacity_;
    std::size_t mask_;
    Cell* buffer_;

    // 두 커서를 서로 다른 캐시라인에 둬서 생산자/소비자가 false sharing으로
    // 서로의 진행을 늦추지 않게 한다.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    alignas(64) std::atomic<std::size_t> enqueuePos_{ 0 };
    alignas(64) std::atomic<std::size_t> dequeuePos_{ 0 };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

public:
    explicit MpmcBoundedQueue(std::size_t capacity)
        : capacity_(RoundUpToPowerOfTwo(capacity))
        , mask_(capacity_ - 1)
        , buffer_(new Cell[capacity_])
    {
        for (std::size_t i = 0; i < capacity_; ++i)
        {
            buffer_[i].Sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~MpmcBoundedQueue()
    {
        delete[] buffer_;
    }

    MpmcBoundedQueue(const MpmcBoundedQueue&) = delete;
    MpmcBoundedQueue& operator=(const MpmcBoundedQueue&) = delete;

    bool TryPush(T value) noexcept
    {
        Cell* cell = nullptr;
        std::size_t pos = enqueuePos_.load(std::memory_order_relaxed);

        for (;;)
        {
            cell = &buffer_[pos & mask_];
            const std::size_t seq = cell->Sequence.load(std::memory_order_acquire);
            const std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);

            if (diff == 0)
            {
                // 이 슬롯은 지금 비어서 쓸 수 있는 상태다. CAS로 내 자리를 확정한다.
                if (enqueuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    break;
                }
            }
            else if (diff < 0)
            {
                return false;  // 큐가 가득 찼다.
            }
            else
            {
                // 다른 생산자가 먼저 pos를 가져갔다. 최신 값으로 다시 시도.
                pos = enqueuePos_.load(std::memory_order_relaxed);
            }
        }

        cell->Data = std::move(value);
        cell->Sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool TryPop(T& out) noexcept
    {
        Cell* cell = nullptr;
        std::size_t pos = dequeuePos_.load(std::memory_order_relaxed);

        for (;;)
        {
            cell = &buffer_[pos & mask_];
            const std::size_t seq = cell->Sequence.load(std::memory_order_acquire);
            const std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);

            if (diff == 0)
            {
                if (dequeuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    break;
                }
            }
            else if (diff < 0)
            {
                return false;  // 큐가 비어 있다.
            }
            else
            {
                pos = dequeuePos_.load(std::memory_order_relaxed);
            }
        }

        out = std::move(cell->Data);

        // 이 슬롯을 다음 바퀴(pos + capacity)에서 다시 채울 수 있도록 시퀀스를 넘긴다.
        cell->Sequence.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    // 통계 출력용 근사치다. enqueue/dequeue 카운터를 각각 다른 시점에 읽으므로
    // 동시성 하에서는 정확한 순간 크기와 다를 수 있다.
    std::size_t SizeApprox() const noexcept
    {
        const std::size_t enq = enqueuePos_.load(std::memory_order_relaxed);
        const std::size_t deq = dequeuePos_.load(std::memory_order_relaxed);
        return (enq >= deq) ? (enq - deq) : 0;
    }
};
