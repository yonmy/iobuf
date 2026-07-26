#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "io_buf.h"
#include "mpmc_bounded_queue.h"

struct IoBufSegmentTierConfig
{
	uint32_t bufferSize = 0; // 이 티어가 나눠주는 Segment의 크기
	uint32_t initialCount = 0; // 시작 시점에 미리 만들어 둘 개수
	uint32_t maxCount = 0; // 만들 수 있는 Segment 총 개수 상한

    static std::vector<IoBufSegmentTierConfig> NormalizeTierConfigs(
        std::span<const IoBufSegmentTierConfig> _configs);
};

// IoBufPool - 크기 티어별 Segment 캐시 풀
//
// 각 티어: 초기 개수만큼 미리 만들어 둔다. 반환된 개수가 부족하면 새로 할당하거나, 상한에 걸리면 풀과 무관하게 새로 만들어 반환한다.
// 반환 방식: Acquire가 돌려주는 shared_ptr에 deleter가 담겨 있다.

template <typename Segment>
class IoBufPool
{
public:
    using Segment_t = Segment;
    using SegmentPtr_t = std::shared_ptr<Segment>;

    struct TierStats
    {
        uint32_t bufferSize = 0;
        int64_t allocated = 0;
        std::size_t pooled = 0;
    };

private:
    struct Tier
    {
        IoBufSegmentTierConfig config;
        MpmcBoundedQueue<Segment*> free;
        std::atomic<int64_t> allocated{0};

        explicit Tier(const IoBufSegmentTierConfig& _config)
            : config(_config)
            , free(std::max<std::size_t>(1, _config.maxCount))
        {
            // 큐 용량은 MaxCount로 고정한다 — 반납되는 Segment 수는 Allocated(<= MaxCount)를
            // 절대 넘지 않으므로 이 이상 커질 일이 없다.
        }
    };

    // mutex가 이동 불가능한 타입이라 vector에 담을때 unique_ptr<Tier>로 감싼다.
    std::vector<std::unique_ptr<Tier>> tiers_;

public:
    explicit IoBufPool(std::span<const IoBufSegmentTierConfig> _configs);
    ~IoBufPool();

    IoBufPool(const IoBufPool&) = delete;
    IoBufPool& operator=(const IoBufPool&) = delete;

    // InitialCount만큼 미리 채워둔다. 스레드가 실서비스로 도는 전에 한 번만 호출한다.
    void Initialize();

    // size 이상을 담을 수 있는 가장 작은 티어에서 발급한다.
    // 티어 내 풀이 비었고, 이 티어의 상한도 다 찼으면, 상한과 무관한 임시
    // 세그먼트를 만들어 반환한다. 이 경우엔 풀로 돌아오지 않고 그냥 delete된다.
    SegmentPtr_t Acquire(uint32_t size);

    // 풀링되는 최대 크기. 이 크기를 넘으면 Coalesce로도 병합 대상이 안 된다.
    uint32_t MaxPoolingSize() const noexcept;

    // Fragment 개수가 maxFragmentCount를 넘으면 풀에서 큰 세그먼트를 빌려
    // 여러 조각을 합쳐 채운다. 병합이 불가능한 경우(필요한 최소 개수도
    // maxFragmentCount를 넘는 경우 등) false를 반환하고 buf는 원래 상태를 유지한다.
    template <typename Fragment>
    bool Coalesce(IoBuf<Fragment>& buf, std::size_t maxFragmentCount);

    std::vector<TierStats> Stats() const;

private:
    // deleter에서 호출된다. tierIndex의 Segment* segment를 되돌린다.
    void Release(std::size_t tierIndex, Segment* segment) noexcept;
};

//------------------------------------------------------------------------
// 구현
//------------------------------------------------------------------------

template <typename Segment>
IoBufPool<Segment>::IoBufPool(std::span<const IoBufSegmentTierConfig> _configs)
{
    for (const auto& config : IoBufSegmentTierConfig::NormalizeTierConfigs(_configs))
    {
        tiers_.push_back(std::make_unique<Tier>(config));
    }
}

template <typename Segment>
IoBufPool<Segment>::~IoBufPool()
{
    for (auto& tier : tiers_)
    {
        Segment* segment = nullptr;
        while (tier->free.TryPop(segment))
        {
            delete segment;
        }
    }
}

template <typename Segment>
void IoBufPool<Segment>::Initialize()
{
    for (auto& tier : tiers_)
    {
        const int64_t target = static_cast<int64_t>(tier->config.initialCount);
        const int64_t already = tier->allocated.load(std::memory_order_relaxed);

        for (int64_t i = already; i < target; ++i)
        {
            Segment* segment = new Segment(tier->config.bufferSize);
            if (!tier->free.TryPush(segment))
            {
                // 큐 용량은 MaxCount로 잡혀 있으니 여기 도달할 일은 없다. 안전망일 뿐이다.
                delete segment;
                break;
            }

            tier->allocated.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

template <typename Segment>
typename IoBufPool<Segment>::SegmentPtr_t IoBufPool<Segment>::Acquire(uint32_t size)
{
    // size 이상을 담을 수 있는 첫 티어. 티어가 크기 오름차순으로 정렬되어 있음!
    auto itr = std::lower_bound(
        tiers_.begin(), tiers_.end(), size,
        [](const std::unique_ptr<Tier>& tier, uint32_t requested)
        {
            return tier->config.bufferSize < requested;
        });

    if (itr == tiers_.end())
    {
        return nullptr;
    }

    Tier& tier = **itr;
    const std::size_t tierIndex = static_cast<std::size_t>(std::distance(tiers_.begin(), itr));

    // deleter가 풀로 돌려준다.
    auto toPool = [this, tierIndex](Segment* segment) noexcept
    {
        Release(tierIndex, segment);
    };

    if (Segment* segment = nullptr; tier.free.TryPop(segment))
    {
        return SegmentPtr_t(segment, toPool);
    }

    // 풀이 비었다. 상한 안에서만 새로 만든다.
    if (const int64_t allocated = tier.allocated.fetch_add(1, std::memory_order_relaxed) + 1;
        allocated <= static_cast<int64_t>(tier.config.maxCount))
    {
        return SegmentPtr_t(new Segment(tier.config.bufferSize), toPool);
    }

    tier.allocated.fetch_sub(1, std::memory_order_relaxed);

    // 이 티어 상한에도 걸렸다. 풀과 무관한 임시 세그먼트를 반환한다.
    return std::make_shared<Segment>(size);
}

template <typename Segment>
void IoBufPool<Segment>::Release(std::size_t tierIndex, Segment* segment) noexcept
{
    if (segment == nullptr)
    {
        return;
    }

    Tier& tier = *tiers_[tierIndex];

    if (!tier.free.TryPush(segment))
    {
        // 큐가 가득 찼다는 건 상한 계산이 깨졌다는 뜻이다 — 누수 대신 안전하게 지운다.
        delete segment;
    }
}

template <typename Segment>
uint32_t IoBufPool<Segment>::MaxPoolingSize() const noexcept
{
    if (tiers_.empty())
    {
        return 0;
    }

    return tiers_.back()->config.bufferSize;
}

template <typename Segment>
template <typename Fragment>
bool IoBufPool<Segment>::Coalesce(IoBuf<Fragment>& buf, std::size_t maxFragmentCount)
{
    if (buf.FragmentCount() <= maxFragmentCount)
    {
        return true;
    }

    const uint32_t chunk = MaxPoolingSize();
    if (chunk == 0)
    {
        return false;
    }

    // 도달 가능한 최소 개수는 총 바이트 수를 블록 크기로 나눈 값이다.
    // 그마저 상한을 넘으면 복사해도 소용이 없으므로 시도하지 않는다.
    const std::size_t minimum = (buf.Size() + chunk - 1) / chunk;
    if (minimum > maxFragmentCount)
    {
        return false;
    }

    typename IoBuf<Fragment>::Fragments_t packed;
    auto& source = buf.fragments_;

    auto itr = source.begin();
    uint32_t consumed = 0;

    while (itr != source.end())
    {
        auto segment = Acquire(chunk);
        const uint32_t capacity = segment->Size();

        // 블록을 끝까지 채운다. Fragment 경계에서 멈추지 않고 쪼개어 담는 것이 중요하다
        // 경계를 존중하면 예를 들어 9KB Fragment 2개가 16KB 블록 하나에 들어가지 못하고
        // 복사만 하고 개수는 그대로인 결과가 나온다. 이미 복사하는 중이므로
        // 경계를 넘겨 채우는 추가 비용은 없고, 결과 개수는 위의 minimum과 정확히 같아진다.
        uint32_t filled = 0;
        while (itr != source.end() && filled < capacity)
        {
            const uint32_t available = itr->Size() - consumed;
            const uint32_t copied = std::min(available, capacity - filled);

            if (copied > 0)
            {
                std::memcpy(segment->Data() + filled, itr->begin() + consumed, copied);
                filled += copied;
                consumed += copied;
            }

            if (consumed == itr->Size())
            {
                ++itr;
                consumed = 0;
            }
        }

        if (filled == 0)
        {
            break;
        }

        packed.emplace_back(std::move(segment), 0, filled);
    }

    buf = IoBuf<Fragment>(std::move(packed));

    return (buf.FragmentCount() <= maxFragmentCount);
}

template <typename Segment>
std::vector<typename IoBufPool<Segment>::TierStats> IoBufPool<Segment>::Stats() const
{
    std::vector<TierStats> stats;
    stats.reserve(tiers_.size());

    for (const auto& tier : tiers_)
    {
        TierStats one;
        one.bufferSize = tier->config.bufferSize;
        one.allocated = tier->allocated.load(std::memory_order_relaxed);
        one.pooled = tier->free.SizeApprox();

        stats.push_back(one);
    }

    return stats;
}
