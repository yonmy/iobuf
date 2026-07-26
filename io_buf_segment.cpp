#include "io_buf_segment.h"

#include <algorithm>
#include <utility>

IoBufSegment::IoBufSegment(uint32_t size)
    : size_(std::clamp(size, MIN_SIZE, MAX_SIZE))
{
    data_.reset(new IoBufData_t[size_]);
}

IoBufSegment::IoBufSegment(IoBufSegment&& rhs) noexcept
    : data_(std::move(rhs.data_))
    , size_(std::exchange(rhs.size_, 0))
{
}

IoBufSegment& IoBufSegment::operator=(IoBufSegment&& rhs) noexcept
{
    if (this != &rhs)
    {
        data_ = std::move(rhs.data_);
        size_ = std::exchange(rhs.size_, 0);
    }

    return *this;
}

void IoBufSegment::Swap(IoBufSegment& rhs) noexcept
{
    data_.swap(rhs.data_);
    std::swap(size_, rhs.size_);
}
