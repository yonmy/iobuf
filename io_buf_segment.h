#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>

using IoBufData_t = char;

// 원시 메모리를 실제 소유하는 원자 단위
class IoBufSegment
{ 
public:
    static constexpr uint32_t MIN_SIZE = 64;

    // 16384: Windows I/O (요청 fragmentation heap)가 안정적으로 다루는 크기이고,
    // 이보다 작으면 성능이 떨어진다고 알려진 경계다.
    static constexpr uint32_t DEFAULT_SIZE = 16384;
    static constexpr uint32_t MAX_SIZE = 8 * 1024 * 1024;

private:
    std::unique_ptr<IoBufData_t[]> data_;
    uint32_t size_ = 0;

public:
    // size는 [MIN_SIZE, MAX_SIZE]로 clamp된다.
    explicit IoBufSegment(uint32_t size = DEFAULT_SIZE);
    ~IoBufSegment() = default;

    // 복사 금지
    IoBufSegment(const IoBufSegment&) = delete;
    IoBufSegment& operator=(const IoBufSegment&) = delete;

    IoBufSegment(IoBufSegment&& rhs) noexcept;
    IoBufSegment& operator=(IoBufSegment&& rhs) noexcept;

    IoBufData_t* Data() noexcept { return data_.get(); }
    const IoBufData_t* Data() const noexcept { return data_.get(); }
    uint32_t Size() const noexcept { return size_; }

    IoBufData_t* begin() noexcept { return data_.get(); }
    IoBufData_t* end() noexcept { return data_.get() + size_; }
    const IoBufData_t* begin() const noexcept { return data_.get(); }
    const IoBufData_t* end() const noexcept { return data_.get() + size_; }

    void Swap(IoBufSegment& rhs) noexcept;
};
