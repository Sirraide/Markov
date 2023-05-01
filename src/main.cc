#include <algorithm>
#include <clopts.hh>
#include <codecvt>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <locale>
#include <random>
#include <ranges>
#include <regex>
#include <unordered_map>
#include <vector>

namespace rgs = std::ranges;
using c32 = char32_t;

void to_lower(auto& str) {
    rgs::transform(str, str.begin(), [](auto c) { return std::tolower(c); });
}
void to_upper(auto& str) {
    rgs::transform(str, str.begin(), [](auto c) { return std::toupper(c); });
}

auto to_utf8(const auto& str) -> std::string {
    std::wstring_convert<std::codecvt_utf8<c32>, c32> conv;
    return conv.to_bytes(str);
}

auto to_utf32(const auto& str) -> std::u32string {
    std::wstring_convert<std::codecvt_utf8<c32>, c32> conv;
    return conv.from_bytes(str);
}

/// Split a string into lines.
auto split_lines(std::string_view str) -> std::vector<std::string_view> {
    std::vector<std::string_view> ret;
    size_t start = 0;
    for (size_t i = 0; i < str.size(); i++) {
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
    size_t start = 0;
    while (start < str.size() and std::isspace(str[start])) start++;
    size_t end = str.size();
    while (end > start and std::isspace(str[end - 1])) end--;
    return str.substr(start, end - start);
}

/// Split a string by a regular expression.
auto split(const std::string& str, const std::string& re) -> std::vector<std::string> {
    std::vector<std::string> ret;
    std::regex rex(re);
    std::sregex_token_iterator iter(str.begin(), str.end(), rex, {-1, 0});
    std::sregex_token_iterator end;
    while (iter != end) {
        ret.push_back(*iter);
        iter++;
    }
    return ret;
}

template <typename tchar_t = char>
struct markov_chain {
    using tchar = tchar_t;
    using tstring = std::basic_string<tchar>;

    size_t order;
    std::unordered_map<tstring, std::vector<tchar>> chain;
    std::mt19937 rng;
    size_t seed;

    markov_chain(const tstring& text, size_t order, size_t _seed = std::random_device()()) : order(order), seed(_seed) {
        for (size_t i = 0; i < text.size() - order; i++)
            chain[text.substr(i, order)].push_back(text[i + order]);
        rng.seed(seed);
    }

    tstring generate(size_t length) {
        tstring result;
        result.reserve(length);

        tstring ngram;
        for (;;) {
            auto next = chain.begin();
            std::advance(next, rng() % chain.size());
            ngram = next->first;
            if (!ngram.starts_with(' ')) continue;

            result.append(next->first);
            break;
        }

        for (size_t i = 0; i < length; i++) {
            auto it = chain.find(ngram);
            if (it == chain.end()) break;
            result += it->second[rng() % it->second.size()];
            ngram = result.substr(i + 1, order);
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
    flag<"--dump-input", "Print the processed text instead of generating output">,
    flag<"--print-seed", "Print the seed used for the random number generator">,
    flag<"--ascii", "Strip non-ascii characters">,
    help<>
>; // clang-format on

void generate(std::string input) {
    size_t length = options::get_or<"--length">(100);
    size_t lines = options::get_or<"--lines">(1);
    size_t order = options::get_or<"--order">(6);

    /// Remove short lines to avoid generating gibberish.
    if (auto min_line = options::get<"--min-line">()) {
        auto input_lines = split_lines(input);
        std::erase_if(input_lines, [min = size_t(*min_line)](const auto& line) { return line.empty() || line.size() < min; });
        input = fmt::format("{}", fmt::join(input_lines, "\n"));
    }

    /// Replace newlines with spaces.
    char* c = input.data();
    while (c = strchr(c, '\n'), c) *c = ' ';

    /// Remove non-ascii chars.
    if (options::get<"--ascii">()) {
        static const std::string ascii_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'\".,-_:;!?() ";
        std::erase_if(input, [](char c) { return !ascii_chars.contains(c); });
    }

    /// Convert to lowercase.
    to_lower(input);

    /// Print the input if requested.
    if (options::get<"--dump-input">()) {
        fmt::print("{}\n", input);
        return;
    }

    /// Convert to utf32.
    auto str = to_utf32(input);

    /// Build the markov chain.
    markov_chain<c32> mc(str, order, size_t(options::get_or<"--seed">(std::random_device()())));

    /// Print the seed.
    if (options::get<"--print-seed">()) fmt::print("Seed: {}\n", mc.seed);

    /// Generate words.
    for (size_t i = 0; i < lines; i++) {
        auto out = to_utf8(mc.generate(length));

        /// Split the output if requested.
        if (auto s = options::get<"--split">()) {
            bool first = true;
            for (auto& line : split(out, *s)) {
                if (first) first = false;
                else if (line.size() > 5) fmt::print("\n");
                fmt::print("{}", trim(line));
            }
            fmt::print("\n");
        }

        else fmt::print("{}\n", trim(out));
    }
}

int main(int argc, char** argv) {
    setlocale(LC_ALL, "");
    options::parse(argc, argv);

    if (options::get<"--stdin">()) {
        std::string input;
        std::string line;
        while (std::getline(std::cin, line)) {
            input += line;
            input += '\n';
        }
        generate(std::move(input));
        return 0;
    }

    auto fs = options::get<"-f">();
    if (fs->empty()) {
        fmt::print(stderr, "{}", options::help());
        return 1;
    }

    for (auto& input : *fs) generate(std::move(input.contents));
}
