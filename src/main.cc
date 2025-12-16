#include <base/Base.hh>
#include <base/FS.hh>
#include <base/Serialisation.hh>
#include <base/Text.hh>
#include <clopts.hh>
#include <print>
#include <random>
#include <regex>

using namespace base;

#ifdef USE_32_BIT_CHAIN
using Character = char32_t;
using String = std::u32string;
#else
using Character = char;
using String = std::string;
#endif

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

//#define FREE_NODES

struct node {
    using large_t = std::flat_map<char, u32>;
    uptr data : 63 = 0;
    uptr small : 1 = true;

    node() = default;
    node(char c, u32 n) { inc_small_count(c, n); }
    node(large_t l) {
        small = false;
        data = reinterpret_cast<uptr>(new large_t(std::move(l)));
    }

#ifdef FREE_NODES
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
#endif // FREE_NODES

    void add(char c) {
        if (small) {
            if (data == 0 or small_char() == c) {
                inc_small_count(c);
                return;
            }

            // Make large.
            small = false;
            auto& large = *new large_t;
            large[small_char()] = small_count();
            large[c] = 1;
            data = reinterpret_cast<uptr>(&large);
            return;
        }

        ++get_large()[c];
    }

    auto get(u32 i) const -> char {
        if (small) return small_char();
        u32 n = 0;
        for (auto [c, p] : get_large()) {
            if (i < n + p) return c;
            n += p;
        }
        Unreachable();
    }

    [[clang::always_inline]] auto get_large() -> large_t& {
        return *reinterpret_cast<large_t*>(data);
    }

    [[clang::always_inline]] auto get_large() const -> const large_t& {
        return *reinterpret_cast<const large_t*>(data);
    }

    [[clang::always_inline]] auto small_char() const -> char {
        return char(data & 0xff);
    }

    [[clang::always_inline]] auto small_count() const -> u32 {
        return u32(data >> 8);
    }

    [[clang::always_inline]] void inc_small_count(char c, u32 n = 1) {
        data = (small_count() + n) << 8 | u32(c);
    }

    [[clang::always_inline]] auto count() const -> u32 {
        if (small) return 1;
        return u32(get_large().size());
    }

    auto total_count() const -> u32 {
        if (small) return small_count();
        return rgs::fold_left(get_large().values(), 0u, [](auto a, auto& v) { return a + v; });
    }
};

template <>
struct ser::Serialiser<node> {
    static auto deserialise(auto& r) -> Result<node> {
        auto count = Try(r.template read<u32>());
        if (count == 0) return node();
        if (count == 1) {
            auto c = Try(r.template read<char>());
            auto n = Try(r.template read<u32>());
            return node(c, n);
        }

        return node(Try(r.template read<node::large_t>()));
    }

    static void serialise(auto& w, const node& n) {
        auto count = n.count();
        w << count;
        if (count == 0) return;
        if (count == 1) {
            w <<  n.small_char() << n.small_count();
            return;
        }

        w << n.get_large();
    }
};

struct markov_chain {
    using char_type = Character;
    using text_type = std::basic_string_view<char_type>;
    using string_type = std::basic_string<char_type>;
    LIBBASE_SERIALISE(markov_chain, chain, order);

    /// Map from ngrams to [char, frequency] pairs.
    using map_type = std::unordered_map<string_type, node>;

    map_type chain;
    usz order;
    usz seed;
    std::mt19937 rng;

    markov_chain(text_type text, usz order, usz _seed = std::random_device()()) : order(order), seed(_seed) {
        ProfileTimer _{"build"};
        for (usz i = 0; i < text.size() - order; i++)
            chain[map_type::key_type{text.substr(i, order)}].add(text[i + order]);
        rng.seed(seed);
    }

    markov_chain(map_type chain, usz order, usz seed = std::random_device()())
        : chain(std::move(chain)), order(order), seed(seed) {
        rng.seed(seed);
    }

    string_type generate(usz length) {
        string_type result, ngram;
        result.reserve(length);

        // Pick an ngram that starts with a space.
        for (;;) {
            auto next = chain.begin();
            std::advance(next, rng() % chain.size());
            ngram = next->first;
            if (!ngram.starts_with(' ')) continue;
            result.append(next->first);
            break;
        }

        // Append to it.
        for (usz iterations = 0; iterations < length; iterations++) {
            // Find the entry for the ngram in the chain; if there is no character
            // that can follow this ngram, give up.
            auto it = chain.find(ngram);
            if (it == chain.end()) break;

            // Otherwise, randomly pick a character; we need to take probabilities
            // into account here, e.g. if 'Y' occurs 10 times more often after 'X'
            // than 'Z', then we should pick 'Y' 10/11 times.
            //
            // To implement this, pick an ‘index’ randomly and then find the element
            // at that ‘index’; e.g. if our map is {{'a', 10}, {'b', 5}, {'c', '20'}},
            // then we pick an index I between 1 and 35, and the next character will
            // be 'a' if I is in 0..<10, 'b' if it is in '10..<15', and 'c' otherwise.
            auto count = it->second.total_count();
            auto i = rng() % count;
            result += it->second.get(u32(i));
            ngram = result.substr(iterations + 1, order);
        }

        return result;
    }
};

using namespace command_line_options;
using options = clopts< // clang-format off
    multiple<option<"-f", "The input files", file<>>>,
    flag<"--stdin", "Read input from stdin instead">,
    option<"--length", "The maximum length of the output", int64_t>,
    option<"--lines", "How many lines to generate", int64_t>,
    option<"--order", "The order of the ngrams", int64_t>,
    option<"--seed", "The seed for the random number generator", int64_t>,
    option<"--min-line", "Ignore lines that are shorter than this", int64_t>,
    option<"--split", "Split output by regex">,
    option<"--save-chain", "Save the chain to a file">,
    option<"--load-chain", "Load the chain from a file", file<std::vector<std::byte>>>,
    flag<"--dump-input", "Print the processed text instead of generating output">,
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

    // Save the chain.
    if (auto* path = opts.get<"--save-chain">()) {
        File::Write(*path, ser::Serialise<std::endian::native>(mc)).value();
        return;
    }

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

void build_chain_and_generate(options::optvals_type& opts, str input) {
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
    markov_chain mc(cleaned_up, order, usz(opts.get<"--seed">(std::random_device()())));
    generate(opts, mc);
}

int main(int argc, char** argv) {
    setlocale(LC_ALL, "");
    auto opts = options::parse(argc, argv);
    if (auto* chain_file = opts.get<"--load-chain">()) {
        Timer t{0ms};
        markov_chain mc{ser::Deserialise<markov_chain, std::endian::native>(chain_file->contents).value()};
        //markov_chain::map_type m;
        //int x = 0;
        //for (auto p : mc.chain) {
        //    m.insert(std::move(p));
        //    if (x++ == 1000) break;
        //}
        //
        //std::println("{}", m);
        //_Exit(41);
        //std::unordered_map<int, int> x;
        //for (const auto& [k, v] : mc.chain) x[v.size()]++;
        //for (const auto& [k, v] : x) std::println("{} : {}", k, v);
        //std::fflush(stdout);
        //_Exit(42);


        std::println(stderr, "[deserialisation] {}", t.elapsed());
        generate(opts, mc);
        return 0;
    }

    if (opts.get<"--stdin">()) {
        std::string input;
        std::string line;
        while (std::getline(std::cin, line)) {
            input += line;
            input += '\n';
        }

        build_chain_and_generate(opts, input);
        return 0;
    }

    auto fs = opts.get<"-f">();
    if (fs.empty()) {
        std::print(stderr, "{}", options::help());
        return 1;
    }

    for (auto& input : fs) build_chain_and_generate(opts, input.contents);
}
