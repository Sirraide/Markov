#include <base/Base.hh>
#include <base/FS.hh>
#include <base/Serialisation.hh>
#include <base/Text.hh>
#include <clopts.hh>
#include <csignal>
#include <print>
#include <random>
#include <regex>
#include <thread>

using namespace base;

namespace {
#ifdef USE_32_BIT_CHAIN
using Character = char32_t;
using String = std::u32string;
#else
using Character = char;
using String = std::string;
#endif

// This program allocates a *lot* of memory because we build huge data structures,
// so this flag disables freeing during the build phase because this ends up being
// much faster.
#define DISABLE_FREE

using Reader = ser::Reader<std::endian::native>;
using Writer = ser::Writer<std::endian::native>;

class Timer {
    chr::milliseconds duration;
    chr::steady_clock::time_point start;

public:
    /// Create a new timer with the given duration and start it.
    explicit Timer(chr::milliseconds duration)
        : duration(duration), start(chr::steady_clock::now()) {}

    /// Get the elapsed time normalised between 0 and 1.
    [[nodiscard]] auto dt() const -> f64 { return dt(duration); }
    [[nodiscard]] auto dt(chr::milliseconds d) const -> f64 {
        return f64(elapsed().count()) / f64(d.count());
    }

    /// Get the time that has elapsed since the animation started.
    [[nodiscard]] auto elapsed() const -> chr::milliseconds {
        return chr::duration_cast<chr::milliseconds>(chr::steady_clock::now() - start);
    }

    /// Check if the timer has expired.
    [[nodiscard]] auto expired() const -> bool {
        return elapsed() >= duration;
    }

    /// Extend the duration of the current iteration of the timer.
    void extend(chr::milliseconds extra) { start += extra; }

    /// Restart the timer.
    void restart() { start = chr::steady_clock::now(); }
};

class ProfileTimer : public Timer {
public:
    std::string scope;
    ProfileTimer(std::string scope, chr::milliseconds duration = 0ms)
        : Timer(duration), scope(std::move(scope)) {}

    ~ProfileTimer() {
        std::println(stderr, "[{}]: {} elapsed", scope, elapsed());
    }
};

/// Split a string into lines.
auto split_lines(std::string_view str) -> std::vector<std::string_view> {
    std::vector<std::string_view> ret;
    usz start = 0;
    for (usz i = 0; i < str.size(); i++) {
        if (str[i] == '\n') {
            ret.push_back(str.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < str.size()) ret.push_back(str.substr(start));
    return ret;
}

/// Trim a string.
auto trim(std::string_view str) -> std::string_view {
    usz start = 0;
    while (start < str.size() and text::IsSpace(str[start])) start++;
    usz end = str.size();
    while (end > start and text::IsSpace(str[end - 1])) end--;
    return str.substr(start, end - start);
}

/// Split a string by a regular expression.
auto split(str s, std::string_view re) -> std::vector<str> {
    std::vector<str> ret;
    regex rex(re);
    while (not s.empty()) {
        auto f = s.find(rex);
        if (not f.has_value()) {
            ret.push_back(s);
            return ret;
        }

        ret.push_back(s.take(f->start));
        s.drop(f->size());
    }
    return ret;
}

struct header {
    static constexpr u32 CurrentVersion = 1;

    /// File format version.
    u32 version = CurrentVersion;

    /// Order of the chain.
    u16 order;

    /// Unused.
    u16 _reserved;

    /// Number of ngrams in the ngram table.
    u64 num_ngrams;
};

struct markov_chain {
    using ngram_type = __int128;
    using offset_type = u64;
    using index_type = u32;
    using freq_size_type = u32;
    using char_type = Character;
    using string_type = std::basic_string<char_type>;

    // Packed so we don’t have to worry about the serialised pointer being unaligned.
    struct [[gnu::packed]] saved_ngram {
        /// Value of the ngram.
        ngram_type ngram;

        /// Offset in bytes relative to the start of the frequency table.
        offset_type freq_pairs_list_offset;
    };

    struct [[gnu::packed]] freq_table_entry {
        freq_size_type freq;
        char_type c;
    };

    struct [[gnu::packed]] freq_table {
        freq_size_type num_elements;
        freq_table_entry entry_data[];
        auto elements() const -> Span<freq_table_entry> {
            return {entry_data, num_elements};
        }
    };

    ByteSpan data;
    header hdr{};
    std::mt19937 rng;
    usz seed;

    explicit markov_chain(ByteSpan data, usz seed) : data{data}, seed{seed} {
        std::memcpy(&hdr, data.data(), sizeof(header));
        Assert(hdr.version == header::CurrentVersion, "Chain is out of date; please regenerate");
        rng.seed(seed);
    }

    auto all_ngrams() const -> Span<saved_ngram> {
        return {
            reinterpret_cast<const saved_ngram*>(data.data() + sizeof(header)),
            hdr.num_ngrams
        };
    }

    /// Find the entry for the ngram in the chain; if there is no character
    /// that can follow this ngram, give up.
    auto find_ngram(ngram_type ngram) const -> std::optional<saved_ngram> {
        auto all = all_ngrams();
        auto n = rgs::lower_bound(all, ngram, rgs::less(), &saved_ngram::ngram);
        if (n == all.end()) return std::nullopt;
        return *n;
    }

    /// Randomly pick a character; we need to take probabilities into account here,
    /// e.g. if 'Y' occurs 10 times more often after 'X' than 'Z', then we should
    /// pick 'Y' 10/11 times.
    ///
    /// To implement this, pick an ‘index’ randomly and then find the element
    /// at that ‘index’; e.g. if our map is {{'a', 10}, {'b', 5}, {'c', '20'}},
    /// then we pick an index I between 1 and 35, and the next character will
    /// be 'a' if I is in 0..<10, 'b' if it is in '10..<15', and 'c' otherwise.
    auto ngram_get_random_char(const saved_ngram& n) -> char_type {
        auto table_offs = data.data() + sizeof(hdr) + hdr.num_ngrams * sizeof(saved_ngram) + n.freq_pairs_list_offset;
        auto table = reinterpret_cast<const freq_table*>(table_offs);
        auto i = rng() % table->num_elements;
        freq_size_type sum = 0;
        for (auto e : table->elements()) {
            sum += e.freq;
            if (i <= sum) return e.c;
        }
        Unreachable();
    }

    void dump() {
        std::println("header");
        std::println("  chain size: {}", utils::HumanReadable(data.size_bytes()));
        std::println("  version: {}", hdr.version);
        std::println("  ngrams: {}", hdr.num_ngrams);
        std::println("  order: {}", hdr.order);
        std::println("ngrams");
        for (auto n : all_ngrams()) {
            std::string s;
            s.resize(hdr.order);
            std::memcpy(s.data(), &n.ngram, hdr.order);
            std::println("  - '{}' at {}", s, n.freq_pairs_list_offset);
        }
    }

    string_type generate(usz length) {
        ngram_type ngram;
        string_type result;
        result.reserve(length);

        // Pick a random ngram to start with.
        auto all = all_ngrams();
        auto start = rng() % hdr.num_ngrams;
        auto saved = all[start];
        result.resize(hdr.order);
        std::memcpy(result.data(), &saved.ngram, hdr.order);
        ngram = saved.ngram;

        // Append to it.
        for (usz iterations = 0; iterations < length; iterations++) {
            auto n = find_ngram(ngram);
            if (not n.has_value()) break;
            result += ngram_get_random_char(*n);
            std::memcpy(&ngram, result.data() + iterations + 1, hdr.order);
        }

        return result;
    }
};

struct node {
    using char_type = markov_chain::char_type;
    using freq_type = markov_chain::freq_size_type;
    using large_t = std::vector<std::pair<char_type, freq_type>>;
    uptr data : 63 = 0;
    uptr small : 1 = true;

    node() = default;
    node(char_type c, freq_type n) { inc_small_count(c, n); }
    node(large_t l) {
        small = false;
        data = reinterpret_cast<uptr>(new large_t(std::move(l)));
    }

#ifndef DISABLE_FREE
    ~node() {
        if (not small) delete &get_large();
    }
    node(const node&) = delete;
    node& operator=(const node&) = delete;
    node(node&& other) : data(other.data), small(other.small) {
        other.small = true;
        other.data = 0;
    }

    node& operator=(node&& other) {
        if (std::addressof(other) != this) {
            if (not small) delete &get_large();
            small = other.small;
            data = other.data;
            other.small = true;
            other.data = 0;
        }
        return *this;
    }
#endif // not DISABLE_FREE

    void add(char_type c, u32 n = 1) {
        if (small) {
            if (data == 0 or small_char() == c) {
                inc_small_count(c, n);
                return;
            }

            // Make large.
            make_large();
            get_large().emplace_back(c, n);
            return;
        }

        add_to_large(c, n);
    }

    void add_to_large(char_type c, u32 n) {
        DebugAssert(not small);
        auto& l = get_large();
        auto it = rgs::find(l, c, &std::pair<char_type, freq_type>::first);
        if (it == l.end()) l.emplace_back(c, n);
        else it->second += n;
    }

    void make_large() {
        small = false;
        auto& large = *new large_t;
        large.emplace_back(small_char(), small_count());
        data = reinterpret_cast<uptr>(&large);
    }

    void merge(node&& other) {
        if (small) {
            // Case 1: Both are small.
            if (other.small) {
                // Subcase: if the chars are the same, then we don’t have to allocate.
                if (small_char() == other.small_char()) {
                    inc_small_count(small_char(), other.small_count());
                    return;
                }

                // The other node is small as well; make us large.
                make_large();
                add(other.small_char(), other.small_count());
                return;
            }

            // Case 2: We are small, the other node is large, merge us into them.
            other.add(small_char(), other.small_count());
            *this = std::move(other);
            return;
        }

        // Case 3: The other node is small, and we are already large.
        if (other.small) {
            add(other.small_char(), other.small_count());
            return;
        }

        // Case 4: Both are large.
        for (auto [c, freq] : other.get_large()) add_to_large(c, freq);
    }

    [[clang::always_inline]] auto get_large() -> large_t& {
        return *reinterpret_cast<large_t*>(data);
    }

    [[clang::always_inline]] auto get_large() const -> const large_t& {
        return *reinterpret_cast<const large_t*>(data);
    }

    [[clang::always_inline]] auto small_char() const -> char_type {
        return char_type(data & 0xff);
    }

    [[clang::always_inline]] auto small_count() const -> freq_type {
        return freq_type(data >> 8);
    }

    [[clang::always_inline]] void inc_small_count(char_type c, freq_type n = 1) {
        data = (small_count() + n) << 8 | freq_type(c);
    }

    [[clang::always_inline]] auto count() const -> freq_type {
        if (small) return 1;
        return freq_type(get_large().size());
    }
};

struct markov_chain_builder {
    using char_type = char;
    using text_type = std::string_view;
    using pair_type = std::pair<markov_chain::ngram_type, node>;

    /// Map from ngrams to [char, frequency] pairs.
    using map_type = std::vector<pair_type>;
    map_type chain;
    usz order;

    // This is a merge sort that also merges the ngram frequency maps.
    //
    // Invariant: the inputs to merge() are sorted.
    static auto merge(map_type&& a, map_type&& b) -> map_type {
        DebugAssert(rgs::is_sorted(a, rgs::less(), &pair_type::first));
        DebugAssert(rgs::is_sorted(b, rgs::less(), &pair_type::first));

        map_type m;
        auto ia = a.begin();
        auto ib = b.begin();
        auto ea = a.end();
        auto eb = b.end();
        auto Add = [&](markov_chain::ngram_type ngram, node n) {
            if (m.empty() or m.back().first != ngram) m.emplace_back(ngram, std::move(n));
            else m.back().second.merge(std::move(n));
        };

        while (ia != ea and ib != eb) {
            if (ia->first <= ib->first) {
                Add(ia->first, std::move(ia->second));
                ++ia;
            } else {
                Add(ib->first, std::move(ib->second));
                ++ib;
            }
        }

        while (ia != ea) {
            Add(ia->first, std::move(ia->second));
            ++ia;
        }

        while (ib != eb) {
            Add(ib->first, std::move(ib->second));
            ++ib;
        }

        DebugAssert(rgs::is_sorted(m, rgs::less(), &pair_type::first));
        return m;
    }

    static auto parallel_merge(MutableSpan<map_type> maps) -> map_type {
        Assert(not maps.empty());
        if (maps.size() == 1) return std::move(maps.front());
        if (maps.size() == 2) return merge(std::move(maps[0]), std::move(maps[1]));
        if (maps.size() == 3) return merge(
            std::move(maps[0]),
            merge(std::move(maps[1]), std::move(maps[2]))
        );

        map_type a, b;

        {
            auto half = maps.size() / 2;
            std::jthread _{[&] { a = parallel_merge(maps.subspan(0, half)); }};
            std::jthread _{[&] { b = parallel_merge(maps.subspan(half)); }};
        }

        return merge(std::move(a), std::move(b));
    }

    markov_chain_builder(text_type text, usz order, u32 num_collect_threads) : order(order) {
        Assert(order <= sizeof(markov_chain::ngram_type));
        ProfileTimer _{"build"};
        usz end = text.size() - order;
        {
            // Iterate over 'num_els' elements using 'num_threads' in parallel, invoking
            // 'thread_cb' for each element with the thread id and index.
            auto IterateRangeInParallel = [](u64 num_els, u64 num_threads, auto elem_cb, auto done_cb) {
                std::vector<std::jthread> threads;
                u64 partition_size = num_els/num_threads;
                for (u64 tid = 0; tid < num_threads; tid++) threads.emplace_back([&, tid] {
                    u64 last = (tid + 1) * partition_size;
                    if (tid == num_threads - 1) last += num_els % num_threads; // Include trailing data.
                    for (u64 i = tid * partition_size; i < last; i++) std::invoke(elem_cb, tid, i);
                    std::invoke(done_cb, tid);
                });
            };

            // Heap-allocate and leak the maps since freeing them takes for ever.
            std::vector<map_type> collecting_ngrams_maps;
            {
                ProfileTimer _{"build: collecting ngrams"};
                collecting_ngrams_maps.resize(num_collect_threads);
                IterateRangeInParallel(end, num_collect_threads, [&](u64 tid, u64 i) {
                    markov_chain::ngram_type v{};
                    std::memcpy(&v, text.data() + i, order);
                    collecting_ngrams_maps.at(tid).emplace_back(v, node(text[i + order], 1));
                }, [&](u64 tid) {
                    rgs::sort(collecting_ngrams_maps.at(tid), rgs::less(), &pair_type::first);
                });
            }

            // Merge the maps.
            chain = parallel_merge(collecting_ngrams_maps);
        }
    }
};

using namespace command_line_options;
using options = clopts< // clang-format off
    positional<"input", "The input files", file<>, false>,
    option<"--length", "The maximum length of the output", int64_t>,
    option<"--lines", "How many lines to generate", int64_t>,
    option<"--order", "The order of the ngrams", int64_t>,
    option<"--seed", "The seed for the random number generator", int64_t>,
    option<"--min-line", "Ignore lines that are shorter than this", int64_t>,
    option<"--split", "Split output by regex">,
    option<"--save-chain", "Save the chain to a file">,
    option<"--chain", "Load the chain from a file">,
    option<"--collect-threads", "Threads to use to collect ngrams; prefer to set this to a power of 2", int64_t>,
    flag<"--dump-input", "Print the processed text instead of generating output">,
    flag<"--dump-chain", "Print the chain’s contents">,
    flag<"--print-seed", "Print the seed used for the random number generator">,
    flag<"--ascii", "Strip non-ascii characters">,
    flag<"--skip-preprocess", "Do not preprocess the input">,
    help<>
>; // clang-format on

auto clean_up_input(options::optvals_type& opts, str input) -> String {
    ProfileTimer _{"cleanup"};
    std::string cleaned_up;

    // Remove short lines to avoid generating gibberish.
    if (auto min_line = opts.get<"--min-line">()) {
        ProfileTimer _{"cleanup: short lines"};
        cleaned_up = input
            | vws::split('\n')
            | vws::filter([min = *min_line](auto&& r) { return rgs::distance(r) >= min; })
            | vws::join_with(' ')
            | rgs::to<std::string>();
    } else {
        ProfileTimer _{"cleanup: fold ws"};
        cleaned_up = input.fold_ws();
    }

    // Remove non-ascii chars.
    if (opts.get<"--ascii">()) {
        ProfileTimer _{"cleanup: ascii"};
        std::erase_if(cleaned_up, [](char c) { return not text::IsPrint(c); });
    }

    // Convert to lowercase.
    {
        ProfileTimer _{"cleanup: lowercase"};
        cleaned_up = text::ToLower(cleaned_up);
    }

#ifdef USE_32_BIT_CHAIN
    // Convert to utf32.
    ProfileTimer _{"cleanup: utf8->utf32"};
    return text::ToUTF32(cleaned_up);
#else
    return cleaned_up;
#endif
}

void generate(options::optvals_type& opts, markov_chain& mc) {
    usz length = usz(opts.get<"--length">(100));
    usz lines = usz(opts.get<"--lines">(1));

    // Print the seed.
    if (opts.get<"--print-seed">()) std::println(stderr, "Seed: {}", mc.seed);

    // Generate words.
    ProfileTimer _{"generate"};
    for (usz i = 0; i < lines; i++) {
#ifdef USE_32_BIT_CHAIN
        auto out = text::ToUTF8(mc.generate(length));
#else
        auto out = mc.generate(length);
#endif

        // Split the output if requested.
        if (auto s = opts.get<"--split">()) {
            bool first = true;
            for (auto& line : split(out, *s)) {
                if (first) first = false;
                else if (line.size() > 5) std::println();
                std::print("{}", trim(line));
            }
            std::println();
        } else {
            std::println("{}", trim(out));
        }
    }
}

void build_chain(options::optvals_type& opts, fs::PathRef into, str input) {
    usz order = usz(opts.get<"--order">(6));
    String cleaned_up = opts.get<"--skip-preprocess">()
#ifdef USE_32_BIT_CHAIN
        ? text::ToUTF32(input)
#else
        ? String(input)
#endif
        : clean_up_input(opts, input);

    // Print the input if requested.
    if (opts.get<"--dump-input">()) {
        std::println("{}", cleaned_up);
        return;
    }

    // Build the markov chain.
    ProfileTimer timer{"total"};
    markov_chain_builder mc(
        cleaned_up,
        order,
        u32(opts.get<"--collect-threads">(std::thread::hardware_concurrency()))
    );

    // Save the chain.
    header hdr{};
    std::vector<markov_chain::saved_ngram> ngram_buffer;
    std::vector<std::byte> freq_buffer;

    {
        ProfileTimer _{"serialising"};
        DebugAssert(rgs::is_sorted(mc.chain, rgs::less(), &markov_chain_builder::pair_type::first));
        Writer freq_writer{freq_buffer};
        for (const auto& [k, v] : mc.chain) {
            markov_chain::saved_ngram ngram{k, markov_chain::offset_type(freq_buffer.size())};
            ngram_buffer.push_back(ngram);

            markov_chain::freq_size_type count = v.count();
            freq_writer.append_bytes(&count, sizeof(count));
            if (v.small) {
                markov_chain::freq_table_entry e{v.small_count(), v.small_char()};
                freq_writer.append_bytes(&e, sizeof(e));
            } else {
                for (auto [c, freq] : v.get_large()) {
                    markov_chain::freq_table_entry e{freq, c};
                    freq_writer.append_bytes(&e, sizeof(e));
                }
            }
        }

        hdr.num_ngrams = ngram_buffer.size();
        hdr.order = u16(mc.order);
    }

    {
        ProfileTimer _{"saving"};
        auto f = File::Open(into, fs::OpenMode::Write).value();
        f.write(ByteSpan{reinterpret_cast<const std::byte*>(&hdr), sizeof(header)}).value();
        f.write(ByteSpan{reinterpret_cast<const std::byte*>(ngram_buffer.data()), ngram_buffer.size() * sizeof(markov_chain::saved_ngram)}).value();
        f.write(freq_buffer).value();
    }

    // Report this one manually since we’re about to exit without
    // running any destructors.
    std::println("[{}]: {} elapsed", timer.scope, timer.elapsed());

    // Flush streams.
    std::fflush(stdout);
    std::fflush(stderr);

    // Avoid freeing everything we allocated.
    _Exit(0);
}

}
int main(int argc, char** argv) {
    setlocale(LC_ALL, "");
    auto opts = options::parse(argc, argv);
    if (auto* chain_file = opts.get<"--chain">()) {
        Timer t{0ms};
        fs::FileContents chain_data;

        {
            ProfileTimer _{"load chain"};
            chain_data = File::Read(*chain_file).value();
        }

        markov_chain chain{chain_data.span(), usz(opts.get<"--seed">(std::random_device()()))};
        if (opts.get<"--dump-chain">()) {
            chain.dump();
            return 0;
        }

        generate(opts, chain);
        return 0;
    }

    auto* into = opts.get<"--save-chain">();
    if (not into) {
        std::println(stderr, "Either --save-chain or --chain is required");
        return 1;
    }

    auto input = opts.get<"input">();
    if (not input) {
        std::println(stderr, "Input file is required when building a chain");
        return 1;
    }

    build_chain(opts, *into, input->contents);
}
