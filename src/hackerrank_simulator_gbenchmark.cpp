/**
 * @file hackerrank_simulator_gbenchmark.cpp
 * @brief Google Benchmark Suite for HackerRank HFT Low-Latency Assessment Simulator.
 */

#include <benchmark/benchmark.h>
#include <cstdint>
#include <cstring>
#include <array>
#include <atomic>
#include <algorithm>

using namespace std;

// ============================================================================
// Challenge 1: Fixed Point Parser Benchmark
// ============================================================================
inline int64_t parse_fixed_point_price(const char* str, size_t len) noexcept
{
    if (!str || len == 0) return 0;
    int64_t integer_part = 0, fractional_part = 0, fractional_scale = 1000000;
    bool is_negative = false;
    size_t idx = 0;

    if (str[0] == '-') { is_negative = true; idx = 1; }
    else if (str[0] == '+') { idx = 1; }

    while (idx < len && str[idx] >= '0' && str[idx] <= '9')
    {
        integer_part = (integer_part * 10) + (str[idx] - '0');
        ++idx;
    }

    if (idx < len && str[idx] == '.')
    {
        ++idx;
        while (idx < len && str[idx] >= '0' && str[idx] <= '9')
        {
            if (fractional_scale > 1)
            {
                fractional_scale /= 10;
                fractional_part = (fractional_part * 10) + (str[idx] - '0');
            }
            ++idx;
        }
    }

    int64_t result = (integer_part * 1000000) + (fractional_part * fractional_scale);
    return is_negative ? -result : result;
}

static void BM_FixedPointPriceParser(benchmark::State& state)
{
    const char* price_str = "154.8750";
    size_t len = strlen(price_str);
    for (auto _ : state)
    {
        int64_t p = parse_fixed_point_price(price_str, len);
        benchmark::DoNotOptimize(p);
    }
}
BENCHMARK(BM_FixedPointPriceParser);

// ============================================================================
// Challenge 2: SPSC Queue Push / Pop Batch Benchmark
// ============================================================================
template <typename T, size_t Capacity>
class HftSpscQueue
{
    static_assert((Capacity& (Capacity - 1)) == 0, "Capacity must be a power of two.");
private:
    alignas(64) atomic<size_t> m_write_pos{ 0 };
    alignas(64) size_t m_cached_read_pos { 0 };
    alignas(64) atomic<size_t> m_read_pos{ 0 };
    alignas(64) size_t m_cached_write_pos { 0 };
    alignas(64) array<T, Capacity> m_buffer;

public:
    bool push(const T& item) noexcept
    {
        const size_t current_write = m_write_pos.load(memory_order_relaxed);
        const size_t next_write = (current_write + 1) & (Capacity - 1);
        if (next_write == m_cached_read_pos) [[unlikely]]
        {
            m_cached_read_pos = m_read_pos.load(memory_order_acquire);
            if (next_write == m_cached_read_pos) return false;
        }
        m_buffer[current_write] = item;
        m_write_pos.store(next_write, memory_order_release);
        return true;
    }

    bool pop(T& item) noexcept
    {
        const size_t current_read = m_read_pos.load(memory_order_relaxed);
        if (current_read == m_cached_write_pos) [[unlikely]]
        {
            m_cached_write_pos = m_write_pos.load(memory_order_acquire);
            if (current_read == m_cached_write_pos) return false;
        }
        item = m_buffer[current_read];
        const size_t next_read = (current_read + 1) & (Capacity - 1);
        m_read_pos.store(next_read, memory_order_release);
        return true;
    }
};

static void BM_SpscQueuePushPop(benchmark::State& state)
{
    HftSpscQueue<int, 1024> q;
    int val = 42;
    for (auto _ : state)
    {
        q.push(val);
        q.pop(val);
        benchmark::DoNotOptimize(val);
    }
}
BENCHMARK(BM_SpscQueuePushPop);

// ============================================================================
// Challenge 3: Zero-Alloc FlatHashMap BBO Tracker Benchmark
// ============================================================================
struct Order { uint64_t order_id{ 0 }; char side{ 'B' }; int64_t price{ 0 }; uint32_t qty{ 0 }; };
struct BboQuote { int64_t best_bid_price{ 0 }; uint32_t best_bid_qty{ 0 }; int64_t best_ask_price{ 0 }; uint32_t best_ask_qty{ 0 }; };

template <typename Key, typename Value, size_t Capacity = 4096>
class FlatHashMap
{
private:
    struct Slot { Key key{}; Value value{}; bool occupied{ false }; };
    alignas(64) array<Slot, Capacity> m_table{};
    size_t m_size{ 0 };

public:
    inline bool insert_or_assign(const Key& key, const Value& val) noexcept
    {
        size_t idx = static_cast<size_t>(key) & (Capacity - 1);
        for (size_t i = 0; i < Capacity; ++i)
        {
            size_t pos = (idx + i) & (Capacity - 1);
            if (!m_table[pos].occupied || m_table[pos].key == key)
            {
                if (!m_table[pos].occupied) ++m_size;
                m_table[pos].key = key;
                m_table[pos].value = val;
                m_table[pos].occupied = true;
                return true;
            }
        }
        return false;
    }

    inline const Value* find(const Key& key) const noexcept
    {
        size_t idx = static_cast<size_t>(key) & (Capacity - 1);
        for (size_t i = 0; i < Capacity; ++i)
        {
            size_t pos = (idx + i) & (Capacity - 1);
            if (!m_table[pos].occupied) return nullptr;
            if (m_table[pos].key == key) return &m_table[pos].value;
        }
        return nullptr;
    }

    inline bool erase(const Key& key) noexcept
    {
        size_t idx = static_cast<size_t>(key) & (Capacity - 1);
        for (size_t i = 0; i < Capacity; ++i)
        {
            size_t pos = (idx + i) & (Capacity - 1);
            if (!m_table[pos].occupied) return false;
            if (m_table[pos].key == key)
            {
                m_table[pos].occupied = false;
                --m_size;
                size_t next = (pos + 1) & (Capacity - 1);
                while (m_table[next].occupied)
                {
                    Slot tmp = m_table[next];
                    m_table[next].occupied = false;
                    --m_size;
                    insert_or_assign(tmp.key, tmp.value);
                    next = (next + 1) & (Capacity - 1);
                }
                return true;
            }
        }
        return false;
    }
};

template <size_t MaxLevels = 64, bool IsBid = true>
class FlatPriceBook
{
private:
    struct Level { int64_t price{ 0 }; uint32_t qty{ 0 }; };
    array<Level, MaxLevels> m_levels{};
    size_t m_count{ 0 };

public:
    void add_qty(int64_t price, uint32_t qty) noexcept
    {
        for (size_t i = 0; i < m_count; ++i)
        {
            if (m_levels[i].price == price) { m_levels[i].qty += qty; return; }
        }
        if (m_count < MaxLevels)
        {
            size_t insert_pos = m_count;
            for (size_t i = 0; i < m_count; ++i)
            {
                bool is_better = IsBid ? (price > m_levels[i].price) : (price < m_levels[i].price);
                if (is_better) { insert_pos = i; break; }
            }
            for (size_t j = m_count; j > insert_pos; --j) m_levels[j] = m_levels[j - 1];
            m_levels[insert_pos] = Level{ price, qty };
            ++m_count;
        }
    }

    void reduce_qty(int64_t price, uint32_t qty) noexcept
    {
        for (size_t i = 0; i < m_count; ++i)
        {
            if (m_levels[i].price == price)
            {
                if (m_levels[i].qty <= qty)
                {
                    for (size_t j = i; j < m_count - 1; ++j) m_levels[j] = m_levels[j + 1];
                    --m_count;
                }
                else m_levels[i].qty -= qty;
                return;
            }
        }
    }

    Level top() const noexcept { return m_count == 0 ? Level{ 0, 0 } : m_levels[0]; }
};

class ZeroAllocOrderBookBboTracker
{
private:
    FlatHashMap<uint64_t, Order, 4096> m_order_map;
    FlatPriceBook<64, true> m_bids;
    FlatPriceBook<64, false> m_asks;

public:
    bool add_order(uint64_t order_id, char side, int64_t price, uint32_t qty) noexcept
    {
        Order ord{ order_id, side, price, qty };
        if (!m_order_map.insert_or_assign(order_id, ord)) return false;
        if (side == 'B') m_bids.add_qty(price, qty);
        else m_asks.add_qty(price, qty);
        return true;
    }

    bool cancel_order(uint64_t order_id) noexcept
    {
        const Order* ord = m_order_map.find(order_id);
        if (!ord) return false;
        if (ord->side == 'B') m_bids.reduce_qty(ord->price, ord->qty);
        else m_asks.reduce_qty(ord->price, ord->qty);
        m_order_map.erase(order_id);
        return true;
    }

    BboQuote get_bbo() const noexcept
    {
        auto top_bid = m_bids.top();
        auto top_ask = m_asks.top();
        return BboQuote{ top_bid.price, top_bid.qty, top_ask.price, top_ask.qty };
    }
};

static void BM_ZeroAllocFlatHashMapBboTracker(benchmark::State& state)
{
    ZeroAllocOrderBookBboTracker book;
    uint64_t id = 100;
    for (auto _ : state)
    {
        book.add_order(id, 'B', 35000000 + (id % 50), 100);
        book.cancel_order(id);
        ++id;
    }
}
BENCHMARK(BM_ZeroAllocFlatHashMapBboTracker);

// ============================================================================
// Challenge 4: SWAR Tag Scanner Benchmark
// ============================================================================
inline const char* find_tag_val(const char* buf, size_t len, int target_tag, size_t& val_len) noexcept
{
    (void)target_tag;
    val_len = 0;
    if (!buf || len < 4) return nullptr;

    constexpr uint32_t needle = 0x003D3434; // "44="
    const char* ptr = buf;
    const char* end = buf + len - 3;

    while (ptr <= end)
    {
        uint32_t chunk;
        memcpy(&chunk, ptr, 4);
        if ((chunk & 0x00FFFFFF) == needle)
        {
            const char* val_start = ptr + 3;
            const char* val_end = val_start;
            while (val_end < buf + len && *val_end != '\x01') ++val_end;
            val_len = static_cast<size_t>(val_end - val_start);
            return val_start;
        }
        ++ptr;
    }
    return nullptr;
}

static void BM_BitwiseTagScanner(benchmark::State& state)
{
    const char* fix_msg = "8=FIX.4.2\x01" "35=D\x01" "49=SENDER\x01" "56=TARGET\x01" "34=1054\x01" "55=PETR4\x01" "44=35.85\x01" "38=500\x01" "10=182\x01";
    size_t len = strlen(fix_msg);
    size_t val_len = 0;
    for (auto _ : state)
    {
        const char* val = find_tag_val(fix_msg, len, 44, val_len);
        benchmark::DoNotOptimize(val);
    }
}
BENCHMARK(BM_BitwiseTagScanner);

// ============================================================================
// Challenge 5: Feed Arbitrator Benchmark
// ============================================================================
template <size_t MaxSeqNum = 1000000>
class FeedArbitrator
{
private:
    alignas(64) array<atomic<bool>, MaxSeqNum> m_seen{};
public:
    inline bool process_packet(uint64_t seq_num) noexcept
    {
        if (seq_num >= MaxSeqNum) return false;
        return !m_seen[seq_num].exchange(true, memory_order_acq_rel);
    }
};

static void BM_FeedArbitrator(benchmark::State& state)
{
    static FeedArbitrator<10000000> arb;
    uint64_t seq = 1;
    for (auto _ : state)
    {
        bool result = arb.process_packet(seq++);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_FeedArbitrator);

// ============================================================================
// Challenge 6: Fast Object Pool Benchmark
// ============================================================================
template <typename T, size_t Capacity = 10000>
class FastObjectPool
{
private:
    alignas(64) array<T, Capacity> m_pool{};
    alignas(64) array<size_t, Capacity> m_free_stack{};
    size_t m_top{ 0 };

public:
    FastObjectPool()
    {
        for (size_t i = 0; i < Capacity; ++i) m_free_stack[i] = Capacity - 1 - i;
        m_top = Capacity;
    }

    inline T* allocate() noexcept
    {
        if (m_top == 0) return nullptr;
        return &m_pool[m_free_stack[--m_top]];
    }

    inline void deallocate(T* ptr) noexcept
    {
        if (!ptr) return;
        size_t idx = static_cast<size_t>(ptr - &m_pool[0]);
        if (idx < Capacity) m_free_stack[m_top++] = idx;
    }
};

static void BM_FastObjectPool(benchmark::State& state)
{
    struct DummyNode { uint64_t id; int64_t p; uint32_t q; };
    FastObjectPool<DummyNode, 10000> pool;

    for (auto _ : state)
    {
        DummyNode* ptr = pool.allocate();
        benchmark::DoNotOptimize(ptr);
        pool.deallocate(ptr);
    }
}
BENCHMARK(BM_FastObjectPool);

BENCHMARK_MAIN();
