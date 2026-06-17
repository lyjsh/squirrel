#include "JsonHighlight.h"

#include <cassert>
#include <utility>

int main()
{
    {
        const std::pair<int, int> range = JsonHighlight::ComputeVisibleLineRange(0.f, 100.f, 10.f, 100);
        assert(range.first == 0);
        assert(range.second == 11);
    }

    {
        const std::pair<int, int> range = JsonHighlight::ComputeVisibleLineRange(35.f, 20.f, 10.f, 10);
        assert(range.first == 2);
        assert(range.second == 7);
    }

    {
        const std::pair<int, int> range = JsonHighlight::ComputeVisibleLineRange(35.f, 20.f, 0.f, 10);
        assert(range.first == 0);
        assert(range.second == 10);
    }

    {
        const std::pair<int, int> range = JsonHighlight::NormalizeSelectionRange(8, 3, 20);
        assert(range.first == 3);
        assert(range.second == 8);
    }

    {
        const std::pair<int, int> range = JsonHighlight::NormalizeSelectionRange(-4, 30, 12);
        assert(range.first == 0);
        assert(range.second == 12);
    }

    return 0;
}
