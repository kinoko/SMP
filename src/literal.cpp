#include "CryptGMM/literal.hpp"
#include <sstream>
#include <iterator>
std::string trim(const std::string &line) {
    size_t first = line.find_first_not_of(' ');
    if (first == std::string::npos)
        return line;
    size_t last = line.find_last_not_of(' ');
    return line.substr(first, last - first + 1);
}

std::vector<std::string> splitBySpace(const std::string &line) {
    std::istringstream iss(trim(line));
    std::vector<std::string> tokens{std::istream_iterator<std::string>{iss},
        std::istream_iterator<std::string>()};
    return tokens;
}