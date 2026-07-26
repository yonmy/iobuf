#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include "io_buf_segment.h"

//
// IoBufFragment - Segment의 부분 구간을 가리키는 view.
//
// [segment, offset, size] 세 값만 있으면 이 데이터를 표현한다. Segment가
// shared_ptr<Segment>로 참조 카운트를 공유한다는 뜻이다. Fragment가 여러 개
// 살아있어도 서로 겹치지 않는 구간만 가리킨다. 수신 쪽 하나에서 조각을 내면
// 메모리를 새로 복사할 필요 없이 io_buf_segment.h의 실제 주소를 참조한다.
//
// Fragment가 Segment에 요구하는 것은 Size() / begin() / Data() 뿐이다.
//
template <typename Segment>
class IoBufFragment
{
public:
    using Segment_t = Segment;
    using SegmentPtr_t = std::shared_ptr<Segment>;

private:
    SegmentPtr_t segment_;
    uint32_t offset_ = 0;
    uint32_t size_ = 0;

public:
    IoBufFragment() noexcept = default;

    // offset / size는 범위 안으로 clamp된다. segment가 null이거나 크기가 0이면
    // 빈 Fragment가 된다 — 호출측이 항상 조건을 걸러내지 않아도 view로 안전하게 처리된다.
    IoBufFragment(
        SegmentPtr_t segment,
        uint32_t offset = 0,
        uint32_t size = std::numeric_limits<uint32_t>::max()) noexcept;

    IoBufFragment(const IoBufFragment&) = default;
    IoBufFragment& operator=(const IoBufFragment&) = default;

    IoBufFragment(IoBufFragment&& rhs) noexcept;
    IoBufFragment& operator=(IoBufFragment&& rhs) noexcept;

    IoBufData_t* begin() noexcept;
    IoBufData_t* end() noexcept;
    const IoBufData_t* begin() const noexcept;
    const IoBufData_t* end() const noexcept;

    IoBufData_t* Data() noexcept { return begin(); }
    const IoBufData_t* Data() const noexcept { return begin(); }

    const Segment_t* GetSegment() const noexcept { return segment_.get(); }
    uint32_t Offset() const noexcept { return offset_; }
    uint32_t Size() const noexcept { return size_; }
    bool Empty() const noexcept { return (size_ == 0); }

    void Swap(IoBufFragment& rhs) noexcept;
    void Clear() noexcept;

    // 앞쪽 size 바이트를 떼어 반환하고, 자신은 남은 뒤 구간만 갖는다.
    // 두 Fragment는 같은 Segment의 서로 다른 구간만 가리킨다.
    //
    // correctable == false 이면 요청 크기만큼 없을 때(size_ < size) 실패로
    // 빈 Fragment를 반환한다. correctable == true 이면 있는 만큼만 잘라 준다.
    IoBufFragment Split(uint32_t size, bool correctable = true) noexcept;

    // -- Merge와 Push를 나눈 이유 --------------------------------------------
    //
    // Merge: 같은 Segment의 두 구간이 겹치지 않고 인접해야 성립한다(순서 무관).
    //        합쳤을 때 크기가 두 구간의 실제 합과 다르면(사이 간극/겹침) 실패.
    //
    // Push: 순서 전제, 자신의 끝이 other의 시작과 맞닿아 있을 때만 넓힌다.
    //       IoBuf::PushBack에서 인접 Fragment를 병합할 때 쓰인다.
    bool Merge(const IoBufFragment& other) noexcept;
    bool Push(const IoBufFragment& other) noexcept;
};

// -----------------------------------------------------------------------
// 구현
// -----------------------------------------------------------------------

template <typename Segment>
IoBufFragment<Segment>::IoBufFragment(SegmentPtr_t segment, uint32_t offset, uint32_t size) noexcept
{
    if (!segment)
    {
        return;
    }

    const uint32_t capacity = segment->Size();
    if (capacity == 0)
    {
        return;
    }

    offset_ = std::min(offset, capacity);
    size_ = std::min(size, capacity - offset_);
    segment_ = std::move(segment);
}

template <typename Segment>
IoBufFragment<Segment>::IoBufFragment(IoBufFragment&& rhs) noexcept
    : segment_(std::move(rhs.segment_))
    , offset_(std::exchange(rhs.offset_, 0))
    , size_(std::exchange(rhs.size_, 0))
{
}

template <typename Segment>
IoBufFragment<Segment>& IoBufFragment<Segment>::operator=(IoBufFragment&& rhs) noexcept
{
    if (this != &rhs)
    {
        segment_ = std::move(rhs.segment_);
        offset_ = std::exchange(rhs.offset_, 0);
        size_ = std::exchange(rhs.size_, 0);
    }

    return *this;
}

template <typename Segment>
IoBufData_t* IoBufFragment<Segment>::begin() noexcept
{
    assert(segment_);
    return (segment_->begin() + offset_);
}

template <typename Segment>
IoBufData_t* IoBufFragment<Segment>::end() noexcept
{
    return (begin() + size_);
}

template <typename Segment>
const IoBufData_t* IoBufFragment<Segment>::begin() const noexcept
{
    assert(segment_);
    return (segment_->begin() + offset_);
}

template <typename Segment>
const IoBufData_t* IoBufFragment<Segment>::end() const noexcept
{
    return (begin() + size_);
}

template <typename Segment>
void IoBufFragment<Segment>::Swap(IoBufFragment& rhs) noexcept
{
    segment_.swap(rhs.segment_);
    std::swap(offset_, rhs.offset_);
    std::swap(size_, rhs.size_);
}

template <typename Segment>
void IoBufFragment<Segment>::Clear() noexcept
{
    segment_.reset();
    offset_ = 0;
    size_ = 0;
}

template <typename Segment>
IoBufFragment<Segment> IoBufFragment<Segment>::Split(uint32_t size, bool correctable) noexcept
{
    if (size_ < size && !correctable)
    {
        return IoBufFragment();
    }

    const uint32_t cut = std::min(size_, size);
    IoBufFragment head(segment_, offset_, cut);

    offset_ += cut;
    size_ -= cut;

    return head;
}

template <typename Segment>
bool IoBufFragment<Segment>::Merge(const IoBufFragment& other) noexcept
{
    if (!segment_ || segment_ != other.segment_)
    {
        return false;
    }

    const uint32_t mergedBegin = std::min(offset_, other.offset_);
    const uint32_t mergedEnd = std::max(offset_ + size_, other.offset_ + other.size_);
    const uint32_t mergedSize = mergedEnd - mergedBegin;

    if (mergedSize > (size_ + other.size_))
    {
        return false;
    }

    offset_ = mergedBegin;
    size_ = mergedSize;

    return true;
}

template <typename Segment>
bool IoBufFragment<Segment>::Push(const IoBufFragment& other) noexcept
{
    if (!segment_ || segment_ != other.segment_)
    {
        return false;
    }

    if ((offset_ + size_) != other.offset_)
    {
        return false;
    }

    size_ += other.size_;
    return true;
}
