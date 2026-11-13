#include <gopp/sort.hpp>

namespace gopp::sort {

void Sort(Interface& data) {
    isize n = data.Len();
    if (n <= 1) return;
    detail::interface_data d{data};
    slices::detail::pdqsort(d, 0, n, slices::detail::bits_len(n));
}

void Stable(Interface& data) {
    detail::interface_data d{data};
    slices::detail::stable(d, data.Len());
}

bool IsSorted(Interface& data) {
    for (isize i = data.Len() - 1; i > 0; i--) {
        if (data.Less(i, i - 1)) return false;
    }
    return true;
}

}  // namespace gopp::sort
