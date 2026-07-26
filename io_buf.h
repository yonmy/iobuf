#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <list>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "io_buf_fragment.h"

template <typename Segment>
class IoBufPool;

// IoBuf — 물리적으로 흩어진 Fragment들을 하나의 논리 IO 버퍼처럼 다루는 뷰.
//
// 계층 정리 (io_buf_segment.h 포함):
//   IoBufSegment  : 원시 메모리를 실제 소유하는 원자 단위. 참조 카운트로 공유된다.
//   IoBufFragment : Segment의 [offset, offset + size) 구간만 보는 뷰.
//                   shared_ptr<Segment>로 참조 카운트를 공유한다.
//   IoBuf         : Fragment 리스트. 물리적으로 흩어져도 논리적으로는 하나의
//                   연속된 버퍼처럼 Split/Push로 다룰 수 있다.
//
// 이 계층은 아래 목적을 위해 존재한다.
//
//   수신: 커널이 받은 데이터를 PushBack으로 밀어넣고 Split으로 슬라이스해서 전달.
//   송신: 여러 개의 IoBuf를 PushBack으로 묶어 GetFragments()로 scatter-gather 배열로
//         변환해 WSASend / RIOSend에 넘긴다.

template <typename Fragment>
class IoBuf
{
public:
    using Fragment_t = Fragment;
    using Fragments_t = std::list<Fragment>;
    using Segment_t = typename Fragment::Segment_t;
    using SegmentPtr_t = typename Fragment::SegmentPtr_t;

    // IoBufPool::Coalesce가 fragments_에 직접 접근해 재구성할 수 있도록 허용한다.
    template <typename Segment> friend class IoBufPool;

private:
    // Fragment 개수와 관계없이 바이트 단위로 순회한다. 모든 forward_iterator를 만족하고
    // range-for와 알고리즘에 그대로 쓸 수 있다.
    template <bool IsConst>
    class BasicIterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = IoBufData_t;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<IsConst, const IoBufData_t&, IoBufData_t&>;
        using pointer = std::conditional_t<IsConst, const IoBufData_t*, IoBufData_t*>;

        using Fragments_t = std::conditional_t<IsConst, const typename IoBuf::Fragments_t, typename IoBuf::Fragments_t>;
        using ListIterator = std::conditional_t<IsConst,
            typename IoBuf::Fragments_t::const_iterator, typename IoBuf::Fragments_t::iterator>;

    private:
        Fragments_t* fragments_ = nullptr;
        ListIterator itr_{};
        uint32_t at_ = 0;

    public:
        BasicIterator() noexcept = default;
        BasicIterator(Fragments_t& fragments, ListIterator itr, uint32_t at) noexcept;

        reference operator*() const noexcept;
        pointer operator->() const noexcept;

        BasicIterator& operator++() noexcept;
        BasicIterator operator++(int) noexcept;

        bool operator==(const BasicIterator& rhs) const noexcept;
        bool operator!=(const BasicIterator& rhs) const noexcept;

        // 바이트 수만큼 건너뛰며, Fragment 경계를 넘으면 필요한 만큼 이동한다.
        BasicIterator& Advance(uint32_t n) noexcept;

        // 현재 위치가 속한 Fragment를 오른쪽으로 잘라주기 위해 반복자를 건네준다.
        ListIterator FragmentIterator() const noexcept { return itr_; }
        uint32_t RemainInFragment() const noexcept;

    private:
        // 소진된 Fragment를 건너뛰며 다음 유효한 위치를 가리키게 한다.
        // 이 불변식을 유지해야 operator*가 항상 유효한 위치를 참조한다.
        void Normalize() noexcept;
    };

public:
    using Iterator = BasicIterator<false>;
    using ConstIterator = BasicIterator<true>;

private:
    Fragments_t fragments_;

    // 전체 바이트 수 캐시. 매번 Fragment를 순회하지 않도록 모든 변경 시점에 갱신한다.
    uint32_t size_ = 0;

public:
    IoBuf() = default;
    ~IoBuf() = default;

    IoBuf(const IoBuf&) = default;
    IoBuf& operator=(const IoBuf&) = default;

    IoBuf(IoBuf&& rhs) noexcept;
    IoBuf& operator=(IoBuf&& rhs) noexcept;

    explicit IoBuf(const Fragment& fragment);
    explicit IoBuf(Fragments_t&& fragments);

    Iterator begin() noexcept;
    Iterator end() noexcept;
    ConstIterator begin() const noexcept;
    ConstIterator cbegin() const noexcept { return begin(); }
    ConstIterator end() const noexcept;
    ConstIterator cend() const noexcept { return end(); }

    uint32_t Size() const noexcept { return size_; }
    bool Empty() const noexcept { return (size_ == 0); }

    // scatter-gather 전송 시 배열 상한 검사는 io_buf_pool.h 참조.
    std::size_t FragmentCount() const noexcept { return fragments_.size(); }

    // 실제 WSABUF / RIO_BUF 배열을 만들기 위한 읽기 전용 접근.
    const Fragments_t& GetFragments() const noexcept { return fragments_; }

    void Swap(IoBuf& other) noexcept;
    void Clear() noexcept;

    // 인접한 Fragment끼리 병합한다. Split 등으로 잘게 쪼개진 채 남아있는 조각들을 정리할 때 쓴다.
    void Optimize() noexcept;

    // 뒤에 붙인다. 이전 마지막 Fragment와 인접하면(Push) 병합해서 넣고, 아니면 새 노드로 추가한다.
    void PushBack(const Fragment& fragment);
    void PushBack(Fragment&& fragment);
    void PushBack(const IoBuf& other);
    void PushBack(IoBuf&& other);

    Fragment PopFront() noexcept;

    // size 바이트를 떼어 새 IoBuf로 반환한다.
    // correctable == true 라면 남은 데이터가 size보다 적어도 있는 만큼만 반환한다.
    // false 라면 요청량을 채우지 못하면 아무것도 떼지 않고 빈 IoBuf를 반환한다.
    IoBuf Split(uint32_t size, bool correctable = true);

    void Rotate(uint32_t size);

    // offset에서 dstSize 바이트를 dst로 복사한다. Fragment 경계를 넘어도 처리한다.
    // 요청 범위가 데이터 크기를 넘으면 false를 반환한다.
    bool ReadMemory(void* dst, std::size_t dstSize, uint32_t offset = 0) const noexcept;

    // trivially copyable 타입을 offset에서 그대로 읽는다. 데이터가 부족하면 nullopt.
    template <typename T>
    std::optional<T> Read(uint32_t offset = 0) const noexcept;

    // terminate 문자가 나올 때까지 읽는다. 아직 도착하지 않았으면 nullopt.
    std::optional<std::string> ReadLine(char terminate = '\n') const noexcept;
};

// -----------------------------------------------------------------------
// BasicIterator 구현
// -----------------------------------------------------------------------

template <typename Fragment>
template <bool IsConst>
IoBuf<Fragment>::BasicIterator<IsConst>::BasicIterator(
    Fragments_t& fragments, ListIterator itr, uint32_t at) noexcept
    : fragments_(&fragments)
    , itr_(itr)
    , at_(at)
{
    Normalize();
}

template <typename Fragment>
template <bool IsConst>
void IoBuf<Fragment>::BasicIterator<IsConst>::Normalize() noexcept
{
    while (fragments_ != nullptr && itr_ != fragments_->end() && at_ >= itr_->Size())
    {
        at_ = 0;
        ++itr_;
    }
}

template <typename Fragment>
template <bool IsConst>
typename IoBuf<Fragment>::template BasicIterator<IsConst>::reference
IoBuf<Fragment>::BasicIterator<IsConst>::operator*() const noexcept
{
    // Normalize()가 유지하는 불변식 덕분에 여기서 별도 경계 검사가 필요 없다.
    return *(itr_->begin() + at_);
}

template <typename Fragment>
template <bool IsConst>
typename IoBuf<Fragment>::template BasicIterator<IsConst>::pointer
IoBuf<Fragment>::BasicIterator<IsConst>::operator->() const noexcept
{
    return (itr_->begin() + at_);
}

template <typename Fragment>
template <bool IsConst>
typename IoBuf<Fragment>::template BasicIterator<IsConst>&
IoBuf<Fragment>::BasicIterator<IsConst>::operator++() noexcept
{
    ++at_;
    Normalize();
    return *this;
}

template <typename Fragment>
template <bool IsConst>
typename IoBuf<Fragment>::template BasicIterator<IsConst>
IoBuf<Fragment>::BasicIterator<IsConst>::operator++(int) noexcept
{
    BasicIterator copied(*this);
    operator++();
    return copied;
}

template <typename Fragment>
template <bool IsConst>
typename IoBuf<Fragment>::template BasicIterator<IsConst>&
IoBuf<Fragment>::BasicIterator<IsConst>::Advance(uint32_t n) noexcept
{
    while (n > 0 && fragments_ != nullptr && itr_ != fragments_->end())
    {
        const uint32_t remain = itr_->Size() - at_;
        if (n < remain)
        {
            at_ += n;
            return *this;
        }

        n -= remain;
        at_ = 0;
        ++itr_;
        Normalize();
    }

    return *this;
}

template <typename Fragment>
template <bool IsConst>
uint32_t IoBuf<Fragment>::BasicIterator<IsConst>::RemainInFragment() const noexcept
{
    if (fragments_ == nullptr || itr_ == fragments_->end())
    {
        return 0;
    }

    return (itr_->Size() - at_);
}

template <typename Fragment>
template <bool IsConst>
bool IoBuf<Fragment>::BasicIterator<IsConst>::operator==(const BasicIterator& rhs) const noexcept
{
    return (itr_ == rhs.itr_ && at_ == rhs.at_);
}

template <typename Fragment>
template <bool IsConst>
bool IoBuf<Fragment>::BasicIterator<IsConst>::operator!=(const BasicIterator& rhs) const noexcept
{
    return !(operator==(rhs));
}

// -----------------------------------------------------------------------
// IoBuf 구현
// -----------------------------------------------------------------------

template <typename Fragment>
IoBuf<Fragment>::IoBuf(IoBuf&& rhs) noexcept
    : fragments_(std::move(rhs.fragments_))
    , size_(std::exchange(rhs.size_, 0))
{
    rhs.fragments_.clear();
}

template <typename Fragment>
IoBuf<Fragment>& IoBuf<Fragment>::operator=(IoBuf&& rhs) noexcept
{
    if (this != &rhs)
    {
        fragments_ = std::move(rhs.fragments_);
        size_ = std::exchange(rhs.size_, 0);
        rhs.fragments_.clear();
    }

    return *this;
}

template <typename Fragment>
IoBuf<Fragment>::IoBuf(const Fragment& fragment)
{
    PushBack(fragment);
}

template <typename Fragment>
IoBuf<Fragment>::IoBuf(Fragments_t&& fragments)
{
    for (auto& fragment : fragments)
    {
        PushBack(std::move(fragment));
    }

    fragments.clear();
}

template <typename Fragment>
typename IoBuf<Fragment>::Iterator IoBuf<Fragment>::begin() noexcept
{
    return Iterator(fragments_, fragments_.begin(), 0);
}

template <typename Fragment>
typename IoBuf<Fragment>::Iterator IoBuf<Fragment>::end() noexcept
{
    return Iterator(fragments_, fragments_.end(), 0);
}

template <typename Fragment>
typename IoBuf<Fragment>::ConstIterator IoBuf<Fragment>::begin() const noexcept
{
    return ConstIterator(fragments_, fragments_.begin(), 0);
}

template <typename Fragment>
typename IoBuf<Fragment>::ConstIterator IoBuf<Fragment>::end() const noexcept
{
    return ConstIterator(fragments_, fragments_.end(), 0);
}

template <typename Fragment>
void IoBuf<Fragment>::Swap(IoBuf& other) noexcept
{
    fragments_.swap(other.fragments_);
    std::swap(size_, other.size_);
}

template <typename Fragment>
void IoBuf<Fragment>::Clear() noexcept
{
    fragments_.clear();
    size_ = 0;
}

template <typename Fragment>
void IoBuf<Fragment>::Optimize() noexcept
{
    if (fragments_.size() < 2)
    {
        return;
    }

    auto itr = fragments_.begin();
    auto prev = itr++;
    while (itr != fragments_.end())
    {
        if (prev->Merge(*itr))
        {
            // 삭제하되 prev는 그대로 두어 다음 것과도 이어서 합칠 수 있게 한다.
            itr = fragments_.erase(itr);
        }
        else
        {
            prev = itr++;
        }
    }
}

template <typename Fragment>
void IoBuf<Fragment>::PushBack(const Fragment& fragment)
{
    if (fragment.Empty())
    {
        return;
    }

    if (!fragments_.empty() && fragments_.back().Push(fragment))
    {
        size_ += fragment.Size();
        return;
    }

    fragments_.push_back(fragment);
    size_ += fragment.Size();
}

template <typename Fragment>
void IoBuf<Fragment>::PushBack(Fragment&& fragment)
{
    if (fragment.Empty())
    {
        return;
    }

    const uint32_t added = fragment.Size();

    if (!fragments_.empty() && fragments_.back().Push(fragment))
    {
        size_ += added;
        return;
    }

    fragments_.push_back(std::move(fragment));
    size_ += added;
}

template <typename Fragment>
void IoBuf<Fragment>::PushBack(const IoBuf& other)
{
    for (const auto& fragment : other.fragments_)
    {
        PushBack(fragment);
    }
}

template <typename Fragment>
void IoBuf<Fragment>::PushBack(IoBuf&& other)
{
    for (auto& fragment : other.fragments_)
    {
        PushBack(std::move(fragment));
    }

    other.Clear();
}

template <typename Fragment>
Fragment IoBuf<Fragment>::PopFront() noexcept
{
    if (fragments_.empty())
    {
        return Fragment();
    }

    Fragment front = std::move(fragments_.front());
    fragments_.pop_front();
    size_ -= front.Size();

    return front;
}

template <typename Fragment>
IoBuf<Fragment> IoBuf<Fragment>::Split(uint32_t size, bool correctable)
{
    IoBuf ret;

    if (size_ < size)
    {
        if (!correctable)
        {
            return ret;
        }

        // 요청량보다 부족하다
        size = size_;
    }

    uint32_t remain = size;
    while (remain > 0 && !fragments_.empty())
    {
        Fragment& front = fragments_.front();
        if (remain < front.Size())
        {
            ret.PushBack(front.Split(remain));
            size_ -= remain;
            remain = 0;
        }
        else
        {
            remain -= front.Size();
            size_ -= front.Size();
            ret.PushBack(std::move(front));
            fragments_.pop_front();
        }
    }

    return ret;
}

template <typename Fragment>
void IoBuf<Fragment>::Rotate(uint32_t size)
{
    if (fragments_.empty() || size_ == 0)
    {
        return;
    }

    uint32_t remain = (size % size_);
    while (remain > 0 && !fragments_.empty())
    {
        Fragment& front = fragments_.front();
        if (remain < front.Size())
        {
            fragments_.push_back(front.Split(remain));
            remain = 0;
        }
        else
        {
            remain -= front.Size();
            fragments_.push_back(std::move(front));
            fragments_.pop_front();
        }
    }
}

template <typename Fragment>
bool IoBuf<Fragment>::ReadMemory(void* dst, std::size_t dstSize, uint32_t offset) const noexcept
{
    if (static_cast<std::size_t>(size_) < (dstSize + offset))
    {
        return false;
    }

    auto* out = static_cast<IoBufData_t*>(dst);
    std::size_t remain = dstSize;
    std::size_t skip = offset;

    for (const auto& fragment : fragments_)
    {
        if (remain == 0)
        {
            break;
        }

        const std::size_t fragmentSize = fragment.Size();
        if (skip >= fragmentSize)
        {
            skip -= fragmentSize;
            continue;
        }

        // 경계마다 memcpy 한 번으로 끝낸다.
        const std::size_t copied = std::min(fragmentSize - skip, remain);
        std::memcpy(out, fragment.begin() + skip, copied);

        out += copied;
        remain -= copied;
        skip = 0;
    }

    return true;
}

template <typename Fragment>
template <typename T>
std::optional<T> IoBuf<Fragment>::Read(uint32_t offset) const noexcept
{
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

    T value{};
    if (!ReadMemory(&value, sizeof(T), offset))
    {
        return std::nullopt;
    }

    return value;
}

template <typename Fragment>
std::optional<std::string> IoBuf<Fragment>::ReadLine(char terminate) const noexcept
{
    uint32_t length = 0;
    for (ConstIterator itr = begin(); itr != end(); ++itr, ++length)
    {
        if (*itr != terminate)
        {
            continue;
        }

        std::string line;
        if (length > 0)
        {
            line.resize(length);
            ReadMemory(line.data(), line.size());
        }

        return line;
    }

    return std::nullopt;
}
