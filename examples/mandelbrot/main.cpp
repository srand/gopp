// mandelbrot: renders the Mandelbrot set in ASCII with a pool of worker
// fibers. Row numbers go out on one channel, rendered rows come back on
// another, and main puts them in order.
//
//     $ mandelbrot [-w 100] [-h 40] [-workers 8]
#include <gopp/chan.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace gopp;

constexpr std::string_view palette = " .:-=+*#%@";
constexpr int maxIterations = 500;

struct Row {
    int y;
    std::string pixels;
};

// Row y of a width x height picture of the set, from -2.1-1.2i to 0.9+1.2i.
std::string renderRow(int y, int width, int height) {
    std::string pixels;
    pixels.reserve(width);
    double cy = 1.2 - 2.4 * y / (height - 1);
    for (int x = 0; x < width; x++) {
        double cx = -2.1 + 3.0 * x / (width - 1);
        double zx = 0, zy = 0;
        int i = 0;
        for (; zx * zx + zy * zy <= 4 && i < maxIterations; i++) {
            double nextZx = zx * zx - zy * zy + cx;
            zy = 2 * zx * zy + cy;
            zx = nextZx;
        }
        // Points in the set are the darkest; the others by how fast they escape.
        std::size_t last = palette.size() - 1;
        pixels += palette[i == maxIterations ? last : std::size_t(i) * last / maxIterations];
    }
    return pixels;
}

int main(int argc, char** argv) {
    isize width = 100, height = 40, workers = 8;
    flag::IntVar(&width, "w", width, "the width in characters");
    flag::IntVar(&height, "h", height, "the height in lines");
    flag::IntVar(&workers, "workers", workers, "the rows rendered at once");
    flag::Parse(argc, argv);

    auto jobs = make_chan<int>(height);
    auto rows = make_chan<Row>(height);
    sync::WaitGroup pool;
    for (isize i = 0; i < workers; i++) {
        pool.Go([=] {
            for (int y : jobs) rows.send(Row{y, renderRow(y, int(width), int(height))});
        });
    }

    for (int y = 0; y < height; y++) jobs.send(y);
    jobs.close();
    pool.Wait();
    rows.close();

    std::vector<std::string> image(height);
    for (Row row : rows) image[row.y] = std::move(row.pixels);
    for (const auto& line : image) fmt::Println("{}", line);
}
