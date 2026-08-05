/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <array>
#include <stdexcept>
#include <vector>

namespace zlink::samples::bingo
{

class bingo_card_t
{
  public:
    static constexpr std::size_t cell_count = 9;
    static constexpr int free_cell_index = 4;

    explicit bingo_card_t (const std::vector<int> &numbers)
    {
        if (numbers.size () != cell_count) {
            throw std::runtime_error ("bingo card must contain exactly 9 numbers");
        }

        std::array<bool, 16> seen{};
        for (std::size_t i = 0; i < numbers.size (); ++i) {
            const auto number = numbers[i];
            if (i == free_cell_index && number == 0) {
                _numbers[i] = number;
                continue;
            }
            if (number < 1 || number > 15) {
                throw std::runtime_error ("bingo card number must be between 1 and 15");
            }
            if (seen[static_cast<std::size_t> (number)]) {
                throw std::runtime_error ("bingo card numbers must be unique");
            }
            seen[static_cast<std::size_t> (number)] = true;
            _numbers[i] = number;
        }
        _marks[free_cell_index] = true;
    }

    const std::array<int, cell_count> &numbers () const noexcept { return _numbers; }
    const std::array<bool, cell_count> &marks () const noexcept { return _marks; }

    void mark (int number)
    {
        for (std::size_t i = 0; i < _numbers.size (); ++i) {
            if (_numbers[i] == number) {
                _marks[i] = true;
            }
        }
    }

    int completed_lines () const noexcept
    {
        static constexpr std::array<std::array<int, 3>, 8> lines{{
          {{0, 1, 2}},
          {{3, 4, 5}},
          {{6, 7, 8}},
          {{0, 3, 6}},
          {{1, 4, 7}},
          {{2, 5, 8}},
          {{0, 4, 8}},
          {{2, 4, 6}},
        }};

        int completed = 0;
        for (const auto &line : lines) {
            if (_marks[static_cast<std::size_t> (line[0])]
                && _marks[static_cast<std::size_t> (line[1])]
                && _marks[static_cast<std::size_t> (line[2])]) {
                ++completed;
            }
        }
        return completed;
    }

  private:
    std::array<int, cell_count> _numbers{};
    std::array<bool, cell_count> _marks{};
};

} // namespace zlink::samples::bingo
