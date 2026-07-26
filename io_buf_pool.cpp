#include "io_buf_pool.h"

#include <algorithm>

std::vector<IoBufSegmentTierConfig> IoBufSegmentTierConfig::NormalizeTierConfigs(
    std::span<const IoBufSegmentTierConfig> _configs)
{
    // 설정 목록을 크기 오름차순으로 정렬하고, 같은 크기는 MaxCount가 큰 쪽만 남긴다.

    std::vector<IoBufSegmentTierConfig> normalized;
    normalized.reserve(_configs.size());

    for (const auto& config : _configs)
    {
        // 최소 크기 미만인 설정은 무시하고 진행한다.
        if (config.bufferSize < IoBufSegment::MIN_SIZE)
        {
            continue;
        }

        normalized.push_back(config);
    }

    // Acquire가 lower_bound로 티어를 검색하고, 크기 오름차순 정렬을 전제로 한다.
    // 같은 크기가 두 개 이상이면 MaxCount가 큰 것만 남게, 아래 unique에서 그것이 살아남는다.
    std::sort(
        normalized.begin(), normalized.end(),
        [](const IoBufSegmentTierConfig& lhs, const IoBufSegmentTierConfig& rhs)
        {
            if (lhs.bufferSize != rhs.bufferSize)
            {
                return lhs.bufferSize < rhs.bufferSize;
            }

            return lhs.maxCount > rhs.maxCount;
        });

    normalized.erase(
        std::unique(
            normalized.begin(), normalized.end(),
            [](const IoBufSegmentTierConfig& lhs, const IoBufSegmentTierConfig& rhs)
            {
                return lhs.bufferSize == rhs.bufferSize;
            }),
        normalized.end());

    // InitialCount가 MaxCount를 넘으면 예외를 던지는 대신 상한으로 캐스팅한다.
    for (auto& config : normalized)
    {
        config.initialCount = std::min(config.initialCount, config.maxCount);
    }

    return normalized;
}
