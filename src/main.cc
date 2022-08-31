#include <fmt/format.h>
#include <fmt/xchar.h>
#include <random>
#include <unordered_map>
#include <vector>
#include <codecvt>
#include <locale>
#include "../clopts/include/clopts.hh"

template <typename tstring>
tstring to_lower(tstring str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::tolower(c); });
    return str;
}

template <typename tstring>
tstring to_upper(tstring str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::toupper(c); });
    return str;
}

auto to_utf8(const auto& str)
    -> decltype(std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t>{}.to_bytes(str)) //
    requires requires { std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t>{}.to_bytes(str); }
{
    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> conv;
    return conv.to_bytes(str);
}

auto to_utf32(const auto& str)
    -> decltype(std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t>().from_bytes(str)) //
    requires requires { std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t>{}.from_bytes(str); }
{
    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> conv;
    return conv.from_bytes(str);
}

/// Split a string into lines.
template <typename tstring>
std::vector<tstring> split_lines(const tstring& str) {
    std::vector<tstring> ret;
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
template <typename tstring>
tstring trim(const tstring& str) {
    size_t start = 0;
    for (size_t i = 0; i < str.size(); i++) {
        if (str[i] != ' ' && str[i] != '\t') {
            start = i;
            break;
        }
    }
    size_t end = str.size();
    for (size_t i = str.size() - 1; i >= start; i--) {
        if (str[i] != ' ' && str[i] != '\t') {
            end = i + 1;
            break;
        }
    }
    return str.substr(start, end - start);
}


template<typename tchar_t = char>
struct markov_chain {
    using tchar = tchar_t;
    using tstring = std::basic_string<tchar>;

    size_t order;
    std::unordered_map<tstring, std::vector<tchar>> chain;
    std::mt19937 rng;

    markov_chain(const tstring& text, size_t order, size_t seed = std::random_device()()) : order(order) {
        for (size_t i = 0; i < text.size() - order; i++)
            chain[text.substr(i, order)].push_back(text[i + order]);
        rng.seed(seed);
    }

    tstring generate(size_t length) {
        tstring result;
        result.reserve(length);

        tstring ngram;
        for (;;) {
            auto seed = chain.begin();
            std::advance(seed, rng() % chain.size());
            ngram = seed->first;
            if (!ngram.starts_with(' ')) continue;

            result.append(seed->first);
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
    multiple<option<"-f", "The input file", file_data, true>>,
    option<"--length", "The maximum length of the output", int64_t>,
    option<"--lines", "How many lines to generate", int64_t>,
    option<"--order", "The order of the ngrams", int64_t>,
    option<"--seed", "The seed for the random number generator", int64_t>,
    option<"--min-line", "Ignore lines that are shorter than this", int64_t>,
    flag<"--dump-input", "Print the processed text instead of generating output">,
    help
>; // clang-format on

int main(int argc, char** argv) {
    setlocale(LC_ALL, "");

    auto opts = options::parse(argc, argv);

    size_t length = opts.has<"--length">() ? opts.get<"--length">() : 100;
    size_t lines = opts.has<"--lines">() ? opts.get<"--lines">() : 1;
    size_t order = opts.has<"--order">() ? opts.get<"--order">() : 6;
    size_t min_line = opts.has<"--min-line">() ? opts.get<"--min-line">() : 0;

    for (auto& input : opts.get<"-f">()) {
        /// Remove short line to avoid generating gibberish.
        if (min_line) {
            auto input_lines = split_lines(input);
            std::erase_if(input_lines, [min_line](const auto& line) { return line.empty() || line.size() < min_line; });
            input = fmt::format("{}", fmt::join(input_lines, "\n"));
        }

        /// Replace newlines with spaces.
        char* c = input.data();
        while (c = strchr(c, '\n'), c) *c = ' ';

        /// Remove non-ascii chars.
        static const std::string ascii_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'\".,-_:;!?() ";
        std::erase_if(input, [](char c) { return !ascii_chars.contains(c); });

        /// Convert to lowercase.
        auto lower = to_lower(input);

        /// Print the input if requested.
        if (opts.has<"--dump-input">()) {
            fmt::print("{}\n", lower);
            continue;
        }

        /// Convert to utf32.
        auto str = to_utf32(lower);

        /// Build the markov chain.
        markov_chain<char32_t> mc(str, order);

        /// Generate words.
        for (size_t i = 0; i < lines; i++) {
            auto out = to_utf8(mc.generate(length));
            fmt::print("{}\n", trim(out));
        }
    }
}