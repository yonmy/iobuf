#include <cassert>
#include <concepts>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>

#include "io_buf.h"
#include "io_buf_pool.h"

#if defined(_WIN32)
#include <windows.h>
#endif

using Fragment_t = IoBufFragment<IoBufSegment>;
using IoBuf_t = IoBuf<Fragment_t>;
using IoBufPool_t = IoBufPool<IoBufSegment>;

namespace
{

int g_failed = 0;

void Check(bool condition, const char* what)
{
    std::cout << "  [";
    if (condition)
    {
        std::cout << "OK";
    }
    else
    {
        std::cout << "FAIL";
        ++g_failed;
    }

    std::cout << "]" << what << std::endl;
}

std::vector<IoBufSegmentTierConfig> MakeTiers()
{
    return {
        {256, 4, 64},
        {4096, 4, 32},
        {16384, 1, 16},
    };
}

uint32_t FillPattern(IoBufSegment& segment, uint32_t offset, uint32_t size, char seed)
{
    for (uint32_t i = 0; i < size; ++i)
    {
        *(segment.Data() + offset + i) = static_cast<char>(seed + static_cast<char>(i % 26));
    }

    return size;
}

// 1. 복사 없는 분할
void TestZeroCopySplit(IoBufPool_t& pool)
{
    std::cout << "(1) 복사 없는 분할" << std::endl;
    // 하나의 수신 블록을 나눠도, 잘라낸 조각의 주소가 원본 블록 내부를 그대로 가리키는지 확인한다.

    auto segment = pool.Acquire(256);
    const char* origin = segment->Data();

    const uint32_t sizes[] = {40, 60, 50};
    uint32_t written = 0;
    for (int i = 0; i < 3; ++i)
    {
        written += FillPattern(*segment, written, sizes[i], static_cast<char>('A' + i));
    }

    IoBuf_t stream(Fragment_t(segment, 0, written));
    Check(stream.Size() == 150, "수신 스트림 150바이트");
    Check(stream.FragmentCount() == 1, "Fragment 1개");

    IoBuf_t packet = stream.Split(40);
    Check(packet.Size() == 40, "첫 40바이트 분리");
    Check(stream.Size() == 110, "잔여 110바이트");
    Check(packet.GetFragments().front().begin() == origin, "패킷의 첫 블록은 원본을 가리킴");
    Check(stream.GetFragments().front().begin() == origin + 40, "잔여도 원본 블록을 가리킴");

    // 이미 다 소진했을 때: correctable=false 면 아무것도 떼지 않는다.
    IoBuf_t partial = stream.Split(999, false);
    Check(partial.Empty(), "부족한 분할 시도는 빈 결과");
    Check(stream.Size() == 110, "실패 시 원본 그대로");

    // correctable=true 면 보유한 만큼만 반환한다.
    IoBuf_t drained = stream.Split(999, true);
    Check(drained.Size() == 110, "보정 옵션은 보유한 만큼");
    Check(stream.Empty(), "원본 비워짐");
}

void TestAdjacencyMerge(IoBufPool_t& pool)
{
    std::cout << "(2) 인접 병합" << std::endl;
    // 같은 블록에서 연달아 잘린 조각을 다시 붙여도 인접한 것만 병합된다.
    // 서로 다른 블록끼리는 Optimize()가 병합하지 않는다(Fragment::Merge).

    auto segment = pool.Acquire(256);
    FillPattern(*segment, 0, 120, 'a');

    IoBuf_t accumulated;
    accumulated.PushBack(Fragment_t(segment, 0, 40));
    accumulated.PushBack(Fragment_t(segment, 40, 40));
    accumulated.PushBack(Fragment_t(segment, 80, 40));
    Check(accumulated.Size() == 120, "누적 크기 120바이트");
    Check(accumulated.FragmentCount() == 1, "인접한 조각은 노드 1개로 뭉침");

    IoBuf_t shuffled;
    shuffled.PushBack(Fragment_t(segment, 60, 60));
    shuffled.PushBack(Fragment_t(segment, 0, 60));
    Check(shuffled.FragmentCount() == 2, "역순 인접은 뭉치지 않음");
    shuffled.Optimize();
    Check(shuffled.FragmentCount() == 1, "Optimize가 인접임을 병합");
    Check(shuffled.Size() == 120, "병합 후에도 총량 유지");

    // 서로 다른 블록은 안 섞인다.
    auto other = pool.Acquire(256);
    FillPattern(*other, 0, 40, 'z');
    IoBuf_t crossed;
    crossed.PushBack(Fragment_t(segment, 0, 40));
    crossed.PushBack(Fragment_t(other, 0, 40));
    crossed.Optimize();
    Check(crossed.FragmentCount() == 2, "다른 블록끼리는 병합 불가");
}

void TestTraversal(IoBufPool_t& pool)
{
    std::cout << "(2) 경계를 넘는 순회와 읽기" << std::endl;

    // 세 개의 서로 다른 블록에 "HELLO / WORLD\n / TAIL" 을 나눠 담는다.
    auto a = pool.Acquire(256);
    auto b = pool.Acquire(256);
    auto c = pool.Acquire(256);
    std::memcpy(a->Data(), "HELLO ", 6);
    std::memcpy(b->Data(), "WORLD\n", 6);
    std::memcpy(c->Data(), "TAIL", 4);

    IoBuf_t buf;
    buf.PushBack(Fragment_t(a, 0, 6));
    buf.PushBack(Fragment_t(b, 0, 6));
    buf.PushBack(Fragment_t(c, 0, 4));
    Check(buf.FragmentCount() == 3, "3개 블록에 걸친 버퍼");
    Check(buf.Size() == 16, "총 16바이트");

    // range-for가 Fragment 경계를 넘어서 바이트 단위로 순회한다.
    std::string joined;
    for (char ch : buf)
    {
        joined.push_back(ch);
    }

    Check(joined == "HELLO WORLD\nTAIL", "순회 결과가 순서와 일치");

    // 경계를 걸친 구간 읽기
    char window[8] = {};
    Check(buf.ReadMemory(window, 7, 3), "경계 걸쳐 읽기 성공");
    Check(std::string(window, 7) == "LO WORL", "경계 걸친 내용 읽기");

    // 헤더용 텍스트 라인(PROXY protocol 식 헤더)
    auto line = buf.ReadLine('\n');
    Check(line.has_value() && *line == "HELLO WORLD", "ReadLine이 구분자 앞까지 반환");

    // 고정폭 값 읽기 — offset(4)에서 두 바이트를 ReadMemory와 비교한다.
    auto word = buf.Read<uint16_t>(4);
    uint16_t viaMemory = 0;
    buf.ReadMemory(&viaMemory, sizeof(viaMemory), 4);
    Check(word.has_value() && *word == viaMemory, "Read<T>가 ReadMemory와 일치");
    Check(word.has_value() && (*word & 0xFF) == 'O', "첫 바이트가 'O'");

    // 임의 위치로 전진
    auto itr = buf.begin();
    itr.Advance(12);
    Check(*itr == 'T', "Advance로 세 번째 블록 진입");
}

void TestCoalesce(IoBufPool_t& pool)
{
    std::cout << "(4) 백엔드 제약에 맞춘 병합" << std::endl;
    // Fragment 개수가 상한을 넘으면 전송 직전에 병합되는지 확인한다.
    // 
    // 서로 다른 블록 20개로 구성된 버퍼 — 병합 없이는 20개다.
    IoBuf_t sendBuf;
    std::string expected;
    for (int i = 0; i < 20; ++i)
    {
        auto segment = pool.Acquire(256);
        const uint32_t size = 32;
        FillPattern(*segment, 0, size, static_cast<char>('a' + (i % 26)));
        expected.append(segment->Data(), size);
        sendBuf.PushBack(Fragment_t(segment, 0, size));
    }

    Check(sendBuf.FragmentCount() == 20, "병합 전 Fragment 20개");
    Check(sendBuf.Size() == 640, "총 640바이트");

    // 상한 16개 병합
    Check(pool.Coalesce(sendBuf, 16), "16개 이하로 병합 성공");
    Check(sendBuf.FragmentCount() <= 16, "16개 이하로 축소");
    Check(sendBuf.Size() == 640, "병합해도 내용은 보존");

    std::string actual(sendBuf.Size(), '\0');
    Check(sendBuf.ReadMemory(actual.data(), actual.size()), "병합 결과 읽기 성공");
    Check(actual == expected, "병합 후에도 바이트가 동일");

    // sendBuf는 이미 16384바이트 청크 1개(가장 큰 티어)에 다 들어갈 만큼 작아서
    // 8개/1개 제한을 의미 있게 시험할 수 없다. 최소 필요 청크 수가 1보다 크도록
    // 훨씬 큰 버퍼(60000바이트, 5000바이트 세그먼트 12개)를 따로 만든다.
    IoBuf_t rioBuf;
    std::string rioExpected;
    for (int i = 0; i < 12; ++i)
    {
        auto segment = pool.Acquire(5000);
        const uint32_t size = 5000;
        FillPattern(*segment, 0, size, static_cast<char>('a' + (i % 26)));
        rioExpected.append(segment->Data(), size);
        rioBuf.PushBack(Fragment_t(segment, 0, size));
    }

    Check(rioBuf.FragmentCount() == 12, "병합 전 Fragment 12개");
    Check(rioBuf.Size() == 60000, "총 60000바이트");

    // 60000바이트는 16384바이트 청크 4개가 필요하다(ceil(60000/16384)=4) — 8개 이하로 축소된다.
    Check(pool.Coalesce(rioBuf, 8), "8개 이하로 병합 성공");
    Check(rioBuf.FragmentCount() <= 8, "8개 이하로 축소");
    Check(rioBuf.Size() == 60000, "병합해도 내용은 보존");

    std::string rioActual(rioBuf.Size(), '\0');
    Check(rioBuf.ReadMemory(rioActual.data(), rioActual.size()), "병합 결과 읽기 성공");
    Check(rioActual == rioExpected, "병합 후에도 바이트가 동일");

    // 지금 rioBuf는 최소 4개 청크가 필요한 상태다 — 1개로는 도달 불가능하므로 false.
    Check(!pool.Coalesce(rioBuf, 1), "불가능한 병합 요청은 false");
}

void TestPoolRecycle()
{
    std::cout << "(5) 풀 반환" << std::endl;
    // 마지막 Fragment가 사라지는 순간 블록이 풀에 돌아오는지 확인한다.

    const auto tiers = MakeTiers();
    IoBufPool_t pool(tiers);
    pool.Initialize();

    const auto warmed = pool.Stats();
    Check(warmed[0].pooled == 4, "예열로 256B 티어에 4개 대기");

    const char* address = nullptr;
    {
        auto segment = pool.Acquire(200);
        address = segment->Data();

        // 같은 블록을 두 Fragment가 나눠 가리켜도 각자 소진되어야 반납된다.
        IoBuf_t first(Fragment_t(segment, 0, 100));
        IoBuf_t second(Fragment_t(segment, 100, 100));
        segment.reset();

        Check(pool.Stats()[0].pooled == 3, "공유 중이면 반납되지 않음");

        first.Clear();
        Check(pool.Stats()[0].pooled == 3, "한쪽이 남아 있으면 아직 반납 안 됨");

        second.Clear();
        Check(pool.Stats()[0].pooled == 4, "마지막 참조 해제 시 반납");
    }

    // 반납된 것 자체가 재사용 가능한지 검증
    bool reusedSameBlock = false;
    for (int i = 0; i < 4; ++i)
    {
        auto reused = pool.Acquire(200);
        if (reused->Data() == address)
        {
            reusedSameBlock = true;
        }
    }
    Check(reusedSameBlock, "반납된 블록이 풀에서 재사용됨");

    // 어떤 티어에도 담기지 않는 크기는 풀과 무관하게 새로 할당된다.
    auto oversized = pool.Acquire(64 * 1024);
    const auto state = pool.Stats();
    Check(state[2].allocated == 1, "해당 티어에 할당 수는 늘지 않음");
}

}  // namespace

int main()
{
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif

    const auto tiers = MakeTiers();
    IoBufPool_t pool(tiers);
    pool.Initialize();

    TestZeroCopySplit(pool);
    TestAdjacencyMerge(pool);
    TestTraversal(pool);
    TestCoalesce(pool);
    TestPoolRecycle();

    std::printf("\n%s (실패 %d개)\n", (g_failed == 0) ? "전체 통과" : "실패 있음", g_failed);

    return (g_failed == 0) ? 0 : 1;
}
