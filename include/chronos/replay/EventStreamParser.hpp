#pragma once

#include "chronos/core/Types.hpp"

#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <istream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace chronos {

class EventStreamParser {
public:
    // CSV schema: Action,OrderId,Price,Quantity,Side[,OrderType]. A header is
    // optional. Actions are A/I (add), C (cancel), or M (modify); side is
    // B/S; order type is L/M and defaults to Limit. Cancel rows use 0
    // placeholders for unused price and quantity fields.
    [[nodiscard]] static std::vector<EventCommand> parseStream(std::istream& input) {
        std::vector<EventCommand> commands;
        std::string line;
        std::size_t lineNumber = 0;
        bool firstNonBlank = true;
        while (std::getline(input, line)) {
            ++lineNumber;
            std::string_view view = trim(line);
            if (view.empty()) {
                continue;
            }
            if (firstNonBlank && isHeader(view)) {
                firstNonBlank = false;
                continue;
            }
            firstNonBlank = false;
            try {
                commands.push_back(parseRow(view));
            } catch (const std::exception& error) {
                throw std::runtime_error("CSV line " + std::to_string(lineNumber) + ": " + error.what());
            }
        }
        if (input.bad()) {
            throw std::runtime_error("error while reading event stream");
        }
        return commands;
    }

    [[nodiscard]] static std::vector<EventCommand> parseFile(const std::string& path) {
        std::ifstream input(path);
        if (!input) {
            throw std::runtime_error("cannot open event stream: " + path);
        }
        return parseStream(input);
    }

private:
    static std::string_view trim(std::string_view value) noexcept {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
            value.remove_suffix(1);
        }
        return value;
    }

    static bool isHeader(std::string_view line) noexcept {
        const std::size_t comma = line.find(',');
        std::string_view first = trim(line.substr(0, comma));
        if (first.size() != 6) {
            return false;
        }
        constexpr std::string_view action = "action";
        for (std::size_t i = 0; i < action.size(); ++i) {
            if (static_cast<char>(std::tolower(static_cast<unsigned char>(first[i]))) != action[i]) {
                return false;
            }
        }
        return true;
    }

    static std::array<std::string_view, 6> split(std::string_view line, std::size_t& count) {
        std::array<std::string_view, 6> fields{};
        count = 0;
        std::size_t begin = 0;
        while (begin <= line.size()) {
            if (count == fields.size()) {
                throw std::invalid_argument("too many CSV fields");
            }
            const std::size_t comma = line.find(',', begin);
            const std::size_t end = comma == std::string_view::npos ? line.size() : comma;
            fields[count++] = trim(line.substr(begin, end - begin));
            if (comma == std::string_view::npos) {
                break;
            }
            begin = comma + 1;
        }
        return fields;
    }

    static std::uint64_t parseUnsigned(std::string_view field, const char* label) {
        if (field.empty() || field.front() == '-') {
            throw std::invalid_argument(std::string("invalid ") + label);
        }
        std::uint64_t value = 0;
        const char* first = field.data();
        const char* last = first + field.size();
        const auto result = std::from_chars(first, last, value);
        if (result.ec != std::errc{} || result.ptr != last) {
            throw std::invalid_argument(std::string("invalid ") + label);
        }
        return value;
    }

    static EventCommand parseRow(std::string_view line) {
        std::size_t count = 0;
        const auto fields = split(line, count);
        if (count != 5 && count != 6) {
            throw std::invalid_argument("expected 5 or 6 fields: Action,OrderId,Price,Quantity,Side[,OrderType]");
        }

        EventCommand command;
        if (fields[0] == "A" || fields[0] == "a" || fields[0] == "I" || fields[0] == "i") {
            command.type = RequestType::Add;
        } else if (fields[0] == "C" || fields[0] == "c") {
            command.type = RequestType::Cancel;
        } else if (fields[0] == "M" || fields[0] == "m") {
            command.type = RequestType::Modify;
        } else {
            throw std::invalid_argument("unknown action code");
        }

        command.order.id = parseUnsigned(fields[1], "OrderId");
        command.order.price = parseUnsigned(fields[2], "Price");
        command.order.quantity = parseUnsigned(fields[3], "Quantity");
        if (fields[4] == "B" || fields[4] == "b") {
            command.order.side = Side::Buy;
        } else if (fields[4] == "S" || fields[4] == "s") {
            command.order.side = Side::Sell;
        } else {
            throw std::invalid_argument("Side must be B or S");
        }
        if (count == 6) {
            if (fields[5] == "L" || fields[5] == "l") {
                command.order.type = OrderType::Limit;
            } else if (fields[5] == "M" || fields[5] == "m") {
                command.order.type = OrderType::Market;
            } else {
                throw std::invalid_argument("OrderType must be L or M");
            }
        }
        if ((command.type == RequestType::Add || command.type == RequestType::Modify) && command.order.quantity == 0) {
            throw std::invalid_argument("Add/Modify quantity must be positive");
        }
        return command;
    }
};

}  // namespace chronos
